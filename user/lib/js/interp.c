/*
 * The interpreter (27o): one loop over a function's code, a frame per call
 * of a compiled function. Calls between compiled functions stay inside the
 * loop (a new frame, the loop goes on), so deep recursion uses the value
 * stack and the frame array, not the C stack; a call from C (a native
 * function, js_call) runs a loop of its own until its frame returns.
 *
 * The value stack holds each frame's arguments (as the caller pushed them:
 * function, this, arguments), its registers and its operand stack.
 * Exceptions: native code returns JS_EXCEPTION with the value kept in the
 * world; the loop then pops to the innermost handler of a `try` (TRY),
 * restoring its stack depth and scope, or leaves the frame.
 */
#include <math.h>
#include "jsint.h"

/* --- closures ------------------------------------------------------------ */

struct jsobject *closure_new(struct js *J, struct funcdef *def, struct scope *env, struct jsobject *home)
{
    struct jsobject *f = obj_new(J, J->proto[P_FUNCTION], CLASS_FUNCTION);
    f->u.fn.def = def;
    f->u.fn.env = env;
    f->u.fn.home = home;
    f->u.fn.fields = JS_UNDEFINED;
    f->h.flags |= OBJ_CALLABLE;
    if (def->flags & FD_CLASS_CTOR)
        f->h.flags |= OBJ_CONSTRUCTOR | OBJ_CLASS_CTOR | ((def->flags & FD_DERIVED) ? OBJ_DERIVED : 0);
    else if (!(def->flags & FD_NO_CTOR))
        f->h.flags |= OBJ_CONSTRUCTOR;
    prop_add(J, f, str_val(J->atom[A_length]), js_number(def->nparams), PROP_CONFIGURABLE);
    prop_add(J, f, str_val(J->atom[A_name]), str_val(def->name ? def->name : J->atom[A_empty]), PROP_CONFIGURABLE);
    if (!(def->flags & (FD_NO_CTOR | FD_CLASS_CTOR))) {
        struct jsobject *p = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
        prop_add(J, p, str_val(J->atom[A_constructor]), obj_val(f), PROP_WRITABLE | PROP_CONFIGURABLE);
        prop_add(J, f, str_val(J->atom[A_prototype]), obj_val(p), PROP_WRITABLE);
    } else if ((def->flags & (FD_GENERATOR | FD_ASYNC)) == FD_GENERATOR) {
        /* (27r) its generators' prototype: one inheriting %GeneratorPrototype%, no constructor */
        struct jsobject *p = obj_new(J, J->proto[P_GENERATOR], CLASS_OBJECT);
        prop_add(J, f, str_val(J->atom[A_prototype]), obj_val(p), PROP_WRITABLE);
    }
    return f;
}

struct scope *scope_new(struct js *J, struct scope *parent, uint32_t n)
{
    struct scope *s = gc_alloc(J, CELL_SCOPE, sizeof *s + (size_t)n * sizeof(jsval));
    if (!s)
        abort();
    s->h.extra = n;
    s->parent = parent;
    for (uint32_t i = 0; i < n; i++)
        s->vars[i] = JS_UNDEFINED;
    return s;
}

int vm_current_line(struct js *J, struct jsstring **file)
{
    for (uint32_t k = J->nframes; k-- > 0;) {
        struct frame *F = &J->frames[k];
        if (!F->def)
            continue;
        uint32_t off = (uint32_t)(F->pc - F->def->code);
        int line = 0;
        for (uint32_t i = 0; i < F->def->nlines && F->def->lines[i].pc <= off; i++)
            line = (int)F->def->lines[i].line;
        if (file)
            *file = F->def->file;
        return line;
    }
    if (file)
        *file = 0;
    return 0;
}

/* --- small helpers --------------------------------------------------------- */

static jsval array_append(struct js *J, struct jsobject *a, jsval v)
{
    if (a->length >= a->cap && !array_grow(J, a, a->length + 1))
        return throw_oom(J);
    a->elems[a->length++] = v;
    return JS_TRUE;
}

static jsval spread_into(struct js *J, struct jsobject *a, jsval iterable)
{
    if (js_is_object(iterable) && obj_class(to_obj(iterable)) == CLASS_ARRAY && !(to_obj(iterable)->h.flags & OBJ_SPARSE)) {
        struct iter it;
        if (iter_open(J, iterable, &it) < 0)
            return JS_EXCEPTION;
        if (it.fast) {
            struct jsobject *src = to_obj(iterable);
            for (uint32_t i = 0; i < src->length; i++) {
                jsval v = i < src->cap ? src->elems[i] : JS_EMPTY;
                if (v == JS_EMPTY) {
                    v = obj_get(J, src, str_val(atom_index(J, i)), iterable);
                    if (v == JS_EXCEPTION)
                        return v;
                }
                if (array_append(J, a, v) == JS_EXCEPTION)
                    return JS_EXCEPTION;
            }
            return JS_TRUE;
        }
        jsval v;
        int r;
        while ((r = iter_step(J, &it, &v)) > 0)
            if (array_append(J, a, v) == JS_EXCEPTION)
                return JS_EXCEPTION;
        return r < 0 ? JS_EXCEPTION : JS_TRUE;
    }
    struct iter it;
    if (iter_open(J, iterable, &it) < 0)
        return JS_EXCEPTION;
    jsval v;
    int r;
    while ((r = iter_step(J, &it, &v)) > 0)
        if (array_append(J, a, v) == JS_EXCEPTION)
            return JS_EXCEPTION;
    return r < 0 ? JS_EXCEPTION : JS_TRUE;
}

static jsval not_function(struct js *J, jsval v, bool ctor)
{
    char *d = js_describe(J, v);
    throw_type(J, ctor ? "%s is not a constructor" : "%s is not a function", d);
    free(d);
    return JS_EXCEPTION;
}

static jsval make_arguments(struct js *J, struct frame *F, const uint8_t *map, unsigned nmap)
{
    struct jsobject *a = obj_new(J, J->proto[P_OBJECT], CLASS_ARGUMENTS);
    if (nmap && F->argc) {
        uint32_t n = F->argc < nmap ? F->argc : nmap;
        a->u.args.map = malloc(n * sizeof *a->u.args.map);
        if (!a->u.args.map)
            abort();
        for (uint32_t i = 0; i < n; i++)
            a->u.args.map[i] = (int32_t)((unsigned)map[2 * i] | (unsigned)map[2 * i + 1] << 8);
        a->u.args.nmap = n;
        a->u.args.scope = F->scope;
    }
    if (F->argc && !array_grow(J, a, F->argc))
        return throw_oom(J);
    for (uint32_t i = 0; i < F->argc; i++)
        a->elems[i] = F->argv[i];
    a->length = F->argc;
    prop_add(J, a, str_val(J->atom[A_length]), js_number(F->argc), PROP_WRITABLE | PROP_CONFIGURABLE);
    prop_add(J, a, J->symbol[SYM_ITERATOR], J->array_proto_values, PROP_WRITABLE | PROP_CONFIGURABLE);
    if (F->def->flags & FD_STRICT) {
        jsval t = obj_val(J->throw_type_error);
        struct pdesc d = { .get = t, .set = t, .has = DESC_GET | DESC_SET | DESC_ENUMERABLE | DESC_CONFIGURABLE };
        obj_define(J, a, str_val(J->atom[A_callee]), &d, false);
    } else
        prop_add(J, a, str_val(J->atom[A_callee]), obj_val(F->callee), PROP_WRITABLE | PROP_CONFIGURABLE);
    return obj_val(a);
}

static jsval op_add(struct js *J, jsval a, jsval b)
{
    if (js_is_number(a) && js_is_number(b))
        return js_number(js_number_value(a) + js_number_value(b));
    if (js_is_string(a) && js_is_string(b)) {
        struct jsstring *s = str_concat(J, to_str(a), to_str(b));
        return s ? str_val(s) : JS_EXCEPTION;
    }
    a = js_to_primitive(J, a, 0);
    if (a == JS_EXCEPTION)
        return a;
    b = js_to_primitive(J, b, 0);
    if (b == JS_EXCEPTION)
        return b;
    if (js_is_string(a) || js_is_string(b)) {
        struct jsstring *x = js_to_str(J, a);
        if (!x)
            return JS_EXCEPTION;
        struct jsstring *y = js_to_str(J, b);
        if (!y)
            return JS_EXCEPTION;
        struct jsstring *s = str_concat(J, x, y);
        return s ? str_val(s) : JS_EXCEPTION;
    }
    bool threw = false;
    double x = js_to_number(J, a, &threw);
    if (threw)
        return JS_EXCEPTION;
    double y = js_to_number(J, b, &threw);
    if (threw)
        return JS_EXCEPTION;
    return js_number(x + y);
}

double js_pow(double x, double y)
{
    if (y != y)
        return NAN;
    if (y == 0)
        return 1;
    if ((x == 1 || x == -1) && isinf(y))
        return NAN;
    if (y >= 0 && y <= 1000 && y == (int)y) {
        /* whole exponents by squaring, as JavaScriptCore does (its results,
         * to the last bit, are what the tests compare with) */
        double r = 1, b = x;
        for (int e = (int)y; e; e >>= 1) {
            if (e & 1)
                r *= b;
            b *= b;
        }
        return r;
    }
    return pow(x, y);
}

static jsval op_arith(struct js *J, int o, jsval a, jsval b)
{
    double x, y;
    if (js_is_number(a) && js_is_number(b)) {
        x = js_number_value(a);
        y = js_number_value(b);
    } else {
        bool threw = false;
        x = js_to_number(J, a, &threw);
        if (threw)
            return JS_EXCEPTION;
        y = js_to_number(J, b, &threw);
        if (threw)
            return JS_EXCEPTION;
    }
    switch (o) {
    case OP_SUB: return js_number(x - y);
    case OP_MUL: return js_number(x * y);
    case OP_DIV: return js_number(x / y);
    case OP_MOD: return js_number(fmod(x, y));
    case OP_POW: return js_number(js_pow(x, y));
    case OP_BAND: return js_number(js_to_int32(x) & js_to_int32(y));
    case OP_BOR: return js_number(js_to_int32(x) | js_to_int32(y));
    case OP_BXOR: return js_number(js_to_int32(x) ^ js_to_int32(y));
    case OP_SHL: return js_number((int32_t)((uint32_t)js_to_int32(x) << (js_to_uint32(y) & 31)));
    case OP_SAR: return js_number(js_to_int32(x) >> (js_to_uint32(y) & 31));
    case OP_SHR: return js_number(js_to_uint32(x) >> (js_to_uint32(y) & 31));
    }
    return JS_UNDEFINED;
}

/* a < b (or a <= b): 1 true, 0 false, 2 undefined (NaN), -1 threw.
 * left_first: the order the operands are converted in. */
static int less_than(struct js *J, jsval a, jsval b, bool left_first)
{
    jsval pa, pb;
    if (left_first) {
        pa = js_to_primitive(J, a, 1);
        if (pa == JS_EXCEPTION)
            return -1;
        pb = js_to_primitive(J, b, 1);
        if (pb == JS_EXCEPTION)
            return -1;
    } else {
        pb = js_to_primitive(J, b, 1);
        if (pb == JS_EXCEPTION)
            return -1;
        pa = js_to_primitive(J, a, 1);
        if (pa == JS_EXCEPTION)
            return -1;
    }
    if (js_is_string(pa) && js_is_string(pb))
        return str_compare(J, to_str(pa), to_str(pb)) < 0;
    bool threw = false;
    double x = js_to_number(J, pa, &threw);
    if (threw)
        return -1;
    double y = js_to_number(J, pb, &threw);
    if (threw)
        return -1;
    if (x != x || y != y)
        return 2;
    return x < y;
}

static jsval op_compare(struct js *J, int o, jsval a, jsval b)
{
    if (js_is_number(a) && js_is_number(b)) {
        double x = js_number_value(a), y = js_number_value(b);
        switch (o) {
        case OP_LT: return js_bool(x < y);
        case OP_LE: return js_bool(x <= y);
        case OP_GT: return js_bool(x > y);
        default: return js_bool(x >= y);
        }
    }
    int r;
    switch (o) {
    case OP_LT:
        r = less_than(J, a, b, true);
        return r < 0 ? JS_EXCEPTION : js_bool(r == 1);
    case OP_GT:
        r = less_than(J, b, a, false);
        return r < 0 ? JS_EXCEPTION : js_bool(r == 1);
    case OP_LE:
        r = less_than(J, b, a, false);
        return r < 0 ? JS_EXCEPTION : js_bool(r == 0);
    default:
        r = less_than(J, a, b, true);
        return r < 0 ? JS_EXCEPTION : js_bool(r == 0);
    }
}

static jsval global_lookup(struct js *J, jsval key, bool for_typeof)
{
    struct prop *p = prop_find(&J->global_lex->props, key);
    if (p) {
        if (p->value == JS_EMPTY) {
            char *k = js_describe(J, key);
            throw_reference(J, "Cannot access '%s' before initialization.", k);
            free(k);
            return JS_EXCEPTION;
        }
        return p->value;
    }
    p = prop_find(&J->global->props, key);
    if (p && !(p->flags & PROP_ACCESSOR))
        return p->value;
    int has = obj_has(J, J->global, key);
    if (has < 0)
        return JS_EXCEPTION;
    if (!has) {
        if (for_typeof)
            return JS_UNDEFINED;
        char *k = js_describe(J, key);
        throw_reference(J, "Can't find variable: %s", k);
        free(k);
        return JS_EXCEPTION;
    }
    return obj_get(J, J->global, key, obj_val(J->global));
}

static int global_store(struct js *J, jsval key, jsval v, bool strict)
{
    struct prop *p = prop_find(&J->global_lex->props, key);
    if (p) {
        char *k;
        if (p->value == JS_EMPTY) {
            k = js_describe(J, key);
            throw_reference(J, "Cannot access '%s' before initialization.", k);
            free(k);
            return -1;
        }
        if (!(p->flags & PROP_WRITABLE)) {
            throw_type(J, "Attempted to assign to readonly property.");
            return -1;
        }
        p->value = v;
        return 1;
    }
    p = prop_find(&J->global->props, key);
    if (p && (p->flags & (PROP_ACCESSOR | PROP_WRITABLE)) == PROP_WRITABLE) {
        p->value = v;
        return 1;
    }
    if (strict && !p) {
        int has = obj_has(J, J->global, key);
        if (has < 0)
            return -1;
        if (!has) {
            char *k = js_describe(J, key);
            throw_reference(J, "Can't find variable: %s", k);
            free(k);
            return -1;
        }
    }
    return obj_set(J, J->global, key, v, obj_val(J->global), strict);
}

static jsval tdz_error(struct js *J, jsval name)
{
    if (js_is_string(name) && to_str(name) == J->atom[A_this])
        return throw_reference(J, "Must call super constructor in derived class before accessing 'this' or "
                                  "returning from derived constructor");
    char *k = js_describe(J, name);
    throw_reference(J, "Cannot access '%s' before initialization.", k);
    free(k);
    return JS_EXCEPTION;
}

void set_function_name(struct js *J, struct jsobject *f, jsval key, const char *prefix)
{
    struct jsstring *name;
    if (js_is_symbol(key)) {
        struct jssymbol *s = to_sym(key);
        if (!s->description)
            name = J->atom[A_empty];
        else if (s->h.flags & 2)
            name = s->description;              /* private names keep their # */
        else {
            struct sbuf b = { 0 };
            sb_unit(&b, '[');
            sb_str(J, &b, s->description);
            sb_unit(&b, ']');
            name = sb_finish(J, &b);
        }
    } else
        name = to_str(key);
    if (prefix) {
        struct sbuf b = { 0 };
        sb_ascii(&b, prefix);
        sb_str(J, &b, name);
        name = sb_finish(J, &b);
    }
    obj_define_value(J, f, str_val(J->atom[A_name]), str_val(name), PROP_CONFIGURABLE);
}

static jsval define_method(struct js *J, struct jsobject *o, jsval key, jsval fv, int kind, bool enumerable)
{
    struct jsobject *f = to_obj(fv);
    if (obj_class(f) == CLASS_FUNCTION)
        f->u.fn.home = o;
    int k = kind & 3;
    if (k || (kind & 12) || js_is_symbol(key))
        set_function_name(J, f, key, k == 1 ? "get " : k == 2 ? "set " : 0);
    uint32_t flags = (enumerable ? PROP_ENUMERABLE : 0) | PROP_CONFIGURABLE;
    if (kind & 4)
        flags = 0;                              /* private methods and accessors */
    if (k == 0) {
        struct pdesc d = { .value = fv, .flags = flags | ((kind & 4) ? 0 : PROP_WRITABLE),
                           .has = DESC_VALUE | DESC_WRITABLE | DESC_ENUMERABLE | DESC_CONFIGURABLE };
        if (obj_define(J, o, key, &d, true) < 0)
            return JS_EXCEPTION;
        return JS_TRUE;
    }
    struct pdesc d = { .flags = flags, .has = DESC_ENUMERABLE | DESC_CONFIGURABLE };
    if (k == 1) {
        d.get = fv;
        d.has |= DESC_GET;
    } else {
        d.set = fv;
        d.has |= DESC_SET;
    }
    if (obj_define(J, o, key, &d, true) < 0)
        return JS_EXCEPTION;
    return JS_TRUE;
}

static jsval copy_data_props(struct js *J, struct jsobject *target, jsval src, jsval *excluded, uint32_t nex)
{
    if (js_is_nullish(src))
        return JS_TRUE;
    struct jsobject *from = js_to_object(J, src);
    if (!from)
        return JS_EXCEPTION;
    jsval *keys;
    uint32_t n;
    if (obj_own_keys(J, from, 3, &keys, &n) < 0) {
        free(keys);
        return JS_EXCEPTION;
    }
    js_root_push(J, keys, n);
    jsval r = JS_TRUE;
    for (uint32_t i = 0; i < n && r != JS_EXCEPTION; i++) {
        bool skip = false;
        for (uint32_t k = 0; k < nex; k++)
            if (excluded[k] == keys[i])
                skip = true;
        if (skip || (js_is_symbol(keys[i]) && (to_sym(keys[i])->h.flags & 2)))
            continue;
        struct pdesc d;
        int has = obj_get_own(J, from, keys[i], &d);
        if (has < 0)
            r = JS_EXCEPTION;
        if (has <= 0 || !(d.flags & PROP_ENUMERABLE))
            continue;
        jsval v = obj_get(J, from, keys[i], obj_val(from));
        if (v == JS_EXCEPTION) {
            r = v;
            break;
        }
        obj_define_value(J, target, keys[i], v, PROP_DEFAULT);
    }
    js_root_pop(J);
    free(keys);
    return r;
}

/* (64r) A private member is the object's own, also a proxy's (never its
 * target's). */
static int private_own(struct js *J, struct jsobject *o, jsval key, struct pdesc *d)
{
    if (obj_class(o) != CLASS_PROXY)
        return obj_get_own(J, o, key, d);
    struct prop *p = prop_find(&o->props, key);
    if (!p)
        return 0;
    d->flags = p->flags;
    d->get = d->set = d->value = JS_UNDEFINED;
    if (p->flags & PROP_ACCESSOR) {
        struct accessor *a = js_pointer(p->value);
        d->get = a->get;
        d->set = a->set;
    } else {
        d->value = p->value;
    }
    return 1;
}

/* for-in's keys: every object's up the chain, the first of each name (a
 * non-enumerable one still hides the same name further up). (64r) All
 * the objects' keys and prototypes first, then the descriptors, in that
 * order: what JavaScriptCore asks of a proxy on the way (ownKeys and
 * getPrototypeOf, then getOwnPropertyDescriptor for each key). */
struct for_in_level {
    struct jsobject *o;
    jsval *keys;
    uint32_t n;
};

static jsval for_in_start(struct js *J, jsval v)
{
    struct jsobject *w = obj_new(J, 0, CLASS_FOR_IN);
    w->u.forin.object = JS_UNDEFINED;
    if (js_is_nullish(v))
        return obj_val(w);
    struct jsobject *o = js_to_object(J, v);
    if (!o)
        return JS_EXCEPTION;
    w->u.forin.object = obj_val(o);
    J->gc_inhibit++;
    bool threw = false;
    struct for_in_level *levels = 0;
    uint32_t nlevels = 0, total = 0;
    for (struct jsobject *cur = o; cur && !threw;) {
        if (nlevels == 100000) {                /* a proxy's prototypes need not end */
            throw_range(J, "Maximum call stack size exceeded.");
            threw = true;
            break;
        }
        levels = realloc(levels, (nlevels + 1) * sizeof *levels);
        if (!levels)
            abort();
        struct for_in_level *l = &levels[nlevels++];
        l->o = cur;
        if (obj_own_keys(J, cur, 1, &l->keys, &l->n) < 0 || obj_get_proto(J, cur, &cur) < 0)
            threw = true;
        total += l->n;
    }
    jsval *all = malloc(((size_t)total + 1) * sizeof *all);
    if (!all)
        abort();
    uint32_t n = 0;
    for (uint32_t li = 0; li < nlevels && !threw; li++) {
        struct for_in_level *l = &levels[li];
        for (uint32_t i = 0; i < l->n && !threw; i++) {
            bool seen = false;
            for (uint32_t k = 0; k < n && !seen; k++)
                seen = js_pointer(all[k]) == js_pointer(l->keys[i]);
            if (seen)
                continue;
            struct pdesc d;
            int has = obj_get_own(J, l->o, l->keys[i], &d);
            if (has < 0)
                threw = true;
            else if (has > 0)                    /* hidden ones marked, dropped below */
                all[n++] = (d.flags & PROP_ENUMERABLE) ? l->keys[i]
                           : JS_MAKE_TAG(JS_T_EMPTY) | (uint64_t)(uintptr_t)js_pointer(l->keys[i]);
        }
    }
    for (uint32_t li = 0; li < nlevels; li++)
        free(levels[li].keys);
    free(levels);
    if (threw) {
        J->gc_inhibit--;
        free(all);
        return JS_EXCEPTION;
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++)
        if (js_tag(all[i]) != JS_T_EMPTY)
            all[k++] = all[i];
    J->gc_inhibit--;
    w->u.forin.keys = all;
    w->u.forin.n = k;
    return obj_val(w);
}

/* --- calls --------------------------------------------------------------- */

static jsval stack_overflow(struct js *J)
{
    return throw_range(J, "Maximum call stack size exceeded.");
}

/* Sets up a frame for compiled function f, its registers at `top`. */
static struct frame *enter(struct js *J, struct jsobject *f, jsval this_val, jsval *argv, uint32_t argc,
                           jsval new_target, bool construct, jsval *top)
{
    struct funcdef *def = f->u.fn.def;
    if (J->nframes >= FRAME_MAX || top + def->nregs + def->stack_max + 16 > J->stack_end) {
        stack_overflow(J);
        return 0;
    }
    if (!construct && !(def->flags & (FD_STRICT | FD_ARROW))) {
        if (js_is_nullish(this_val))
            this_val = obj_val(J->global);
        else if (!js_is_object(this_val)) {
            struct jsobject *o = js_to_object(J, this_val);
            if (!o)
                return 0;
            this_val = obj_val(o);
        }
    }
    struct frame *F = &J->frames[J->nframes++];
    F->def = def;
    F->callee = f;
    F->this_val = this_val;
    F->new_target = new_target;
    F->argv = argv;
    F->argc = argc;
    F->regs = top;
    for (uint32_t i = 0; i < def->nregs; i++)
        top[i] = JS_UNDEFINED;
    F->sp_base = top + def->nregs;
    F->scope = f->u.fn.env;
    F->pc = def->code;
    F->handlers = J->nhandlers;
    F->entry = 0;
    F->construct = construct;
    F->gen = 0;
    jsval *high = F->sp_base + def->stack_max;
    if (high > J->sp_high)
        J->sp_high = high;
    return F;
}

static jsval run(struct js *J);

/* this for [[Construct]] of a base class or function */
static jsval construct_this(struct js *J, struct jsobject *f, jsval new_target)
{
    struct jsobject *proto = js_proto_from_ctor(J, new_target, J->proto[P_OBJECT]);
    if (!proto)
        return JS_EXCEPTION;
    return obj_val(obj_new(J, proto, CLASS_OBJECT));
    (void)f;
}

static jsval call_native(struct js *J, struct jsobject *f, jsval this_val, int argc, jsval *argv, jsval new_target)
{
    if (J->native_depth >= NATIVE_DEPTH_MAX)
        return stack_overflow(J);
    /* argv holds at least the function's length (and 4) values, padded with
     * undefined; argc stays the number the caller gave */
    int want = f->u.native.length < 4 ? 4 : f->u.native.length;
    jsval *args = argv;
    jsval *saved_high = J->sp_high;
    if (argc < want) {
        jsval *top = J->sp_high > J->stack ? J->sp_high : J->stack;
        if (top + want + 16 > J->stack_end)
            return stack_overflow(J);
        for (int i = 0; i < argc; i++)
            top[i] = argv[i];
        for (int i = argc; i < want; i++)
            top[i] = JS_UNDEFINED;
        args = top;
        J->sp_high = top + want;
    }
    J->native_depth++;
    J->native_magic = f->u.native.magic;
    J->native_callee = f;                       /* (27r: its data; read it first, as the magic) */
    jsval r = f->u.native.fn(J, this_val, argc, args, new_target);
    J->native_depth--;
    J->sp_high = saved_high;
    if (r == JS_EXCEPTION && !J->has_exception)
        throw_type(J, "Internal error");
    return r;
}

jsval vm_call(struct js *J, jsval fv, jsval this_val, int argc, jsval *argv, jsval new_target)
{
    if (!js_is_object(fv))
        return not_function(J, fv, !js_is_undefined(new_target));
    struct jsobject *f = to_obj(fv);
    bool construct = !js_is_undefined(new_target);
    if (construct ? !(f->h.flags & OBJ_CONSTRUCTOR) : !(f->h.flags & OBJ_CALLABLE))
        return not_function(J, fv, construct);
    switch (obj_class(f)) {
    case CLASS_NATIVE:
        return call_native(J, f, this_val, argc, argv, new_target);
    case CLASS_BOUND: {
        uint32_t n = f->u.bound.argc + (uint32_t)argc;
        jsval *top = J->sp_high;
        if (top + n + 16 > J->stack_end)
            return stack_overflow(J);
        for (uint32_t i = 0; i < f->u.bound.argc; i++)
            top[i] = f->u.bound.args[i];
        for (int i = 0; i < argc; i++)
            top[f->u.bound.argc + (uint32_t)i] = argv[i];
        jsval *saved = J->sp_high;
        J->sp_high = top + n;
        jsval nt = new_target;
        if (construct && nt == fv)
            nt = f->u.bound.target;
        if (J->native_depth >= NATIVE_DEPTH_MAX) {
            J->sp_high = saved;
            return stack_overflow(J);
        }
        J->native_depth++;
        jsval r = vm_call(J, f->u.bound.target, f->u.bound.this_val, (int)n, top, nt);
        J->native_depth--;
        J->sp_high = saved;
        return r;
    }
    case CLASS_FUNCTION:
        break;
    case CLASS_PROXY:                           /* (64r) */
        return proxy_call(J, f, this_val, argc, argv, new_target);
    default:
        return not_function(J, fv, construct);
    }
    if ((f->h.flags & OBJ_CLASS_CTOR) && !construct)
        return throw_type(J, "Cannot call a class constructor without |new|");
    if (J->native_depth >= NATIVE_DEPTH_MAX)
        return stack_overflow(J);
    /* f this args, then the frame, above everything in use */
    jsval *base = J->sp_high > J->stack ? J->sp_high : J->stack;
    if (base + argc + 2 + 16 > J->stack_end)
        return stack_overflow(J);
    jsval *saved_high = J->sp_high;
    base[0] = fv;
    base[1] = this_val;
    for (int i = 0; i < argc; i++)
        base[2 + i] = argv[i];
    J->sp_high = base + 2 + argc;
    jsval t = this_val;
    if (construct) {
        if (f->h.flags & OBJ_DERIVED)
            t = JS_EMPTY;
        else {
            t = construct_this(J, f, new_target);
            if (t == JS_EXCEPTION) {
                J->sp_high = saved_high;
                return t;
            }
        }
        base[1] = t;
    }
    struct frame *F = enter(J, f, t, base + 2, (uint32_t)argc, new_target, construct, base + 2 + argc);
    if (!F) {
        J->sp_high = saved_high;
        return JS_EXCEPTION;
    }
    F->entry = 1;
    J->native_depth++;
    jsval r = run(J);
    J->native_depth--;
    J->sp_high = saved_high;
    return r;
}

jsval js_call(struct js *J, jsval f, jsval this_val, int argc, jsval *argv)
{
    return vm_call(J, f, this_val, argc, argv, JS_UNDEFINED);
}

jsval js_construct(struct js *J, jsval f, int argc, jsval *argv)
{
    return vm_call(J, f, JS_UNDEFINED, argc, argv, f);
}

jsval vm_run_script(struct js *J, struct funcdef *def)
{
    struct jsobject *f = closure_new(J, def, 0, 0);
    return vm_call(J, obj_val(f), obj_val(J->global), 0, 0, JS_UNDEFINED);
}

/* --- generators (27r): a frame kept off the stack ----------------------- */

static void handler_room(struct js *J)
{
    if (J->nhandlers == J->handlers_cap) {
        uint32_t cap = J->handlers_cap ? J->handlers_cap * 2 : 64;
        struct handler *h = realloc(J->handlers, cap * sizeof *h);
        if (!h)
            abort();
        J->handlers = h;
        J->handlers_cap = cap;
    }
}

/* Keeps frame F (stopped at pc with its operand stack up to sp) in st:
 * its arguments, registers, operand stack and open handlers. */
void gen_save(struct js *J, struct genstate *st, struct frame *F, jsval *sp, const uint8_t *pc)
{
    struct funcdef *def = F->def;
    uint32_t depth = (uint32_t)(sp - F->sp_base), n = F->argc + def->nregs + depth;
    jsval *buf = realloc(st->saved, (n ? n : 1) * sizeof *buf);
    if (!buf)
        abort();
    memcpy(buf, F->argv, F->argc * sizeof *buf);
    memcpy(buf + F->argc, F->regs, def->nregs * sizeof *buf);
    memcpy(buf + F->argc + def->nregs, F->sp_base, depth * sizeof *buf);
    st->saved = buf;
    st->argc = F->argc, st->nregs = def->nregs, st->depth = depth;
    st->scope = F->scope;
    st->pc = (uint32_t)(pc - def->code);
    uint32_t nh = J->nhandlers - F->handlers;
    struct savedhandler *h = realloc(st->handlers, (nh ? nh : 1) * sizeof *h);
    if (!h)
        abort();
    for (uint32_t i = 0; i < nh; i++) {
        struct handler *from = &J->handlers[F->handlers + i];
        h[i].pc = (uint32_t)(from->pc - def->code);
        h[i].sp = (uint32_t)(from->sp - F->sp_base);
        h[i].scope = from->scope;
    }
    st->handlers = h;
    st->nhandlers = nh;
}

jsval gen_resume(struct js *J, struct jsobject *g, int mode, jsval v, bool *done)
{
    struct genstate *st = g->u.gen;
    *done = true;
    if (st->state == GEN_RUNNING)
        return throw_type(J, "Generator is executing");
    if (st->state == GEN_START && mode != RESUME_NEXT)
        st->state = GEN_DONE;                   /* (a return or throw before it began: it never runs) */
    if (st->state == GEN_DONE) {
        if (mode == RESUME_THROW)
            return js_throw(J, v);
        return mode == RESUME_RETURN ? v : JS_UNDEFINED;
    }
    struct funcdef *def = st->callee->u.fn.def;
    jsval *base = J->sp_high > J->stack ? J->sp_high : J->stack;
    if (J->nframes >= FRAME_MAX || J->native_depth >= NATIVE_DEPTH_MAX ||
        base + 2 + st->argc + def->nregs + def->stack_max + 16 > J->stack_end)
        return stack_overflow(J);
    jsval *saved_high = J->sp_high;
    base[0] = obj_val(st->callee);
    base[1] = st->this_val;
    jsval *argv = base + 2, *regs = argv + st->argc, *sp_base = regs + def->nregs;
    memcpy(argv, st->saved, st->argc * sizeof *argv);
    memcpy(regs, st->saved + st->argc, def->nregs * sizeof *regs);
    memcpy(sp_base, st->saved + st->argc + def->nregs, st->depth * sizeof *sp_base);
    jsval *sp = sp_base + st->depth;
    if (st->state == GEN_SUSPENDED) {           /* (the yield's result: what came, and how) */
        *sp++ = v;
        *sp++ = js_number(mode);
    }
    J->sp_high = sp_base + def->stack_max;
    if (J->sp_high < sp)
        J->sp_high = sp;
    struct frame *F = &J->frames[J->nframes++];
    F->def = def;
    F->callee = st->callee;
    F->this_val = st->this_val;
    F->new_target = st->new_target;
    F->argv = argv;
    F->argc = st->argc;
    F->regs = regs;
    F->sp_base = sp_base;
    F->scope = st->scope;
    F->pc = def->code + st->pc;
    F->handlers = J->nhandlers;
    F->entry = 1;
    F->construct = 0;
    F->gen = g;
    for (uint32_t i = 0; i < st->nhandlers; i++) {
        handler_room(J);
        struct handler *h = &J->handlers[J->nhandlers++];
        h->pc = def->code + st->handlers[i].pc;
        h->sp = sp_base + st->handlers[i].sp;
        h->scope = st->handlers[i].scope;
    }
    J->resume_sp = sp;
    st->state = GEN_RUNNING;
    J->native_depth++;
    jsval r = run(J);
    J->native_depth--;
    J->sp_high = saved_high;
    if (J->yielded) {
        J->yielded = false;
        st->state = GEN_SUSPENDED;
        *done = false;
        return r;
    }
    st->state = GEN_DONE;
    free(st->saved);
    st->saved = 0;
    st->argc = st->nregs = st->depth = 0;
    st->nhandlers = 0;
    return r;
}

/* --- the loop ---------------------------------------------------------- */

#define U16(p) ((unsigned)(p)[0] | ((unsigned)(p)[1] << 8))
static inline uint32_t U32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static inline int32_t I32(const uint8_t *p)
{
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}

static jsval run(struct js *J)
{
    struct frame *F = &J->frames[J->nframes - 1];
    struct funcdef *def = F->def;
    const uint8_t *pc = F->pc;
    jsval *sp = J->resume_sp ? J->resume_sp : F->sp_base;      /* (27r: a resumed generator's) */
    J->resume_sp = 0;
    jsval *regs = F->regs;
    jsval *K = def->consts;
    jsval a, b, c, r, fv, nt;
    jsval *argv;
    uint32_t argc;
    struct jsobject *f;
    struct frame *nf;

#define RELOAD() do { def = F->def; regs = F->regs; K = def->consts; } while (0)
#define CHECK(x) do { if ((x) == JS_EXCEPTION) goto exception; } while (0)
#define KEY(i) (K[(i)])

    for (;;) {
        F->pc = pc;
        switch ((enum opcode)*pc++) {
        case OP_NOP:
            break;
        case OP_UNDEF:
            *sp++ = JS_UNDEFINED;
            break;
        case OP_NULL:
            *sp++ = JS_NULL;
            break;
        case OP_TRUE:
            *sp++ = JS_TRUE;
            break;
        case OP_FALSE:
            *sp++ = JS_FALSE;
            break;
        case OP_EMPTY:
            *sp++ = JS_EMPTY;
            break;
        case OP_INT:
            *sp++ = js_number(I32(pc));
            pc += 4;
            break;
        case OP_CONST:
            *sp++ = K[U32(pc)];
            pc += 4;
            break;
        case OP_THIS_FRAME:
            *sp++ = F->this_val;
            break;
        case OP_NEW_TARGET_FRAME:
            *sp++ = F->new_target;
            break;
        case OP_CALLEE:
            *sp++ = obj_val(F->callee);
            break;
        case OP_HOME_FRAME:
            *sp++ = F->callee->u.fn.home ? obj_val(F->callee->u.fn.home) : JS_UNDEFINED;
            break;
        case OP_GLOBAL_THIS:
            *sp++ = obj_val(J->global);
            break;
        case OP_ARG: {
            unsigned i = U16(pc);
            pc += 2;
            *sp++ = i < F->argc ? F->argv[i] : JS_UNDEFINED;
            break;
        }
        case OP_REST: {
            unsigned i = U16(pc);
            pc += 2;
            uint32_t n = F->argc > i ? F->argc - i : 0;
            struct jsobject *arr = array_new(J, n);
            for (uint32_t k = 0; k < n; k++)
                arr->elems[k] = F->argv[i + k];
            *sp++ = obj_val(arr);
            break;
        }
        case OP_ARGUMENTS: {
            unsigned nmap = U16(pc);
            r = make_arguments(J, F, pc + 2, nmap);
            CHECK(r);
            pc += 2 + 2 * nmap;
            *sp++ = r;
            break;
        }
        case OP_GET_REG:
            *sp++ = regs[U16(pc)];
            pc += 2;
            break;
        case OP_SET_REG:
            regs[U16(pc)] = sp[-1];
            pc += 2;
            break;
        case OP_PUT_REG:
            regs[U16(pc)] = *--sp;
            pc += 2;
            break;
        case OP_GET_REG_CHECK:
            a = regs[U16(pc)];
            if (a == JS_EMPTY) {
                tdz_error(J, KEY(U32(pc + 2)));
                goto exception;
            }
            pc += 6;
            *sp++ = a;
            break;
        case OP_SET_REG_CHECK:
            if (regs[U16(pc)] == JS_EMPTY) {
                tdz_error(J, KEY(U32(pc + 2)));
                goto exception;
            }
            regs[U16(pc)] = sp[-1];
            pc += 6;
            break;
        case OP_GET_SCOPE: case OP_SET_SCOPE: case OP_PUT_SCOPE:
        case OP_GET_SCOPE_CHECK: case OP_SET_SCOPE_CHECK: {
            enum opcode o = (enum opcode)pc[-1];
            unsigned h = U16(pc), i = U16(pc + 2);
            struct scope *s = F->scope;
            while (h--)
                s = s->parent;
            if (o == OP_GET_SCOPE)
                *sp++ = s->vars[i];
            else if (o == OP_SET_SCOPE)
                s->vars[i] = sp[-1];
            else if (o == OP_PUT_SCOPE)
                s->vars[i] = *--sp;
            else {
                if (s->vars[i] == JS_EMPTY) {
                    tdz_error(J, KEY(U32(pc + 4)));
                    goto exception;
                }
                if (o == OP_GET_SCOPE_CHECK)
                    *sp++ = s->vars[i];
                else
                    s->vars[i] = sp[-1];
                pc += 4;
            }
            pc += 4;
            break;
        }
        case OP_SET_THIS_ONCE_REG: {
            unsigned i = U16(pc);
            pc += 2;
            if (regs[i] != JS_EMPTY) {
                throw_reference(J, "super() called twice in derived constructor");
                goto exception;
            }
            regs[i] = sp[-1];
            F->this_val = sp[-1];
            break;
        }
        case OP_SET_THIS_ONCE_SCOPE: {
            unsigned h = U16(pc), i = U16(pc + 2);
            pc += 4;
            struct scope *s = F->scope;
            while (h--)
                s = s->parent;
            if (s->vars[i] != JS_EMPTY) {
                throw_reference(J, "super() called twice in derived constructor");
                goto exception;
            }
            s->vars[i] = sp[-1];
            break;
        }
        case OP_THROW_CONST:
            pc += 4;
            throw_type(J, "Attempted to assign to readonly property.");
            goto exception;
        case OP_PUSH_SCOPE: {
            unsigned n = U16(pc);
            pc += 2;
            F->scope = scope_new(J, F->scope, n);
            break;
        }
        case OP_POP_SCOPE:
            F->scope = F->scope->parent;
            break;
        case OP_COPY_SCOPE: {
            struct scope *s = F->scope;
            struct scope *n = scope_new(J, s->parent, s->h.extra);
            memcpy(n->vars, s->vars, s->h.extra * sizeof(jsval));
            F->scope = n;
            break;
        }
        case OP_GET_GLOBAL: case OP_GET_GLOBAL_TYPEOF:
            r = global_lookup(J, KEY(U32(pc)), pc[-1] == OP_GET_GLOBAL_TYPEOF);
            CHECK(r);
            pc += 4;
            *sp++ = r;
            break;
        case OP_SET_GLOBAL:
            if (global_store(J, KEY(U32(pc)), sp[-1], pc[4]) < 0)
                goto exception;
            pc += 5;
            break;
        case OP_DEFINE_GLOBAL_VAR: {
            jsval key = KEY(U32(pc));
            pc += 4;
            if (!prop_find(&J->global->props, key) && !obj_has(J, J->global, key) && (J->global->h.flags & OBJ_EXTENSIBLE))
                prop_add(J, J->global, key, JS_UNDEFINED, PROP_WRITABLE | PROP_ENUMERABLE);
            break;
        }
        case OP_DEFINE_GLOBAL_FUNC: {
            jsval key = KEY(U32(pc));
            pc += 4;
            a = *--sp;
            struct prop *p = prop_find(&J->global->props, key);
            if (!p)
                prop_add(J, J->global, key, a, PROP_WRITABLE | PROP_ENUMERABLE);
            else if (p->flags & PROP_CONFIGURABLE) {
                p->value = a;
                p->flags = PROP_WRITABLE | PROP_ENUMERABLE;
            } else if (obj_set(J, J->global, key, a, obj_val(J->global), true) < 0)
                goto exception;
            break;
        }
        case OP_DECLARE_GLOBAL_LEX: {
            jsval key = KEY(U32(pc));
            bool is_const = pc[4];
            pc += 5;
            struct prop *p = prop_find(&J->global_lex->props, key);
            struct prop *g = prop_find(&J->global->props, key);
            if (p || (g && !(g->flags & PROP_CONFIGURABLE))) {
                char *k = js_describe(J, key);
                throw_syntax(J, "Can't create duplicate variable: '%s'", k);
                free(k);
                goto exception;
            }
            prop_add(J, J->global_lex, key, JS_EMPTY, is_const ? 0 : PROP_WRITABLE);
            break;
        }
        case OP_INIT_GLOBAL_LEX: {
            struct prop *p = prop_find(&J->global_lex->props, KEY(U32(pc)));
            pc += 4;
            a = *--sp;
            if (p)
                p->value = a;
            break;
        }
        case OP_GET_PROP: {
            jsval key = KEY(U32(pc));
            a = sp[-1];
            if (js_is_object(a)) {
                struct jsobject *o = to_obj(a);
                int cls = obj_class(o);
                if (cls == CLASS_OBJECT || cls == CLASS_FUNCTION) {
                    struct prop *p = prop_find(&o->props, key);
                    if (p && !(p->flags & PROP_ACCESSOR)) {
                        sp[-1] = p->value;
                        pc += 4;
                        break;
                    }
                }
                r = obj_get(J, o, key, a);
            } else
                r = js_get_value(J, a, key);
            CHECK(r);
            pc += 4;
            sp[-1] = r;
            break;
        }
        case OP_GET_METHOD:
            a = sp[-1];
            r = js_get_value(J, a, KEY(U32(pc)));
            CHECK(r);
            pc += 4;
            sp[-1] = r;
            *sp++ = a;
            break;
        case OP_SET_PROP:
            if (js_set_value(J, sp[-2], KEY(U32(pc)), sp[-1], pc[4]) < 0)
                goto exception;
            pc += 5;
            sp[-2] = sp[-1];
            sp--;
            break;
        case OP_GET_ELEM: case OP_GET_METHOD_ELEM: {
            a = sp[-2];
            b = sp[-1];
            if (js_is_object(a) && js_is_number(b)) {
                struct jsobject *o = to_obj(a);
                double d = js_number_value(b);
                if (obj_class(o) == CLASS_ARRAY && !(o->h.flags & OBJ_SPARSE) && d >= 0 && d < o->length &&
                    d == (uint32_t)d && (uint32_t)d < o->cap && o->elems[(uint32_t)d] != JS_EMPTY) {
                    r = o->elems[(uint32_t)d];
                    goto got_elem;
                }
            } else if (js_is_string(a) && js_is_number(b)) {
                double d = js_number_value(b);
                struct jsstring *s = to_str(a);
                if (d >= 0 && d < str_len(s) && d == (uint32_t)d) {
                    r = str_val(str_from_char(J, str_char_at(J, s, (uint32_t)d)));
                    goto got_elem;
                }
            }
            {
                jsval key = key_from_value(J, b);
                CHECK(key);
                r = js_get_value(J, a, key);
                CHECK(r);
            }
        got_elem:
            if (pc[-1] == OP_GET_METHOD_ELEM) {
                sp[-2] = r;
                sp[-1] = a;
            } else {
                sp[-2] = r;
                sp--;
            }
            break;
        }
        case OP_SET_ELEM: {
            a = sp[-3];
            b = sp[-2];
            c = sp[-1];
            if (js_is_object(a) && js_is_number(b)) {
                struct jsobject *o = to_obj(a);
                double d = js_number_value(b);
                if (obj_class(o) == CLASS_ARRAY && !(o->h.flags & OBJ_SPARSE) && d >= 0 && d < o->length &&
                    d == (uint32_t)d && (uint32_t)d < o->cap && o->elems[(uint32_t)d] != JS_EMPTY) {
                    o->elems[(uint32_t)d] = c;
                    goto set_elem_done;
                }
            }
            {
                jsval key = key_from_value(J, b);
                CHECK(key);
                if (js_set_value(J, a, key, c, pc[0]) < 0)
                    goto exception;
            }
        set_elem_done:
            pc += 1;
            sp[-3] = c;
            sp -= 2;
            break;
        }
        case OP_GET_SUPER: case OP_GET_SUPER_ELEM: {
            jsval key;
            if (pc[-1] == OP_GET_SUPER) {
                key = KEY(U32(pc));
                pc += 4;
                a = sp[-2];
                b = sp[-1];
                sp -= 2;
            } else {
                key = key_from_value(J, sp[-1]);
                CHECK(key);
                a = sp[-3];
                b = sp[-2];
                sp -= 3;
            }
            if (!js_is_object(a)) {
                throw_syntax(J, "'super' keyword unexpected here");
                goto exception;
            }
            struct jsobject *p = to_obj(a)->proto;
            if (!p) {
                throw_type(J, "Cannot read properties of null");
                goto exception;
            }
            r = obj_get(J, p, key, b);
            CHECK(r);
            *sp++ = r;
            break;
        }
        case OP_SET_SUPER: case OP_SET_SUPER_ELEM: {
            jsval key;
            bool strict;
            if (pc[-1] == OP_SET_SUPER) {
                key = KEY(U32(pc));
                strict = pc[4];
                pc += 5;
                a = sp[-3];
                b = sp[-2];
                c = sp[-1];
                sp -= 3;
            } else {
                key = key_from_value(J, sp[-2]);
                CHECK(key);
                strict = pc[0];
                pc += 1;
                a = sp[-4];
                b = sp[-3];
                c = sp[-1];
                sp -= 4;
            }
            struct jsobject *p = js_is_object(a) ? to_obj(a)->proto : 0;
            if (!p) {
                throw_type(J, "Cannot set properties of null");
                goto exception;
            }
            if (obj_set(J, p, key, c, b, strict) < 0)
                goto exception;
            *sp++ = c;
            break;
        }
        case OP_DELETE_PROP: {
            struct jsobject *o = js_to_object(J, sp[-1]);
            if (!o)
                goto exception;
            int d = obj_delete(J, o, KEY(U32(pc)), pc[4]);
            if (d < 0)
                goto exception;
            pc += 5;
            sp[-1] = js_bool(d);
            break;
        }
        case OP_DELETE_ELEM: {
            jsval key = key_from_value(J, sp[-1]);
            CHECK(key);
            struct jsobject *o = js_to_object(J, sp[-2]);
            if (!o)
                goto exception;
            int d = obj_delete(J, o, key, pc[0]);
            if (d < 0)
                goto exception;
            pc += 1;
            sp[-2] = js_bool(d);
            sp--;
            break;
        }
        case OP_DELETE_GLOBAL: {
            jsval key = KEY(U32(pc));
            pc += 4;
            if (prop_find(&J->global_lex->props, key)) {
                *sp++ = JS_FALSE;
                break;
            }
            int d = obj_delete(J, J->global, key, false);
            if (d < 0)
                goto exception;
            *sp++ = js_bool(d);
            break;
        }
        case OP_TO_PROPKEY:
            r = key_from_value(J, sp[-1]);
            CHECK(r);
            sp[-1] = r;
            break;
        case OP_IN: {
            if (!js_is_object(sp[-1])) {
                char *k = js_describe(J, sp[-2]);
                throw_type(J, "Cannot use 'in' operator to search for '%s' in %s", k,
                           js_is_string(sp[-1]) ? "a string" : "a primitive");
                free(k);
                goto exception;
            }
            jsval key = key_from_value(J, sp[-2]);
            CHECK(key);
            int h = obj_has(J, to_obj(sp[-1]), key);
            if (h < 0)
                goto exception;
            sp[-2] = js_bool(h);
            sp--;
            break;
        }
        case OP_INSTANCEOF:
            r = js_instanceof(J, sp[-2], sp[-1]);
            CHECK(r);
            sp[-2] = r;
            sp--;
            break;
        case OP_TYPEOF:
            sp[-1] = str_val(js_typeof(J, sp[-1]));
            break;
        case OP_OBJECT:
            *sp++ = obj_val(obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT));
            break;
        case OP_ARRAY:
            *sp++ = obj_val(array_new(J, 0));
            break;
        case OP_ARRAY_APPEND:
            CHECK(array_append(J, to_obj(sp[-2]), sp[-1]));
            sp--;
            break;
        case OP_ARRAY_HOLE:
            CHECK(array_append(J, to_obj(sp[-1]), JS_EMPTY));
            break;
        case OP_ARRAY_SPREAD:
            CHECK(spread_into(J, to_obj(sp[-2]), sp[-1]));
            sp--;
            break;
        case OP_DEFINE_FIELD: {
            jsval key = KEY(U32(pc));
            pc += 4;
            struct jsobject *o = to_obj(sp[-2]);
            struct pdesc d = { .value = sp[-1], .flags = PROP_DEFAULT,
                               .has = DESC_VALUE | DESC_WRITABLE | DESC_ENUMERABLE | DESC_CONFIGURABLE };
            if (obj_class(o) == CLASS_OBJECT && !key_is_index(key) && !prop_find(&o->props, key) &&
                (o->h.flags & OBJ_EXTENSIBLE))
                prop_add(J, o, key, sp[-1], PROP_DEFAULT);
            else if (obj_define(J, o, key, &d, true) < 0)
                goto exception;
            sp--;
            break;
        }
        case OP_DEFINE_ELEM: {
            struct pdesc d = { .value = sp[-1], .flags = PROP_DEFAULT,
                               .has = DESC_VALUE | DESC_WRITABLE | DESC_ENUMERABLE | DESC_CONFIGURABLE };
            if (obj_define(J, to_obj(sp[-3]), sp[-2], &d, true) < 0)
                goto exception;
            sp -= 2;
            break;
        }
        case OP_DEFINE_METHOD: {
            jsval key = KEY(U32(pc));
            int kind = pc[4], en = pc[5];
            pc += 6;
            CHECK(define_method(J, to_obj(sp[-2]), key, sp[-1], kind, en));
            sp--;
            break;
        }
        case OP_DEFINE_METHOD_ELEM: {
            int kind = pc[0] | 8, en = pc[1];     /* 8: a computed key names the function */
            pc += 2;
            CHECK(define_method(J, to_obj(sp[-3]), sp[-2], sp[-1], kind, en));
            sp -= 2;
            break;
        }
        case OP_SET_PROTO_LIT:
            if (js_is_object(sp[-1]))
                to_obj(sp[-2])->proto = to_obj(sp[-1]);
            else if (sp[-1] == JS_NULL)
                to_obj(sp[-2])->proto = 0;
            sp--;
            break;
        case OP_COPY_DATA_PROPS:
            CHECK(copy_data_props(J, to_obj(sp[-2]), sp[-1], 0, 0));
            sp--;
            break;
        case OP_OBJECT_REST: {
            unsigned n = U16(pc);
            pc += 2;
            jsval *keys = sp - n;
            struct jsobject *o = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
            CHECK(copy_data_props(J, o, keys[-1], keys, n));
            sp -= n;
            sp[-1] = obj_val(o);
            break;
        }
        case OP_CHECK_COERCIBLE:
            if (js_is_nullish(sp[-1])) {
                throw_type(J, "Right side of assignment cannot be destructured");
                goto exception;
            }
            break;
        case OP_CLOSURE: {
            struct funcdef *d = js_pointer(K[U32(pc)]);
            pc += 4;
            *sp++ = obj_val(closure_new(J, d, F->scope, 0));
            break;
        }
        case OP_CLASS: {
            jsval name = KEY(U32(pc));
            bool heritage = pc[4];
            pc += 5;
            struct jsobject *proto_parent = J->proto[P_OBJECT], *ctor_parent = J->proto[P_FUNCTION];
            jsval ctorv = sp[-1];
            if (heritage) {
                jsval h = sp[-2];
                if (h == JS_NULL)
                    proto_parent = 0;
                else {
                    if (!js_is_constructor(h)) {
                        char *d = js_describe(J, h);
                        throw_type(J, "Class extends value %s is not a constructor or null", d);
                        free(d);
                        goto exception;
                    }
                    jsval pp = js_get_value(J, h, str_val(J->atom[A_prototype]));
                    CHECK(pp);
                    if (!js_is_object(pp) && pp != JS_NULL) {
                        throw_type(J, "Class extends value does not have valid prototype property");
                        goto exception;
                    }
                    proto_parent = js_is_object(pp) ? to_obj(pp) : 0;
                    ctor_parent = to_obj(h);
                }
            }
            struct jsobject *ctor = to_obj(ctorv);
            struct jsobject *proto = obj_new(J, proto_parent, CLASS_OBJECT);
            ctor->proto = ctor_parent;
            ctor->h.flags |= OBJ_CLASS_CTOR | OBJ_CONSTRUCTOR | (heritage ? OBJ_DERIVED : 0);
            ctor->u.fn.home = proto;
            obj_define_value(J, ctor, str_val(J->atom[A_name]), name, PROP_CONFIGURABLE);
            obj_define_value(J, ctor, str_val(J->atom[A_prototype]), obj_val(proto), 0);
            prop_add(J, proto, str_val(J->atom[A_constructor]), ctorv, PROP_WRITABLE | PROP_CONFIGURABLE);
            if (heritage) {
                sp[-2] = ctorv;
                sp[-1] = obj_val(proto);
            } else {
                sp[-1] = ctorv;
                *sp++ = obj_val(proto);
            }
            break;
        }
        case OP_SET_FIELDS: {
            struct jsobject *init = to_obj(sp[-1]);
            init->u.fn.home = to_obj(sp[-2]);
            to_obj(sp[-3])->u.fn.fields = sp[-1];
            sp--;
            break;
        }
        case OP_CALL_STATIC_INIT: {
            struct jsobject *init = to_obj(sp[-1]);
            init->u.fn.home = to_obj(sp[-2]);
            r = vm_call(J, sp[-1], sp[-2], 0, 0, JS_UNDEFINED);
            CHECK(r);
            sp--;
            break;
        }
        case OP_SET_NAME_ELEM: {
            int kind = pc[0];
            pc += 1;
            if (js_is_object(sp[-1]))
                set_function_name(J, to_obj(sp[-1]), sp[-2], kind == 1 ? "get " : kind == 2 ? "set " : 0);
            break;
        }
        case OP_CALL_SPREAD: case OP_NEW_SPREAD: case OP_SUPER_CALL_SPREAD: {
            /* f this array -> f this args..., then the plain call */
            struct jsobject *arr = to_obj(sp[-1]);
            uint32_t n = arr->length;
            if (sp - 1 + n + 16 > J->stack_end) {
                stack_overflow(J);
                goto exception;
            }
            sp--;
            for (uint32_t i = 0; i < n; i++)
                *sp++ = i < arr->cap && arr->elems[i] != JS_EMPTY ? arr->elems[i] : JS_UNDEFINED;
            if (sp > J->sp_high)
                J->sp_high = sp;
            argc = n;
            if (pc[-1] == OP_CALL_SPREAD)
                goto do_call;
            if (pc[-1] == OP_NEW_SPREAD)
                goto do_new;
            goto do_super;
        }
        case OP_CALL:
            argc = U16(pc);
            pc += 2;
        do_call:
            argv = sp - argc;
            fv = argv[-2];
            if (!js_is_object(fv) || !(to_obj(fv)->h.flags & OBJ_CALLABLE)) {
                not_function(J, fv, false);
                goto exception;
            }
            f = to_obj(fv);
            if (obj_class(f) == CLASS_FUNCTION) {
                if (f->h.flags & OBJ_CLASS_CTOR) {
                    throw_type(J, "Cannot call a class constructor without |new|");
                    goto exception;
                }
                F->pc = pc;
                nf = enter(J, f, argv[-1], argv, argc, JS_UNDEFINED, false, sp);
                if (!nf)
                    goto exception;
                F = nf;
                RELOAD();
                pc = F->pc;
                sp = F->sp_base;
                break;
            }
            F->pc = pc;
            r = obj_class(f) == CLASS_NATIVE ? call_native(J, f, argv[-1], (int)argc, argv, JS_UNDEFINED)
                                             : vm_call(J, fv, argv[-1], (int)argc, argv, JS_UNDEFINED);
            CHECK(r);
            sp = argv - 2;
            *sp++ = r;
            break;
        case OP_NEW:
            argc = U16(pc);
            pc += 2;
        do_new:
            argv = sp - argc;
            fv = argv[-2];
            nt = argv[-1];
            if (!js_is_constructor(fv)) {
                not_function(J, fv, true);
                goto exception;
            }
            f = to_obj(fv);
        construct_call:
            if (obj_class(f) == CLASS_FUNCTION) {
                jsval t;
                if (f->h.flags & OBJ_DERIVED)
                    t = JS_EMPTY;
                else {
                    t = construct_this(J, f, nt);
                    CHECK(t);
                }
                argv[-1] = t;
                F->pc = pc;
                nf = enter(J, f, t, argv, argc, nt, true, sp);
                if (!nf)
                    goto exception;
                F = nf;
                RELOAD();
                pc = F->pc;
                sp = F->sp_base;
                break;
            }
            F->pc = pc;
            r = obj_class(f) == CLASS_NATIVE ? call_native(J, f, JS_UNDEFINED, (int)argc, argv, nt)
                                             : vm_call(J, obj_val(f), JS_UNDEFINED, (int)argc, argv, nt);
            CHECK(r);
            sp = argv - 2;
            *sp++ = r;
            break;
        case OP_SUPER_CALL:
            argc = U16(pc);
            pc += 2;
        do_super:
            argv = sp - argc;
            nt = argv[-1];
            f = js_is_object(argv[-2]) ? to_obj(argv[-2])->proto : 0;
            if (!f || !(f->h.flags & OBJ_CONSTRUCTOR)) {
                throw_type(J, "Super constructor is not a constructor");
                goto exception;
            }
            argv[-2] = obj_val(f);
            goto construct_call;
        case OP_INIT_FIELDS: {
            jsval t = sp[-2];
            jsval fv = sp[-1];
            sp -= 2;
            if (js_is_object(fv) && obj_class(to_obj(fv)) == CLASS_FUNCTION) {
                jsval init = to_obj(fv)->u.fn.fields;
                if (js_is_callable(init)) {
                    r = vm_call(J, init, t, 0, 0, JS_UNDEFINED);
                    CHECK(r);
                }
            }
            break;
        }
        case OP_RETURN: case OP_RETURN_UNDEF: {
            r = pc[-1] == OP_RETURN ? sp[-1] : JS_UNDEFINED;
            if (F->construct && !js_is_object(r)) {
                if (F->callee->h.flags & OBJ_DERIVED) {
                    if (r != JS_UNDEFINED) {
                        throw_type(J, "Derived constructors may only return object or undefined");
                        goto exception;
                    }
                    r = F->this_val;
                    if (r == JS_EMPTY) {
                        tdz_error(J, str_val(J->atom[A_this]));
                        goto exception;
                    }
                } else
                    r = F->this_val;
            }
        frame_return: {
            bool entry = F->entry;
            jsval *base = F->argv - 2;
            J->nhandlers = F->handlers;
            J->nframes--;
            if (entry)
                return r;
            F = &J->frames[J->nframes - 1];
            RELOAD();
            pc = F->pc;
            sp = base;
            *sp++ = r;
            break;
        }
        }
        case OP_INITIAL_YIELD: {                /* (27r) the call returns its generator */
            bool async = *pc++;
            f = gen_create(J, F, sp, pc, async);
            if (!f)
                goto exception;
            if (!async) {
                r = obj_val(f);
                goto frame_return;
            }
            bool entry = F->entry;              /* (async: its frame goes; it runs on as the generator's) */
            jsval *base = F->argv - 2;
            J->nhandlers = F->handlers;
            J->nframes--;
            r = async_begin(J, f);
            if (entry)
                return r;
            F = &J->frames[J->nframes - 1];
            RELOAD();
            pc = F->pc;
            sp = base;
            CHECK(r);
            *sp++ = r;
            break;
        }
        case OP_DELEGATE_START: {               /* (27r) yield*: GetIterator (no fast path: its own next) */
            jsval m = js_get_method(J, sp[-1], J->symbol[SYM_ITERATOR]);
            CHECK(m);
            if (js_is_undefined(m)) {
                throw_type(J, "Spread syntax requires ...iterable[Symbol.iterator] to be a function");
                goto exception;
            }
            a = vm_call(J, m, sp[-1], 0, 0, JS_UNDEFINED);
            CHECK(a);
            if (!js_is_object(a)) {
                throw_type(J, "Result of the Symbol.iterator method is not an object");
                goto exception;
            }
            b = js_get_value(J, a, str_val(J->atom[A_next]));
            CHECK(b);
            sp[-1] = a;
            sp[0] = b;
            sp[1] = JS_UNDEFINED;
            sp[2] = js_number(RESUME_NEXT);
            sp += 3;
            break;
        }
        case OP_DELEGATE_STEP: {                /* iterator, next, received, kind -> iterator, next, result
                                                   (on), or the value (done, or returned: the second place) */
            const uint8_t *to_done = pc + 4 + I32(pc), *to_return = pc + 8 + I32(pc + 4);
            pc += 8;
            int kind = (int)js_number_value(sp[-1]);
            jsval recv = sp[-2], it = sp[-4], next = sp[-3];
            if (kind == RESUME_NEXT)
                r = vm_call(J, next, it, 1, &recv, JS_UNDEFINED);
            else {
                jsval m = js_get_method(J, it, str_val(J->atom[kind == RESUME_THROW ? A_throw : A_return]));
                CHECK(m);
                if (js_is_undefined(m)) {
                    if (kind == RESUME_RETURN) {
                        sp -= 4;
                        *sp++ = recv;
                        pc = to_return;
                        break;
                    }
                    jsval close = js_get_method(J, it, str_val(J->atom[A_return]));   /* (no throw: closed, then a TypeError) */
                    CHECK(close);
                    if (!js_is_undefined(close))
                        CHECK(vm_call(J, close, it, 0, 0, JS_UNDEFINED));
                    throw_type(J, "The iterator does not provide a 'throw' method");
                    goto exception;
                }
                r = vm_call(J, m, it, 1, &recv, JS_UNDEFINED);
            }
            CHECK(r);
            if (!js_is_object(r)) {
                throw_type(J, "Iterator result is not an object");
                goto exception;
            }
            a = js_get_value(J, r, str_val(J->atom[A_done]));
            CHECK(a);
            if (js_to_boolean(a)) {
                a = js_get_value(J, r, str_val(J->atom[A_value]));
                CHECK(a);
                sp -= 4;
                *sp++ = a;
                pc = kind == RESUME_RETURN ? to_return : to_done;
                break;
            }
            sp -= 2;
            *sp++ = r;
            break;
        }
        case OP_YIELD_RAW:
            J->yield_raw = true;
            /* fall through */
        case OP_YIELD: case OP_AWAIT:           /* (27r) a resumed generator's frame is an entry: out */
            a = *--sp;
            gen_save(J, F->gen->u.gen, F, sp, pc);
            J->nhandlers = F->handlers;
            J->nframes--;
            J->yielded = true;
            return a;
        case OP_RESUME_SWITCH: {                /* (27r) how it was resumed: next, throw, return */
            int32_t off = I32(pc);
            pc += 4;
            int kind = (int)js_number_value(*--sp);
            if (kind == RESUME_THROW) {
                js_throw(J, *--sp);
                goto exception;
            }
            if (kind == RESUME_RETURN)
                pc += off;
            break;
        }
        case OP_CHECK_DERIVED_RETURN:
            a = sp[-2];
            b = sp[-1];
            sp--;
            if (js_is_object(a))
                break;
            if (a != JS_UNDEFINED) {
                throw_type(J, "Derived constructors may only return object or undefined");
                goto exception;
            }
            if (b == JS_EMPTY) {
                tdz_error(J, str_val(J->atom[A_this]));
                goto exception;
            }
            sp[-1] = b;
            break;
        case OP_ADD:
            r = op_add(J, sp[-2], sp[-1]);
            CHECK(r);
            sp[-2] = r;
            sp--;
            break;
        case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_POW:
        case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SAR: case OP_SHR:
            r = op_arith(J, pc[-1], sp[-2], sp[-1]);
            CHECK(r);
            sp[-2] = r;
            sp--;
            break;
        case OP_NEG: case OP_PLUS: case OP_INC: case OP_DEC: case OP_TO_NUMERIC: case OP_BNOT: {
            double d;
            if (js_is_number(sp[-1]))
                d = js_number_value(sp[-1]);
            else {
                bool threw = false;
                d = js_to_number(J, sp[-1], &threw);
                if (threw)
                    goto exception;
            }
            switch (pc[-1]) {
            case OP_NEG: d = -d; break;
            case OP_INC: d += 1; break;
            case OP_DEC: d -= 1; break;
            case OP_BNOT: d = ~js_to_int32(d); break;
            }
            sp[-1] = js_number(d);
            break;
        }
        case OP_NOT:
            sp[-1] = js_bool(!js_to_boolean(sp[-1]));
            break;
        case OP_EQ: case OP_NE: {
            int e = js_loose_equal(J, sp[-2], sp[-1]);
            if (e < 0)
                goto exception;
            sp[-2] = js_bool(pc[-1] == OP_EQ ? e : !e);
            sp--;
            break;
        }
        case OP_SEQ:
            sp[-2] = js_bool(js_strict_equal(J, sp[-2], sp[-1]));
            sp--;
            break;
        case OP_SNE:
            sp[-2] = js_bool(!js_strict_equal(J, sp[-2], sp[-1]));
            sp--;
            break;
        case OP_LT: case OP_LE: case OP_GT: case OP_GE:
            r = op_compare(J, pc[-1], sp[-2], sp[-1]);
            CHECK(r);
            sp[-2] = r;
            sp--;
            break;
        case OP_JMP:
            pc += 4 + I32(pc);
            break;
        case OP_JMP_FALSE:
            a = *--sp;
            pc += 4 + (js_to_boolean(a) ? 0 : I32(pc));
            break;
        case OP_JMP_TRUE:
            a = *--sp;
            pc += 4 + (js_to_boolean(a) ? I32(pc) : 0);
            break;
        case OP_JMP_FALSE_KEEP:
            if (!js_to_boolean(sp[-1]))
                pc += 4 + I32(pc);
            else {
                sp--;
                pc += 4;
            }
            break;
        case OP_JMP_TRUE_KEEP:
            if (js_to_boolean(sp[-1]))
                pc += 4 + I32(pc);
            else {
                sp--;
                pc += 4;
            }
            break;
        case OP_JMP_NOT_NULLISH_KEEP:
            if (!js_is_nullish(sp[-1]))
                pc += 4 + I32(pc);
            else {
                sp--;
                pc += 4;
            }
            break;
        case OP_JMP_NULLISH_UNDEF:
            if (js_is_nullish(sp[-1])) {
                sp[-1] = JS_UNDEFINED;
                pc += 4 + I32(pc);
            } else
                pc += 4;
            break;
        case OP_JMP_NULLISH_CALLEE:
            if (js_is_nullish(sp[-2])) {
                sp--;
                sp[-1] = JS_UNDEFINED;
                pc += 4 + I32(pc);
            } else
                pc += 4;
            break;
        case OP_JMP_NOT_UNDEF:
            pc += 4 + (sp[-1] != JS_UNDEFINED ? I32(pc) : 0);
            break;
        case OP_DUP:
            sp[0] = sp[-1];
            sp++;
            break;
        case OP_DUP2:
            sp[0] = sp[-2];
            sp[1] = sp[-1];
            sp += 2;
            break;
        case OP_POP:
            sp--;
            break;
        case OP_SWAP:
            a = sp[-1];
            sp[-1] = sp[-2];
            sp[-2] = a;
            break;
        case OP_ROT3R:
            a = sp[-1];
            sp[-1] = sp[-2];
            sp[-2] = sp[-3];
            sp[-3] = a;
            break;
        case OP_ROT4R:
            a = sp[-1];
            sp[-1] = sp[-2];
            sp[-2] = sp[-3];
            sp[-3] = sp[-4];
            sp[-4] = a;
            break;
        case OP_THROW:
            js_throw(J, *--sp);
            goto exception;
        case OP_THROW_ERROR: {
            int kind = pc[0];
            jsval msg = KEY(U32(pc + 1));
            pc += 5;
            char *m = js_describe(J, msg);
            throw_error(J, kind == 1 ? P_REFERENCE_ERROR : kind == 2 ? P_SYNTAX_ERROR : P_TYPE_ERROR, "%s", m);
            free(m);
            goto exception;
        }
        case OP_TRY: {
            if (J->nhandlers == J->handlers_cap) {
                uint32_t cap = J->handlers_cap ? J->handlers_cap * 2 : 64;
                struct handler *h = realloc(J->handlers, cap * sizeof *h);
                if (!h)
                    abort();
                J->handlers = h;
                J->handlers_cap = cap;
            }
            struct handler *h = &J->handlers[J->nhandlers++];
            h->pc = pc + 4 + I32(pc);
            h->sp = sp;
            h->scope = F->scope;
            pc += 4;
            break;
        }
        case OP_END_TRY:
            J->nhandlers--;
            break;
        case OP_GET_ITER: {
            struct iter it;
            if (iter_open(J, sp[-1], &it) < 0)
                goto exception;
            if (it.fast) {
                sp[-1] = it.it;
                sp[0] = JS_EMPTY;
                sp[1] = js_number(0);
            } else {
                sp[-1] = it.it;
                sp[0] = it.next;
                sp[1] = JS_UNDEFINED;
            }
            sp += 2;
            break;
        }
        case OP_ITER_NEXT: case OP_ITER_STEP: {
            bool step = pc[-1] == OP_ITER_STEP;
            jsval v = JS_UNDEFINED;
            int got;
            if (sp[-1] == JS_NULL)
                got = 0;                        /* done before */
            else if (sp[-2] == JS_EMPTY) {
                struct jsobject *arr = to_obj(sp[-3]);
                uint32_t i = (uint32_t)js_number_value(sp[-1]);
                if (i < arr->length) {
                    if (i < arr->cap && arr->elems[i] != JS_EMPTY && !(arr->h.flags & OBJ_SPARSE))
                        v = arr->elems[i];
                    else {
                        v = obj_get(J, arr, str_val(atom_index(J, i)), sp[-3]);
                        CHECK(v);
                    }
                    sp[-1] = js_number(i + 1);
                    got = 1;
                } else
                    got = 0;
            } else {
                struct iter it = { .it = sp[-3], .next = sp[-2] };
                got = iter_step(J, &it, &v);
                if (got < 0) {
                    sp[-1] = JS_NULL;           /* (a throwing next is not closed) */
                    goto exception;
                }
            }
            if (!got) {
                sp[-1] = JS_NULL;
                if (!step) {
                    pc += 4 + I32(pc);
                    break;
                }
                v = JS_UNDEFINED;
            }
            if (!step)
                pc += 4;
            *sp++ = v;
            break;
        }
        case OP_ITER_REST: {
            struct jsobject *arr = array_new(J, 0);
            *sp++ = obj_val(arr);
            while (sp[-2] != JS_NULL) {
                jsval v;
                int got;
                if (sp[-3] == JS_EMPTY) {
                    struct jsobject *src = to_obj(sp[-4]);
                    uint32_t i = (uint32_t)js_number_value(sp[-2]);
                    if (i >= src->length) {
                        got = 0;
                    } else {
                        v = obj_get(J, src, str_val(atom_index(J, i)), sp[-4]);
                        CHECK(v);
                        sp[-2] = js_number(i + 1);
                        got = 1;
                    }
                } else {
                    struct iter it = { .it = sp[-4], .next = sp[-3] };
                    got = iter_step(J, &it, &v);
                    if (got < 0) {
                        sp[-2] = JS_NULL;
                        goto exception;
                    }
                }
                if (!got) {
                    sp[-2] = JS_NULL;
                    break;
                }
                CHECK(array_append(J, arr, v));
            }
            break;
        }
        case OP_ITER_CLOSE:
            if (sp[-1] != JS_NULL && sp[-2] != JS_EMPTY) {
                struct iter it = { .it = sp[-3], .next = sp[-2] };
                sp -= 3;
                if (iter_close(J, &it, false) < 0)
                    goto exception;
            } else
                sp -= 3;
            break;
        case OP_ITER_CLOSE_EXC: {
            jsval e = sp[-1];
            if (sp[-2] != JS_NULL && sp[-3] != JS_EMPTY) {
                struct iter it = { .it = sp[-4], .next = sp[-3] };
                J->exception = e;
                J->has_exception = true;
                iter_close(J, &it, true);
            }
            sp -= 3;
            sp[-1] = e;
            break;
        }
        case OP_FOR_IN_START:
            r = for_in_start(J, sp[-1]);
            CHECK(r);
            sp[-1] = r;
            break;
        case OP_FOR_IN_NEXT: {
            struct jsobject *w = to_obj(sp[-1]);
            jsval key = JS_EMPTY;
            while (w->u.forin.at < w->u.forin.n) {
                jsval k = w->u.forin.keys[w->u.forin.at++];
                /* skip properties deleted meanwhile (64r: not asked of a
                 * proxy, as JavaScriptCore does not) */
                struct jsobject *on = to_obj(w->u.forin.object);
                int h = obj_class(on) == CLASS_PROXY ? 1 : obj_has(J, on, k);
                if (h < 0)
                    goto exception;
                if (h) {
                    key = k;
                    break;
                }
            }
            if (key == JS_EMPTY) {
                pc += 4 + I32(pc);
                break;
            }
            pc += 4;
            *sp++ = key;
            break;
        }
        case OP_TEMPLATE_OBJECT: {
            uint32_t k = U32(pc);
            pc += 4;
            if (!js_is_object(K[k + 1])) {
                struct jsobject *strings = to_obj(K[k]);
                uint32_t n = strings->length / 2;
                struct jsobject *t = array_new(J, n), *raw = array_new(J, n);
                for (uint32_t i = 0; i < n; i++) {
                    t->elems[i] = strings->elems[i];
                    raw->elems[i] = strings->elems[n + i];
                }
                obj_set_level(J, raw, true);
                obj_define_value(J, t, str_val(J->atom[A_raw]), obj_val(raw), 0);
                obj_set_level(J, t, true);
                K[k + 1] = obj_val(t);
            }
            *sp++ = K[k + 1];
            break;
        }
        case OP_REGEXP: {
            uint32_t k = U32(pc);
            pc += 4;
            r = regexp_create(J, K[k], K[k + 1], JS_UNDEFINED);
            CHECK(r);
            *sp++ = r;
            break;
        }
        case OP_TO_STRING:
            if (!js_is_string(sp[-1])) {
                struct jsstring *s = js_to_str(J, sp[-1]);
                if (!s)
                    goto exception;
                sp[-1] = str_val(s);
            }
            break;
        case OP_PRIVATE_NAME: {
            jsval d = KEY(U32(pc));
            pc += 4;
            struct jssymbol *s = gc_alloc(J, CELL_SYMBOL, sizeof *s);
            s->description = to_str(d);
            s->hash = ++J->symbol_count * 2654435761u;
            s->h.flags = 2;
            *sp++ = js_tagged(JS_T_SYMBOL, s);
            break;
        }
        case OP_GET_PRIVATE: case OP_SET_PRIVATE: {
            bool set = pc[-1] == OP_SET_PRIVATE;
            jsval o = set ? sp[-3] : sp[-2], key = set ? sp[-2] : sp[-1];
            struct pdesc d;
            int has = js_is_object(o) ? private_own(J, to_obj(o), key, &d) : 0;
            if (has <= 0) {
                char *k = js_describe(J, key);
                throw_type(J, "Cannot %s private member %s %s an object whose class did not declare it",
                           set ? "write" : "read", k, set ? "to" : "from");
                free(k);
                goto exception;
            }
            if (!set) {
                if (d.flags & PROP_ACCESSOR) {
                    if (d.get == JS_UNDEFINED) {
                        throw_type(J, "'%s' was defined without a getter", "#private");
                        goto exception;
                    }
                    r = vm_call(J, d.get, o, 0, 0, JS_UNDEFINED);
                    CHECK(r);
                } else
                    r = d.value;
                sp[-2] = r;
                sp--;
                break;
            }
            jsval v = sp[-1];
            if (d.flags & PROP_ACCESSOR) {
                if (d.set == JS_UNDEFINED) {
                    throw_type(J, "'%s' was defined without a setter", "#private");
                    goto exception;
                }
                CHECK(vm_call(J, d.set, o, 1, &v, JS_UNDEFINED));
            } else if (!(d.flags & PROP_WRITABLE)) {
                throw_type(J, "Cannot assign to private method");
                goto exception;
            } else
                prop_find(&to_obj(o)->props, key)->value = v;
            sp[-3] = v;
            sp -= 2;
            break;
        }
        case OP_DEFINE_PRIVATE: {
            struct jsobject *o = to_obj(sp[-3]);
            if (prop_find(&o->props, sp[-2])) {
                char *k = js_describe(J, sp[-2]);
                throw_type(J, "Cannot initialize %s twice on the same object", k);
                free(k);
                goto exception;
            }
            bool method = js_is_callable(sp[-1]) && obj_class(to_obj(sp[-1])) == CLASS_FUNCTION &&
                          (to_obj(sp[-1])->u.fn.def->flags & FD_METHOD) && !(to_obj(sp[-1])->u.fn.def->flags & FD_FIELD_INIT);
            (void)method;
            prop_add(J, o, sp[-2], sp[-1], PROP_WRITABLE);
            sp -= 2;
            break;
        }
        case OP_PRIVATE_IN:
            if (!js_is_object(sp[-1])) {
                throw_type(J, "Cannot use 'in' operator to search for a private field in a primitive");
                goto exception;
            }
            sp[-2] = js_bool(prop_find(&to_obj(sp[-1])->props, sp[-2]) != 0);
            sp--;
            break;
        case OP_DEBUGGER:
            break;
        case OP_UNSUPPORTED: {
            char *m = js_describe(J, KEY(U32(pc)));
            throw_syntax(J, "%s", m);
            free(m);
            goto exception;
        }
        default:
            throw_type(J, "Bad instruction %d", pc[-1]);
            goto exception;
        }
        continue;

    exception:
        for (;;) {
            if (J->nhandlers > F->handlers) {
                struct handler *h = &J->handlers[--J->nhandlers];
                sp = h->sp;
                F->scope = h->scope;
                pc = h->pc;
                *sp++ = js_take_exception(J);
                break;
            }
            bool entry = F->entry;
            J->nframes--;
            if (entry)
                return JS_EXCEPTION;
            F = &J->frames[J->nframes - 1];
            RELOAD();
            pc = F->pc;
        }
    }
}

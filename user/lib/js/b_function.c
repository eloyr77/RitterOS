/*
 * Function, Function.prototype (call, apply, bind, toString), Error and its
 * kinds, Symbol, and the built-in iterators' prototypes (27o).
 */
#include <math.h>
#include "jsint.h"

/* --- Function ------------------------------------------------------------- */

static jsval f_ctor(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val;
    struct sbuf params = { 0 };
    for (int i = 0; i < argc - 1; i++) {
        struct jsstring *s = js_to_str(J, argv[i]);
        if (!s) {
            sb_free(&params);
            return JS_EXCEPTION;
        }
        if (i)
            sb_unit(&params, ',');
        sb_str(J, &params, s);
    }
    struct jsstring *body = argc ? js_to_str(J, argv[argc - 1]) : J->atom[A_empty];
    struct jsstring *p = sb_finish(J, &params);
    if (!body || !p)
        return JS_EXCEPTION;
    struct funcdef *def = js_compile_function(J, p, body, 0);
    if (!def)
        return JS_EXCEPTION;
    struct jsobject *f = closure_new(J, def, 0, 0);
    if (nt != JS_UNDEFINED && nt != obj_val(J->ctor[C_FUNCTION])) {
        struct jsobject *pr = js_proto_from_ctor(J, nt, J->proto[P_FUNCTION]);
        if (pr)
            f->proto = pr;
    }
    return obj_val(f);
}

static jsval fp_call(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)nt;
    if (!js_is_callable(this_val))
        return throw_type(J, "Function.prototype.call called on a value that is not a function");
    return vm_call(J, this_val, argc ? argv[0] : JS_UNDEFINED, argc ? argc - 1 : 0, argv + 1, JS_UNDEFINED);
}

static jsval fp_apply(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    if (!js_is_callable(this_val))
        return throw_type(J, "Function.prototype.apply was called on a value that is not a function");
    if (js_is_nullish(argv[1]))
        return vm_call(J, this_val, argv[0], 0, 0, JS_UNDEFINED);
    jsval *list;
    uint32_t n;
    if (js_list_from_array_like(J, argv[1], &list, &n) < 0)
        return JS_EXCEPTION;
    js_root_push(J, list, n);
    jsval r = vm_call(J, this_val, argv[0], (int)n, list, JS_UNDEFINED);
    js_root_pop(J);
    free(list);
    return r;
}

static jsval fp_bind(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)nt;
    if (!js_is_callable(this_val))
        return throw_type(J, "Function.prototype.bind called on a value that is not a function");
    struct jsobject *t = to_obj(this_val);
    struct jsobject *b = obj_new(J, t->proto, CLASS_BOUND);
    b->h.flags |= OBJ_CALLABLE | (t->h.flags & OBJ_CONSTRUCTOR);
    b->u.bound.target = this_val;
    b->u.bound.this_val = argc ? argv[0] : JS_UNDEFINED;
    uint32_t n = argc > 1 ? (uint32_t)argc - 1 : 0;
    if (n) {
        b->u.bound.args = malloc(n * sizeof(jsval));
        if (!b->u.bound.args)
            abort();
        memcpy(b->u.bound.args, argv + 1, n * sizeof(jsval));
        b->u.bound.argc = n;
    }
    double len = 0;
    struct pdesc d;
    int h = obj_get_own(J, t, str_val(J->atom[A_length]), &d);
    if (h < 0)
        return JS_EXCEPTION;
    if (h) {
        jsval l = obj_get(J, t, str_val(J->atom[A_length]), this_val);
        if (l == JS_EXCEPTION)
            return l;
        if (js_is_number(l)) {
            double x = js_number_value(l);
            if (x == INFINITY)
                len = INFINITY;
            else if (x == x && x != -INFINITY) {
                x = trunc(x);
                len = x - n > 0 ? x - n : 0;
            }
        }
    }
    prop_add(J, b, str_val(J->atom[A_length]), js_number(len), PROP_CONFIGURABLE);
    jsval name = obj_get(J, t, str_val(J->atom[A_name]), this_val);
    if (name == JS_EXCEPTION)
        return name;
    struct sbuf sb = { 0 };
    sb_ascii(&sb, "bound ");
    if (js_is_string(name))
        sb_str(J, &sb, to_str(name));
    struct jsstring *bn = sb_finish(J, &sb);
    prop_add(J, b, str_val(J->atom[A_name]), str_val(bn), PROP_CONFIGURABLE);
    return obj_val(b);
}

static jsval fp_to_string(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    if (!js_is_callable(this_val))
        return throw_type(J, "Function.prototype.toString requires that 'this' be a Function");
    struct jsobject *f = to_obj(this_val);
    if (obj_class(f) == CLASS_FUNCTION) {
        struct funcdef *d = f->u.fn.def;
        if (d->source && d->src_end > d->src_start && d->src_end <= str_len(d->source))
            return str_val(str_sub(J, d->source, d->src_start, d->src_end));
    }
    struct sbuf b = { 0 };
    sb_ascii(&b, "function ");
    if (obj_class(f) != CLASS_BOUND && obj_class(f) != CLASS_PROXY) {  /* (64r: no trap asked) */
        jsval name = obj_get(J, f, str_val(J->atom[A_name]), this_val);
        if (js_is_string(name))
            sb_str(J, &b, to_str(name));
    }
    sb_ascii(&b, "() {\n    [native code]\n}");
    struct jsstring *s = sb_finish(J, &b);
    return s ? str_val(s) : JS_EXCEPTION;
}

static jsval fp_has_instance(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    int r = js_ordinary_has_instance(J, this_val, argv[0]);
    return r < 0 ? JS_EXCEPTION : js_bool(r);
}

static jsval throw_type_error_fn(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)t, (void)c, (void)a, (void)n;
    return throw_type(J, "'caller', 'callee', and 'arguments' properties may not be accessed on strict mode functions");
}

void init_function(struct js *J)
{
    struct jsobject *fp = J->proto[P_FUNCTION];
    struct jsobject *c = make_ctor(J, "Function", f_ctor, 1, fp);
    J->ctor[C_FUNCTION] = c;
    obj_define_native(J, fp, "apply", fp_apply, 2, 0);
    obj_define_native(J, fp, "bind", fp_bind, 1, 0);
    obj_define_native(J, fp, "call", fp_call, 1, 0);
    obj_define_native(J, fp, "toString", fp_to_string, 0, 0);
    struct jsobject *hi = make_native(J, "[Symbol.hasInstance]", fp_has_instance, 1, 0);
    obj_define_value(J, fp, J->symbol[SYM_HAS_INSTANCE], obj_val(hi), 0);
    J->throw_type_error = make_native(J, "", throw_type_error_fn, 0, 0);
    obj_prevent_extensions(J, J->throw_type_error);
}

/* --- Error ------------------------------------------------------------- */

void error_capture(struct js *J, struct jsobject *e)
{
    struct jsstring *file = 0;
    int line = vm_current_line(J, &file);
    e->u.err.file = file;
    e->u.err.line = line;
    e->h.flags |= OBJ_ERROR_DATA;
    /* a stack in the style of JavaScriptCore: function@file:line, innermost first */
    struct sbuf b = { 0 };
    int count = 0;
    for (uint32_t k = J->nframes; k-- > 0 && count < 32; count++) {
        struct frame *F = &J->frames[k];
        if (!F->def)
            continue;
        if (b.n)
            sb_unit(&b, '\n');
        if (F->def->flags & FD_SCRIPT)
            sb_ascii(&b, "global code");
        else if (F->def->name)
            sb_str(J, &b, F->def->name);
        sb_unit(&b, '@');
        if (F->def->file)
            sb_str(J, &b, F->def->file);
        uint32_t off = (uint32_t)(F->pc - F->def->code);
        int ln = 0;
        for (uint32_t i = 0; i < F->def->nlines && F->def->lines[i].pc <= off; i++)
            ln = (int)F->def->lines[i].line;
        char buf[16];
        snprintf(buf, sizeof buf, ":%d", ln);
        sb_ascii(&b, buf);
    }
    struct jsstring *s = sb_finish(J, &b);
    if (s)
        prop_add(J, e, str_val(J->atom[A_stack]), str_val(s), PROP_WRITABLE | PROP_CONFIGURABLE);
}

jsval error_create(struct js *J, struct jsobject *proto, jsval message)
{
    struct jsobject *e = obj_new(J, proto, CLASS_ERROR);
    if (message != JS_UNDEFINED) {
        struct jsstring *m = js_to_str(J, message);
        if (!m)
            return JS_EXCEPTION;
        prop_add(J, e, str_val(J->atom[A_message]), str_val(m), PROP_WRITABLE | PROP_CONFIGURABLE);
    }
    error_capture(J, e);
    return obj_val(e);
}

static const char *const error_names[] = {
    "Error", "TypeError", "RangeError", "ReferenceError", "SyntaxError", "EvalError", "URIError", "AggregateError",
};

static int error_kind_of(struct js *J, jsval nt, int magic)
{
    (void)J, (void)nt;
    return magic;
}

static jsval install_cause(struct js *J, struct jsobject *e, jsval options)
{
    if (!js_is_object(options))
        return JS_TRUE;
    int h = obj_has(J, to_obj(options), str_val(J->atom[A_cause]));
    if (h < 0)
        return JS_EXCEPTION;
    if (h) {
        jsval c = obj_get(J, to_obj(options), str_val(J->atom[A_cause]), options);
        if (c == JS_EXCEPTION)
            return c;
        prop_add(J, e, str_val(J->atom[A_cause]), c, PROP_WRITABLE | PROP_CONFIGURABLE);
    }
    return JS_TRUE;
}

static jsval e_ctor_kind(struct js *J, int kind, int argc, jsval *argv, jsval nt)
{
    static const int protos[] = { P_ERROR, P_TYPE_ERROR, P_RANGE_ERROR, P_REFERENCE_ERROR, P_SYNTAX_ERROR,
                                  P_EVAL_ERROR, P_URI_ERROR, P_AGGREGATE_ERROR };
    struct jsobject *fallback = J->proto[protos[kind]];
    struct jsobject *proto = nt == JS_UNDEFINED ? fallback : js_proto_from_ctor(J, nt, fallback);
    if (!proto)
        return JS_EXCEPTION;
    int mi = kind == 7 ? 1 : 0;
    jsval e = error_create(J, proto, argv[mi]);
    if (e == JS_EXCEPTION)
        return e;
    if (install_cause(J, to_obj(e), argc > mi + 1 ? argv[mi + 1] : JS_UNDEFINED) == JS_EXCEPTION)
        return JS_EXCEPTION;
    if (kind == 7) {
        struct jsobject *list = array_new(J, 0);
        struct iter it;
        if (iter_open(J, argv[0], &it) < 0)
            return JS_EXCEPTION;
        jsval v;
        int r;
        while ((r = iter_step(J, &it, &v)) > 0) {
            array_grow(J, list, list->length + 1);
            list->elems[list->length++] = v;
        }
        if (r < 0)
            return JS_EXCEPTION;
        prop_add(J, to_obj(e), str_val(J->atom[A_errors]), obj_val(list), PROP_WRITABLE | PROP_CONFIGURABLE);
    }
    return e;
}

#define ERROR_CTOR(fname, kind) \
    static jsval fname(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)t; return e_ctor_kind(J, error_kind_of(J, n, kind), c, a, n); }
ERROR_CTOR(e_error, 0)
ERROR_CTOR(e_type, 1)
ERROR_CTOR(e_range, 2)
ERROR_CTOR(e_reference, 3)
ERROR_CTOR(e_syntax, 4)
ERROR_CTOR(e_eval, 5)
ERROR_CTOR(e_uri, 6)
ERROR_CTOR(e_aggregate, 7)

static jsval ep_to_string(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    if (!js_is_object(this_val))
        return throw_type(J, "Error.prototype.toString called on a non-object");
    jsval name = js_get_value(J, this_val, str_val(J->atom[A_name]));
    if (name == JS_EXCEPTION)
        return name;
    struct jsstring *n = name == JS_UNDEFINED ? atom_cstr(J, "Error") : js_to_str(J, name);
    if (!n)
        return JS_EXCEPTION;
    jsval msg = js_get_value(J, this_val, str_val(J->atom[A_message]));
    if (msg == JS_EXCEPTION)
        return msg;
    struct jsstring *m = msg == JS_UNDEFINED ? J->atom[A_empty] : js_to_str(J, msg);
    if (!m)
        return JS_EXCEPTION;
    if (!str_len(n))
        return str_val(m);
    if (!str_len(m))
        return str_val(n);
    struct sbuf b = { 0 };
    sb_str(J, &b, n);
    sb_ascii(&b, ": ");
    sb_str(J, &b, m);
    struct jsstring *s = sb_finish(J, &b);
    return s ? str_val(s) : JS_EXCEPTION;
}

static jsval e_capture_stack_trace(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    (void)t, (void)c, (void)n;
    if (js_is_object(argv[0]))
        error_capture(J, to_obj(argv[0]));
    return JS_UNDEFINED;
}

void init_error(struct js *J)
{
    static const js_native fns[] = { e_error, e_type, e_range, e_reference, e_syntax, e_eval, e_uri, e_aggregate };
    static const int protos[] = { P_ERROR, P_TYPE_ERROR, P_RANGE_ERROR, P_REFERENCE_ERROR, P_SYNTAX_ERROR,
                                  P_EVAL_ERROR, P_URI_ERROR, P_AGGREGATE_ERROR };
    struct jsobject *base = 0;
    for (int i = 0; i < 8; i++) {
        struct jsobject *p = obj_new(J, i ? J->proto[P_ERROR] : J->proto[P_OBJECT], CLASS_OBJECT);
        J->proto[protos[i]] = p;
        struct jsobject *c = make_ctor(J, error_names[i], fns[i], i == 7 ? 2 : 1, p);
        if (i == 0) {
            base = c;
            J->ctor[C_ERROR] = c;
            obj_define_native(J, p, "toString", ep_to_string, 0, 0);
            obj_define_native(J, c, "captureStackTrace", e_capture_stack_trace, 1, 0);
        } else
            c->proto = base;
        obj_define_value(J, p, str_val(J->atom[A_name]), str_val(atom_cstr(J, error_names[i])),
                         PROP_WRITABLE | PROP_CONFIGURABLE);
        obj_define_value(J, p, str_val(J->atom[A_message]), str_val(J->atom[A_empty]),
                         PROP_WRITABLE | PROP_CONFIGURABLE);
    }
}

/* --- Symbol ---------------------------------------------------------------- */

static struct jssymbol *new_symbol(struct js *J, struct jsstring *description)
{
    struct jssymbol *s = gc_alloc(J, CELL_SYMBOL, sizeof *s);
    if (!s)
        abort();
    s->description = description;
    s->hash = ++J->symbol_count * 2654435761u;
    return s;
}

static jsval sym_ctor(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc;
    if (nt != JS_UNDEFINED)
        return throw_type(J, "Symbol is not a constructor");
    struct jsstring *d = 0;
    if (argv[0] != JS_UNDEFINED) {
        d = js_to_str(J, argv[0]);
        if (!d)
            return JS_EXCEPTION;
    }
    return js_tagged(JS_T_SYMBOL, new_symbol(J, d));
}

static jsval sym_for(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    struct jsstring *s = js_to_str(J, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    jsval key = str_val(atom(J, s));
    struct prop *p = prop_find(&J->symbol_registry->props, key);
    if (p)
        return p->value;
    struct jssymbol *sym = new_symbol(J, to_str(key));
    sym->h.flags |= 1;
    prop_add(J, J->symbol_registry, key, js_tagged(JS_T_SYMBOL, sym), PROP_DEFAULT);
    return js_tagged(JS_T_SYMBOL, sym);
}

static jsval sym_key_for(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    if (!js_is_symbol(argv[0]))
        return throw_type(J, "Symbol.keyFor: not a symbol");
    struct jssymbol *s = to_sym(argv[0]);
    if (!(s->h.flags & 1))
        return JS_UNDEFINED;
    return str_val(s->description);
}

static jsval this_symbol(struct js *J, jsval v)
{
    if (js_is_symbol(v))
        return v;
    if (js_is_object(v) && obj_class(to_obj(v)) == CLASS_SYMBOL)
        return to_obj(v)->u.prim;
    return throw_type(J, "Symbol.prototype method called on a value that is not a symbol");
}

static jsval sp_to_string(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    jsval s = this_symbol(J, this_val);
    if (s == JS_EXCEPTION)
        return s;
    return str_val(js_to_str_noexcept(J, s));
}

static jsval sp_value_of(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    return this_symbol(J, this_val);
}

static jsval sp_description(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    jsval s = this_symbol(J, this_val);
    if (s == JS_EXCEPTION)
        return s;
    struct jssymbol *sym = to_sym(s);
    return sym->description ? str_val(sym->description) : JS_UNDEFINED;
}

void init_symbol(struct js *J)
{
    static const char *const names[SYM_COUNT] = {
        "iterator", "asyncIterator", "hasInstance", "toPrimitive", "toStringTag", "species",
        "isConcatSpreadable", "unscopables", "match", "matchAll", "replace", "search", "split",
    };
    for (int i = 0; i < SYM_COUNT; i++) {
        char buf[48];
        snprintf(buf, sizeof buf, "Symbol.%s", names[i]);
        J->symbol[i] = js_tagged(JS_T_SYMBOL, new_symbol(J, atom_cstr(J, buf)));
    }
    struct jsobject *p = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
    J->proto[P_SYMBOL] = p;
    struct jsobject *c = make_ctor(J, "Symbol", sym_ctor, 0, p);
    J->ctor[C_SYMBOL] = c;
    for (int i = 0; i < SYM_COUNT; i++)
        obj_define_value(J, c, key_cstr(J, names[i]), J->symbol[i], 0);
    obj_define_native(J, c, "for", sym_for, 1, 0);
    obj_define_native(J, c, "keyFor", sym_key_for, 1, 0);
    obj_define_native(J, p, "toString", sp_to_string, 0, 0);
    obj_define_native(J, p, "valueOf", sp_value_of, 0, 0);
    struct jsobject *tp = make_native(J, "[Symbol.toPrimitive]", sp_value_of, 1, 0);
    obj_define_value(J, p, J->symbol[SYM_TO_PRIMITIVE], obj_val(tp), PROP_CONFIGURABLE);
    obj_define_value(J, p, J->symbol[SYM_TO_STRING_TAG], str_val(atom_cstr(J, "Symbol")), PROP_CONFIGURABLE);
    struct jsobject *dg = make_native(J, "get description", sp_description, 0, 0);
    obj_define_accessor(J, p, key_cstr(J, "description"), obj_val(dg), JS_UNDEFINED, PROP_CONFIGURABLE);
}

/* --- iterators ----------------------------------------------------------- */

static jsval it_self(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)J, (void)argc, (void)argv, (void)nt;
    return this_val;
}

static struct jsobject *this_iterator(struct js *J, jsval v, int first_kind, int last_kind, const char *what)
{
    if (js_is_object(v) && obj_class(to_obj(v)) == CLASS_ITERATOR) {
        struct jsobject *o = to_obj(v);
        if (o->u.iter.kind >= first_kind && o->u.iter.kind <= last_kind)
            return o;
    }
    throw_type(J, "%s.next called on an incompatible receiver", what);
    return 0;
}

static jsval array_iter_next(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    struct jsobject *it = this_iterator(J, this_val, ITER_ARRAY_VALUES, ITER_ARRAY_ENTRIES, "%ArrayIterator%");
    if (!it)
        return JS_EXCEPTION;
    if (it->u.iter.done)
        return iter_result(J, JS_UNDEFINED, true);
    jsval t = it->u.iter.target;
    double len;
    struct jsobject *a = to_obj(t);
    if (obj_class(a) == CLASS_ARRAY)
        len = a->length;
    else {
        jsval l = obj_get(J, a, str_val(J->atom[A_length]), t);
        if (l == JS_EXCEPTION || js_to_length(J, l, &len) < 0)
            return JS_EXCEPTION;
    }
    uint32_t i = it->u.iter.index;
    if (i >= len) {
        it->u.iter.done = true;
        return iter_result(J, JS_UNDEFINED, true);
    }
    it->u.iter.index = i + 1;
    if (it->u.iter.kind == ITER_ARRAY_KEYS)
        return iter_result(J, js_number(i), false);
    jsval v = obj_get(J, a, str_val(atom_index(J, i)), t);
    if (v == JS_EXCEPTION)
        return v;
    if (it->u.iter.kind == ITER_ARRAY_VALUES)
        return iter_result(J, v, false);
    jsval pair[2] = { js_number(i), v };
    return iter_result(J, js_create_from_list(J, pair, 2), false);
}

static jsval string_iter_next(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    struct jsobject *it = this_iterator(J, this_val, ITER_STRING, ITER_STRING, "%StringIterator%");
    if (!it)
        return JS_EXCEPTION;
    struct jsstring *s = to_str(it->u.iter.target);
    uint32_t i = it->u.iter.index, n = str_len(s);
    if (it->u.iter.done || i >= n) {
        it->u.iter.done = true;
        return iter_result(J, JS_UNDEFINED, true);
    }
    uint32_t c = str_char_at(J, s, i), k = 1;
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n) {
        uint32_t d = str_char_at(J, s, i + 1);
        if (d >= 0xDC00 && d <= 0xDFFF)
            k = 2;
    }
    it->u.iter.index = i + k;
    return iter_result(J, str_val(str_sub(J, s, i, i + k)), false);
}

void init_iterator(struct js *J)
{
    struct jsobject *ip = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
    J->proto[P_ITERATOR] = ip;
    struct jsobject *self = make_native(J, "[Symbol.iterator]", it_self, 0, 0);
    obj_define_value(J, ip, J->symbol[SYM_ITERATOR], obj_val(self), PROP_WRITABLE | PROP_CONFIGURABLE);

    struct jsobject *ap = obj_new(J, ip, CLASS_OBJECT);
    J->proto[P_ARRAY_ITERATOR] = ap;
    obj_define_native(J, ap, "next", array_iter_next, 0, 0);
    J->array_iter_next = obj_get(J, ap, str_val(J->atom[A_next]), obj_val(ap));
    obj_define_value(J, ap, J->symbol[SYM_TO_STRING_TAG], str_val(atom_cstr(J, "Array Iterator")), PROP_CONFIGURABLE);

    struct jsobject *sp = obj_new(J, ip, CLASS_OBJECT);
    J->proto[P_STRING_ITERATOR] = sp;
    obj_define_native(J, sp, "next", string_iter_next, 0, 0);
    obj_define_value(J, sp, J->symbol[SYM_TO_STRING_TAG], str_val(atom_cstr(J, "String Iterator")), PROP_CONFIGURABLE);
}

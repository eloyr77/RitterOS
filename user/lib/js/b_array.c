/*
 * Array and Array.prototype (27o). Every method works on any array-like
 * `this` as the language says (length, then elements by key); dense arrays
 * take a shortcut where nothing a script could see differs.
 */
#include <math.h>
#include "jsint.h"

static bool dense(struct jsobject *o)
{
    return obj_class(o) == CLASS_ARRAY && !(o->h.flags & OBJ_SPARSE);
}

static jsval index_key(struct js *J, double i)
{
    if (i >= 0 && i <= 4294967294.0)
        return str_val(atom_index(J, (uint32_t)i));
    return str_val(atom(J, number_to_string(J, i, 10)));
}

static int length_of(struct js *J, struct jsobject *o, double *len)
{
    if (obj_class(o) == CLASS_ARRAY) {
        *len = o->length;
        return 0;
    }
    jsval l = obj_get(J, o, str_val(J->atom[A_length]), obj_val(o));
    if (l == JS_EXCEPTION)
        return -1;
    return js_to_length(J, l, len);
}

static jsval get_i(struct js *J, struct jsobject *o, double i)
{
    if (dense(o) && i < o->length && i < o->cap) {
        jsval v = o->elems[(uint32_t)i];
        if (v != JS_EMPTY)
            return v;
    }
    return obj_get(J, o, index_key(J, i), obj_val(o));
}

static int has_i(struct js *J, struct jsobject *o, double i)
{
    if (dense(o) && i < o->length && i < o->cap && o->elems[(uint32_t)i] != JS_EMPTY)
        return 1;
    return obj_has(J, o, index_key(J, i));
}

static int set_i(struct js *J, struct jsobject *o, double i, jsval v)
{
    return obj_set(J, o, index_key(J, i), v, obj_val(o), true);
}

static int create_i(struct js *J, struct jsobject *o, double i, jsval v)
{
    struct pdesc d = { .value = v, .flags = PROP_DEFAULT,
                       .has = DESC_VALUE | DESC_WRITABLE | DESC_ENUMERABLE | DESC_CONFIGURABLE };
    return obj_define(J, o, index_key(J, i), &d, true);
}

static int delete_i(struct js *J, struct jsobject *o, double i)
{
    return obj_delete(J, o, index_key(J, i), true);
}

static int set_length(struct js *J, struct jsobject *o, double len)
{
    return obj_set(J, o, str_val(J->atom[A_length]), js_number(len), obj_val(o), true);
}

static struct jsobject *this_obj(struct js *J, jsval v)
{
    return js_to_object(J, v);
}

static jsval callback(struct js *J, jsval f)
{
    if (!js_is_callable(f)) {
        char *d = js_describe(J, f);
        throw_type(J, "%s is not a function", d);
        free(d);
        return JS_EXCEPTION;
    }
    return f;
}

/* ArraySpeciesCreate */
static struct jsobject *species_create(struct js *J, struct jsobject *o, double len)
{
    if (obj_class(o) == CLASS_ARRAY) {
        jsval c = obj_get(J, o, str_val(J->atom[A_constructor]), obj_val(o));
        if (c == JS_EXCEPTION)
            return 0;
        if (js_is_object(c)) {
            c = obj_get(J, to_obj(c), J->symbol[SYM_SPECIES], c);
            if (c == JS_EXCEPTION)
                return 0;
            if (c == JS_NULL)
                c = JS_UNDEFINED;
        }
        if (c != JS_UNDEFINED && c != obj_val(J->ctor[C_ARRAY])) {
            if (!js_is_constructor(c)) {
                throw_type(J, "Symbol.species is not a constructor");
                return 0;
            }
            jsval arg = js_number(len);
            jsval r = vm_call(J, c, JS_UNDEFINED, 1, &arg, c);
            if (r == JS_EXCEPTION)
                return 0;
            if (!js_is_object(r)) {
                throw_type(J, "Species constructor did not return an object");
                return 0;
            }
            return to_obj(r);
        }
    }
    if (len > 4294967295.0) {
        throw_range(J, "Invalid array length");
        return 0;
    }
    struct jsobject *a = array_new(J, 0);
    if (len > 0 && len <= (1 << 20))
        array_grow(J, a, (uint32_t)len);
    a->length = 0;
    if (len > 0)
        array_set_length(J, a, (uint32_t)len, false);
    return a;
}

static jsval a_ctor(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val;
    struct jsobject *proto = js_proto_from_ctor(J, nt, J->proto[P_ARRAY]);
    if (!proto)
        return JS_EXCEPTION;
    struct jsobject *a;
    if (argc == 1 && js_is_number(argv[0])) {
        double d = js_number_value(argv[0]);
        if (d < 0 || d != (uint32_t)d)
            return throw_range(J, "Invalid array length");
        a = array_new(J, (uint32_t)d);
    } else {
        a = array_new(J, (uint32_t)argc);
        for (int i = 0; i < argc; i++)
            a->elems[i] = argv[i];
    }
    a->proto = proto;
    return obj_val(a);
}

static jsval a_is_array(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    (void)t, (void)c, (void)n;
    int r = value_is_array(J, argv[0]);
    return r < 0 ? JS_EXCEPTION : js_bool(r);
}

static jsval a_of(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)nt;
    struct jsobject *a;
    if (js_is_constructor(this_val) && this_val != obj_val(J->ctor[C_ARRAY])) {
        jsval len = js_number(argc);
        jsval r = vm_call(J, this_val, JS_UNDEFINED, 1, &len, this_val);
        if (r == JS_EXCEPTION)
            return r;
        a = to_obj(r);
    } else
        a = array_new(J, 0);
    for (int i = 0; i < argc; i++)
        if (create_i(J, a, i, argv[i]) < 0)
            return JS_EXCEPTION;
    if (set_length(J, a, argc) < 0)
        return JS_EXCEPTION;
    return obj_val(a);
}

static jsval a_from(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    jsval items = argv[0], mapfn = argv[1], this_arg = argv[2];
    if (mapfn != JS_UNDEFINED && callback(J, mapfn) == JS_EXCEPTION)
        return JS_EXCEPTION;
    bool custom = js_is_constructor(this_val) && this_val != obj_val(J->ctor[C_ARRAY]);
    if (js_is_nullish(items))
        return throw_type(J, "Array.from requires an array-like object");
    jsval using = js_get_method(J, items, J->symbol[SYM_ITERATOR]);
    if (using == JS_EXCEPTION)
        return using;
    struct jsobject *a;
    if (using != JS_UNDEFINED) {
        if (custom) {
            jsval r = vm_call(J, this_val, JS_UNDEFINED, 0, 0, this_val);
            if (r == JS_EXCEPTION)
                return r;
            a = to_obj(r);
        } else
            a = array_new(J, 0);
        struct iter it;
        if (iter_open(J, items, &it) < 0)
            return JS_EXCEPTION;
        double k = 0;
        jsval v;
        int r;
        while ((r = iter_step(J, &it, &v)) > 0) {
            if (mapfn != JS_UNDEFINED) {
                jsval args[2] = { v, js_number(k) };
                v = vm_call(J, mapfn, this_arg, 2, args, JS_UNDEFINED);
                if (v == JS_EXCEPTION) {
                    iter_close(J, &it, true);
                    return v;
                }
            }
            if (create_i(J, a, k, v) < 0) {
                iter_close(J, &it, true);
                return JS_EXCEPTION;
            }
            k++;
        }
        if (r < 0)
            return JS_EXCEPTION;
        if (set_length(J, a, k) < 0)
            return JS_EXCEPTION;
        return obj_val(a);
    }
    struct jsobject *src = js_to_object(J, items);
    double len;
    if (length_of(J, src, &len) < 0)
        return JS_EXCEPTION;
    if (custom) {
        jsval l = js_number(len);
        jsval r = vm_call(J, this_val, JS_UNDEFINED, 1, &l, this_val);
        if (r == JS_EXCEPTION)
            return r;
        a = to_obj(r);
    } else {
        if (len > 4294967295.0)
            return throw_range(J, "Invalid array length");
        a = array_new(J, 0);
    }
    for (double k = 0; k < len; k++) {
        jsval v = get_i(J, src, k);
        if (v == JS_EXCEPTION)
            return v;
        if (mapfn != JS_UNDEFINED) {
            jsval args[2] = { v, js_number(k) };
            v = vm_call(J, mapfn, this_arg, 2, args, JS_UNDEFINED);
            if (v == JS_EXCEPTION)
                return v;
        }
        if (create_i(J, a, k, v) < 0)
            return JS_EXCEPTION;
    }
    if (set_length(J, a, len) < 0)
        return JS_EXCEPTION;
    return obj_val(a);
}

static jsval a_species(struct js *J, jsval this_val, int c, jsval *a, jsval n)
{
    (void)J, (void)c, (void)a, (void)n;
    return this_val;
}

/* --- Array.prototype ------------------------------------------------------ */

static double rel_index(double rel, double len)
{
    if (rel < 0)
        return rel + len < 0 ? 0 : rel + len;
    return rel > len ? len : rel;
}

static jsval ap_at(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    double len;
    if (length_of(J, o, &len) < 0)
        return JS_EXCEPTION;
    bool threw = false;
    double k = js_to_integer(J, argv[0], &threw);
    if (threw)
        return JS_EXCEPTION;
    if (k < 0)
        k += len;
    if (k < 0 || k >= len)
        return JS_UNDEFINED;
    return get_i(J, o, k);
}

static int is_concat_spreadable(struct js *J, jsval v)
{
    if (!js_is_object(v))
        return 0;
    jsval s = obj_get(J, to_obj(v), J->symbol[SYM_IS_CONCAT_SPREADABLE], v);
    if (s == JS_EXCEPTION)
        return -1;
    if (s != JS_UNDEFINED)
        return js_to_boolean(s);
    return value_is_array(J, v);
}

static jsval ap_concat(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)nt;
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    struct jsobject *a = species_create(J, o, 0);
    if (!a)
        return JS_EXCEPTION;
    double n = 0;
    for (int i = -1; i < argc; i++) {
        jsval e = i < 0 ? obj_val(o) : argv[i];
        int sp = is_concat_spreadable(J, e);
        if (sp < 0)
            return JS_EXCEPTION;
        if (sp) {
            struct jsobject *eo = to_obj(e);
            double len;
            if (length_of(J, eo, &len) < 0)
                return JS_EXCEPTION;
            if (n + len > 9007199254740991.0)
                return throw_type(J, "Array too long");
            for (double k = 0; k < len; k++, n++) {
                int h = has_i(J, eo, k);
                if (h < 0)
                    return JS_EXCEPTION;
                if (!h)
                    continue;
                jsval v = get_i(J, eo, k);
                if (v == JS_EXCEPTION || create_i(J, a, n, v) < 0)
                    return JS_EXCEPTION;
            }
        } else {
            if (create_i(J, a, n, e) < 0)
                return JS_EXCEPTION;
            n++;
        }
    }
    if (set_length(J, a, n) < 0)
        return JS_EXCEPTION;
    return obj_val(a);
}

static jsval ap_copy_within(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)nt;
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    double len;
    if (length_of(J, o, &len) < 0)
        return JS_EXCEPTION;
    bool threw = false;
    double to = rel_index(js_to_integer(J, argv[0], &threw), len);
    double from = rel_index(js_to_integer(J, argv[1], &threw), len);
    double fin = argc > 2 && argv[2] != JS_UNDEFINED ? rel_index(js_to_integer(J, argv[2], &threw), len) : len;
    if (threw)
        return JS_EXCEPTION;
    double count = fmin(fin - from, len - to);
    int dir = 1;
    if (from < to && to < from + count) {
        dir = -1;
        from += count - 1;
        to += count - 1;
    }
    for (; count > 0; count--, from += dir, to += dir) {
        int h = has_i(J, o, from);
        if (h < 0)
            return JS_EXCEPTION;
        if (h) {
            jsval v = get_i(J, o, from);
            if (v == JS_EXCEPTION || set_i(J, o, to, v) < 0)
                return JS_EXCEPTION;
        } else if (delete_i(J, o, to) < 0)
            return JS_EXCEPTION;
    }
    return obj_val(o);
}

static jsval ap_iter(struct js *J, jsval this_val, int kind)
{
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    return make_iterator(J, J->proto[P_ARRAY_ITERATOR], obj_val(o), kind);
}

static jsval ap_entries(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)a, (void)n; return ap_iter(J, t, ITER_ARRAY_ENTRIES); }
static jsval ap_keys(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)a, (void)n; return ap_iter(J, t, ITER_ARRAY_KEYS); }
static jsval ap_values(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)a, (void)n; return ap_iter(J, t, ITER_ARRAY_VALUES); }

/* every (0), some (1), forEach (2), map (3), filter (4) */
static jsval iterate(struct js *J, jsval this_val, jsval *argv, int kind)
{
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    double len;
    if (length_of(J, o, &len) < 0)
        return JS_EXCEPTION;
    if (callback(J, argv[0]) == JS_EXCEPTION)
        return JS_EXCEPTION;
    struct jsobject *a = 0;
    double to = 0;
    if (kind == 3 || kind == 4) {
        a = species_create(J, o, kind == 3 ? len : 0);
        if (!a)
            return JS_EXCEPTION;
    }
    for (double k = 0; k < len; k++) {
        int h = has_i(J, o, k);
        if (h < 0)
            return JS_EXCEPTION;
        if (!h)
            continue;
        jsval v = get_i(J, o, k);
        if (v == JS_EXCEPTION)
            return v;
        jsval args[3] = { v, js_number(k), obj_val(o) };
        jsval r = vm_call(J, argv[0], argv[1], 3, args, JS_UNDEFINED);
        if (r == JS_EXCEPTION)
            return r;
        switch (kind) {
        case 0:
            if (!js_to_boolean(r))
                return JS_FALSE;
            break;
        case 1:
            if (js_to_boolean(r))
                return JS_TRUE;
            break;
        case 3:
            if (create_i(J, a, k, r) < 0)
                return JS_EXCEPTION;
            break;
        case 4:
            if (js_to_boolean(r) && create_i(J, a, to++, v) < 0)
                return JS_EXCEPTION;
            break;
        }
    }
    switch (kind) {
    case 0: return JS_TRUE;
    case 1: return JS_FALSE;
    case 2: return JS_UNDEFINED;
    }
    return obj_val(a);
}

static jsval ap_every(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)n; return iterate(J, t, a, 0); }
static jsval ap_some(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)n; return iterate(J, t, a, 1); }
static jsval ap_for_each(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)n; return iterate(J, t, a, 2); }
static jsval ap_map(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)n; return iterate(J, t, a, 3); }
static jsval ap_filter(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)n; return iterate(J, t, a, 4); }

static jsval ap_fill(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)nt;
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    double len;
    if (length_of(J, o, &len) < 0)
        return JS_EXCEPTION;
    bool threw = false;
    double k = argc > 1 ? rel_index(js_to_integer(J, argv[1], &threw), len) : 0;
    double fin = argc > 2 && argv[2] != JS_UNDEFINED ? rel_index(js_to_integer(J, argv[2], &threw), len) : len;
    if (threw)
        return JS_EXCEPTION;
    for (; k < fin; k++)
        if (set_i(J, o, k, argv[0]) < 0)
            return JS_EXCEPTION;
    return obj_val(o);
}

/* find (0), findIndex (1), findLast (2), findLastIndex (3) */
static jsval find(struct js *J, jsval this_val, jsval *argv, int kind)
{
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    double len;
    if (length_of(J, o, &len) < 0)
        return JS_EXCEPTION;
    if (callback(J, argv[0]) == JS_EXCEPTION)
        return JS_EXCEPTION;
    bool last = kind >= 2;
    for (double i = 0; i < len; i++) {
        double k = last ? len - 1 - i : i;
        jsval v = get_i(J, o, k);
        if (v == JS_EXCEPTION)
            return v;
        jsval args[3] = { v, js_number(k), obj_val(o) };
        jsval r = vm_call(J, argv[0], argv[1], 3, args, JS_UNDEFINED);
        if (r == JS_EXCEPTION)
            return r;
        if (js_to_boolean(r))
            return (kind & 1) ? js_number(k) : v;
    }
    return (kind & 1) ? js_number(-1) : JS_UNDEFINED;
}

static jsval ap_find(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)n; return find(J, t, a, 0); }
static jsval ap_find_index(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)n; return find(J, t, a, 1); }
static jsval ap_find_last(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)n; return find(J, t, a, 2); }
static jsval ap_find_last_index(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)n; return find(J, t, a, 3); }

static jsval flatten(struct js *J, struct jsobject *target, struct jsobject *src, double len, double *start,
                     double depth, jsval mapper, jsval this_arg)
{
    for (double k = 0; k < len; k++) {
        int h = has_i(J, src, k);
        if (h < 0)
            return JS_EXCEPTION;
        if (!h)
            continue;
        jsval e = get_i(J, src, k);
        if (e == JS_EXCEPTION)
            return e;
        if (mapper != JS_UNDEFINED) {
            jsval args[3] = { e, js_number(k), obj_val(src) };
            e = vm_call(J, mapper, this_arg, 3, args, JS_UNDEFINED);
            if (e == JS_EXCEPTION)
                return e;
        }
        if (depth > 0 && js_is_object(e) && obj_class(to_obj(e)) == CLASS_ARRAY) {
            double elen;
            if (length_of(J, to_obj(e), &elen) < 0)
                return JS_EXCEPTION;
            if (J->native_depth > NATIVE_DEPTH_MAX)
                return throw_range(J, "Maximum call stack size exceeded.");
            J->native_depth++;
            jsval r = flatten(J, target, to_obj(e), elen, start, depth - 1, JS_UNDEFINED, JS_UNDEFINED);
            J->native_depth--;
            if (r == JS_EXCEPTION)
                return r;
        } else {
            if (create_i(J, target, *start, e) < 0)
                return JS_EXCEPTION;
            (*start)++;
        }
    }
    return JS_TRUE;
}

static jsval ap_flat(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)nt;
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    double len;
    if (length_of(J, o, &len) < 0)
        return JS_EXCEPTION;
    double depth = 1;
    if (argc && argv[0] != JS_UNDEFINED) {
        bool threw = false;
        depth = js_to_integer(J, argv[0], &threw);
        if (threw)
            return JS_EXCEPTION;
        if (depth < 0)
            depth = 0;
    }
    struct jsobject *a = species_create(J, o, 0);
    if (!a)
        return JS_EXCEPTION;
    double n = 0;
    if (flatten(J, a, o, len, &n, depth, JS_UNDEFINED, JS_UNDEFINED) == JS_EXCEPTION)
        return JS_EXCEPTION;
    return obj_val(a);
}

static jsval ap_flat_map(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    double len;
    if (length_of(J, o, &len) < 0)
        return JS_EXCEPTION;
    if (callback(J, argv[0]) == JS_EXCEPTION)
        return JS_EXCEPTION;
    struct jsobject *a = species_create(J, o, 0);
    if (!a)
        return JS_EXCEPTION;
    double n = 0;
    if (flatten(J, a, o, len, &n, 1, argv[0], argv[1]) == JS_EXCEPTION)
        return JS_EXCEPTION;
    return obj_val(a);
}

/* includes (0), indexOf (1), lastIndexOf (2) */
static jsval search(struct js *J, jsval this_val, int argc, jsval *argv, int kind)
{
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    double len;
    if (length_of(J, o, &len) < 0)
        return JS_EXCEPTION;
    if (len == 0)
        return kind == 0 ? JS_FALSE : js_number(-1);
    bool threw = false;
    double n;
    if (kind == 2)
        n = argc > 1 ? js_to_integer(J, argv[1], &threw) : len - 1;
    else
        n = argc > 1 ? js_to_integer(J, argv[1], &threw) : 0;
    if (threw)
        return JS_EXCEPTION;
    if (kind == 2) {
        double k = n >= 0 ? fmin(n, len - 1) : len + n;
        for (; k >= 0; k--) {
            int h = has_i(J, o, k);
            if (h < 0)
                return JS_EXCEPTION;
            if (!h)
                continue;
            jsval v = get_i(J, o, k);
            if (v == JS_EXCEPTION)
                return v;
            if (js_strict_equal(J, v, argv[0]))
                return js_number(k);
        }
        return js_number(-1);
    }
    double k = n >= 0 ? n : (len + n < 0 ? 0 : len + n);
    for (; k < len; k++) {
        if (kind == 1) {
            int h = has_i(J, o, k);
            if (h < 0)
                return JS_EXCEPTION;
            if (!h)
                continue;
        }
        jsval v = get_i(J, o, k);
        if (v == JS_EXCEPTION)
            return v;
        if (kind == 0 ? js_same_value(J, v, argv[0], true) : js_strict_equal(J, v, argv[0]))
            return kind == 0 ? JS_TRUE : js_number(k);
    }
    return kind == 0 ? JS_FALSE : js_number(-1);
}

static jsval ap_includes(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)n; return search(J, t, c, a, 0); }
static jsval ap_index_of(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)n; return search(J, t, c, a, 1); }
static jsval ap_last_index_of(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)n; return search(J, t, c, a, 2); }

/* locale: toLocaleString's locales and options (27s), 0 for join */
static jsval join(struct js *J, struct jsobject *o, jsval sepv, jsval *locale)
{
    double len;
    if (length_of(J, o, &len) < 0)
        return JS_EXCEPTION;
    struct jsstring *sep;
    if (sepv == JS_UNDEFINED)
        sep = str_from_char(J, ',');
    else if (!(sep = js_to_str(J, sepv)))
        return JS_EXCEPTION;
    if (!visiting_push(J, o))
        return str_val(J->atom[A_empty]);
    struct sbuf b = { 0 };
    jsval r = JS_UNDEFINED;
    for (double k = 0; k < len; k++) {
        if (k > 0)
            sb_str(J, &b, sep);
        jsval v = get_i(J, o, k);
        if (v == JS_EXCEPTION) {
            r = v;
            break;
        }
        if (js_is_nullish(v))
            continue;
        if (locale) {
            v = js_call_method(J, v, "toLocaleString", 2, locale);
            if (v == JS_EXCEPTION) {
                r = v;
                break;
            }
        }
        struct jsstring *s = js_to_str(J, v);
        if (!s) {
            r = JS_EXCEPTION;
            break;
        }
        sb_str(J, &b, s);
        if (b.failed)
            break;
    }
    visiting_pop(J);
    if (r == JS_EXCEPTION) {
        sb_free(&b);
        return r;
    }
    struct jsstring *s = sb_finish(J, &b);
    return s ? str_val(s) : JS_EXCEPTION;
}

static jsval ap_join(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    return join(J, o, argv[0], 0);
}

static jsval ap_to_locale_string(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    return join(J, o, JS_UNDEFINED, argv);
}

static jsval ap_to_string(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    jsval f = obj_get(J, o, str_val(J->atom[A_join]), obj_val(o));
    if (f == JS_EXCEPTION)
        return f;
    if (js_is_callable(f))
        return vm_call(J, f, obj_val(o), 0, 0, JS_UNDEFINED);
    jsval ots = obj_get(J, J->proto[P_OBJECT], str_val(J->atom[A_toString]), obj_val(J->proto[P_OBJECT]));
    return vm_call(J, ots, obj_val(o), 0, 0, JS_UNDEFINED);
}

static jsval ap_pop(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    if (dense(o) && !(o->h.flags & OBJ_LENGTH_FIXED) && o->length > 0 && o->length <= o->cap &&
        o->elems[o->length - 1] != JS_EMPTY) {
        jsval v = o->elems[--o->length];
        o->elems[o->length] = JS_EMPTY;
        return v;
    }
    double len;
    if (length_of(J, o, &len) < 0)
        return JS_EXCEPTION;
    if (len == 0) {
        if (set_length(J, o, 0) < 0)
            return JS_EXCEPTION;
        return JS_UNDEFINED;
    }
    jsval v = get_i(J, o, len - 1);
    if (v == JS_EXCEPTION || delete_i(J, o, len - 1) < 0 || set_length(J, o, len - 1) < 0)
        return JS_EXCEPTION;
    return v;
}

static jsval ap_push(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)nt;
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    if (dense(o) && !(o->h.flags & OBJ_LENGTH_FIXED) && (o->h.flags & OBJ_EXTENSIBLE) &&
        (uint64_t)o->length + (uint64_t)argc < (1u << 30)) {
        bool plain = true;
        for (struct jsobject *p = o->proto; p; p = p->proto)
            if ((p->h.flags & (OBJ_HAS_INDEX | OBJ_SPARSE)) ||
                (obj_class(p) == CLASS_ARRAY && p->length > 0))
                plain = false;
        if (plain) {
            if (!array_grow(J, o, o->length + (uint32_t)argc))
                return throw_oom(J);
            for (int i = 0; i < argc; i++)
                o->elems[o->length++] = argv[i];
            return js_number(o->length);
        }
    }
    double len;
    if (length_of(J, o, &len) < 0)
        return JS_EXCEPTION;
    if (len + argc > 9007199254740991.0)
        return throw_type(J, "Pushing too many elements");
    for (int i = 0; i < argc; i++)
        if (set_i(J, o, len + i, argv[i]) < 0)
            return JS_EXCEPTION;
    if (set_length(J, o, len + argc) < 0)
        return JS_EXCEPTION;
    return js_number(len + argc);
}

static jsval reduce(struct js *J, jsval this_val, int argc, jsval *argv, bool right)
{
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    double len;
    if (length_of(J, o, &len) < 0)
        return JS_EXCEPTION;
    if (callback(J, argv[0]) == JS_EXCEPTION)
        return JS_EXCEPTION;
    double k = 0;
    jsval acc = JS_UNDEFINED;
    if (argc >= 2)
        acc = argv[1];
    else {
        bool found = false;
        for (; k < len && !found; k++) {
            double i = right ? len - 1 - k : k;
            int h = has_i(J, o, i);
            if (h < 0)
                return JS_EXCEPTION;
            if (h) {
                acc = get_i(J, o, i);
                if (acc == JS_EXCEPTION)
                    return acc;
                found = true;
            }
        }
        if (!found)
            return throw_type(J, "Reduce of empty array with no initial value");
    }
    for (; k < len; k++) {
        double i = right ? len - 1 - k : k;
        int h = has_i(J, o, i);
        if (h < 0)
            return JS_EXCEPTION;
        if (!h)
            continue;
        jsval v = get_i(J, o, i);
        if (v == JS_EXCEPTION)
            return v;
        jsval args[4] = { acc, v, js_number(i), obj_val(o) };
        acc = vm_call(J, argv[0], JS_UNDEFINED, 4, args, JS_UNDEFINED);
        if (acc == JS_EXCEPTION)
            return acc;
    }
    return acc;
}

static jsval ap_reduce(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)n; return reduce(J, t, c, a, false); }
static jsval ap_reduce_right(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)n; return reduce(J, t, c, a, true); }

static jsval ap_reverse(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    double len;
    if (length_of(J, o, &len) < 0)
        return JS_EXCEPTION;
    double mid = floor(len / 2);
    for (double lower = 0; lower < mid; lower++) {
        double upper = len - 1 - lower;
        int hl = has_i(J, o, lower), hu = hl < 0 ? -1 : has_i(J, o, upper);
        if (hl < 0 || hu < 0)
            return JS_EXCEPTION;
        jsval lv = hl ? get_i(J, o, lower) : JS_UNDEFINED;
        if (lv == JS_EXCEPTION)
            return lv;
        jsval uv = hu ? get_i(J, o, upper) : JS_UNDEFINED;
        if (uv == JS_EXCEPTION)
            return uv;
        if (hu) {
            if (set_i(J, o, lower, uv) < 0)
                return JS_EXCEPTION;
        } else if (delete_i(J, o, lower) < 0)
            return JS_EXCEPTION;
        if (hl) {
            if (set_i(J, o, upper, lv) < 0)
                return JS_EXCEPTION;
        } else if (delete_i(J, o, upper) < 0)
            return JS_EXCEPTION;
    }
    return obj_val(o);
}

static jsval ap_shift(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    if (dense(o) && !(o->h.flags & OBJ_LENGTH_FIXED) && o->length > 0 && o->length <= o->cap) {
        bool holes = false;
        for (uint32_t i = 0; i < o->length; i++)
            if (o->elems[i] == JS_EMPTY)
                holes = true;
        if (!holes) {
            jsval v = o->elems[0];
            memmove(o->elems, o->elems + 1, (size_t)(o->length - 1) * sizeof(jsval));
            o->elems[--o->length] = JS_EMPTY;
            return v;
        }
    }
    double len;
    if (length_of(J, o, &len) < 0)
        return JS_EXCEPTION;
    if (len == 0) {
        if (set_length(J, o, 0) < 0)
            return JS_EXCEPTION;
        return JS_UNDEFINED;
    }
    jsval first = get_i(J, o, 0);
    if (first == JS_EXCEPTION)
        return first;
    for (double k = 1; k < len; k++) {
        int h = has_i(J, o, k);
        if (h < 0)
            return JS_EXCEPTION;
        if (h) {
            jsval v = get_i(J, o, k);
            if (v == JS_EXCEPTION || set_i(J, o, k - 1, v) < 0)
                return JS_EXCEPTION;
        } else if (delete_i(J, o, k - 1) < 0)
            return JS_EXCEPTION;
    }
    if (delete_i(J, o, len - 1) < 0 || set_length(J, o, len - 1) < 0)
        return JS_EXCEPTION;
    return first;
}

static jsval ap_unshift(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)nt;
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    double len;
    if (length_of(J, o, &len) < 0)
        return JS_EXCEPTION;
    if (argc > 0) {
        if (len + argc > 9007199254740991.0)
            return throw_type(J, "Unshift too long");
        if (dense(o) && !(o->h.flags & OBJ_LENGTH_FIXED) && (o->h.flags & OBJ_EXTENSIBLE) &&
            len + argc < (1u << 28) && o->length <= o->cap) {
            if (!array_grow(J, o, o->length + (uint32_t)argc))
                return throw_oom(J);
            memmove(o->elems + argc, o->elems, (size_t)o->length * sizeof(jsval));
            for (int i = 0; i < argc; i++)
                o->elems[i] = argv[i];
            o->length += (uint32_t)argc;
            return js_number(o->length);
        }
        for (double k = len; k > 0; k--) {
            int h = has_i(J, o, k - 1);
            if (h < 0)
                return JS_EXCEPTION;
            if (h) {
                jsval v = get_i(J, o, k - 1);
                if (v == JS_EXCEPTION || set_i(J, o, k - 1 + argc, v) < 0)
                    return JS_EXCEPTION;
            } else if (delete_i(J, o, k - 1 + argc) < 0)
                return JS_EXCEPTION;
        }
        for (int i = 0; i < argc; i++)
            if (set_i(J, o, i, argv[i]) < 0)
                return JS_EXCEPTION;
    }
    if (set_length(J, o, len + argc) < 0)
        return JS_EXCEPTION;
    return js_number(len + argc);
}

static jsval ap_slice(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)nt;
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    double len;
    if (length_of(J, o, &len) < 0)
        return JS_EXCEPTION;
    bool threw = false;
    double k = rel_index(js_to_integer(J, argv[0], &threw), len);
    double fin = argc > 1 && argv[1] != JS_UNDEFINED ? rel_index(js_to_integer(J, argv[1], &threw), len) : len;
    if (threw)
        return JS_EXCEPTION;
    double count = fin > k ? fin - k : 0;
    struct jsobject *a = species_create(J, o, count);
    if (!a)
        return JS_EXCEPTION;
    double n = 0;
    for (; k < fin; k++, n++) {
        int h = has_i(J, o, k);
        if (h < 0)
            return JS_EXCEPTION;
        if (!h)
            continue;
        jsval v = get_i(J, o, k);
        if (v == JS_EXCEPTION || create_i(J, a, n, v) < 0)
            return JS_EXCEPTION;
    }
    if (set_length(J, a, n) < 0)
        return JS_EXCEPTION;
    return obj_val(a);
}

/* --- sorting --------------------------------------------------------------- */

struct sorter {
    struct js *J;
    jsval cmp;
    bool failed;
};

static int compare(struct sorter *S, jsval a, jsval b)
{
    struct js *J = S->J;
    if (S->failed)
        return 0;
    if (a == JS_UNDEFINED)
        return b == JS_UNDEFINED ? 0 : 1;
    if (b == JS_UNDEFINED)
        return -1;
    if (S->cmp != JS_UNDEFINED) {
        jsval args[2] = { a, b };
        jsval r = vm_call(J, S->cmp, JS_UNDEFINED, 2, args, JS_UNDEFINED);
        if (r == JS_EXCEPTION) {
            S->failed = true;
            return 0;
        }
        bool threw = false;
        double d = js_to_number(J, r, &threw);
        if (threw) {
            S->failed = true;
            return 0;
        }
        return d < 0 ? -1 : d > 0 ? 1 : 0;
    }
    if (js_is_string(a) && js_is_string(b))
        return str_compare(J, to_str(a), to_str(b));
    struct jsstring *x = js_to_str(J, a);
    if (!x) {
        S->failed = true;
        return 0;
    }
    jsval xv = str_val(x);
    struct jsstring *y = js_to_str(J, b);
    if (!y) {
        S->failed = true;
        return 0;
    }
    (void)xv;
    return str_compare(J, x, y);
}

/* a stable merge sort of v[0..n) using tmp */
static void merge_sort(struct sorter *S, jsval *v, jsval *tmp, uint32_t n)
{
    if (n < 2)
        return;
    if (n <= 8) {
        for (uint32_t i = 1; i < n && !S->failed; i++) {
            jsval x = v[i];
            uint32_t j = i;
            while (j > 0 && compare(S, v[j - 1], x) > 0 && !S->failed) {
                v[j] = v[j - 1];
                j--;
            }
            v[j] = x;
        }
        return;
    }
    uint32_t m = n / 2;
    merge_sort(S, v, tmp, m);
    merge_sort(S, v + m, tmp, n - m);
    if (S->failed)
        return;
    if (compare(S, v[m - 1], v[m]) <= 0)
        return;
    memcpy(tmp, v, m * sizeof *v);
    uint32_t i = 0, j = m, k = 0;
    while (i < m && j < n && !S->failed) {
        if (compare(S, v[j], tmp[i]) < 0)
            v[k++] = v[j++];
        else
            v[k++] = tmp[i++];
    }
    while (i < m)
        v[k++] = tmp[i++];
}

/* the values of o[0..len) without holes, sorted; *count values, *holes holes */
static jsval *sorted_values(struct js *J, struct jsobject *o, double len, jsval cmp, uint32_t *count, bool skip_holes)
{
    if (len > 100000000)
        return (throw_range(J, "Array too long to sort"), (jsval *)0);
    uint32_t n = 0;
    jsval *v = malloc(((size_t)len + 1) * sizeof *v);
    jsval *tmp = malloc(((size_t)len / 2 + 1) * sizeof *tmp);
    if (!v || !tmp)
        abort();
    js_root_push(J, v, 0);
    for (double k = 0; k < len; k++) {
        if (skip_holes) {
            int h = has_i(J, o, k);
            if (h < 0)
                goto fail;
            if (!h)
                continue;
        }
        jsval x = get_i(J, o, k);
        if (x == JS_EXCEPTION)
            goto fail;
        v[n++] = x;
        J->roots[J->nroots - 1].n = n;
    }
    struct sorter S = { J, cmp, false };
    merge_sort(&S, v, tmp, n);
    free(tmp);
    js_root_pop(J);
    if (S.failed) {
        free(v);
        return 0;
    }
    *count = n;
    return v;
fail:
    js_root_pop(J);
    free(v);
    free(tmp);
    return 0;
}

static jsval ap_sort(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    if (argv[0] != JS_UNDEFINED && !js_is_callable(argv[0]))
        return throw_type(J, "The comparison function must be either a function or undefined");
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    double len;
    if (length_of(J, o, &len) < 0)
        return JS_EXCEPTION;
    uint32_t n;
    jsval *v = sorted_values(J, o, len, argv[0], &n, true);
    if (!v)
        return JS_EXCEPTION;
    js_root_push(J, v, n);
    jsval r = obj_val(o);
    for (uint32_t i = 0; i < n; i++)
        if (set_i(J, o, i, v[i]) < 0) {
            r = JS_EXCEPTION;
            break;
        }
    for (double k = n; r != JS_EXCEPTION && k < len; k++)
        if (delete_i(J, o, k) < 0)
            r = JS_EXCEPTION;
    js_root_pop(J);
    free(v);
    return r;
}

static jsval ap_to_sorted(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    if (argv[0] != JS_UNDEFINED && !js_is_callable(argv[0]))
        return throw_type(J, "The comparison function must be either a function or undefined");
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    double len;
    if (length_of(J, o, &len) < 0)
        return JS_EXCEPTION;
    if (len > 4294967295.0)
        return throw_range(J, "Invalid array length");
    uint32_t n;
    jsval *v = sorted_values(J, o, len, argv[0], &n, false);
    if (!v)
        return JS_EXCEPTION;
    jsval r = js_create_from_list(J, v, n);
    free(v);
    return r;
}

static jsval ap_splice(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)nt;
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    double len;
    if (length_of(J, o, &len) < 0)
        return JS_EXCEPTION;
    bool threw = false;
    double start = rel_index(js_to_integer(J, argv[0], &threw), len);
    if (threw)
        return JS_EXCEPTION;
    double del;
    int items = argc > 2 ? argc - 2 : 0;
    if (argc == 0)
        del = 0;
    else if (argc == 1)
        del = len - start;
    else {
        del = js_to_integer(J, argv[1], &threw);
        if (threw)
            return JS_EXCEPTION;
        del = fmin(fmax(del, 0), len - start);
    }
    if (len + items - del > 9007199254740991.0)
        return throw_type(J, "Splice too long");
    struct jsobject *a = species_create(J, o, del);
    if (!a)
        return JS_EXCEPTION;
    for (double k = 0; k < del; k++) {
        int h = has_i(J, o, start + k);
        if (h < 0)
            return JS_EXCEPTION;
        if (h) {
            jsval v = get_i(J, o, start + k);
            if (v == JS_EXCEPTION || create_i(J, a, k, v) < 0)
                return JS_EXCEPTION;
        }
    }
    if (set_length(J, a, del) < 0)
        return JS_EXCEPTION;
    if (items < del) {
        for (double k = start; k < len - del; k++) {
            int h = has_i(J, o, k + del);
            if (h < 0)
                return JS_EXCEPTION;
            if (h) {
                jsval v = get_i(J, o, k + del);
                if (v == JS_EXCEPTION || set_i(J, o, k + items, v) < 0)
                    return JS_EXCEPTION;
            } else if (delete_i(J, o, k + items) < 0)
                return JS_EXCEPTION;
        }
        for (double k = len; k > len - del + items; k--)
            if (delete_i(J, o, k - 1) < 0)
                return JS_EXCEPTION;
    } else if (items > del) {
        for (double k = len - del; k > start; k--) {
            int h = has_i(J, o, k + del - 1);
            if (h < 0)
                return JS_EXCEPTION;
            if (h) {
                jsval v = get_i(J, o, k + del - 1);
                if (v == JS_EXCEPTION || set_i(J, o, k + items - 1, v) < 0)
                    return JS_EXCEPTION;
            } else if (delete_i(J, o, k + items - 1) < 0)
                return JS_EXCEPTION;
        }
    }
    for (int i = 0; i < items; i++)
        if (set_i(J, o, start + i, argv[2 + i]) < 0)
            return JS_EXCEPTION;
    if (set_length(J, o, len - del + items) < 0)
        return JS_EXCEPTION;
    return obj_val(a);
}

static jsval ap_to_reversed(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    double len;
    if (length_of(J, o, &len) < 0)
        return JS_EXCEPTION;
    if (len > 4294967295.0)
        return throw_range(J, "Invalid array length");
    struct jsobject *a = array_new(J, (uint32_t)len);
    for (double k = 0; k < len; k++) {
        jsval v = get_i(J, o, len - 1 - k);
        if (v == JS_EXCEPTION || create_i(J, a, k, v) < 0)
            return JS_EXCEPTION;
    }
    return obj_val(a);
}

static jsval ap_to_spliced(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)nt;
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    double len;
    if (length_of(J, o, &len) < 0)
        return JS_EXCEPTION;
    bool threw = false;
    double start = rel_index(js_to_integer(J, argv[0], &threw), len);
    double skip;
    int items = argc > 2 ? argc - 2 : 0;
    if (argc == 0)
        skip = 0;
    else if (argc == 1)
        skip = len - start;
    else
        skip = fmin(fmax(js_to_integer(J, argv[1], &threw), 0), len - start);
    if (threw)
        return JS_EXCEPTION;
    double newlen = len + items - skip;
    if (newlen > 9007199254740991.0)
        return throw_type(J, "Array too long");
    if (newlen > 4294967295.0)
        return throw_range(J, "Invalid array length");
    struct jsobject *a = array_new(J, 0);
    double i = 0;
    for (double k = 0; k < start; k++, i++) {
        jsval v = get_i(J, o, k);
        if (v == JS_EXCEPTION || create_i(J, a, i, v) < 0)
            return JS_EXCEPTION;
    }
    for (int k = 0; k < items; k++, i++)
        if (create_i(J, a, i, argv[2 + k]) < 0)
            return JS_EXCEPTION;
    for (double k = start + skip; k < len; k++, i++) {
        jsval v = get_i(J, o, k);
        if (v == JS_EXCEPTION || create_i(J, a, i, v) < 0)
            return JS_EXCEPTION;
    }
    return obj_val(a);
}

static jsval ap_with(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    struct jsobject *o = this_obj(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    double len;
    if (length_of(J, o, &len) < 0)
        return JS_EXCEPTION;
    bool threw = false;
    double rel = js_to_integer(J, argv[0], &threw);
    if (threw)
        return JS_EXCEPTION;
    double at = rel < 0 ? len + rel : rel;
    if (at < 0 || at >= len)
        return throw_range(J, "Invalid index");
    if (len > 4294967295.0)
        return throw_range(J, "Invalid array length");
    struct jsobject *a = array_new(J, 0);
    for (double k = 0; k < len; k++) {
        jsval v = k == at ? argv[1] : get_i(J, o, k);
        if (v == JS_EXCEPTION || create_i(J, a, k, v) < 0)
            return JS_EXCEPTION;
    }
    return obj_val(a);
}

void init_array(struct js *J)
{
    struct jsobject *p = array_new(J, 0);
    p->proto = J->proto[P_OBJECT];
    J->proto[P_ARRAY] = p;
    struct jsobject *c = make_ctor(J, "Array", a_ctor, 1, p);
    J->ctor[C_ARRAY] = c;
    obj_define_native(J, c, "from", a_from, 1, 0);
    obj_define_native(J, c, "isArray", a_is_array, 1, 0);
    obj_define_native(J, c, "of", a_of, 0, 0);
    struct jsobject *sg = make_native(J, "get [Symbol.species]", a_species, 0, 0);
    obj_define_accessor(J, c, J->symbol[SYM_SPECIES], obj_val(sg), JS_UNDEFINED, PROP_CONFIGURABLE);

    static const struct { const char *name; js_native fn; int len; } methods[] = {
        { "at", ap_at, 1 }, { "concat", ap_concat, 1 }, { "copyWithin", ap_copy_within, 2 },
        { "entries", ap_entries, 0 }, { "every", ap_every, 1 }, { "fill", ap_fill, 1 },
        { "filter", ap_filter, 1 }, { "find", ap_find, 1 }, { "findIndex", ap_find_index, 1 },
        { "findLast", ap_find_last, 1 }, { "findLastIndex", ap_find_last_index, 1 },
        { "flat", ap_flat, 0 }, { "flatMap", ap_flat_map, 1 }, { "forEach", ap_for_each, 1 },
        { "includes", ap_includes, 1 }, { "indexOf", ap_index_of, 1 }, { "join", ap_join, 1 },
        { "keys", ap_keys, 0 }, { "lastIndexOf", ap_last_index_of, 1 }, { "map", ap_map, 1 },
        { "pop", ap_pop, 0 }, { "push", ap_push, 1 }, { "reduce", ap_reduce, 1 },
        { "reduceRight", ap_reduce_right, 1 }, { "reverse", ap_reverse, 0 }, { "shift", ap_shift, 0 },
        { "slice", ap_slice, 2 }, { "some", ap_some, 1 }, { "sort", ap_sort, 1 },
        { "splice", ap_splice, 2 }, { "toLocaleString", ap_to_locale_string, 0 },
        { "toReversed", ap_to_reversed, 0 }, { "toSorted", ap_to_sorted, 1 },
        { "toSpliced", ap_to_spliced, 2 }, { "toString", ap_to_string, 0 },
        { "unshift", ap_unshift, 1 }, { "values", ap_values, 0 }, { "with", ap_with, 2 },
    };
    for (size_t i = 0; i < sizeof methods / sizeof methods[0]; i++)
        obj_define_native(J, p, methods[i].name, methods[i].fn, methods[i].len, 0);
    J->array_proto_values = obj_get(J, p, key_cstr(J, "values"), obj_val(p));
    obj_define_value(J, p, J->symbol[SYM_ITERATOR], J->array_proto_values, PROP_WRITABLE | PROP_CONFIGURABLE);
    struct jsobject *un = obj_new(J, 0, CLASS_OBJECT);
    static const char *const unscopable[] = { "at", "copyWithin", "entries", "fill", "find", "findIndex",
                                              "findLast", "findLastIndex", "flat", "flatMap", "includes",
                                              "keys", "toReversed", "toSorted", "toSpliced", "values" };
    for (size_t i = 0; i < sizeof unscopable / sizeof unscopable[0]; i++)
        obj_define_value(J, un, key_cstr(J, unscopable[i]), JS_TRUE, PROP_DEFAULT);
    obj_define_value(J, p, J->symbol[SYM_UNSCOPABLES], obj_val(un), PROP_CONFIGURABLE);
}

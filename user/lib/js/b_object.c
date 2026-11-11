/*
 * Object, its static functions and Object.prototype (27o), and Reflect.
 */
#include "jsint.h"

/* ToPropertyDescriptor */
int to_desc(struct js *J, jsval v, struct pdesc *d)
{
    if (!js_is_object(v)) {
        throw_type(J, "Property description must be an object");
        return -1;
    }
    struct jsobject *o = to_obj(v);
    memset(d, 0, sizeof *d);
    d->value = d->get = d->set = JS_UNDEFINED;
    static const struct { int atom; uint32_t has, flag; } bools[] = {
        { A_enumerable, DESC_ENUMERABLE, PROP_ENUMERABLE },
        { A_configurable, DESC_CONFIGURABLE, PROP_CONFIGURABLE },
        { A_value, DESC_VALUE, 0 },
        { A_writable, DESC_WRITABLE, PROP_WRITABLE },
    };
    for (int i = 0; i < 4; i++) {
        jsval key = str_val(J->atom[bools[i].atom]);
        int h = obj_has(J, o, key);
        if (h < 0)
            return -1;
        if (!h)
            continue;
        jsval x = obj_get(J, o, key, v);
        if (x == JS_EXCEPTION)
            return -1;
        d->has |= bools[i].has;
        if (bools[i].has == DESC_VALUE)
            d->value = x;
        else if (js_to_boolean(x))
            d->flags |= bools[i].flag;
    }
    for (int i = 0; i < 2; i++) {
        jsval key = str_val(J->atom[i ? A_set : A_get]);
        int h = obj_has(J, o, key);
        if (h < 0)
            return -1;
        if (!h)
            continue;
        jsval f = obj_get(J, o, key, v);
        if (f == JS_EXCEPTION)
            return -1;
        if (f != JS_UNDEFINED && !js_is_callable(f)) {
            throw_type(J, i ? "Setter must be a function" : "Getter must be a function");
            return -1;
        }
        if (i) {
            d->set = f;
            d->has |= DESC_SET;
        } else {
            d->get = f;
            d->has |= DESC_GET;
        }
    }
    if ((d->has & (DESC_GET | DESC_SET)) && (d->has & (DESC_VALUE | DESC_WRITABLE))) {
        throw_type(J, "Invalid property descriptor. Cannot both specify accessors and a value or writable attribute");
        return -1;
    }
    return 0;
}

jsval from_desc(struct js *J, const struct pdesc *d)
{
    struct jsobject *o = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
    if (d->flags & PROP_ACCESSOR) {
        prop_add(J, o, str_val(J->atom[A_get]), d->get, PROP_DEFAULT);
        prop_add(J, o, str_val(J->atom[A_set]), d->set, PROP_DEFAULT);
    } else {
        prop_add(J, o, str_val(J->atom[A_value]), d->value, PROP_DEFAULT);
        prop_add(J, o, str_val(J->atom[A_writable]), js_bool(d->flags & PROP_WRITABLE), PROP_DEFAULT);
    }
    prop_add(J, o, str_val(J->atom[A_enumerable]), js_bool(d->flags & PROP_ENUMERABLE), PROP_DEFAULT);
    prop_add(J, o, str_val(J->atom[A_configurable]), js_bool(d->flags & PROP_CONFIGURABLE), PROP_DEFAULT);
    return obj_val(o);
}

static jsval o_ctor(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc;
    if (nt != JS_UNDEFINED && nt != obj_val(J->ctor[C_OBJECT])) {
        struct jsobject *p = js_proto_from_ctor(J, nt, J->proto[P_OBJECT]);
        if (!p)
            return JS_EXCEPTION;
        return obj_val(obj_new(J, p, CLASS_OBJECT));
    }
    if (js_is_nullish(argv[0]))
        return obj_val(obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT));
    struct jsobject *o = js_to_object(J, argv[0]);
    return o ? obj_val(o) : JS_EXCEPTION;
}

static struct jsobject *arg_object(struct js *J, jsval v)
{
    return js_to_object(J, v);
}

static jsval o_keys_like(struct js *J, jsval v, int mode)   /* 0 keys, 1 values, 2 entries */
{
    struct jsobject *o = arg_object(J, v);
    if (!o)
        return JS_EXCEPTION;
    jsval *keys;
    uint32_t n;
    if (obj_own_keys(J, o, 1, &keys, &n) < 0) {
        free(keys);
        return JS_EXCEPTION;
    }
    js_root_push(J, keys, n);
    struct jsobject *a = array_new(J, 0);
    jsval r = obj_val(a);
    for (uint32_t i = 0; i < n; i++) {
        struct pdesc d;
        int h = obj_get_own(J, o, keys[i], &d);
        if (h < 0) {
            r = JS_EXCEPTION;
            break;
        }
        if (!h || !(d.flags & PROP_ENUMERABLE))
            continue;
        jsval item;
        if (mode == 0)
            item = keys[i];
        else {
            jsval val = obj_get(J, o, keys[i], obj_val(o));
            if (val == JS_EXCEPTION) {
                r = val;
                break;
            }
            if (mode == 1)
                item = val;
            else {
                jsval pair[2] = { keys[i], val };
                item = js_create_from_list(J, pair, 2);
            }
        }
        if (a->length >= a->cap)
            array_grow(J, a, a->length + 1);
        a->elems[a->length++] = item;
    }
    js_root_pop(J);
    free(keys);
    return r;
}

static jsval o_keys(struct js *J, jsval t, int c, jsval *argv, jsval n) { (void)t, (void)c, (void)n; return o_keys_like(J, argv[0], 0); }
static jsval o_values(struct js *J, jsval t, int c, jsval *argv, jsval n) { (void)t, (void)c, (void)n; return o_keys_like(J, argv[0], 1); }
static jsval o_entries(struct js *J, jsval t, int c, jsval *argv, jsval n) { (void)t, (void)c, (void)n; return o_keys_like(J, argv[0], 2); }

static jsval own_names(struct js *J, jsval v, int flags)
{
    struct jsobject *o = arg_object(J, v);
    if (!o)
        return JS_EXCEPTION;
    jsval *keys;
    uint32_t n;
    if (obj_own_keys(J, o, flags, &keys, &n) < 0) {
        free(keys);
        return JS_EXCEPTION;
    }
    jsval r = js_create_from_list(J, keys, n);
    free(keys);
    return r;
}

static jsval o_get_own_property_names(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    (void)t, (void)c, (void)n;
    return own_names(J, argv[0], 1);
}

static jsval o_get_own_property_symbols(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    (void)t, (void)c, (void)n;
    return own_names(J, argv[0], 2);
}

static jsval o_assign(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)nt;
    struct jsobject *to = js_to_object(J, argv[0]);
    if (!to)
        return JS_EXCEPTION;
    for (int i = 1; i < argc; i++) {
        if (js_is_nullish(argv[i]))
            continue;
        struct jsobject *from = js_to_object(J, argv[i]);
        jsval *keys;
        uint32_t n;
        if (obj_own_keys(J, from, 3, &keys, &n) < 0) {
            free(keys);
            return JS_EXCEPTION;
        }
        js_root_push(J, keys, n);
        for (uint32_t k = 0; k < n; k++) {
            struct pdesc d;
            int h = obj_get_own(J, from, keys[k], &d);
            if (h < 0) {
                js_root_pop(J);
                free(keys);
                return JS_EXCEPTION;
            }
            if (!h || !(d.flags & PROP_ENUMERABLE))
                continue;
            jsval v = obj_get(J, from, keys[k], obj_val(from));
            if (v == JS_EXCEPTION || obj_set(J, to, keys[k], v, obj_val(to), true) < 0) {
                js_root_pop(J);
                free(keys);
                return JS_EXCEPTION;
            }
        }
        js_root_pop(J);
        free(keys);
    }
    return obj_val(to);
}

static jsval define_properties(struct js *J, struct jsobject *o, jsval props)
{
    struct jsobject *p = js_to_object(J, props);
    if (!p)
        return JS_EXCEPTION;
    jsval *keys;
    uint32_t n;
    if (obj_own_keys(J, p, 3, &keys, &n) < 0) {
        free(keys);
        return JS_EXCEPTION;
    }
    js_root_push(J, keys, n);
    struct pdesc *descs = calloc(n + 1, sizeof *descs);
    jsval *roots = calloc((size_t)n * 3 + 1, sizeof *roots);
    js_root_push(J, roots, (size_t)n * 3);
    uint32_t nd = 0;
    jsval r = obj_val(o);
    jsval *dkeys = calloc(n + 1, sizeof *dkeys);
    for (uint32_t i = 0; i < n; i++) {
        struct pdesc pd;
        int h = obj_get_own(J, p, keys[i], &pd);
        if (h < 0) {
            r = JS_EXCEPTION;
            goto out;
        }
        if (!h || !(pd.flags & PROP_ENUMERABLE))
            continue;
        jsval dv = obj_get(J, p, keys[i], obj_val(p));
        if (dv == JS_EXCEPTION || to_desc(J, dv, &descs[nd]) < 0) {
            r = JS_EXCEPTION;
            goto out;
        }
        roots[nd * 3] = descs[nd].value;
        roots[nd * 3 + 1] = descs[nd].get;
        roots[nd * 3 + 2] = descs[nd].set;
        dkeys[nd++] = keys[i];
    }
    for (uint32_t i = 0; i < nd; i++)
        if (obj_define(J, o, dkeys[i], &descs[i], true) < 0) {
            r = JS_EXCEPTION;
            break;
        }
out:
    js_root_pop(J);
    js_root_pop(J);
    free(roots);
    free(descs);
    free(dkeys);
    free(keys);
    return r;
}

static jsval o_create(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    if (!js_is_object(argv[0]) && argv[0] != JS_NULL)
        return throw_type(J, "Object prototype may only be an Object or null.");
    struct jsobject *o = obj_new(J, js_is_object(argv[0]) ? to_obj(argv[0]) : 0, CLASS_OBJECT);
    if (argv[1] != JS_UNDEFINED)
        return define_properties(J, o, argv[1]);
    return obj_val(o);
}

static jsval o_define_property(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    if (!js_is_object(argv[0]))
        return throw_type(J, "Object.defineProperty called on non-object");
    jsval key = key_from_value(J, argv[1]);
    if (key == JS_EXCEPTION)
        return key;
    struct pdesc d;
    if (to_desc(J, argv[2], &d) < 0)
        return JS_EXCEPTION;
    if (obj_define(J, to_obj(argv[0]), key, &d, true) < 0)
        return JS_EXCEPTION;
    return argv[0];
}

static jsval o_define_properties(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    if (!js_is_object(argv[0]))
        return throw_type(J, "Object.defineProperties called on non-object");
    return define_properties(J, to_obj(argv[0]), argv[1]);
}

static jsval o_get_own_property_descriptor(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    struct jsobject *o = arg_object(J, argv[0]);
    if (!o)
        return JS_EXCEPTION;
    jsval key = key_from_value(J, argv[1]);
    if (key == JS_EXCEPTION)
        return key;
    struct pdesc d;
    int h = obj_get_own(J, o, key, &d);
    if (h < 0)
        return JS_EXCEPTION;
    if (!h)
        return JS_UNDEFINED;
    return from_desc(J, &d);
}

static jsval o_get_own_property_descriptors(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    struct jsobject *o = arg_object(J, argv[0]);
    if (!o)
        return JS_EXCEPTION;
    jsval *keys;
    uint32_t n;
    if (obj_own_keys(J, o, 3, &keys, &n) < 0) {
        free(keys);
        return JS_EXCEPTION;
    }
    js_root_push(J, keys, n);
    struct jsobject *r = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
    bool threw = false;
    for (uint32_t i = 0; i < n && !threw; i++) {
        struct pdesc d;
        int have = obj_get_own(J, o, keys[i], &d);
        if (have < 0)
            threw = true;
        else if (have)
            obj_define_value(J, r, keys[i], from_desc(J, &d), PROP_DEFAULT);
    }
    js_root_pop(J);
    free(keys);
    return threw ? JS_EXCEPTION : obj_val(r);
}

static jsval o_get_prototype_of(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    struct jsobject *o = arg_object(J, argv[0]), *p;
    if (!o || obj_get_proto(J, o, &p) < 0)
        return JS_EXCEPTION;
    return p ? obj_val(p) : JS_NULL;
}

static jsval o_set_prototype_of(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    if (js_is_nullish(argv[0]))
        return throw_type(J, "Object.setPrototypeOf called on null or undefined");
    if (!js_is_object(argv[1]) && argv[1] != JS_NULL)
        return throw_type(J, "Object prototype may only be an Object or null.");
    if (!js_is_object(argv[0]))
        return argv[0];
    int r = obj_set_proto(J, to_obj(argv[0]), js_is_object(argv[1]) ? to_obj(argv[1]) : 0);
    if (r < 0)
        return JS_EXCEPTION;
    if (!r)
        return throw_type(J, "Cyclic __proto__ value");
    return argv[0];
}

static jsval o_is(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    return js_bool(js_same_value(J, argv[0], argv[1], false));
}

/* (64r) What freeze, seal and preventExtensions do with a proxy's "no":
 * a TypeError. */
static jsval level_result(struct js *J, int r, jsval v)
{
    if (r < 0)
        return JS_EXCEPTION;
    if (!r)
        return throw_type(J, "Unable to prevent extension in Object.preventExtensions");
    return v;
}

static jsval o_freeze(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    (void)t, (void)c, (void)n;
    if (js_is_object(argv[0]))
        return level_result(J, obj_set_level(J, to_obj(argv[0]), true), argv[0]);
    return argv[0];
}

static jsval o_seal(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    (void)t, (void)c, (void)n;
    if (js_is_object(argv[0]))
        return level_result(J, obj_set_level(J, to_obj(argv[0]), false), argv[0]);
    return argv[0];
}

static jsval o_prevent_extensions(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    (void)t, (void)c, (void)n;
    if (js_is_object(argv[0]))
        return level_result(J, obj_prevent_extensions(J, to_obj(argv[0])), argv[0]);
    return argv[0];
}

static jsval test_result(int r)
{
    return r < 0 ? JS_EXCEPTION : js_bool(r);
}

static jsval o_is_frozen(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    (void)t, (void)c, (void)n;
    return js_is_object(argv[0]) ? test_result(obj_test_level(J, to_obj(argv[0]), true)) : JS_TRUE;
}

static jsval o_is_sealed(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    (void)t, (void)c, (void)n;
    return js_is_object(argv[0]) ? test_result(obj_test_level(J, to_obj(argv[0]), false)) : JS_TRUE;
}

static jsval o_is_extensible(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    (void)t, (void)c, (void)n;
    return js_is_object(argv[0]) ? test_result(obj_is_extensible(J, to_obj(argv[0]))) : JS_FALSE;
}

static jsval o_from_entries(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    if (js_is_nullish(argv[0]))
        return throw_type(J, "Object.fromEntries requires an iterable");
    struct jsobject *o = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
    struct iter it;
    if (iter_open(J, argv[0], &it) < 0)
        return JS_EXCEPTION;
    jsval e;
    int r;
    while ((r = iter_step(J, &it, &e)) > 0) {
        if (!js_is_object(e)) {
            throw_type(J, "Iterator value is not an entry object");
            iter_close(J, &it, true);
            return JS_EXCEPTION;
        }
        jsval k = js_get_value(J, e, str_val(J->atom[A_zero]));
        if (k == JS_EXCEPTION) {
            iter_close(J, &it, true);
            return k;
        }
        jsval v = js_get_value(J, e, str_val(atom_index(J, 1)));
        if (v == JS_EXCEPTION) {
            iter_close(J, &it, true);
            return v;
        }
        jsval key = key_from_value(J, k);
        if (key == JS_EXCEPTION) {
            iter_close(J, &it, true);
            return key;
        }
        obj_define_value(J, o, key, v, PROP_DEFAULT);
    }
    return r < 0 ? JS_EXCEPTION : obj_val(o);
}

static jsval o_has_own(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    struct jsobject *o = js_to_object(J, argv[0]);
    if (!o)
        return JS_EXCEPTION;
    jsval key = key_from_value(J, argv[1]);
    if (key == JS_EXCEPTION)
        return key;
    struct pdesc d;
    int h = obj_get_own(J, o, key, &d);
    return h < 0 ? JS_EXCEPTION : js_bool(h);
}

static jsval o_group_by(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    if (!js_is_callable(argv[1]))
        return throw_type(J, "Object.groupBy: callback is not a function");
    struct jsobject *o = obj_new(J, 0, CLASS_OBJECT);
    struct iter it;
    if (iter_open(J, argv[0], &it) < 0)
        return JS_EXCEPTION;
    jsval v;
    int r;
    double k = 0;
    while ((r = iter_step(J, &it, &v)) > 0) {
        jsval args[2] = { v, js_number(k++) };
        jsval g = vm_call(J, argv[1], JS_UNDEFINED, 2, args, JS_UNDEFINED);
        if (g == JS_EXCEPTION) {
            iter_close(J, &it, true);
            return g;
        }
        jsval key = key_from_value(J, g);
        if (key == JS_EXCEPTION) {
            iter_close(J, &it, true);
            return key;
        }
        jsval list = obj_get(J, o, key, obj_val(o));
        if (!js_is_object(list)) {
            list = obj_val(array_new(J, 0));
            obj_define_value(J, o, key, list, PROP_DEFAULT);
        }
        struct jsobject *a = to_obj(list);
        array_grow(J, a, a->length + 1);
        a->elems[a->length++] = v;
    }
    return r < 0 ? JS_EXCEPTION : obj_val(o);
}

/* --- Object.prototype ----------------------------------------------------- */

static jsval op_has_own_property(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    jsval key = key_from_value(J, argv[0]);
    if (key == JS_EXCEPTION)
        return key;
    struct jsobject *o = js_to_object(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    struct pdesc d;
    int h = obj_get_own(J, o, key, &d);
    return h < 0 ? JS_EXCEPTION : js_bool(h);
}

static jsval op_is_prototype_of(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    if (!js_is_object(argv[0]))
        return JS_FALSE;
    struct jsobject *o = js_to_object(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    struct jsobject *p = to_obj(argv[0]);
    for (;;) {
        if (obj_get_proto(J, p, &p) < 0)        /* (64r: a proxy's trap) */
            return JS_EXCEPTION;
        if (!p)
            return JS_FALSE;
        if (p == o)
            return JS_TRUE;
    }
}

static jsval op_property_is_enumerable(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    jsval key = key_from_value(J, argv[0]);
    if (key == JS_EXCEPTION)
        return key;
    struct jsobject *o = js_to_object(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    struct pdesc d;
    int h = obj_get_own(J, o, key, &d);
    return h < 0 ? JS_EXCEPTION : js_bool(h && (d.flags & PROP_ENUMERABLE));
}

static jsval op_to_string(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    if (this_val == JS_UNDEFINED)
        return str_val(atom_cstr(J, "[object Undefined]"));
    if (this_val == JS_NULL)
        return str_val(atom_cstr(J, "[object Null]"));
    struct jsobject *o = js_to_object(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    const char *tag = "Object";
    switch (obj_class(o)) {
    case CLASS_ARRAY: tag = "Array"; break;
    case CLASS_ARGUMENTS: tag = "Arguments"; break;
    case CLASS_FUNCTION: case CLASS_NATIVE: case CLASS_BOUND: tag = "Function"; break;
    case CLASS_ERROR: tag = "Error"; break;
    case CLASS_BOOLEAN: tag = "Boolean"; break;
    case CLASS_NUMBER: tag = "Number"; break;
    case CLASS_STRING: tag = "String"; break;
    case CLASS_DATE: tag = "Date"; break;
    case CLASS_REGEXP: tag = "RegExp"; break;
    }
    if (js_is_callable(obj_val(o)))
        tag = "Function";
    if (obj_class(o) == CLASS_PROXY) {          /* (64r) IsArray looks through it */
        int a = value_is_array(J, obj_val(o));
        if (a < 0)
            return JS_EXCEPTION;
        if (a)
            tag = "Array";
    }
    jsval t = obj_get(J, o, J->symbol[SYM_TO_STRING_TAG], obj_val(o));
    if (t == JS_EXCEPTION)
        return t;
    struct sbuf b = { 0 };
    sb_ascii(&b, "[object ");
    if (js_is_string(t))
        sb_str(J, &b, to_str(t));
    else
        sb_ascii(&b, tag);
    sb_unit(&b, ']');
    struct jsstring *s = sb_finish(J, &b);
    return s ? str_val(s) : JS_EXCEPTION;
}

static jsval op_to_locale_string(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    return js_call_method(J, this_val, "toString", 0, 0);
}

static jsval op_value_of(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    struct jsobject *o = js_to_object(J, this_val);
    return o ? obj_val(o) : JS_EXCEPTION;
}

static jsval op_proto_get(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    struct jsobject *o = js_to_object(J, this_val), *p;
    if (!o || obj_get_proto(J, o, &p) < 0)
        return JS_EXCEPTION;
    return p ? obj_val(p) : JS_NULL;
}

static jsval op_proto_set(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    if (js_is_nullish(this_val))
        return throw_type(J, "Object.prototype.__proto__ called on null or undefined");
    if (!js_is_object(argv[0]) && argv[0] != JS_NULL)
        return JS_UNDEFINED;
    if (!js_is_object(this_val))
        return JS_UNDEFINED;
    int r = obj_set_proto(J, to_obj(this_val), js_is_object(argv[0]) ? to_obj(argv[0]) : 0);
    if (r < 0)
        return JS_EXCEPTION;
    if (!r)
        return throw_type(J, "Cyclic __proto__ value");
    return JS_UNDEFINED;
}

/* __defineGetter__ and friends (Annex B) */
static jsval op_define_accessor(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt, bool getter)
{
    (void)argc, (void)nt;
    struct jsobject *o = js_to_object(J, this_val);
    if (!o)
        return JS_EXCEPTION;
    if (!js_is_callable(argv[1]))
        return throw_type(J, "invalid getter or setter");
    jsval key = key_from_value(J, argv[0]);
    if (key == JS_EXCEPTION)
        return key;
    struct pdesc d = { .flags = PROP_ENUMERABLE | PROP_CONFIGURABLE, .has = DESC_ENUMERABLE | DESC_CONFIGURABLE };
    if (getter)
        d.get = argv[1], d.has |= DESC_GET;
    else
        d.set = argv[1], d.has |= DESC_SET;
    return obj_define(J, o, key, &d, true) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

static jsval op_define_getter(struct js *J, jsval t, int c, jsval *a, jsval n) { return op_define_accessor(J, t, c, a, n, true); }
static jsval op_define_setter(struct js *J, jsval t, int c, jsval *a, jsval n) { return op_define_accessor(J, t, c, a, n, false); }

/* --- Reflect ------------------------------------------------------------- */

static jsval r_apply(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    (void)t, (void)c, (void)n;
    if (!js_is_callable(argv[0]))
        return throw_type(J, "Reflect.apply: target is not a function");
    jsval *list;
    uint32_t count;
    if (js_list_from_array_like(J, argv[2], &list, &count) < 0)
        return JS_EXCEPTION;
    js_root_push(J, list, count);
    jsval r = vm_call(J, argv[0], argv[1], (int)count, list, JS_UNDEFINED);
    js_root_pop(J);
    free(list);
    return r;
}

static jsval r_construct(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    (void)t, (void)n;
    if (!js_is_constructor(argv[0]))
        return throw_type(J, "Reflect.construct: target is not a constructor");
    jsval nt = c >= 3 ? argv[2] : argv[0];
    if (!js_is_constructor(nt))
        return throw_type(J, "Reflect.construct: newTarget is not a constructor");
    jsval *list;
    uint32_t count;
    if (js_list_from_array_like(J, argv[1], &list, &count) < 0)
        return JS_EXCEPTION;
    js_root_push(J, list, count);
    jsval r = vm_call(J, argv[0], JS_UNDEFINED, (int)count, list, nt);
    js_root_pop(J);
    free(list);
    return r;
}

static jsval r_need_object(struct js *J, jsval v, const char *fn)
{
    if (!js_is_object(v))
        return throw_type(J, "Reflect.%s called on non-object", fn);
    return v;
}

static jsval r_define_property(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    (void)t, (void)c, (void)n;
    if (r_need_object(J, argv[0], "defineProperty") == JS_EXCEPTION)
        return JS_EXCEPTION;
    jsval key = key_from_value(J, argv[1]);
    if (key == JS_EXCEPTION)
        return key;
    struct pdesc d;
    if (to_desc(J, argv[2], &d) < 0)
        return JS_EXCEPTION;
    int r = obj_define(J, to_obj(argv[0]), key, &d, false);
    return r < 0 ? JS_EXCEPTION : js_bool(r);
}

static jsval r_delete_property(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    (void)t, (void)c, (void)n;
    if (r_need_object(J, argv[0], "deleteProperty") == JS_EXCEPTION)
        return JS_EXCEPTION;
    jsval key = key_from_value(J, argv[1]);
    if (key == JS_EXCEPTION)
        return key;
    int r = obj_delete(J, to_obj(argv[0]), key, false);
    return r < 0 ? JS_EXCEPTION : js_bool(r);
}

static jsval r_get(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    (void)t, (void)n;
    if (r_need_object(J, argv[0], "get") == JS_EXCEPTION)
        return JS_EXCEPTION;
    jsval key = key_from_value(J, argv[1]);
    if (key == JS_EXCEPTION)
        return key;
    return obj_get(J, to_obj(argv[0]), key, c >= 3 ? argv[2] : argv[0]);
}

static jsval r_set(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    (void)t, (void)n;
    if (r_need_object(J, argv[0], "set") == JS_EXCEPTION)
        return JS_EXCEPTION;
    jsval key = key_from_value(J, argv[1]);
    if (key == JS_EXCEPTION)
        return key;
    int r = obj_set(J, to_obj(argv[0]), key, argv[2], c >= 4 ? argv[3] : argv[0], false);
    return r < 0 ? JS_EXCEPTION : js_bool(r);
}

static jsval r_get_own_property_descriptor(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    if (r_need_object(J, argv[0], "getOwnPropertyDescriptor") == JS_EXCEPTION)
        return JS_EXCEPTION;
    return o_get_own_property_descriptor(J, t, c, argv, n);
}

static jsval r_get_prototype_of(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    if (r_need_object(J, argv[0], "getPrototypeOf") == JS_EXCEPTION)
        return JS_EXCEPTION;
    return o_get_prototype_of(J, t, c, argv, n);
}

static jsval r_set_prototype_of(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    (void)t, (void)c, (void)n;
    if (r_need_object(J, argv[0], "setPrototypeOf") == JS_EXCEPTION)
        return JS_EXCEPTION;
    if (!js_is_object(argv[1]) && argv[1] != JS_NULL)
        return throw_type(J, "Object prototype may only be an Object or null.");
    int r = obj_set_proto(J, to_obj(argv[0]), js_is_object(argv[1]) ? to_obj(argv[1]) : 0);
    return r < 0 ? JS_EXCEPTION : js_bool(r);
}

static jsval r_has(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    (void)t, (void)c, (void)n;
    if (r_need_object(J, argv[0], "has") == JS_EXCEPTION)
        return JS_EXCEPTION;
    jsval key = key_from_value(J, argv[1]);
    if (key == JS_EXCEPTION)
        return key;
    int r = obj_has(J, to_obj(argv[0]), key);
    return r < 0 ? JS_EXCEPTION : js_bool(r);
}

static jsval r_is_extensible(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    if (r_need_object(J, argv[0], "isExtensible") == JS_EXCEPTION)
        return JS_EXCEPTION;
    return o_is_extensible(J, t, c, argv, n);
}

static jsval r_own_keys(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    (void)t, (void)c, (void)n;
    if (r_need_object(J, argv[0], "ownKeys") == JS_EXCEPTION)
        return JS_EXCEPTION;
    return own_names(J, argv[0], 3);
}

static jsval r_prevent_extensions(struct js *J, jsval t, int c, jsval *argv, jsval n)
{
    (void)t, (void)c, (void)n;
    if (r_need_object(J, argv[0], "preventExtensions") == JS_EXCEPTION)
        return JS_EXCEPTION;
    return test_result(obj_prevent_extensions(J, to_obj(argv[0])));
}

void init_object(struct js *J)
{
    struct jsobject *proto = J->proto[P_OBJECT];
    struct jsobject *c = make_ctor(J, "Object", o_ctor, 1, proto);
    J->ctor[C_OBJECT] = c;
    obj_define_native(J, c, "assign", o_assign, 2, 0);
    obj_define_native(J, c, "create", o_create, 2, 0);
    obj_define_native(J, c, "defineProperty", o_define_property, 3, 0);
    obj_define_native(J, c, "defineProperties", o_define_properties, 2, 0);
    obj_define_native(J, c, "entries", o_entries, 1, 0);
    obj_define_native(J, c, "freeze", o_freeze, 1, 0);
    obj_define_native(J, c, "fromEntries", o_from_entries, 1, 0);
    obj_define_native(J, c, "getOwnPropertyDescriptor", o_get_own_property_descriptor, 2, 0);
    obj_define_native(J, c, "getOwnPropertyDescriptors", o_get_own_property_descriptors, 1, 0);
    obj_define_native(J, c, "getOwnPropertyNames", o_get_own_property_names, 1, 0);
    obj_define_native(J, c, "getOwnPropertySymbols", o_get_own_property_symbols, 1, 0);
    obj_define_native(J, c, "getPrototypeOf", o_get_prototype_of, 1, 0);
    obj_define_native(J, c, "groupBy", o_group_by, 2, 0);
    obj_define_native(J, c, "hasOwn", o_has_own, 2, 0);
    obj_define_native(J, c, "is", o_is, 2, 0);
    obj_define_native(J, c, "isExtensible", o_is_extensible, 1, 0);
    obj_define_native(J, c, "isFrozen", o_is_frozen, 1, 0);
    obj_define_native(J, c, "isSealed", o_is_sealed, 1, 0);
    obj_define_native(J, c, "keys", o_keys, 1, 0);
    obj_define_native(J, c, "preventExtensions", o_prevent_extensions, 1, 0);
    obj_define_native(J, c, "seal", o_seal, 1, 0);
    obj_define_native(J, c, "setPrototypeOf", o_set_prototype_of, 2, 0);
    obj_define_native(J, c, "values", o_values, 1, 0);

    obj_define_native(J, proto, "hasOwnProperty", op_has_own_property, 1, 0);
    obj_define_native(J, proto, "isPrototypeOf", op_is_prototype_of, 1, 0);
    obj_define_native(J, proto, "propertyIsEnumerable", op_property_is_enumerable, 1, 0);
    obj_define_native(J, proto, "toString", op_to_string, 0, 0);
    obj_define_native(J, proto, "toLocaleString", op_to_locale_string, 0, 0);
    obj_define_native(J, proto, "valueOf", op_value_of, 0, 0);
    obj_define_native(J, proto, "__defineGetter__", op_define_getter, 2, 0);
    obj_define_native(J, proto, "__defineSetter__", op_define_setter, 2, 0);
    struct jsobject *pg = make_native(J, "get __proto__", op_proto_get, 0, 0);
    struct jsobject *ps = make_native(J, "set __proto__", op_proto_set, 1, 0);
    obj_define_accessor(J, proto, str_val(J->atom[A_proto]), obj_val(pg), obj_val(ps), PROP_CONFIGURABLE);

    struct jsobject *r = obj_new(J, proto, CLASS_OBJECT);
    obj_define_native(J, r, "apply", r_apply, 3, 0);
    obj_define_native(J, r, "construct", r_construct, 2, 0);
    obj_define_native(J, r, "defineProperty", r_define_property, 3, 0);
    obj_define_native(J, r, "deleteProperty", r_delete_property, 2, 0);
    obj_define_native(J, r, "get", r_get, 2, 0);
    obj_define_native(J, r, "getOwnPropertyDescriptor", r_get_own_property_descriptor, 2, 0);
    obj_define_native(J, r, "getPrototypeOf", r_get_prototype_of, 1, 0);
    obj_define_native(J, r, "has", r_has, 2, 0);
    obj_define_native(J, r, "isExtensible", r_is_extensible, 1, 0);
    obj_define_native(J, r, "ownKeys", r_own_keys, 1, 0);
    obj_define_native(J, r, "preventExtensions", r_prevent_extensions, 1, 0);
    obj_define_native(J, r, "set", r_set, 3, 0);
    obj_define_native(J, r, "setPrototypeOf", r_set_prototype_of, 2, 0);
    obj_define_value(J, r, J->symbol[SYM_TO_STRING_TAG], str_val(atom_cstr(J, "Reflect")), PROP_CONFIGURABLE);
    obj_define_value(J, J->global, key_cstr(J, "Reflect"), obj_val(r), PROP_WRITABLE | PROP_CONFIGURABLE);
}

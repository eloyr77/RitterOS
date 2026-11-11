/*
 * Proxy (64r): an object whose internal methods are a handler's traps. A
 * proxy (CLASS_PROXY) keeps its target and handler (u.proxy; the handler
 * null once revoked) and nothing else: object.c's internal methods send a
 * proxy here, and each one calls the trap of its name on the handler, or
 * does the same thing on the target when the handler has none. What a
 * trap answers is checked against the target as the standard says (the
 * "invariants": a trap may not report a property the target cannot have,
 * hide one the target cannot lose, and so on), so code that relies on
 * those (Object.freeze, non-configurable properties) can still trust them.
 * A proxy of something callable can be called (the trap `apply`), of a
 * constructor constructed (`construct`). Private fields (#x) live on the
 * proxy itself, as the standard has it.
 */
#include "jsint.h"

static bool revoked(struct js *J, struct jsobject *o)
{
    if (o->u.proxy.handler != JS_NULL)
        return false;
    throw_type(J, "Proxy has already been revoked. No more operations are allowed to be performed on it");
    return true;
}

/* The handler's trap called name: JS_UNDEFINED if it has none, else a
 * function (or JS_EXCEPTION). */
static jsval trap(struct js *J, struct jsobject *o, const char *name)
{
    if (revoked(J, o))
        return JS_EXCEPTION;
    return js_get_method(J, o->u.proxy.handler, key_cstr(J, name));
}

static jsval call_trap(struct js *J, struct jsobject *o, jsval fn, int argc, jsval *argv)
{
    return vm_call(J, fn, o->u.proxy.handler, argc, argv, JS_UNDEFINED);
}

static int fail(struct js *J, const char *what)
{
    throw_type(J, "Proxy object's '%s' trap returned a result the target does not allow", what);
    return -1;
}

static bool is_accessor_desc(const struct pdesc *d)
{
    return d->has & (DESC_GET | DESC_SET);
}

static bool is_data_desc(const struct pdesc *d)
{
    return d->has & (DESC_VALUE | DESC_WRITABLE);
}

/* A property as obj_get_own gives it (flags complete) seen as a
 * definition with every field. */
static void as_full(struct pdesc *d)
{
    d->has = DESC_ENUMERABLE | DESC_CONFIGURABLE |
             ((d->flags & PROP_ACCESSOR) ? DESC_GET | DESC_SET : DESC_VALUE | DESC_WRITABLE);
}

/* IsCompatiblePropertyDescriptor: could the definition d be made on a
 * property that is now current (have = 0: there is none) of an object
 * extensible or not? */
static bool compatible(struct js *J, bool extensible, const struct pdesc *d, int have, const struct pdesc *current)
{
    if (!have)
        return extensible;
    if (!d->has)
        return true;
    if (current->flags & PROP_CONFIGURABLE)
        return true;
    if ((d->has & DESC_CONFIGURABLE) && (d->flags & PROP_CONFIGURABLE))
        return false;
    if ((d->has & DESC_ENUMERABLE) && (d->flags & PROP_ENUMERABLE) != (current->flags & PROP_ENUMERABLE))
        return false;
    bool generic = !is_accessor_desc(d) && !is_data_desc(d);
    bool current_accessor = current->flags & PROP_ACCESSOR;
    if (!generic && is_accessor_desc(d) != current_accessor)
        return false;
    if (current_accessor) {
        if ((d->has & DESC_GET) && !js_same_value(J, d->get, current->get, false))
            return false;
        if ((d->has & DESC_SET) && !js_same_value(J, d->set, current->set, false))
            return false;
    } else if (!(current->flags & PROP_WRITABLE)) {
        if ((d->has & DESC_WRITABLE) && (d->flags & PROP_WRITABLE))
            return false;
        if ((d->has & DESC_VALUE) && !js_same_value(J, d->value, current->value, false))
            return false;
    }
    return true;
}

/* A definition as an object with only the fields it gives (the trap
 * defineProperty's third argument). */
static jsval desc_object(struct js *J, const struct pdesc *d)
{
    struct jsobject *o = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
    if (d->has & DESC_VALUE)
        obj_define_value(J, o, str_val(J->atom[A_value]), d->value, PROP_DEFAULT);
    if (d->has & DESC_WRITABLE)
        obj_define_value(J, o, str_val(J->atom[A_writable]), js_bool(d->flags & PROP_WRITABLE), PROP_DEFAULT);
    if (d->has & DESC_GET)
        obj_define_value(J, o, str_val(J->atom[A_get]), d->get, PROP_DEFAULT);
    if (d->has & DESC_SET)
        obj_define_value(J, o, str_val(J->atom[A_set]), d->set, PROP_DEFAULT);
    if (d->has & DESC_ENUMERABLE)
        obj_define_value(J, o, str_val(J->atom[A_enumerable]), js_bool(d->flags & PROP_ENUMERABLE), PROP_DEFAULT);
    if (d->has & DESC_CONFIGURABLE)
        obj_define_value(J, o, str_val(J->atom[A_configurable]), js_bool(d->flags & PROP_CONFIGURABLE), PROP_DEFAULT);
    return obj_val(o);
}

static int proxy_get_own_impl(struct js *J, struct jsobject *o, jsval key, struct pdesc *d)
{
    jsval fn = trap(J, o, "getOwnPropertyDescriptor");
    if (fn == JS_EXCEPTION)
        return -1;
    struct jsobject *target = to_obj(o->u.proxy.target);
    if (fn == JS_UNDEFINED)
        return obj_get_own(J, target, key, d);
    jsval args[2] = { o->u.proxy.target, key };
    jsval r = call_trap(J, o, fn, 2, args);
    if (r == JS_EXCEPTION)
        return -1;
    if (!js_is_object(r) && r != JS_UNDEFINED)
        return fail(J, "getOwnPropertyDescriptor");
    struct pdesc current;
    int have = obj_get_own(J, target, key, &current);
    if (have < 0)
        return -1;
    if (r == JS_UNDEFINED) {
        if (!have)
            return 0;
        if (!(current.flags & PROP_CONFIGURABLE))
            return fail(J, "getOwnPropertyDescriptor");
        int ext = obj_is_extensible(J, target);
        if (ext < 0)
            return -1;
        if (!ext)
            return fail(J, "getOwnPropertyDescriptor");
        return 0;
    }
    int ext = obj_is_extensible(J, target);
    if (ext < 0)
        return -1;
    struct pdesc got;
    if (to_desc(J, r, &got) < 0)
        return -1;
    /* CompletePropertyDescriptor (to_desc leaves what is missing false or
     * undefined) */
    if (is_accessor_desc(&got))
        got.flags |= PROP_ACCESSOR;
    if (have)
        as_full(&current);
    struct pdesc full = got;
    full.has = DESC_ENUMERABLE | DESC_CONFIGURABLE |
               ((got.flags & PROP_ACCESSOR) ? DESC_GET | DESC_SET : DESC_VALUE | DESC_WRITABLE);
    if (!compatible(J, ext, &full, have, &current))
        return fail(J, "getOwnPropertyDescriptor");
    if (!(got.flags & PROP_CONFIGURABLE)) {
        if (!have || (current.flags & PROP_CONFIGURABLE))
            return fail(J, "getOwnPropertyDescriptor");
        if ((got.has & DESC_WRITABLE) && !(got.flags & PROP_WRITABLE) && (current.flags & PROP_WRITABLE))
            return fail(J, "getOwnPropertyDescriptor");
    }
    *d = full;
    return 1;
}

static int proxy_define_impl(struct js *J, struct jsobject *o, jsval key, const struct pdesc *d, bool throw_on_fail)
{
    jsval fn = trap(J, o, "defineProperty");
    if (fn == JS_EXCEPTION)
        return -1;
    struct jsobject *target = to_obj(o->u.proxy.target);
    if (fn == JS_UNDEFINED)
        return obj_define(J, target, key, d, throw_on_fail);
    jsval args[3] = { o->u.proxy.target, key, desc_object(J, d) };
    jsval r = call_trap(J, o, fn, 3, args);
    if (r == JS_EXCEPTION)
        return -1;
    if (!js_to_boolean(r)) {
        if (!throw_on_fail)
            return 0;
        char *k = js_describe(J, key);
        throw_type(J, "Proxy object's 'defineProperty' trap returned falsy for property '%s'", k);
        free(k);
        return -1;
    }
    struct pdesc current;
    int have = obj_get_own(J, target, key, &current);
    if (have < 0)
        return -1;
    int ext = obj_is_extensible(J, target);
    if (ext < 0)
        return -1;
    bool config_false = (d->has & DESC_CONFIGURABLE) && !(d->flags & PROP_CONFIGURABLE);
    if (!have) {
        if (!ext || config_false)
            return fail(J, "defineProperty");
        return 1;
    }
    as_full(&current);
    if (!compatible(J, ext, d, have, &current))
        return fail(J, "defineProperty");
    if (config_false && (current.flags & PROP_CONFIGURABLE))
        return fail(J, "defineProperty");
    if (!(current.flags & PROP_ACCESSOR) && !(current.flags & PROP_CONFIGURABLE) &&
        (current.flags & PROP_WRITABLE) && (d->has & DESC_WRITABLE) && !(d->flags & PROP_WRITABLE))
        return fail(J, "defineProperty");
    return 1;
}

static int proxy_has_impl(struct js *J, struct jsobject *o, jsval key)
{
    jsval fn = trap(J, o, "has");
    if (fn == JS_EXCEPTION)
        return -1;
    struct jsobject *target = to_obj(o->u.proxy.target);
    if (fn == JS_UNDEFINED)
        return obj_has(J, target, key);
    jsval args[2] = { o->u.proxy.target, key };
    jsval r = call_trap(J, o, fn, 2, args);
    if (r == JS_EXCEPTION)
        return -1;
    if (js_to_boolean(r))
        return 1;
    struct pdesc current;
    int have = obj_get_own(J, target, key, &current);
    if (have < 0)
        return -1;
    if (have) {
        if (!(current.flags & PROP_CONFIGURABLE))
            return fail(J, "has");
        int ext = obj_is_extensible(J, target);
        if (ext < 0)
            return -1;
        if (!ext)
            return fail(J, "has");
    }
    return 0;
}

static jsval proxy_get_impl(struct js *J, struct jsobject *o, jsval key, jsval receiver)
{
    jsval fn = trap(J, o, "get");
    if (fn == JS_EXCEPTION)
        return JS_EXCEPTION;
    struct jsobject *target = to_obj(o->u.proxy.target);
    if (fn == JS_UNDEFINED)
        return obj_get(J, target, key, receiver);
    jsval args[3] = { o->u.proxy.target, key, receiver };
    jsval r = call_trap(J, o, fn, 3, args);
    if (r == JS_EXCEPTION)
        return r;
    struct pdesc current;
    int have = obj_get_own(J, target, key, &current);
    if (have < 0)
        return JS_EXCEPTION;
    if (have && !(current.flags & PROP_CONFIGURABLE)) {
        if (!(current.flags & PROP_ACCESSOR) && !(current.flags & PROP_WRITABLE) &&
            !js_same_value(J, r, current.value, false)) {
            fail(J, "get");
            return JS_EXCEPTION;
        }
        if ((current.flags & PROP_ACCESSOR) && current.get == JS_UNDEFINED && r != JS_UNDEFINED) {
            fail(J, "get");
            return JS_EXCEPTION;
        }
    }
    return r;
}

static int proxy_set_impl(struct js *J, struct jsobject *o, jsval key, jsval v, jsval receiver, bool strict)
{
    jsval fn = trap(J, o, "set");
    if (fn == JS_EXCEPTION)
        return -1;
    struct jsobject *target = to_obj(o->u.proxy.target);
    if (fn == JS_UNDEFINED)
        return obj_set(J, target, key, v, receiver, strict);
    jsval args[4] = { o->u.proxy.target, key, v, receiver };
    jsval r = call_trap(J, o, fn, 4, args);
    if (r == JS_EXCEPTION)
        return -1;
    if (!js_to_boolean(r)) {
        if (!strict)
            return 0;
        char *k = js_describe(J, key);
        throw_type(J, "Proxy object's 'set' trap returned falsy value for property '%s'", k);
        free(k);
        return -1;
    }
    struct pdesc current;
    int have = obj_get_own(J, target, key, &current);
    if (have < 0)
        return -1;
    if (have && !(current.flags & PROP_CONFIGURABLE)) {
        if (!(current.flags & PROP_ACCESSOR) && !(current.flags & PROP_WRITABLE) &&
            !js_same_value(J, v, current.value, false))
            return fail(J, "set");
        if ((current.flags & PROP_ACCESSOR) && current.set == JS_UNDEFINED)
            return fail(J, "set");
    }
    return 1;
}

static int proxy_delete_impl(struct js *J, struct jsobject *o, jsval key, bool strict)
{
    jsval fn = trap(J, o, "deleteProperty");
    if (fn == JS_EXCEPTION)
        return -1;
    struct jsobject *target = to_obj(o->u.proxy.target);
    if (fn == JS_UNDEFINED)
        return obj_delete(J, target, key, strict);
    jsval args[2] = { o->u.proxy.target, key };
    jsval r = call_trap(J, o, fn, 2, args);
    if (r == JS_EXCEPTION)
        return -1;
    if (!js_to_boolean(r)) {
        if (!strict)
            return 0;
        char *k = js_describe(J, key);
        throw_type(J, "Proxy object's 'deleteProperty' trap returned falsy value for property '%s'", k);
        free(k);
        return -1;
    }
    struct pdesc current;
    int have = obj_get_own(J, target, key, &current);
    if (have < 0)
        return -1;
    if (have) {
        if (!(current.flags & PROP_CONFIGURABLE))
            return fail(J, "deleteProperty");
        int ext = obj_is_extensible(J, target);
        if (ext < 0)
            return -1;
        if (!ext)
            return fail(J, "deleteProperty");
    }
    return 1;
}

/* Every own key the trap ownKeys gives (strings and symbols, no key twice),
 * checked against the target: its non-configurable keys must be there,
 * and all of its keys and no others if it is not extensible. As
 * obj_own_keys: a malloc'd array, -1 on a throw. */
static int proxy_own_keys_impl(struct js *J, struct jsobject *o, jsval **keysp, uint32_t *np)
{
    *keysp = 0;
    *np = 0;
    jsval fn = trap(J, o, "ownKeys");
    if (fn == JS_EXCEPTION)
        return -1;
    struct jsobject *target = to_obj(o->u.proxy.target);
    if (fn == JS_UNDEFINED)
        return obj_own_keys(J, target, 3, keysp, np);
    jsval args[1] = { o->u.proxy.target };
    jsval r = call_trap(J, o, fn, 1, args);
    if (r == JS_EXCEPTION)
        return -1;
    if (!js_is_object(r)) {
        throw_type(J, "Proxy handler's ownKeys trap must return an object");
        return -1;
    }
    jsval *keys;
    uint32_t n;
    if (js_list_from_array_like(J, r, &keys, &n) < 0)
        return -1;
    js_root_push(J, keys, n);
    int result = -1;
    jsval *tkeys = 0;
    uint32_t tn = 0;
    bool *used = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!js_is_string(keys[i]) && !js_is_symbol(keys[i])) {
            throw_type(J, "Proxy handler's ownKeys trap result must contain only strings and symbols");
            goto out;
        }
        keys[i] = key_from_value(J, keys[i]);
        if (keys[i] == JS_EXCEPTION)
            goto out;
        for (uint32_t k = 0; k < i; k++)
            if (keys[k] == keys[i]) {
                throw_type(J, "Proxy handler's ownKeys trap result must not contain any duplicate names");
                goto out;
            }
    }
    int ext = obj_is_extensible(J, target);
    if (ext < 0 || obj_own_keys(J, target, 3, &tkeys, &tn) < 0)
        goto out;
    js_root_push(J, tkeys, tn);
    used = calloc(n ? n : 1, sizeof *used);
    if (!used)
        abort();
    /* the target's non-configurable keys first, then (not extensible) the rest */
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t t = 0; t < tn; t++) {
            struct pdesc d;
            int have = obj_get_own(J, target, tkeys[t], &d);
            if (have < 0)
                goto out_target;
            bool fixed = have && !(d.flags & PROP_CONFIGURABLE);
            if (pass == 0 ? !fixed : (ext || fixed))
                continue;
            bool found = false;
            for (uint32_t i = 0; i < n && !found; i++)
                if (keys[i] == tkeys[t]) {
                    found = true;
                    used[i] = true;
                }
            if (!found) {
                fail(J, "ownKeys");
                goto out_target;
            }
        }
    }
    if (!ext)
        for (uint32_t i = 0; i < n; i++)
            if (!used[i]) {
                fail(J, "ownKeys");
                goto out_target;
            }
    result = 0;
out_target:
    js_root_pop(J);
    free(tkeys);
out:
    free(used);
    js_root_pop(J);
    if (result < 0) {
        free(keys);
        return -1;
    }
    *keysp = keys;
    *np = n;
    return 0;
}

static int proxy_get_proto_impl(struct js *J, struct jsobject *o, struct jsobject **proto)
{
    jsval fn = trap(J, o, "getPrototypeOf");
    if (fn == JS_EXCEPTION)
        return -1;
    struct jsobject *target = to_obj(o->u.proxy.target);
    if (fn == JS_UNDEFINED)
        return obj_get_proto(J, target, proto);
    jsval args[1] = { o->u.proxy.target };
    jsval r = call_trap(J, o, fn, 1, args);
    if (r == JS_EXCEPTION)
        return -1;
    if (!js_is_object(r) && r != JS_NULL) {
        throw_type(J, "Proxy handler's getPrototypeOf trap should either return an object or null");
        return -1;
    }
    *proto = r == JS_NULL ? 0 : to_obj(r);
    int ext = obj_is_extensible(J, target);
    if (ext < 0)
        return -1;
    if (ext)
        return 0;
    struct jsobject *tp;
    if (obj_get_proto(J, target, &tp) < 0)
        return -1;
    if (tp != *proto)
        return fail(J, "getPrototypeOf");
    return 0;
}

static int proxy_set_proto_impl(struct js *J, struct jsobject *o, struct jsobject *proto)
{
    jsval fn = trap(J, o, "setPrototypeOf");
    if (fn == JS_EXCEPTION)
        return -1;
    struct jsobject *target = to_obj(o->u.proxy.target);
    if (fn == JS_UNDEFINED)
        return obj_set_proto(J, target, proto);
    jsval args[2] = { o->u.proxy.target, proto ? obj_val(proto) : JS_NULL };
    jsval r = call_trap(J, o, fn, 2, args);
    if (r == JS_EXCEPTION)
        return -1;
    if (!js_to_boolean(r))
        return 0;
    int ext = obj_is_extensible(J, target);
    if (ext < 0)
        return -1;
    if (ext)
        return 1;
    struct jsobject *tp;
    if (obj_get_proto(J, target, &tp) < 0)
        return -1;
    if (tp != proto)
        return fail(J, "setPrototypeOf");
    return 1;
}

static int proxy_is_extensible_impl(struct js *J, struct jsobject *o)
{
    jsval fn = trap(J, o, "isExtensible");
    if (fn == JS_EXCEPTION)
        return -1;
    struct jsobject *target = to_obj(o->u.proxy.target);
    if (fn == JS_UNDEFINED)
        return obj_is_extensible(J, target);
    jsval args[1] = { o->u.proxy.target };
    jsval r = call_trap(J, o, fn, 1, args);
    if (r == JS_EXCEPTION)
        return -1;
    int ext = obj_is_extensible(J, target);
    if (ext < 0)
        return -1;
    if (js_to_boolean(r) != (ext != 0))
        return fail(J, "isExtensible");
    return ext;
}

static int proxy_prevent_extensions_impl(struct js *J, struct jsobject *o)
{
    jsval fn = trap(J, o, "preventExtensions");
    if (fn == JS_EXCEPTION)
        return -1;
    struct jsobject *target = to_obj(o->u.proxy.target);
    if (fn == JS_UNDEFINED)
        return obj_prevent_extensions(J, target);
    jsval args[1] = { o->u.proxy.target };
    jsval r = call_trap(J, o, fn, 1, args);
    if (r == JS_EXCEPTION)
        return -1;
    if (!js_to_boolean(r))
        return 0;
    int ext = obj_is_extensible(J, target);
    if (ext < 0)
        return -1;
    if (ext)
        return fail(J, "preventExtensions");
    return 1;
}

/* [[Call]] and [[Construct]] (vm_call sends a proxy here). */
static jsval proxy_call_impl(struct js *J, struct jsobject *o, jsval this_val, int argc, jsval *argv, jsval new_target)
{
    bool construct = !js_is_undefined(new_target);
    jsval fn = trap(J, o, construct ? "construct" : "apply");
    if (fn == JS_EXCEPTION)
        return JS_EXCEPTION;
    jsval target = o->u.proxy.target;
    if (fn == JS_UNDEFINED)
        return vm_call(J, target, this_val, argc, argv, new_target);
    jsval list = js_create_from_list(J, argv, (uint32_t)argc);
    if (list == JS_EXCEPTION)
        return list;
    jsval r;
    if (construct) {
        jsval args[3] = { target, list, new_target };
        r = call_trap(J, o, fn, 3, args);
        if (r != JS_EXCEPTION && !js_is_object(r))
            return throw_type(J, "Result of 'construct' trap is not an object");
    } else {
        jsval args[3] = { target, this_val, list };
        r = call_trap(J, o, fn, 3, args);
    }
    return r;
}

/* --- the internal methods as object.c calls them ---------------------------
 * Each counts as a native call level: a proxy forwarding to its target
 * forwards in C, and a proxy may sit in its own target's prototype chain
 * (the cycle check stops at a proxy, as the standard's does), so without a
 * limit a property lookup could recurse until the C stack ran out. */
static bool deep(struct js *J)
{
    if (J->native_depth >= NATIVE_DEPTH_MAX) {
        throw_range(J, "Maximum call stack size exceeded.");
        return true;
    }
    J->native_depth++;
    return false;
}

int proxy_get_own(struct js *J, struct jsobject *o, jsval key, struct pdesc *d)
{
    if (deep(J))
        return -1;
    int r = proxy_get_own_impl(J, o, key, d);
    J->native_depth--;
    return r;
}

int proxy_define(struct js *J, struct jsobject *o, jsval key, const struct pdesc *d, bool throw_on_fail)
{
    if (deep(J))
        return -1;
    int r = proxy_define_impl(J, o, key, d, throw_on_fail);
    J->native_depth--;
    return r;
}

int proxy_has(struct js *J, struct jsobject *o, jsval key)
{
    if (deep(J))
        return -1;
    int r = proxy_has_impl(J, o, key);
    J->native_depth--;
    return r;
}

jsval proxy_get(struct js *J, struct jsobject *o, jsval key, jsval receiver)
{
    if (deep(J))
        return JS_EXCEPTION;
    jsval r = proxy_get_impl(J, o, key, receiver);
    J->native_depth--;
    return r;
}

int proxy_set(struct js *J, struct jsobject *o, jsval key, jsval v, jsval receiver, bool strict)
{
    if (deep(J))
        return -1;
    int r = proxy_set_impl(J, o, key, v, receiver, strict);
    J->native_depth--;
    return r;
}

int proxy_delete(struct js *J, struct jsobject *o, jsval key, bool strict)
{
    if (deep(J))
        return -1;
    int r = proxy_delete_impl(J, o, key, strict);
    J->native_depth--;
    return r;
}

int proxy_own_keys(struct js *J, struct jsobject *o, jsval **keysp, uint32_t *np)
{
    if (deep(J)) {
        *keysp = 0;
        *np = 0;
        return -1;
    }
    int r = proxy_own_keys_impl(J, o, keysp, np);
    J->native_depth--;
    return r;
}

int proxy_get_proto(struct js *J, struct jsobject *o, struct jsobject **proto)
{
    if (deep(J))
        return -1;
    int r = proxy_get_proto_impl(J, o, proto);
    J->native_depth--;
    return r;
}

int proxy_set_proto(struct js *J, struct jsobject *o, struct jsobject *proto)
{
    if (deep(J))
        return -1;
    int r = proxy_set_proto_impl(J, o, proto);
    J->native_depth--;
    return r;
}

int proxy_is_extensible(struct js *J, struct jsobject *o)
{
    if (deep(J))
        return -1;
    int r = proxy_is_extensible_impl(J, o);
    J->native_depth--;
    return r;
}

int proxy_prevent_extensions(struct js *J, struct jsobject *o)
{
    if (deep(J))
        return -1;
    int r = proxy_prevent_extensions_impl(J, o);
    J->native_depth--;
    return r;
}

jsval proxy_call(struct js *J, struct jsobject *o, jsval this_val, int argc, jsval *argv, jsval new_target)
{
    if (deep(J))
        return JS_EXCEPTION;
    jsval r = proxy_call_impl(J, o, this_val, argc, argv, new_target);
    J->native_depth--;
    return r;
}

/* IsArray looks through proxies (a revoked one throws). */
int proxy_is_array(struct js *J, struct jsobject *o)
{
    if (revoked(J, o))
        return -1;
    struct jsobject *t = to_obj(o->u.proxy.target);
    if (obj_class(t) == CLASS_PROXY)
        return proxy_is_array(J, t);
    return obj_class(t) == CLASS_ARRAY;
}

static jsval make_proxy(struct js *J, jsval target, jsval handler)
{
    if (!js_is_object(target) || !js_is_object(handler))
        return throw_type(J, "A Proxy's 'target' and 'handler' should both be objects");
    struct jsobject *p = obj_new(J, 0, CLASS_PROXY);
    p->u.proxy.target = target;
    p->u.proxy.handler = handler;
    p->h.flags |= to_obj(target)->h.flags & (OBJ_CALLABLE | OBJ_CONSTRUCTOR);
    return obj_val(p);
}

static jsval proxy_ctor(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc;
    if (js_is_undefined(nt))
        return throw_type(J, "calling Proxy constructor without new is invalid");
    return make_proxy(J, argv[0], argv[1]);
}

static jsval proxy_revoke(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)argv, (void)nt;
    struct jsobject *self = J->native_callee;
    jsval p = self->u.native.data;
    if (js_is_object(p)) {
        self->u.native.data = JS_NULL;
        to_obj(p)->u.proxy.target = JS_NULL;
        to_obj(p)->u.proxy.handler = JS_NULL;
    }
    return JS_UNDEFINED;
}

static jsval proxy_revocable(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    jsval p = make_proxy(J, argv[0], argv[1]);
    if (p == JS_EXCEPTION)
        return p;
    struct jsobject *revoke = make_native(J, "", proxy_revoke, 0, 0);
    revoke->u.native.data = p;
    struct jsobject *r = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
    obj_define_value(J, r, key_cstr(J, "proxy"), p, PROP_DEFAULT);
    obj_define_value(J, r, key_cstr(J, "revoke"), obj_val(revoke), PROP_DEFAULT);
    return obj_val(r);
}

void init_proxy(struct js *J)
{
    struct jsobject *c = make_ctor(J, "Proxy", proxy_ctor, 2, 0);
    obj_define_native(J, c, "revocable", proxy_revocable, 2, 0);
}

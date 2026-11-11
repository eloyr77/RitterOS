/*
 * Objects and the language's abstract operations (27o): property maps,
 * [[GetOwnProperty]], [[DefineOwnProperty]], [[Get]], [[Set]], [[Delete]],
 * [[OwnPropertyKeys]] for ordinary objects, arrays (exotic `length`, a dense
 * element vector while every element is a plain writable property) and
 * String objects (their characters); then ToPrimitive, ToString, ToNumber,
 * ToObject, ToPropertyKey, equality, typeof and instanceof, the iterator
 * protocol, and throwing errors.
 *
 * A `struct prop *` is only good until the object's map changes: never keep
 * one across a call into scripts or another property's addition.
 */
#include <math.h>
#include "jsint.h"

/* --- property maps ------------------------------------------------------- */

uint32_t key_hash(jsval key)
{
    if (js_is_string(key))
        return to_str(key)->hash;
    return to_sym(key)->hash;
}

static void index_rebuild(struct propmap *m, uint32_t cap)
{
    free(m->index);
    m->index = calloc(cap, sizeof *m->index);
    if (!m->index)
        abort();
    m->index_cap = cap;
    for (uint32_t i = 0; i < m->n; i++) {
        if (m->e[i].key == JS_EMPTY)
            continue;
        uint32_t k = key_hash(m->e[i].key) & (cap - 1);
        while (m->index[k])
            k = (k + 1) & (cap - 1);
        m->index[k] = i + 1;
    }
}

struct prop *prop_find(struct propmap *m, jsval key)
{
    if (!m->index) {
        for (uint32_t i = 0; i < m->n; i++)
            if (m->e[i].key == key)
                return &m->e[i];
        return 0;
    }
    uint32_t mask = m->index_cap - 1;
    for (uint32_t k = key_hash(key) & mask; m->index[k]; k = (k + 1) & mask) {
        struct prop *p = &m->e[m->index[k] - 1];
        if (p->key == key)
            return p;
    }
    return 0;
}

static void compact(struct propmap *m)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < m->n; i++)
        if (m->e[i].key != JS_EMPTY)
            m->e[k++] = m->e[i];
    m->n = k;
    if (m->index)
        index_rebuild(m, m->index_cap);
}

struct prop *prop_add(struct js *J, struct jsobject *o, jsval key, jsval value, uint32_t flags)
{
    struct propmap *m = &o->props;
    if (m->n == m->cap) {
        if (m->live < m->n / 2 && m->n >= 16)
            compact(m);
        else {
            uint32_t cap = m->cap ? m->cap * 2 : 4;
            struct prop *e = realloc(m->e, cap * sizeof *e);
            if (!e)
                abort();
            gc_note_external(J, (cap - m->cap) * sizeof *e);
            m->e = e;
            m->cap = cap;
        }
    }
    struct prop *p = &m->e[m->n++];
    p->key = key;
    p->value = value;
    p->flags = flags;
    m->live++;
    if (m->index) {
        if (m->n * 2 > m->index_cap)
            index_rebuild(m, m->index_cap * 2);
        else {
            uint32_t mask = m->index_cap - 1, k = key_hash(key) & mask;
            while (m->index[k])
                k = (k + 1) & mask;
            m->index[k] = m->n;
        }
    } else if (m->n > 8)
        index_rebuild(m, 32);
    if (key_is_index(key))
        o->h.flags |= OBJ_HAS_INDEX;
    return p;
}

void prop_remove(struct propmap *m, struct prop *p)
{
    p->key = JS_EMPTY;
    p->value = JS_UNDEFINED;
    m->live--;
    if (m->live < m->n / 2 && m->n >= 32)
        compact(m);
}

void propmap_free(struct propmap *m)
{
    free(m->e);
    free(m->index);
    m->e = 0;
    m->index = 0;
}

/* --- objects -------------------------------------------------------------- */

struct jsobject *obj_new(struct js *J, struct jsobject *proto, int cls)
{
    struct jsobject *o = gc_alloc(J, CELL_OBJECT, sizeof(struct jsobject));
    if (!o)
        abort();
    o->h.extra = (uint32_t)cls;
    o->h.flags = OBJ_EXTENSIBLE;
    o->proto = proto;
    return o;
}

/* A sloppy arguments object's element that is a parameter: its slot. */
static jsval *mapped(struct jsobject *o, uint32_t i)
{
    if (obj_class(o) != CLASS_ARGUMENTS || !o->u.args.map || i >= o->u.args.nmap || o->u.args.map[i] < 0)
        return 0;
    return &o->u.args.scope->vars[o->u.args.map[i]];
}

static void unmap(struct jsobject *o, uint32_t i)
{
    if (obj_class(o) == CLASS_ARGUMENTS && o->u.args.map && i < o->u.args.nmap)
        o->u.args.map[i] = -1;
}

static bool is_dense(const struct jsobject *o)
{
    int c = obj_class(o);
    return (c == CLASS_ARRAY || c == CLASS_ARGUMENTS) && !(o->h.flags & OBJ_SPARSE);
}

bool obj_is_array(struct jsobject *o)
{
    return obj_class(o) == CLASS_ARRAY;
}

int value_is_array(struct js *J, jsval v)
{
    if (!js_is_object(v))
        return 0;
    struct jsobject *o = to_obj(v);
    if (obj_class(o) == CLASS_PROXY)
        return proxy_is_array(J, o);
    return obj_class(o) == CLASS_ARRAY;
}

bool array_grow(struct js *J, struct jsobject *a, uint32_t cap)
{
    if (cap <= a->cap)
        return true;
    uint32_t nc = a->cap ? a->cap : 4;
    while (nc < cap)
        nc = nc > 0x7FFFFFFF ? cap : nc * 2;
    jsval *e = realloc(a->elems, (size_t)nc * sizeof *e);
    if (!e)
        return false;
    for (uint32_t i = a->cap; i < nc; i++)
        e[i] = JS_EMPTY;
    gc_note_external(J, (size_t)(nc - a->cap) * sizeof *e);
    a->elems = e;
    a->cap = nc;
    return true;
}

struct jsobject *array_new(struct js *J, uint32_t length)
{
    struct jsobject *a = obj_new(J, J->proto[P_ARRAY], CLASS_ARRAY);
    if (length <= (1u << 20)) {
        if (length && !array_grow(J, a, length))
            abort();
        a->length = length;
    } else {
        a->h.flags |= OBJ_SPARSE;
        a->length = length;
    }
    return a;
}

void array_make_sparse(struct js *J, struct jsobject *a)
{
    if (a->h.flags & OBJ_SPARSE)
        return;
    uint32_t n = a->length < a->cap ? a->length : a->cap;
    for (uint32_t i = 0; i < n; i++) {
        jsval *m = mapped(a, i);
        if (m && a->elems[i] != JS_EMPTY)
            a->elems[i] = *m;
        unmap(a, i);
    }
    jsval *e = a->elems;
    a->elems = 0;
    a->cap = 0;
    a->h.flags |= OBJ_SPARSE | OBJ_HAS_INDEX;
    J->gc_inhibit++;                            /* e is not traced while moving */
    for (uint32_t i = 0; i < n; i++)
        if (e[i] != JS_EMPTY)
            prop_add(J, a, str_val(atom_index(J, i)), e[i], PROP_DEFAULT);
    J->gc_inhibit--;
    free(e);
}

/* Whether no prototype of o can have an index property (so a dense
 * element write cannot meet a setter or a read-only element there). */
static bool chain_plain_for_index(const struct jsobject *o)
{
    for (const struct jsobject *p = o->proto; p; p = p->proto) {
        int c = obj_class(p);
        if ((p->h.flags & (OBJ_HAS_INDEX | OBJ_SPARSE)) || c == CLASS_STRING || c == CLASS_HOST)
            return false;
        if ((c == CLASS_ARRAY || c == CLASS_ARGUMENTS) && p->length > 0)
            return false;
    }
    return true;
}

/* An element of a dense array or arguments object set as a plain property.
 * false: the vector would get too sparse (the caller goes the slow way). */
static bool dense_put(struct js *J, struct jsobject *o, uint32_t i, jsval v)
{
    if (i < o->length) {
        if (i >= o->cap && !array_grow(J, o, i + 1))
            return false;
        o->elems[i] = v;
        return true;
    }
    if (o->h.flags & OBJ_LENGTH_FIXED)
        return false;
    if (i >= o->cap) {
        uint32_t want = i + 1;
        if (want > (1u << 20) && want > o->length * 2u + 1024)
            return false;
        if (!array_grow(J, o, want))
            return false;
    }
    for (uint32_t k = o->length; k < i; k++)
        o->elems[k] = JS_EMPTY;
    o->elems[i] = v;
    o->length = i + 1;
    return true;
}

static uint32_t string_object_len(struct jsobject *o)
{
    return str_len(to_str(o->u.prim));
}

int obj_get_own(struct js *J, struct jsobject *o, jsval key, struct pdesc *d)
{
    int cls = obj_class(o);
    if (cls == CLASS_PROXY)
        return proxy_get_own(J, o, key, d);
    if (key_is_index(key)) {
        uint32_t i = key_index(key);
        if (is_dense(o)) {
            if (i < o->length && i < o->cap && o->elems[i] != JS_EMPTY) {
                jsval *m = mapped(o, i);
                d->value = m ? *m : o->elems[i];
                d->get = d->set = JS_UNDEFINED;
                d->flags = PROP_DEFAULT;
                return 1;
            }
            return 0;
        }
        if (cls == CLASS_STRING && i < string_object_len(o)) {
            d->value = str_val(str_from_char(J, str_char_at(J, to_str(o->u.prim), i)));
            d->get = d->set = JS_UNDEFINED;
            d->flags = PROP_ENUMERABLE;
            return 1;
        }
    } else if (js_is_string(key) && to_str(key) == J->atom[A_length]) {
        if (cls == CLASS_ARRAY) {
            d->value = js_number(o->length);
            d->get = d->set = JS_UNDEFINED;
            d->flags = (o->h.flags & OBJ_LENGTH_FIXED) ? 0 : PROP_WRITABLE;
            return 1;
        }
        if (cls == CLASS_STRING) {
            d->value = js_number(string_object_len(o));
            d->get = d->set = JS_UNDEFINED;
            d->flags = 0;
            return 1;
        }
    }
    struct prop *p = prop_find(&o->props, key);
    if (!p)
        return 0;
    d->flags = p->flags;
    if (p->flags & PROP_ACCESSOR) {
        struct accessor *a = js_pointer(p->value);
        d->get = a->get;
        d->set = a->set;
        d->value = JS_UNDEFINED;
    } else {
        d->value = p->value;
        d->get = d->set = JS_UNDEFINED;
    }
    return 1;
}

jsval obj_get(struct js *J, struct jsobject *o, jsval key, jsval receiver)
{
    bool index = key_is_index(key);
    for (struct jsobject *cur = o; cur; cur = cur->proto) {
        if (obj_class(cur) == CLASS_PROXY)
            return proxy_get(J, cur, key, receiver);
        if (index && is_dense(cur)) {
            uint32_t i = key_index(key);
            if (i < cur->length && i < cur->cap && cur->elems[i] != JS_EMPTY) {
                jsval *m = mapped(cur, i);
                return m ? *m : cur->elems[i];
            }
            continue;
        }
        int cls = obj_class(cur);
        if (cls == CLASS_OBJECT || cls == CLASS_FUNCTION || cls == CLASS_NATIVE) {
            struct prop *p = prop_find(&cur->props, key);
            if (!p)
                continue;
            if (!(p->flags & PROP_ACCESSOR))
                return p->value;
            struct accessor *a = js_pointer(p->value);
            if (a->get == JS_UNDEFINED)
                return JS_UNDEFINED;
            return vm_call(J, a->get, receiver, 0, 0, JS_UNDEFINED);
        }
        struct pdesc d;
        int have = obj_get_own(J, cur, key, &d);
        if (have < 0)
            return JS_EXCEPTION;
        if (have) {
            if (!(d.flags & PROP_ACCESSOR))
                return d.value;
            if (d.get == JS_UNDEFINED)
                return JS_UNDEFINED;
            return vm_call(J, d.get, receiver, 0, 0, JS_UNDEFINED);
        }
    }
    return JS_UNDEFINED;
}

static jsval readonly_fail(struct js *J, bool strict, jsval key)
{
    if (!strict)
        return JS_FALSE;
    char *k = js_describe(J, key);
    throw_type(J, "Attempted to assign to readonly property '%s'.", k);
    free(k);
    return JS_EXCEPTION;
}

int obj_set(struct js *J, struct jsobject *o, jsval key, jsval value, jsval receiver, bool strict)
{
    if (obj_class(o) == CLASS_PROXY)
        return proxy_set(J, o, key, value, receiver, strict);
    /* the common cases first: a dense element, an own writable data property */
    if (receiver == obj_val(o)) {
        if (key_is_index(key) && is_dense(o)) {
            uint32_t i = key_index(key);
            if (i < o->length && i < o->cap && o->elems[i] != JS_EMPTY) {
                o->elems[i] = value;
                jsval *m = mapped(o, i);
                if (m)
                    *m = value;
                return 1;
            }
            if ((o->h.flags & OBJ_EXTENSIBLE) && chain_plain_for_index(o) && dense_put(J, o, i, value))
                return 1;
        } else if (!key_is_index(key)) {
            struct prop *p = prop_find(&o->props, key);
            if (p && (p->flags & (PROP_ACCESSOR | PROP_WRITABLE)) == PROP_WRITABLE) {
                p->value = value;
                return 1;
            }
        }
    }
    struct pdesc d;
    struct jsobject *cur = o;
    int found = 0;
    for (; cur; cur = cur->proto) {
        if (obj_class(cur) == CLASS_PROXY)      /* (64r) a proxy up the chain decides */
            return proxy_set(J, cur, key, value, receiver, strict);
        found = obj_get_own(J, cur, key, &d);
        if (found < 0)
            return -1;
        if (found)
            break;
    }
    if (!found) {
        d.flags = PROP_DEFAULT;
        d.value = JS_UNDEFINED;
    }
    if (d.flags & PROP_ACCESSOR) {
        if (d.set == JS_UNDEFINED)
            return readonly_fail(J, strict, key) == JS_EXCEPTION ? -1 : 0;
        jsval r = vm_call(J, d.set, receiver, 1, &value, JS_UNDEFINED);
        return r == JS_EXCEPTION ? -1 : 1;
    }
    if (!(d.flags & PROP_WRITABLE))
        return readonly_fail(J, strict, key) == JS_EXCEPTION ? -1 : 0;
    if (!js_is_object(receiver))
        return readonly_fail(J, strict, key) == JS_EXCEPTION ? -1 : 0;
    struct jsobject *r = to_obj(receiver);
    struct pdesc ex;
    int has = obj_get_own(J, r, key, &ex);
    if (has < 0)
        return -1;
    if (has) {
        if ((ex.flags & PROP_ACCESSOR) || !(ex.flags & PROP_WRITABLE))
            return readonly_fail(J, strict, key) == JS_EXCEPTION ? -1 : 0;
        struct pdesc nd = { .value = value, .has = DESC_VALUE };
        return obj_define(J, r, key, &nd, strict);
    }
    struct pdesc nd = { .value = value, .flags = PROP_DEFAULT,
                        .has = DESC_VALUE | DESC_WRITABLE | DESC_ENUMERABLE | DESC_CONFIGURABLE };
    return obj_define(J, r, key, &nd, strict);
}

static int define_fail(struct js *J, bool throw_on_fail, jsval key)
{
    if (!throw_on_fail)
        return 0;
    char *k = js_describe(J, key);
    throw_type(J, "Attempting to change the property '%s' in a way it does not allow", k);
    free(k);
    return -1;
}

int array_set_length(struct js *J, struct jsobject *a, uint32_t len, bool strict)
{
    if (len == a->length)
        return 1;
    if (a->h.flags & OBJ_LENGTH_FIXED)
        return define_fail(J, strict, str_val(J->atom[A_length]));
    if (!(a->h.flags & OBJ_SPARSE)) {
        if (len < a->length) {
            for (uint32_t i = len; i < a->length && i < a->cap; i++)
                a->elems[i] = JS_EMPTY;
            a->length = len;
            return 1;
        }
        if (len <= a->cap || len <= (1u << 20) || len <= a->length * 2u) {
            if (!array_grow(J, a, len))
                abort();
            for (uint32_t i = a->length; i < len; i++)
                a->elems[i] = JS_EMPTY;
            a->length = len;
            return 1;
        }
        array_make_sparse(J, a);
    }
    if (len < a->length) {
        /* delete the index properties at len and above, from the top */
        for (;;) {
            struct prop *top = 0;
            for (uint32_t i = 0; i < a->props.n; i++) {
                struct prop *p = &a->props.e[i];
                if (key_is_index(p->key) && key_index(p->key) >= len &&
                    (!top || key_index(p->key) > key_index(top->key)))
                    top = p;
            }
            if (!top)
                break;
            if (!(top->flags & PROP_CONFIGURABLE)) {
                a->length = key_index(top->key) + 1;
                return define_fail(J, strict, str_val(J->atom[A_length]));
            }
            prop_remove(&a->props, top);
        }
    }
    a->length = len;
    return 1;
}

static void desc_complete(struct pdesc *d)
{
    if (!(d->has & (DESC_GET | DESC_SET))) {
        if (!(d->has & DESC_VALUE))
            d->value = JS_UNDEFINED;
    } else {
        if (!(d->has & DESC_GET))
            d->get = JS_UNDEFINED;
        if (!(d->has & DESC_SET))
            d->set = JS_UNDEFINED;
    }
}

static jsval make_accessor(struct js *J, jsval get, jsval set)
{
    struct accessor *a = gc_alloc(J, CELL_ACCESSOR, sizeof *a);
    if (!a)
        abort();
    a->get = get;
    a->set = set;
    return cell_val(a);
}

static int ordinary_define(struct js *J, struct jsobject *o, jsval key, const struct pdesc *din, bool throw_on_fail)
{
    struct pdesc d = *din;
    struct pdesc cur;
    int has = obj_get_own(J, o, key, &cur);
    bool accessor = (d.has & (DESC_GET | DESC_SET)) != 0;
    if (!has) {
        if (!(o->h.flags & OBJ_EXTENSIBLE))
            return define_fail(J, throw_on_fail, key);
        uint32_t flags = 0;
        if ((d.has & DESC_ENUMERABLE) && (d.flags & PROP_ENUMERABLE))
            flags |= PROP_ENUMERABLE;
        if ((d.has & DESC_CONFIGURABLE) && (d.flags & PROP_CONFIGURABLE))
            flags |= PROP_CONFIGURABLE;
        desc_complete(&d);
        if (accessor) {
            prop_add(J, o, key, make_accessor(J, d.get, d.set), flags | PROP_ACCESSOR);
        } else {
            if ((d.has & DESC_WRITABLE) && (d.flags & PROP_WRITABLE))
                flags |= PROP_WRITABLE;
            if (key_is_index(key) && is_dense(o) && flags == PROP_DEFAULT &&
                dense_put(J, o, key_index(key), d.value))
                return 1;
            if (key_is_index(key) && is_dense(o))
                array_make_sparse(J, o);
            prop_add(J, o, key, d.value, flags);
        }
        return 1;
    }
    /* ValidateAndApplyPropertyDescriptor */
    bool cur_acc = (cur.flags & PROP_ACCESSOR) != 0;
    if (!(cur.flags & PROP_CONFIGURABLE)) {
        if ((d.has & DESC_CONFIGURABLE) && (d.flags & PROP_CONFIGURABLE))
            return define_fail(J, throw_on_fail, key);
        if ((d.has & DESC_ENUMERABLE) && ((d.flags & PROP_ENUMERABLE) != (cur.flags & PROP_ENUMERABLE)))
            return define_fail(J, throw_on_fail, key);
        bool generic = !(d.has & (DESC_VALUE | DESC_WRITABLE | DESC_GET | DESC_SET));
        if (!generic) {
            if (accessor != cur_acc)
                return define_fail(J, throw_on_fail, key);
            if (cur_acc) {
                if (((d.has & DESC_GET) && d.get != cur.get) || ((d.has & DESC_SET) && d.set != cur.set))
                    return define_fail(J, throw_on_fail, key);
            } else if (!(cur.flags & PROP_WRITABLE)) {
                if ((d.has & DESC_WRITABLE) && (d.flags & PROP_WRITABLE))
                    return define_fail(J, throw_on_fail, key);
                if ((d.has & DESC_VALUE) && !js_same_value(J, d.value, cur.value, false))
                    return define_fail(J, throw_on_fail, key);
            }
        }
    }
    /* apply */
    uint32_t flags = cur.flags & (PROP_ENUMERABLE | PROP_CONFIGURABLE | PROP_WRITABLE);
    jsval value = cur.value, get = cur.get, set = cur.set;
    bool now_acc = cur_acc;
    if (accessor && !cur_acc) {
        now_acc = true;
        flags &= ~PROP_WRITABLE;
        get = set = JS_UNDEFINED;
    } else if ((d.has & (DESC_VALUE | DESC_WRITABLE)) && cur_acc) {
        now_acc = false;
        value = JS_UNDEFINED;
        flags &= ~PROP_WRITABLE;
    }
    if (d.has & DESC_VALUE)
        value = d.value;
    if (d.has & DESC_GET)
        get = d.get;
    if (d.has & DESC_SET)
        set = d.set;
    if (d.has & DESC_WRITABLE)
        flags = (flags & ~PROP_WRITABLE) | (d.flags & PROP_WRITABLE);
    if (d.has & DESC_ENUMERABLE)
        flags = (flags & ~PROP_ENUMERABLE) | (d.flags & PROP_ENUMERABLE);
    if (d.has & DESC_CONFIGURABLE)
        flags = (flags & ~PROP_CONFIGURABLE) | (d.flags & PROP_CONFIGURABLE);
    if (key_is_index(key) && is_dense(o)) {
        jsval *m = mapped(o, key_index(key));
        if (m && !now_acc && (d.has & DESC_VALUE))
            *m = value;
        if (!now_acc && flags == PROP_DEFAULT) {
            o->elems[key_index(key)] = value;
            return 1;
        }
        array_make_sparse(J, o);
    }
    if (obj_class(o) == CLASS_STRING && key_is_index(key) && key_index(key) < string_object_len(o))
        return 1;                               /* (only compatible changes reach here) */
    struct prop *p = prop_find(&o->props, key);
    if (!p)
        return 1;
    if (now_acc) {
        if (p->flags & PROP_ACCESSOR) {
            struct accessor *a = js_pointer(p->value);
            a->get = get;
            a->set = set;
        } else
            p->value = make_accessor(J, get, set);
        p = prop_find(&o->props, key);
        p->flags = flags | PROP_ACCESSOR;
    } else {
        p->value = value;
        p->flags = flags;
    }
    return 1;
}

int obj_define(struct js *J, struct jsobject *o, jsval key, const struct pdesc *d, bool throw_on_fail)
{
    if (obj_class(o) == CLASS_PROXY)
        return proxy_define(J, o, key, d, throw_on_fail);
    if (obj_class(o) == CLASS_ARRAY) {
        if (js_is_string(key) && to_str(key) == J->atom[A_length]) {
            if (d->has & (DESC_GET | DESC_SET))
                return define_fail(J, throw_on_fail, key);
            if ((d->has & DESC_CONFIGURABLE) && (d->flags & PROP_CONFIGURABLE))
                return define_fail(J, throw_on_fail, key);
            if ((d->has & DESC_ENUMERABLE) && (d->flags & PROP_ENUMERABLE))
                return define_fail(J, throw_on_fail, key);
            if (d->has & DESC_VALUE) {
                bool threw = false;
                double n = js_to_number(J, d->value, &threw);
                if (threw)
                    return -1;
                uint32_t len = js_to_uint32(n);
                if ((double)len != n) {
                    throw_range(J, "Invalid array length");
                    return -1;
                }
                if (len != o->length && (o->h.flags & OBJ_LENGTH_FIXED))
                    return define_fail(J, throw_on_fail, key);
                int r = array_set_length(J, o, len, throw_on_fail);
                if (r <= 0)
                    return r;
            }
            if ((d->has & DESC_WRITABLE) && !(d->flags & PROP_WRITABLE))
                o->h.flags |= OBJ_LENGTH_FIXED;
            else if ((d->has & DESC_WRITABLE) && (o->h.flags & OBJ_LENGTH_FIXED))
                return define_fail(J, throw_on_fail, key);
            return 1;
        }
        if (key_is_index(key)) {
            uint32_t i = key_index(key);
            if (i >= o->length && (o->h.flags & OBJ_LENGTH_FIXED))
                return define_fail(J, throw_on_fail, key);
            int r = ordinary_define(J, o, key, d, throw_on_fail);
            if (r > 0 && i >= o->length)
                o->length = i + 1;
            return r;
        }
    } else if (obj_class(o) == CLASS_STRING) {
        if (js_is_string(key) && to_str(key) == J->atom[A_length]) {
            struct pdesc cur;
            obj_get_own(J, o, key, &cur);
            if (((d->has & DESC_VALUE) && !js_same_value(J, d->value, cur.value, false)) ||
                ((d->has & DESC_WRITABLE) && (d->flags & PROP_WRITABLE)) ||
                ((d->has & DESC_CONFIGURABLE) && (d->flags & PROP_CONFIGURABLE)) ||
                ((d->has & DESC_ENUMERABLE) && (d->flags & PROP_ENUMERABLE)) || (d->has & (DESC_GET | DESC_SET)))
                return define_fail(J, throw_on_fail, key);
            return 1;
        }
    }
    return ordinary_define(J, o, key, d, throw_on_fail);
}

int obj_has(struct js *J, struct jsobject *o, jsval key)
{
    struct pdesc d;
    for (struct jsobject *cur = o; cur; cur = cur->proto) {
        if (obj_class(cur) == CLASS_PROXY)
            return proxy_has(J, cur, key);
        int have = obj_get_own(J, cur, key, &d);
        if (have)
            return have;
    }
    return 0;
}

int obj_delete(struct js *J, struct jsobject *o, jsval key, bool strict)
{
    if (obj_class(o) == CLASS_PROXY)
        return proxy_delete(J, o, key, strict);
    struct pdesc d;
    if (!obj_get_own(J, o, key, &d))
        return 1;
    if (!(d.flags & PROP_CONFIGURABLE)) {
        if (!strict)
            return 0;
        char *k = js_describe(J, key);
        throw_type(J, "Unable to delete property '%s'.", k);
        free(k);
        return -1;
    }
    if (key_is_index(key) && is_dense(o)) {
        o->elems[key_index(key)] = JS_EMPTY;
        unmap(o, key_index(key));
        return 1;
    }
    struct prop *p = prop_find(&o->props, key);
    if (p)
        prop_remove(&o->props, p);
    return 1;
}

static int cmp_index(const void *a, const void *b)
{
    uint32_t x = key_index(*(const jsval *)a), y = key_index(*(const jsval *)b);
    return x < y ? -1 : x > y;
}

/* (64r) A proxy's keys: the trap's, then only the kinds asked for (and
 * only enumerable ones: its getOwnPropertyDescriptor asked for each). On
 * a throw an empty array, so a caller may still free it. */
static int proxy_keys(struct js *J, struct jsobject *o, int flags, jsval **keysp, uint32_t *np)
{
    jsval *keys;
    uint32_t n, k = 0;
    if (proxy_own_keys(J, o, &keys, &n) < 0) {
        *keysp = malloc(sizeof **keysp);
        if (!*keysp)
            abort();
        *np = 0;
        return -1;
    }
    js_root_push(J, keys, n);
    int r = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!(flags & (js_is_symbol(keys[i]) ? 2 : 1)))
            continue;
        if (flags & 4) {
            struct pdesc d;
            int have = obj_get_own(J, o, keys[i], &d);
            if (have < 0) {
                r = -1;
                break;
            }
            if (!have || !(d.flags & PROP_ENUMERABLE))
                continue;
        }
        keys[k++] = keys[i];
    }
    js_root_pop(J);
    *keysp = keys;
    *np = r < 0 ? 0 : k;
    return r;
}

int obj_own_keys(struct js *J, struct jsobject *o, int flags, jsval **keysp, uint32_t *np)
{
    if (obj_class(o) == CLASS_PROXY)
        return proxy_keys(J, o, flags, keysp, np);
    bool want_str = flags & 1, want_sym = flags & 2, only_enum = flags & 4;
    uint32_t cap = o->props.live + 8, n = 0;
    uint32_t dense_n = 0, str_n = 0;
    if (is_dense(o))
        dense_n = o->length < o->cap ? o->length : o->cap;
    if (obj_class(o) == CLASS_STRING)
        str_n = string_object_len(o);
    cap += dense_n + str_n;
    jsval *keys = malloc((size_t)cap * sizeof *keys);
    if (!keys)
        abort();
    J->gc_inhibit++;                            /* (index atoms made below are only in keys) */
    if (want_str) {
        for (uint32_t i = 0; i < str_n; i++)
            keys[n++] = str_val(atom_index(J, i));
        for (uint32_t i = 0; i < dense_n; i++)
            if (o->elems[i] != JS_EMPTY)
                keys[n++] = str_val(atom_index(J, i));
        uint32_t first = n;
        for (uint32_t i = 0; i < o->props.n; i++) {
            struct prop *p = &o->props.e[i];
            if (p->key != JS_EMPTY && key_is_index(p->key) && (!only_enum || (p->flags & PROP_ENUMERABLE)))
                keys[n++] = p->key;
        }
        if (n - first > 1)
            qsort(keys + first, n - first, sizeof *keys, cmp_index);
        if (!only_enum && (obj_class(o) == CLASS_ARRAY || obj_class(o) == CLASS_STRING))
            keys[n++] = str_val(J->atom[A_length]);
        for (uint32_t i = 0; i < o->props.n; i++) {
            struct prop *p = &o->props.e[i];
            if (p->key != JS_EMPTY && js_is_string(p->key) && !key_is_index(p->key) &&
                (!only_enum || (p->flags & PROP_ENUMERABLE)))
                keys[n++] = p->key;
        }
    }
    if (want_sym)
        for (uint32_t i = 0; i < o->props.n; i++) {
            struct prop *p = &o->props.e[i];
            if (p->key != JS_EMPTY && js_is_symbol(p->key) && !(to_sym(p->key)->h.flags & 2) &&
                (!only_enum || (p->flags & PROP_ENUMERABLE)))
                keys[n++] = p->key;
        }
    J->gc_inhibit--;
    *keysp = keys;
    *np = n;
    return 0;
}

void obj_define_value(struct js *J, struct jsobject *o, jsval key, jsval v, uint32_t flags)
{
    struct pdesc d = { .value = v, .flags = flags,
                       .has = DESC_VALUE | DESC_WRITABLE | DESC_ENUMERABLE | DESC_CONFIGURABLE };
    if (!key_is_index(key) && obj_class(o) != CLASS_ARRAY && !prop_find(&o->props, key)) {
        prop_add(J, o, key, v, flags);
        return;
    }
    obj_define(J, o, key, &d, false);
}

void obj_define_accessor(struct js *J, struct jsobject *o, jsval key, jsval get, jsval set, uint32_t flags)
{
    struct pdesc d = { .get = get, .set = set, .flags = flags,
                       .has = DESC_GET | DESC_SET | DESC_ENUMERABLE | DESC_CONFIGURABLE };
    obj_define(J, o, key, &d, false);
}

int obj_prevent_extensions(struct js *J, struct jsobject *o)
{
    if (obj_class(o) == CLASS_PROXY)
        return proxy_prevent_extensions(J, o);
    o->h.flags &= ~OBJ_EXTENSIBLE;
    return 1;
}

int obj_is_extensible(struct js *J, struct jsobject *o)
{
    if (obj_class(o) == CLASS_PROXY)
        return proxy_is_extensible(J, o);
    return (o->h.flags & OBJ_EXTENSIBLE) != 0;
}

int obj_get_proto(struct js *J, struct jsobject *o, struct jsobject **proto)
{
    if (obj_class(o) == CLASS_PROXY)
        return proxy_get_proto(J, o, proto);
    *proto = o->proto;
    return 0;
}

/* (64r) SetIntegrityLevel and TestIntegrityLevel through the internal
 * methods, for a proxy (its traps see every step). */
static int proxy_set_level(struct js *J, struct jsobject *o, bool frozen)
{
    int r = obj_prevent_extensions(J, o);
    if (r <= 0)
        return r;
    jsval *keys;
    uint32_t n;
    if (obj_own_keys(J, o, 3, &keys, &n) < 0) {
        free(keys);
        return -1;
    }
    js_root_push(J, keys, n);
    for (uint32_t i = 0; i < n && r > 0; i++) {
        struct pdesc d = { .has = DESC_CONFIGURABLE };
        if (frozen) {
            struct pdesc cur;
            int have = obj_get_own(J, o, keys[i], &cur);
            if (have < 0) {
                r = -1;
                break;
            }
            if (!have)
                continue;
            if (!(cur.flags & PROP_ACCESSOR))
                d.has |= DESC_WRITABLE;
        }
        if (obj_define(J, o, keys[i], &d, true) < 0)
            r = -1;
    }
    js_root_pop(J);
    free(keys);
    return r;
}

static int proxy_test_level(struct js *J, struct jsobject *o, bool frozen)
{
    int ext = obj_is_extensible(J, o);
    if (ext != 0)
        return ext < 0 ? -1 : 0;
    jsval *keys;
    uint32_t n;
    if (obj_own_keys(J, o, 3, &keys, &n) < 0) {
        free(keys);
        return -1;
    }
    js_root_push(J, keys, n);
    int r = 1;
    for (uint32_t i = 0; i < n && r > 0; i++) {
        struct pdesc d;
        int have = obj_get_own(J, o, keys[i], &d);
        if (have < 0)
            r = -1;
        else if (have && ((d.flags & PROP_CONFIGURABLE) ||
                          (frozen && !(d.flags & PROP_ACCESSOR) && (d.flags & PROP_WRITABLE))))
            r = 0;
    }
    js_root_pop(J);
    free(keys);
    return r;
}

int obj_set_level(struct js *J, struct jsobject *o, bool frozen)
{
    if (obj_class(o) == CLASS_PROXY)
        return proxy_set_level(J, o, frozen);
    o->h.flags &= ~OBJ_EXTENSIBLE;
    if (is_dense(o) && o->length > 0)
        array_make_sparse(J, o);
    if (frozen && obj_class(o) == CLASS_ARRAY)
        o->h.flags |= OBJ_LENGTH_FIXED;
    for (uint32_t i = 0; i < o->props.n; i++) {
        struct prop *p = &o->props.e[i];
        if (p->key == JS_EMPTY)
            continue;
        p->flags &= ~PROP_CONFIGURABLE;
        if (frozen && !(p->flags & PROP_ACCESSOR))
            p->flags &= ~PROP_WRITABLE;
    }
    return 1;
}

int obj_test_level(struct js *J, struct jsobject *o, bool frozen)
{
    if (obj_class(o) == CLASS_PROXY)
        return proxy_test_level(J, o, frozen);
    if (o->h.flags & OBJ_EXTENSIBLE)
        return false;
    if (is_dense(o)) {
        uint32_t n = o->length < o->cap ? o->length : o->cap;
        for (uint32_t i = 0; i < n; i++)
            if (o->elems[i] != JS_EMPTY)
                return false;
    }
    if (frozen && obj_class(o) == CLASS_ARRAY && !(o->h.flags & OBJ_LENGTH_FIXED))
        return false;
    if (obj_class(o) == CLASS_STRING && frozen == false)
        (void)0;
    for (uint32_t i = 0; i < o->props.n; i++) {
        struct prop *p = &o->props.e[i];
        if (p->key == JS_EMPTY)
            continue;
        if (p->flags & PROP_CONFIGURABLE)
            return false;
        if (frozen && !(p->flags & PROP_ACCESSOR) && (p->flags & PROP_WRITABLE))
            return false;
    }
    (void)J;
    return true;
}

int obj_set_proto(struct js *J, struct jsobject *o, struct jsobject *proto)
{
    if (obj_class(o) == CLASS_PROXY)
        return proxy_set_proto(J, o, proto);
    if (o->proto == proto)
        return 1;
    if (!(o->h.flags & OBJ_EXTENSIBLE))
        return 0;
    for (struct jsobject *p = proto; p && obj_class(p) != CLASS_PROXY; p = p->proto)
        if (p == o)
            return 0;
    o->proto = proto;
    return 1;
}

/* --- keys and conversions ----------------------------------------------- */

jsval key_cstr(struct js *J, const char *ascii)
{
    return str_val(atom_cstr(J, ascii));
}

jsval key_from_value(struct js *J, jsval v)
{
    if (js_is_string(v))
        return str_val(atom(J, to_str(v)));
    if (js_is_number(v)) {
        double d = js_number_value(v);
        if (d >= 0 && d <= 4294967294.0 && d == (double)(uint32_t)d)
            return str_val(atom_index(J, (uint32_t)d));
        return str_val(atom(J, number_to_string(J, d, 10)));
    }
    if (js_is_symbol(v))
        return v;
    if (js_is_object(v)) {
        jsval p = js_to_primitive(J, v, 2);
        if (p == JS_EXCEPTION)
            return JS_EXCEPTION;
        return key_from_value(J, p);
    }
    struct jsstring *s = js_to_str(J, v);
    if (!s)
        return JS_EXCEPTION;
    return str_val(atom(J, s));
}

jsval js_to_primitive(struct js *J, jsval v, int hint)
{
    if (!js_is_object(v))
        return v;
    jsval ex = js_get_method(J, v, J->symbol[SYM_TO_PRIMITIVE]);
    if (ex == JS_EXCEPTION)
        return JS_EXCEPTION;
    if (ex != JS_UNDEFINED) {
        jsval h = str_val(J->atom[hint == 2 ? A_string : hint == 1 ? A_number : A_default]);
        jsval r = vm_call(J, ex, v, 1, &h, JS_UNDEFINED);
        if (r == JS_EXCEPTION)
            return r;
        if (js_is_object(r))
            return throw_type(J, "Symbol.toPrimitive returned an object");
        return r;
    }
    int order[2] = { A_valueOf, A_toString };
    if (hint == 2)
        order[0] = A_toString, order[1] = A_valueOf;
    for (int i = 0; i < 2; i++) {
        jsval m = obj_get(J, to_obj(v), str_val(J->atom[order[i]]), v);
        if (m == JS_EXCEPTION)
            return m;
        if (js_is_callable(m)) {
            jsval r = vm_call(J, m, v, 0, 0, JS_UNDEFINED);
            if (r == JS_EXCEPTION || !js_is_object(r))
                return r;
        }
    }
    return throw_type(J, "No default value");
}

struct jsstring *js_to_str(struct js *J, jsval v)
{
    switch (js_tag(v)) {
    case JS_T_STRING:
        return to_str(v);
    case JS_T_NUMBER: {
        char buf[32];
        double d = js_number_value(v);
        if (d >= 0 && d < 10 && d == (int)d)
            return str_from_char(J, '0' + (uint32_t)d);
        number_to_ascii(d, buf);
        return str_from_ascii(J, buf);
    }
    case JS_T_UNDEFINED:
        return J->atom[A_undefined];
    case JS_T_NULL:
        return J->atom[A_null];
    case JS_T_BOOL:
        return J->atom[v == JS_TRUE ? A_true : A_false];
    case JS_T_SYMBOL:
        throw_type(J, "Cannot convert a symbol to a string");
        return 0;
    case JS_T_OBJECT: {
        jsval p = js_to_primitive(J, v, 2);
        if (p == JS_EXCEPTION)
            return 0;
        return js_to_str(J, p);
    }
    }
    return J->atom[A_empty];
}

jsval js_to_string(struct js *J, jsval v)
{
    struct jsstring *s = js_to_str(J, v);
    return s ? str_val(s) : JS_EXCEPTION;
}

struct jsstring *js_to_str_noexcept(struct js *J, jsval v)
{
    if (js_is_symbol(v)) {
        struct sbuf b = { 0 };
        sb_ascii(&b, "Symbol(");
        if (to_sym(v)->description)
            sb_str(J, &b, to_sym(v)->description);
        sb_ascii(&b, ")");
        return sb_finish(J, &b);
    }
    if (js_is_object(v)) {
        jsval saved = J->exception;
        bool had = J->has_exception;
        struct jsstring *s = js_to_str(J, v);
        J->exception = saved;
        J->has_exception = had;
        return s ? s : str_from_ascii(J, "[object]");
    }
    return js_to_str(J, v);
}

double js_to_number(struct js *J, jsval v, bool *threw)
{
    switch (js_tag(v)) {
    case JS_T_NUMBER:
        return js_number_value(v);
    case JS_T_UNDEFINED:
        return NAN;
    case JS_T_NULL:
        return 0;
    case JS_T_BOOL:
        return v == JS_TRUE ? 1 : 0;
    case JS_T_STRING: {
        struct jsstring *f = str_flat(J, to_str(v));
        return string_to_number(f->chars, str_len(f));
    }
    case JS_T_SYMBOL:
        throw_type(J, "Cannot convert a symbol to a number");
        *threw = true;
        return NAN;
    case JS_T_OBJECT: {
        jsval p = js_to_primitive(J, v, 1);
        if (p == JS_EXCEPTION) {
            *threw = true;
            return NAN;
        }
        return js_to_number(J, p, threw);
    }
    }
    return NAN;
}

jsval js_to_numeric(struct js *J, jsval v)
{
    if (js_is_number(v))
        return v;
    bool threw = false;
    double d = js_to_number(J, v, &threw);
    return threw ? JS_EXCEPTION : js_number(d);
}

bool js_to_boolean(jsval v)
{
    switch (js_tag(v)) {
    case JS_T_NUMBER: {
        double d = js_number_value(v);
        return d == d && d != 0;
    }
    case JS_T_BOOL:
        return v == JS_TRUE;
    case JS_T_STRING:
        return str_len(to_str(v)) != 0;
    case JS_T_OBJECT: case JS_T_SYMBOL:
        return true;
    }
    return false;
}

double js_to_integer(struct js *J, jsval v, bool *threw)
{
    double d = js_is_number(v) ? js_number_value(v) : js_to_number(J, v, threw);
    if (d != d)
        return 0;
    if (isinf(d))
        return d;
    return trunc(d) + 0.0;
}

uint32_t js_to_uint32(double d)
{
    if (d != d || isinf(d))
        return 0;
    if (d >= 0 && d < 4294967296.0)
        return (uint32_t)d;
    double t = fmod(trunc(d), 4294967296.0);
    if (t < 0)
        t += 4294967296.0;
    return (uint32_t)t;
}

int32_t js_to_int32(double d)
{
    if (d >= -2147483648.0 && d <= 2147483647.0)
        return (int32_t)d;
    return (int32_t)js_to_uint32(d);
}

int js_to_length(struct js *J, jsval v, double *len)
{
    bool threw = false;
    double d = js_to_integer(J, v, &threw);
    if (threw)
        return -1;
    if (d <= 0)
        d = 0;
    if (d > 9007199254740991.0)
        d = 9007199254740991.0;
    *len = d;
    return 0;
}

struct jsobject *js_to_object(struct js *J, jsval v)
{
    struct jsobject *o;
    switch (js_tag(v)) {
    case JS_T_OBJECT:
        return to_obj(v);
    case JS_T_UNDEFINED: case JS_T_NULL:
        throw_type(J, "Cannot convert %s to object", v == JS_NULL ? "null" : "undefined");
        return 0;
    case JS_T_BOOL:
        o = obj_new(J, J->proto[P_BOOLEAN], CLASS_BOOLEAN);
        break;
    case JS_T_STRING:
        o = obj_new(J, J->proto[P_STRING], CLASS_STRING);
        break;
    case JS_T_SYMBOL:
        o = obj_new(J, J->proto[P_SYMBOL], CLASS_SYMBOL);
        break;
    default:
        o = obj_new(J, J->proto[P_NUMBER], CLASS_NUMBER);
        break;
    }
    o->u.prim = v;
    return o;
}

/* --- equality, typeof, instanceof ----------------------------------------- */

bool js_strict_equal(struct js *J, jsval a, jsval b)
{
    if (js_is_number(a) && js_is_number(b))
        return js_number_value(a) == js_number_value(b);
    if (js_is_string(a) && js_is_string(b))
        return str_equal(J, to_str(a), to_str(b));
    return a == b;
}

bool js_same_value(struct js *J, jsval a, jsval b, bool zero)
{
    if (js_is_number(a) && js_is_number(b)) {
        double x = js_number_value(a), y = js_number_value(b);
        if (x != x)
            return y != y;
        if (x == 0 && y == 0 && !zero)
            return signbit(x) == signbit(y);
        return x == y;
    }
    return js_strict_equal(J, a, b);
}

int js_loose_equal(struct js *J, jsval a, jsval b)
{
    for (;;) {
        int ta = js_tag(a), tb = js_tag(b);
        if (ta == tb)
            return js_strict_equal(J, a, b);
        if (js_is_nullish(a) && js_is_nullish(b))
            return 1;
        if (js_is_nullish(a) || js_is_nullish(b))
            return 0;
        if (ta == JS_T_NUMBER && tb == JS_T_STRING) {
            bool t = false;
            return js_number_value(a) == js_to_number(J, b, &t);
        }
        if (ta == JS_T_STRING && tb == JS_T_NUMBER) {
            bool t = false;
            return js_to_number(J, a, &t) == js_number_value(b);
        }
        if (ta == JS_T_BOOL) {
            a = js_number(a == JS_TRUE);
            continue;
        }
        if (tb == JS_T_BOOL) {
            b = js_number(b == JS_TRUE);
            continue;
        }
        if (ta == JS_T_OBJECT && tb != JS_T_OBJECT) {
            a = js_to_primitive(J, a, 0);
            if (a == JS_EXCEPTION)
                return -1;
            continue;
        }
        if (tb == JS_T_OBJECT && ta != JS_T_OBJECT) {
            b = js_to_primitive(J, b, 0);
            if (b == JS_EXCEPTION)
                return -1;
            continue;
        }
        return 0;
    }
}

bool js_is_callable(jsval v)
{
    return js_is_object(v) && (to_obj(v)->h.flags & OBJ_CALLABLE);
}

bool js_is_constructor(jsval v)
{
    return js_is_object(v) && (to_obj(v)->h.flags & OBJ_CONSTRUCTOR);
}

struct jsstring *js_typeof(struct js *J, jsval v)
{
    switch (js_tag(v)) {
    case JS_T_NUMBER: return J->atom[A_number];
    case JS_T_STRING: return J->atom[A_string];
    case JS_T_BOOL: return J->atom[A_boolean];
    case JS_T_SYMBOL: return J->atom[A_symbol];
    case JS_T_UNDEFINED: return J->atom[A_undefined];
    case JS_T_NULL: return J->atom[A_object];
    case JS_T_OBJECT: return J->atom[js_is_callable(v) ? A_function : A_object];
    }
    return J->atom[A_undefined];
}

int js_ordinary_has_instance(struct js *J, jsval f, jsval v)
{
    if (!js_is_callable(f))
        return 0;
    if (obj_class(to_obj(f)) == CLASS_BOUND)
        return js_instanceof(J, v, to_obj(f)->u.bound.target) == JS_TRUE ? 1
               : J->has_exception ? -1 : 0;
    if (!js_is_object(v))
        return 0;
    jsval p = obj_get(J, to_obj(f), str_val(J->atom[A_prototype]), f);
    if (p == JS_EXCEPTION)
        return -1;
    if (!js_is_object(p)) {
        throw_type(J, "instanceof called on an object with an invalid prototype property");
        return -1;
    }
    struct jsobject *o = to_obj(v);
    for (;;) {
        if (obj_get_proto(J, o, &o) < 0)        /* (64r: a proxy's trap) */
            return -1;
        if (!o)
            return 0;
        if (o == to_obj(p))
            return 1;
    }
}

jsval js_instanceof(struct js *J, jsval v, jsval target)
{
    if (!js_is_object(target))
        return throw_type(J, "Right side of 'instanceof' is not an object");
    jsval h = js_get_method(J, target, J->symbol[SYM_HAS_INSTANCE]);
    if (h == JS_EXCEPTION)
        return h;
    if (h != JS_UNDEFINED) {
        jsval r = vm_call(J, h, target, 1, &v, JS_UNDEFINED);
        if (r == JS_EXCEPTION)
            return r;
        return js_bool(js_to_boolean(r));
    }
    if (!js_is_callable(target))
        return throw_type(J, "Right side of 'instanceof' is not callable");
    int r = js_ordinary_has_instance(J, target, v);
    if (r < 0)
        return JS_EXCEPTION;
    return js_bool(r);
}

/* --- generic access on values -------------------------------------------- */

static jsval nullish_access(struct js *J, jsval v, jsval key, bool set)
{
    char *k = js_describe(J, key);
    throw_type(J, "Cannot %s properties of %s (%s '%s')", set ? "set" : "read",
               v == JS_NULL ? "null" : "undefined", set ? "setting" : "reading", k);
    free(k);
    return JS_EXCEPTION;
}

jsval js_get_value(struct js *J, jsval v, jsval key)
{
    switch (js_tag(v)) {
    case JS_T_OBJECT:
        return obj_get(J, to_obj(v), key, v);
    case JS_T_STRING: {
        struct jsstring *s = to_str(v);
        if (key_is_index(key)) {
            uint32_t i = key_index(key);
            if (i < str_len(s))
                return str_val(str_from_char(J, str_char_at(J, s, i)));
        } else if (js_is_string(key) && to_str(key) == J->atom[A_length])
            return js_number(str_len(s));
        return obj_get(J, J->proto[P_STRING], key, v);
    }
    case JS_T_NUMBER:
        return obj_get(J, J->proto[P_NUMBER], key, v);
    case JS_T_BOOL:
        return obj_get(J, J->proto[P_BOOLEAN], key, v);
    case JS_T_SYMBOL:
        return obj_get(J, J->proto[P_SYMBOL], key, v);
    }
    return nullish_access(J, v, key, false);
}

int js_set_value(struct js *J, jsval v, jsval key, jsval value, bool strict)
{
    switch (js_tag(v)) {
    case JS_T_OBJECT:
        return obj_set(J, to_obj(v), key, value, v, strict);
    case JS_T_UNDEFINED: case JS_T_NULL:
        nullish_access(J, v, key, true);
        return -1;
    }
    int proto = js_is_string(v) ? P_STRING : js_is_number(v) ? P_NUMBER : js_is_bool(v) ? P_BOOLEAN : P_SYMBOL;
    return obj_set(J, J->proto[proto], key, value, v, strict);
}

jsval js_get_cstr(struct js *J, jsval v, const char *name)
{
    return js_get_value(J, v, key_cstr(J, name));
}

jsval js_get_method(struct js *J, jsval v, jsval key)
{
    jsval f = js_get_value(J, v, key);
    if (f == JS_EXCEPTION || js_is_nullish(f))
        return f == JS_EXCEPTION ? f : JS_UNDEFINED;
    if (!js_is_callable(f)) {
        char *k = js_describe(J, key);
        throw_type(J, "%s is not a function", k);
        free(k);
        return JS_EXCEPTION;
    }
    return f;
}

jsval js_call_method(struct js *J, jsval obj, const char *name, int argc, jsval *argv)
{
    jsval f = js_get_cstr(J, obj, name);
    if (f == JS_EXCEPTION)
        return f;
    if (!js_is_callable(f))
        return throw_type(J, "%s is not a function", name);
    return vm_call(J, f, obj, argc, argv, JS_UNDEFINED);
}

jsval js_invoke(struct js *J, jsval f, jsval this_val, int argc, jsval *argv, jsval new_target)
{
    return vm_call(J, f, this_val, argc, argv, new_target);
}

struct jsobject *js_proto_from_ctor(struct js *J, jsval new_target, struct jsobject *fallback)
{
    if (!js_is_object(new_target))
        return fallback;
    jsval p = obj_get(J, to_obj(new_target), str_val(J->atom[A_prototype]), new_target);
    if (p == JS_EXCEPTION)
        return 0;
    return js_is_object(p) ? to_obj(p) : fallback;
}

jsval js_species_constructor(struct js *J, struct jsobject *o, jsval def)
{
    jsval c = obj_get(J, o, str_val(J->atom[A_constructor]), obj_val(o));
    if (c == JS_EXCEPTION)
        return c;
    if (c == JS_UNDEFINED)
        return def;
    if (!js_is_object(c))
        return throw_type(J, "The .constructor property is not an object");
    jsval s = obj_get(J, to_obj(c), J->symbol[SYM_SPECIES], c);
    if (s == JS_EXCEPTION)
        return s;
    if (js_is_nullish(s))
        return def;
    if (js_is_constructor(s))
        return s;
    return throw_type(J, "Symbol.species is not a constructor");
}

jsval js_create_from_list(struct js *J, jsval *v, uint32_t n)
{
    struct jsobject *a = array_new(J, n);
    for (uint32_t i = 0; i < n; i++)
        a->elems[i] = v[i];
    return obj_val(a);
}

int js_list_from_array_like(struct js *J, jsval v, jsval **listp, uint32_t *np)
{
    if (!js_is_object(v)) {
        throw_type(J, "Argument list is not an object");
        return -1;
    }
    struct jsobject *o = to_obj(v);
    double len;
    if (is_dense(o))
        len = o->length;
    else {
        jsval l = obj_get(J, o, str_val(J->atom[A_length]), v);
        if (l == JS_EXCEPTION || js_to_length(J, l, &len) < 0)
            return -1;
    }
    if (len > 100000000) {
        throw_range(J, "Too many arguments");
        return -1;
    }
    uint32_t n = (uint32_t)len;
    jsval *list = malloc(((size_t)n + 1) * sizeof *list);
    if (!list)
        abort();
    for (uint32_t i = 0; i < n; i++)
        list[i] = JS_UNDEFINED;
    js_root_push(J, list, n);
    for (uint32_t i = 0; i < n; i++) {
        jsval e = obj_get(J, o, str_val(atom_index(J, i)), v);
        if (e == JS_EXCEPTION) {
            js_root_pop(J);
            free(list);
            return -1;
        }
        list[i] = e;
    }
    js_root_pop(J);
    *listp = list;
    *np = n;
    return 0;
}

/* --- the iterator protocol ---------------------------------------------- */

int iter_open(struct js *J, jsval v, struct iter *it)
{
    memset(it, 0, sizeof *it);
    jsval m = js_get_method(J, v, J->symbol[SYM_ITERATOR]);
    if (m == JS_EXCEPTION)
        return -1;
    if (m == JS_UNDEFINED) {
        char *d = js_describe(J, v);
        throw_type(J, "%s is not iterable", d);
        free(d);
        return -1;
    }
    if (m == J->array_proto_values && js_is_object(v) && obj_class(to_obj(v)) == CLASS_ARRAY) {
        jsval next = obj_get(J, J->proto[P_ARRAY_ITERATOR], str_val(J->atom[A_next]),
                             obj_val(J->proto[P_ARRAY_ITERATOR]));
        if (next == J->array_iter_next) {
            it->fast = true;
            it->arr = to_obj(v);
            it->it = v;
            return 0;
        }
    }
    jsval i = vm_call(J, m, v, 0, 0, JS_UNDEFINED);
    if (i == JS_EXCEPTION)
        return -1;
    if (!js_is_object(i)) {
        throw_type(J, "Result of the Symbol.iterator method is not an object");
        return -1;
    }
    jsval next = obj_get(J, to_obj(i), str_val(J->atom[A_next]), i);
    if (next == JS_EXCEPTION)
        return -1;
    it->it = i;
    it->next = next;
    return 0;
}

int iter_step(struct js *J, struct iter *it, jsval *value)
{
    if (it->fast) {
        struct jsobject *a = it->arr;
        if (it->at >= a->length) {
            it->at = UINT32_MAX;
            return 0;
        }
        uint32_t i = it->at++;
        if (is_dense(a) && i < a->cap && a->elems[i] != JS_EMPTY)
            *value = a->elems[i];
        else {
            jsval e = obj_get(J, a, str_val(atom_index(J, i)), obj_val(a));
            if (e == JS_EXCEPTION)
                return -1;
            *value = e;
        }
        return 1;
    }
    if (!js_is_callable(it->next)) {
        throw_type(J, "Iterator's next is not a function");
        return -1;
    }
    jsval r = vm_call(J, it->next, it->it, 0, 0, JS_UNDEFINED);
    if (r == JS_EXCEPTION)
        return -1;
    if (!js_is_object(r)) {
        throw_type(J, "Iterator result is not an object");
        return -1;
    }
    jsval done = obj_get(J, to_obj(r), str_val(J->atom[A_done]), r);
    if (done == JS_EXCEPTION)
        return -1;
    if (js_to_boolean(done))
        return 0;
    jsval v = obj_get(J, to_obj(r), str_val(J->atom[A_value]), r);
    if (v == JS_EXCEPTION)
        return -1;
    *value = v;
    return 1;
}

int iter_close(struct js *J, struct iter *it, bool on_throw)
{
    if (it->fast)
        return 0;
    jsval saved = J->exception;
    bool had = J->has_exception;
    jsval m = js_get_method(J, it->it, str_val(J->atom[A_return]));
    if (m == JS_EXCEPTION || m == JS_UNDEFINED) {
        if (on_throw) {
            J->exception = saved;
            J->has_exception = had;
            return 0;
        }
        return m == JS_EXCEPTION ? -1 : 0;
    }
    jsval r = vm_call(J, m, it->it, 0, 0, JS_UNDEFINED);
    if (on_throw) {
        J->exception = saved;
        J->has_exception = had;
        return 0;
    }
    if (r == JS_EXCEPTION)
        return -1;
    if (!js_is_object(r)) {
        throw_type(J, "Iterator result is not an object");
        return -1;
    }
    return 0;
}

jsval iter_result(struct js *J, jsval value, bool done)
{
    struct jsobject *o = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
    prop_add(J, o, str_val(J->atom[A_value]), value, PROP_DEFAULT);
    prop_add(J, o, str_val(J->atom[A_done]), js_bool(done), PROP_DEFAULT);
    return obj_val(o);
}

jsval make_iterator(struct js *J, struct jsobject *proto, jsval target, int kind)
{
    struct jsobject *o = obj_new(J, proto, CLASS_ITERATOR);
    o->u.iter.target = target;
    o->u.iter.kind = (uint8_t)kind;
    o->u.iter.extra = JS_UNDEFINED;
    return obj_val(o);
}

/* --- errors ---------------------------------------------------------------- */

char *js_describe(struct js *J, jsval v)
{
    if (js_is_string(v))
        return str_to_utf8(J, to_str(v), 0);
    if (js_is_symbol(v)) {
        struct jsstring *s = js_to_str_noexcept(J, v);
        return str_to_utf8(J, s, 0);
    }
    if (js_is_object(v)) {
        if (js_is_callable(v))
            return strdup("function");
        return strdup(obj_class(to_obj(v)) == CLASS_ARRAY ? "array" : "object");
    }
    struct jsstring *s = js_to_str(J, v);
    return s ? str_to_utf8(J, s, 0) : strdup("value");
}

jsval js_error_value(struct js *J, struct jsobject *proto, const char *fmt, va_list ap)
{
    char buf[512];
    vsnprintf(buf, sizeof buf, fmt, ap);
    return error_create(J, proto, str_val(str_from_utf8(J, buf, strlen(buf))));
}

jsval throw_error(struct js *J, int proto, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    jsval e = js_error_value(J, J->proto[proto], fmt, ap);
    va_end(ap);
    return js_throw(J, e);
}

jsval throw_oom(struct js *J)
{
    return throw_range(J, "Out of memory");
}

jsval js_throw(struct js *J, jsval exception)
{
    J->exception = exception;
    J->has_exception = true;
    return JS_EXCEPTION;
}

#define THROW_KIND(fn, kind)                                        \
    jsval fn(struct js *J, const char *fmt, ...)                    \
    {                                                               \
        va_list ap;                                                 \
        va_start(ap, fmt);                                          \
        jsval e = js_error_value(J, J->proto[kind], fmt, ap);       \
        va_end(ap);                                                 \
        return js_throw(J, e);                                      \
    }
THROW_KIND(js_throw_type, P_TYPE_ERROR)
THROW_KIND(js_throw_range, P_RANGE_ERROR)
THROW_KIND(js_throw_reference, P_REFERENCE_ERROR)
THROW_KIND(js_throw_syntax, P_SYNTAX_ERROR)

jsval js_take_exception(struct js *J)
{
    jsval e = J->exception;
    J->exception = JS_UNDEFINED;
    J->has_exception = false;
    return e;
}

bool visiting_push(struct js *J, struct jsobject *o)
{
    for (uint32_t i = 0; i < J->nvisiting; i++)
        if (J->visiting[i] == o)
            return false;
    if (J->nvisiting == J->visiting_cap) {
        uint32_t cap = J->visiting_cap ? J->visiting_cap * 2 : 16;
        struct jsobject **v = realloc(J->visiting, cap * sizeof *v);
        if (!v)
            abort();
        J->visiting = v;
        J->visiting_cap = cap;
    }
    J->visiting[J->nvisiting++] = o;
    return true;
}

void visiting_pop(struct js *J)
{
    J->nvisiting--;
}

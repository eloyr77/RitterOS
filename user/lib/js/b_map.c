/*
 * Map, Set, WeakMap and WeakSet (27o). One ordered hash table (struct
 * mapdata): entries in insertion order with deleted ones left as holes, so
 * an iterator is just a position (and sees what is added meanwhile, as the
 * language wants); the holes are squeezed out only while no iterator is
 * alive. The weak collections hold their keys strongly (no ephemerons in
 * the collector yet): correct results, more memory kept.
 */
#include <math.h>
#include "jsint.h"

static uint32_t value_hash(struct js *J, jsval v)
{
    if (js_is_number(v)) {
        double d = js_number_value(v);
        if (d == 0)
            d = 0;                              /* -0 is +0 */
        uint64_t bits;
        memcpy(&bits, &d, 8);
        return (uint32_t)(bits ^ (bits >> 32)) * 2654435761u;
    }
    if (js_is_string(v))
        return str_hash(J, to_str(v));
    return (uint32_t)((v >> 3) ^ (v >> 29)) * 2654435761u;
}

static bool same_key(struct js *J, jsval a, jsval b)
{
    return js_same_value(J, a, b, true);
}

static struct mapdata *mapdata_new(void)
{
    struct mapdata *m = calloc(1, sizeof *m);
    if (!m)
        abort();
    return m;
}

static void rebuild_buckets(struct mapdata *m, uint32_t nb)
{
    free(m->buckets);
    m->buckets = calloc(nb, sizeof *m->buckets);
    if (!m->buckets)
        abort();
    m->nbuckets = nb;
    for (uint32_t i = 0; i < m->n; i++) {
        if (m->e[i].key == JS_EMPTY)
            continue;
        uint32_t b = m->e[i].hash & (nb - 1);
        m->e[i].next = m->buckets[b];
        m->buckets[b] = i + 1;
    }
}

static void compact(struct mapdata *m)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < m->n; i++)
        if (m->e[i].key != JS_EMPTY)
            m->e[k++] = m->e[i];
    m->n = k;
    rebuild_buckets(m, m->nbuckets ? m->nbuckets : 8);
}

static int64_t map_find(struct js *J, struct mapdata *m, jsval key)
{
    if (!m->nbuckets)
        return -1;
    if (js_is_number(key) && js_number_value(key) == 0)
        key = js_number(0);
    uint32_t h = value_hash(J, key);
    for (uint32_t i = m->buckets[h & (m->nbuckets - 1)]; i; i = m->e[i - 1].next) {
        struct mapentry *e = &m->e[i - 1];
        if (e->key != JS_EMPTY && e->hash == h && same_key(J, e->key, key))
            return i - 1;
    }
    return -1;
}

static void map_put(struct js *J, struct mapdata *m, jsval key, jsval value)
{
    if (js_is_number(key) && js_number_value(key) == 0)
        key = js_number(0);
    int64_t at = map_find(J, m, key);
    if (at >= 0) {
        m->e[at].value = value;
        return;
    }
    if (m->n == m->cap) {
        if (m->live < m->n / 2 && !m->iterators && m->n > 8)
            compact(m);
        else {
            uint32_t cap = m->cap ? m->cap * 2 : 8;
            m->e = realloc(m->e, cap * sizeof *m->e);
            if (!m->e)
                abort();
            gc_note_external(J, (cap - m->cap) * sizeof *m->e);
            m->cap = cap;
        }
    }
    uint32_t h = value_hash(J, key);
    struct mapentry *e = &m->e[m->n++];
    e->key = key;
    e->value = value;
    e->hash = h;
    m->live++;
    if (m->n > m->nbuckets * 2 || !m->nbuckets)
        rebuild_buckets(m, m->nbuckets ? m->nbuckets * 4 : 8);
    else {
        uint32_t b = h & (m->nbuckets - 1);
        e->next = m->buckets[b];
        m->buckets[b] = m->n;
    }
}

static bool map_remove(struct js *J, struct mapdata *m, jsval key)
{
    int64_t at = map_find(J, m, key);
    if (at < 0)
        return false;
    m->e[at].key = JS_EMPTY;
    m->e[at].value = JS_UNDEFINED;
    m->live--;
    if (!m->iterators && m->live < m->n / 4 && m->n > 32)
        compact(m);
    return true;
}

static void map_clear(struct mapdata *m)
{
    for (uint32_t i = 0; i < m->n; i++) {
        m->e[i].key = JS_EMPTY;
        m->e[i].value = JS_UNDEFINED;
    }
    m->live = 0;
    if (!m->iterators) {
        m->n = 0;
        if (m->buckets)
            memset(m->buckets, 0, m->nbuckets * sizeof *m->buckets);
    }
}

static struct mapdata *this_map(struct js *J, jsval v, int cls, const char *what)
{
    if (js_is_object(v) && obj_class(to_obj(v)) == cls && to_obj(v)->u.map)
        return to_obj(v)->u.map;
    throw_type(J, "%s called on an incompatible receiver", what);
    return 0;
}

static jsval collection_ctor(struct js *J, int argc, jsval *argv, jsval nt, int cls, int proto, const char *name)
{
    (void)argc;
    if (nt == JS_UNDEFINED)
        return throw_type(J, "%s constructor requires 'new'", name);
    struct jsobject *p = js_proto_from_ctor(J, nt, J->proto[proto]);
    if (!p)
        return JS_EXCEPTION;
    struct jsobject *o = obj_new(J, p, cls);
    o->u.map = mapdata_new();
    jsval ov = obj_val(o);
    if (js_is_nullish(argv[0]))
        return ov;
    bool pairs = cls == CLASS_MAP || cls == CLASS_WEAKMAP;
    jsval adder = obj_get(J, o, key_cstr(J, pairs ? "set" : "add"), ov);
    if (adder == JS_EXCEPTION)
        return adder;
    if (!js_is_callable(adder))
        return throw_type(J, "'%s' is not a function", pairs ? "set" : "add");
    struct iter it;
    if (iter_open(J, argv[0], &it) < 0)
        return JS_EXCEPTION;
    jsval item;
    int r;
    while ((r = iter_step(J, &it, &item)) > 0) {
        jsval res;
        if (pairs) {
            if (!js_is_object(item)) {
                throw_type(J, "Iterator value %s is not an entry object", "item");
                iter_close(J, &it, true);
                return JS_EXCEPTION;
            }
            jsval k = js_get_value(J, item, str_val(J->atom[A_zero]));
            jsval v = k == JS_EXCEPTION ? k : js_get_value(J, item, str_val(atom_index(J, 1)));
            if (v == JS_EXCEPTION) {
                iter_close(J, &it, true);
                return JS_EXCEPTION;
            }
            jsval args[2] = { k, v };
            res = vm_call(J, adder, ov, 2, args, JS_UNDEFINED);
        } else
            res = vm_call(J, adder, ov, 1, &item, JS_UNDEFINED);
        if (res == JS_EXCEPTION) {
            iter_close(J, &it, true);
            return JS_EXCEPTION;
        }
    }
    return r < 0 ? JS_EXCEPTION : ov;
}

static jsval map_ctor(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)t; return collection_ctor(J, c, a, n, CLASS_MAP, P_MAP, "Map"); }
static jsval set_ctor(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)t; return collection_ctor(J, c, a, n, CLASS_SET, P_SET, "Set"); }
static jsval weakmap_ctor(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)t; return collection_ctor(J, c, a, n, CLASS_WEAKMAP, P_WEAKMAP, "WeakMap"); }
static jsval weakset_ctor(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)t; return collection_ctor(J, c, a, n, CLASS_WEAKSET, P_WEAKSET, "WeakSet"); }

static bool can_be_weak(jsval v)
{
    return js_is_object(v) || (js_is_symbol(v) && !(to_sym(v)->h.flags & 1));
}

/* Map.prototype */
static jsval mp_get(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_MAP, "Map.prototype.get");
    if (!m)
        return JS_EXCEPTION;
    int64_t at = map_find(J, m, a[0]);
    return at >= 0 ? m->e[at].value : JS_UNDEFINED;
}

static jsval mp_set(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_MAP, "Map.prototype.set");
    if (!m)
        return JS_EXCEPTION;
    map_put(J, m, a[0], a[1]);
    return t;
}

static jsval mp_has(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_MAP, "Map.prototype.has");
    if (!m)
        return JS_EXCEPTION;
    return js_bool(map_find(J, m, a[0]) >= 0);
}

static jsval mp_delete(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_MAP, "Map.prototype.delete");
    if (!m)
        return JS_EXCEPTION;
    return js_bool(map_remove(J, m, a[0]));
}

static jsval mp_clear(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)a, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_MAP, "Map.prototype.clear");
    if (!m)
        return JS_EXCEPTION;
    map_clear(m);
    return JS_UNDEFINED;
}

static jsval mp_size(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)a, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_MAP, "get Map.prototype.size");
    return m ? js_number(m->live) : JS_EXCEPTION;
}

static jsval for_each(struct js *J, jsval t, jsval *a, int cls, const char *what)
{
    struct mapdata *m = this_map(J, t, cls, what);
    if (!m)
        return JS_EXCEPTION;
    if (!js_is_callable(a[0]))
        return throw_type(J, "%s: callback is not a function", what);
    m->iterators++;
    for (uint32_t i = 0; i < m->n; i++) {
        if (m->e[i].key == JS_EMPTY)
            continue;
        jsval args[3] = { cls == CLASS_MAP ? m->e[i].value : m->e[i].key, m->e[i].key, t };
        jsval r = vm_call(J, a[0], a[1], 3, args, JS_UNDEFINED);
        if (r == JS_EXCEPTION) {
            m->iterators--;
            return r;
        }
    }
    m->iterators--;
    return JS_UNDEFINED;
}

static jsval mp_for_each(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)n; return for_each(J, t, a, CLASS_MAP, "Map.prototype.forEach"); }
static jsval sp_for_each(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)n; return for_each(J, t, a, CLASS_SET, "Set.prototype.forEach"); }

static jsval collection_iter(struct js *J, jsval t, int cls, int kind, const char *what)
{
    struct mapdata *m = this_map(J, t, cls, what);
    if (!m)
        return JS_EXCEPTION;
    m->iterators++;
    return make_iterator(J, J->proto[cls == CLASS_MAP ? P_MAP_ITERATOR : P_SET_ITERATOR], t, kind);
}

static jsval mp_entries(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)a, (void)n; return collection_iter(J, t, CLASS_MAP, ITER_MAP_ENTRIES, "Map.prototype.entries"); }
static jsval mp_keys(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)a, (void)n; return collection_iter(J, t, CLASS_MAP, ITER_MAP_KEYS, "Map.prototype.keys"); }
static jsval mp_values(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)a, (void)n; return collection_iter(J, t, CLASS_MAP, ITER_MAP_VALUES, "Map.prototype.values"); }
static jsval sp_entries(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)a, (void)n; return collection_iter(J, t, CLASS_SET, ITER_SET_ENTRIES, "Set.prototype.entries"); }
static jsval sp_values(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)a, (void)n; return collection_iter(J, t, CLASS_SET, ITER_SET_VALUES, "Set.prototype.values"); }

static jsval collection_next(struct js *J, jsval this_val, bool is_map)
{
    if (!js_is_object(this_val) || obj_class(to_obj(this_val)) != CLASS_ITERATOR)
        return throw_type(J, "%s Iterator next called on an incompatible receiver", is_map ? "Map" : "Set");
    struct jsobject *it = to_obj(this_val);
    int kind = it->u.iter.kind;
    if (is_map ? (kind < ITER_MAP_KEYS || kind > ITER_MAP_ENTRIES) : (kind < ITER_SET_VALUES || kind > ITER_SET_ENTRIES))
        return throw_type(J, "%s Iterator next called on an incompatible receiver", is_map ? "Map" : "Set");
    if (it->u.iter.done)
        return iter_result(J, JS_UNDEFINED, true);
    struct mapdata *m = to_obj(it->u.iter.target)->u.map;
    while (it->u.iter.index < m->n && m->e[it->u.iter.index].key == JS_EMPTY)
        it->u.iter.index++;
    if (it->u.iter.index >= m->n) {
        it->u.iter.done = true;
        if (m->iterators)
            m->iterators--;
        return iter_result(J, JS_UNDEFINED, true);
    }
    struct mapentry *e = &m->e[it->u.iter.index++];
    switch (kind) {
    case ITER_MAP_KEYS: case ITER_SET_VALUES:
        return iter_result(J, e->key, false);
    case ITER_MAP_VALUES:
        return iter_result(J, e->value, false);
    default: {
        jsval pair[2] = { e->key, kind == ITER_MAP_ENTRIES ? e->value : e->key };
        return iter_result(J, js_create_from_list(J, pair, 2), false);
    }
    }
}

static jsval map_iter_next(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)a, (void)n; return collection_next(J, t, true); }
static jsval set_iter_next(struct js *J, jsval t, int c, jsval *a, jsval n) { (void)c, (void)a, (void)n; return collection_next(J, t, false); }

static jsval map_group_by(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    if (!js_is_callable(argv[1]))
        return throw_type(J, "Map.groupBy: callback is not a function");
    struct jsobject *o = obj_new(J, J->proto[P_MAP], CLASS_MAP);
    o->u.map = mapdata_new();
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
        int64_t at = map_find(J, o->u.map, g);
        jsval list;
        if (at >= 0)
            list = o->u.map->e[at].value;
        else {
            list = obj_val(array_new(J, 0));
            map_put(J, o->u.map, g, list);
        }
        struct jsobject *a = to_obj(list);
        array_grow(J, a, a->length + 1);
        a->elems[a->length++] = v;
    }
    return r < 0 ? JS_EXCEPTION : obj_val(o);
}

/* Set.prototype */
static jsval sp_add(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_SET, "Set.prototype.add");
    if (!m)
        return JS_EXCEPTION;
    if (map_find(J, m, a[0]) < 0)
        map_put(J, m, a[0], JS_UNDEFINED);
    return t;
}

static jsval sp_has(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_SET, "Set.prototype.has");
    return m ? js_bool(map_find(J, m, a[0]) >= 0) : JS_EXCEPTION;
}

static jsval sp_delete(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_SET, "Set.prototype.delete");
    return m ? js_bool(map_remove(J, m, a[0])) : JS_EXCEPTION;
}

static jsval sp_clear(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)a, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_SET, "Set.prototype.clear");
    if (!m)
        return JS_EXCEPTION;
    map_clear(m);
    return JS_UNDEFINED;
}

static jsval sp_size(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)a, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_SET, "get Set.prototype.size");
    return m ? js_number(m->live) : JS_EXCEPTION;
}

/* The set methods (ES2025) for another Set or a set-like (size, has, keys). */
struct setlike {
    jsval obj, has, keys;
    double size;
};

static int get_setlike(struct js *J, jsval v, struct setlike *s)
{
    if (!js_is_object(v)) {
        throw_type(J, "The argument must be a set-like object");
        return -1;
    }
    jsval size = js_get_value(J, v, str_val(J->atom[A_size]));
    if (size == JS_EXCEPTION)
        return -1;
    bool threw = false;
    double d = js_to_number(J, size, &threw);
    if (threw)
        return -1;
    if (d != d) {
        throw_type(J, "The set-like's size is not a number");
        return -1;
    }
    s->size = trunc(d);
    if (s->size < 0) {
        throw_range(J, "The set-like's size is negative");
        return -1;
    }
    s->has = js_get_value(J, v, str_val(J->atom[A_has]));
    if (s->has == JS_EXCEPTION)
        return -1;
    if (!js_is_callable(s->has)) {
        throw_type(J, "The set-like's has is not a function");
        return -1;
    }
    s->keys = js_get_cstr(J, v, "keys");
    if (s->keys == JS_EXCEPTION)
        return -1;
    if (!js_is_callable(s->keys)) {
        throw_type(J, "The set-like's keys is not a function");
        return -1;
    }
    s->obj = v;
    return 0;
}

static struct jsobject *new_set(struct js *J)
{
    struct jsobject *o = obj_new(J, J->proto[P_SET], CLASS_SET);
    o->u.map = mapdata_new();
    return o;
}

static struct jsobject *copy_set(struct js *J, struct mapdata *m)
{
    struct jsobject *o = new_set(J);
    for (uint32_t i = 0; i < m->n; i++)
        if (m->e[i].key != JS_EMPTY)
            map_put(J, o->u.map, m->e[i].key, JS_UNDEFINED);
    return o;
}

static int setlike_has(struct js *J, struct setlike *s, jsval v)
{
    jsval r = vm_call(J, s->has, s->obj, 1, &v, JS_UNDEFINED);
    return r == JS_EXCEPTION ? -1 : js_to_boolean(r);
}

/* each key of the set-like: fn(key) returning -1 to stop with an error, 1 to stop */
static int setlike_each(struct js *J, struct setlike *s, int (*fn)(struct js *, void *, jsval), void *ctx)
{
    jsval it = vm_call(J, s->keys, s->obj, 0, 0, JS_UNDEFINED);
    if (it == JS_EXCEPTION)
        return -1;
    if (!js_is_object(it)) {
        throw_type(J, "keys() did not return an object");
        return -1;
    }
    jsval next = js_get_value(J, it, str_val(J->atom[A_next]));
    if (next == JS_EXCEPTION)
        return -1;
    struct iter iter = { .it = it, .next = next };
    jsval v;
    int r;
    while ((r = iter_step(J, &iter, &v)) > 0) {
        int k = fn(J, ctx, v);
        if (k < 0)
            return -1;
        if (k > 0) {
            iter_close(J, &iter, false);
            return 0;
        }
    }
    return r < 0 ? -1 : 0;
}

static int add_to(struct js *J, void *ctx, jsval v)
{
    if (js_is_number(v) && js_number_value(v) == 0)
        v = js_number(0);
    struct mapdata *m = ctx;
    if (map_find(J, m, v) < 0)
        map_put(J, m, v, JS_UNDEFINED);
    return 0;
}

static jsval sp_union(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_SET, "Set.prototype.union");
    struct setlike s;
    if (!m || get_setlike(J, a[0], &s) < 0)
        return JS_EXCEPTION;
    struct jsobject *r = copy_set(J, m);
    if (setlike_each(J, &s, add_to, r->u.map) < 0)
        return JS_EXCEPTION;
    return obj_val(r);
}

struct both { struct mapdata *mine, *out; };

static int keep_if_mine(struct js *J, void *ctx, jsval v)
{
    struct both *b = ctx;
    if (map_find(J, b->mine, v) >= 0 && map_find(J, b->out, v) < 0)
        map_put(J, b->out, v, JS_UNDEFINED);
    return 0;
}

static jsval sp_intersection(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_SET, "Set.prototype.intersection");
    struct setlike s;
    if (!m || get_setlike(J, a[0], &s) < 0)
        return JS_EXCEPTION;
    struct jsobject *r = new_set(J);
    if (m->live <= s.size) {
        for (uint32_t i = 0; i < m->n; i++) {
            if (m->e[i].key == JS_EMPTY)
                continue;
            int h = setlike_has(J, &s, m->e[i].key);
            if (h < 0)
                return JS_EXCEPTION;
            if (h)
                map_put(J, r->u.map, m->e[i].key, JS_UNDEFINED);
        }
    } else {
        struct both b = { m, r->u.map };
        if (setlike_each(J, &s, keep_if_mine, &b) < 0)
            return JS_EXCEPTION;
    }
    return obj_val(r);
}

static int remove_from(struct js *J, void *ctx, jsval v)
{
    map_remove(J, ctx, v);
    return 0;
}

static jsval sp_difference(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_SET, "Set.prototype.difference");
    struct setlike s;
    if (!m || get_setlike(J, a[0], &s) < 0)
        return JS_EXCEPTION;
    struct jsobject *r = copy_set(J, m);
    if (m->live <= s.size) {
        for (uint32_t i = 0; i < m->n; i++) {
            if (m->e[i].key == JS_EMPTY)
                continue;
            int h = setlike_has(J, &s, m->e[i].key);
            if (h < 0)
                return JS_EXCEPTION;
            if (h)
                map_remove(J, r->u.map, m->e[i].key);
        }
    } else if (setlike_each(J, &s, remove_from, r->u.map) < 0)
        return JS_EXCEPTION;
    return obj_val(r);
}

struct symdiff { struct mapdata *mine, *out; };

static int toggle(struct js *J, void *ctx, jsval v)
{
    struct symdiff *d = ctx;
    if (js_is_number(v) && js_number_value(v) == 0)
        v = js_number(0);
    if (map_find(J, d->mine, v) >= 0)
        map_remove(J, d->out, v);
    else if (map_find(J, d->out, v) < 0)
        map_put(J, d->out, v, JS_UNDEFINED);
    return 0;
}

static jsval sp_symmetric_difference(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_SET, "Set.prototype.symmetricDifference");
    struct setlike s;
    if (!m || get_setlike(J, a[0], &s) < 0)
        return JS_EXCEPTION;
    struct jsobject *r = copy_set(J, m);
    struct symdiff d = { m, r->u.map };
    if (setlike_each(J, &s, toggle, &d) < 0)
        return JS_EXCEPTION;
    return obj_val(r);
}

static jsval sp_is_subset_of(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_SET, "Set.prototype.isSubsetOf");
    struct setlike s;
    if (!m || get_setlike(J, a[0], &s) < 0)
        return JS_EXCEPTION;
    if (m->live > s.size)
        return JS_FALSE;
    for (uint32_t i = 0; i < m->n; i++) {
        if (m->e[i].key == JS_EMPTY)
            continue;
        int h = setlike_has(J, &s, m->e[i].key);
        if (h < 0)
            return JS_EXCEPTION;
        if (!h)
            return JS_FALSE;
    }
    return JS_TRUE;
}

struct found { struct mapdata *m; bool want_in; bool result; };

static int check_in(struct js *J, void *ctx, jsval v)
{
    struct found *f = ctx;
    bool in = map_find(J, f->m, v) >= 0;
    if (in != f->want_in) {
        f->result = false;
        return 1;
    }
    return 0;
}

static jsval sp_is_superset_of(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_SET, "Set.prototype.isSupersetOf");
    struct setlike s;
    if (!m || get_setlike(J, a[0], &s) < 0)
        return JS_EXCEPTION;
    if (m->live < s.size)
        return JS_FALSE;
    struct found f = { m, true, true };
    if (setlike_each(J, &s, check_in, &f) < 0)
        return JS_EXCEPTION;
    return js_bool(f.result);
}

static jsval sp_is_disjoint_from(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_SET, "Set.prototype.isDisjointFrom");
    struct setlike s;
    if (!m || get_setlike(J, a[0], &s) < 0)
        return JS_EXCEPTION;
    if (m->live <= s.size) {
        for (uint32_t i = 0; i < m->n; i++) {
            if (m->e[i].key == JS_EMPTY)
                continue;
            int h = setlike_has(J, &s, m->e[i].key);
            if (h < 0)
                return JS_EXCEPTION;
            if (h)
                return JS_FALSE;
        }
        return JS_TRUE;
    }
    struct found f = { m, false, true };
    if (setlike_each(J, &s, check_in, &f) < 0)
        return JS_EXCEPTION;
    return js_bool(f.result);
}

/* WeakMap / WeakSet */
static jsval wm_get(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_WEAKMAP, "WeakMap.prototype.get");
    if (!m)
        return JS_EXCEPTION;
    int64_t at = map_find(J, m, a[0]);
    return at >= 0 ? m->e[at].value : JS_UNDEFINED;
}

static jsval wm_set(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_WEAKMAP, "WeakMap.prototype.set");
    if (!m)
        return JS_EXCEPTION;
    if (!can_be_weak(a[0]))
        return throw_type(J, "Invalid value used as weak map key");
    map_put(J, m, a[0], a[1]);
    return t;
}

static jsval wm_has(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_WEAKMAP, "WeakMap.prototype.has");
    return m ? js_bool(map_find(J, m, a[0]) >= 0) : JS_EXCEPTION;
}

static jsval wm_delete(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_WEAKMAP, "WeakMap.prototype.delete");
    return m ? js_bool(map_remove(J, m, a[0])) : JS_EXCEPTION;
}

static jsval ws_add(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_WEAKSET, "WeakSet.prototype.add");
    if (!m)
        return JS_EXCEPTION;
    if (!can_be_weak(a[0]))
        return throw_type(J, "Invalid value used in weak set");
    if (map_find(J, m, a[0]) < 0)
        map_put(J, m, a[0], JS_UNDEFINED);
    return t;
}

static jsval ws_has(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_WEAKSET, "WeakSet.prototype.has");
    return m ? js_bool(map_find(J, m, a[0]) >= 0) : JS_EXCEPTION;
}

static jsval ws_delete(struct js *J, jsval t, int c, jsval *a, jsval n)
{
    (void)c, (void)n;
    struct mapdata *m = this_map(J, t, CLASS_WEAKSET, "WeakSet.prototype.delete");
    return m ? js_bool(map_remove(J, m, a[0])) : JS_EXCEPTION;
}

static jsval species_getter(struct js *J, jsval this_val, int c, jsval *a, jsval n)
{
    (void)J, (void)c, (void)a, (void)n;
    return this_val;
}

static void define_size(struct js *J, struct jsobject *p, js_native fn)
{
    struct jsobject *g = make_native(J, "get size", fn, 0, 0);
    obj_define_accessor(J, p, str_val(J->atom[A_size]), obj_val(g), JS_UNDEFINED, PROP_CONFIGURABLE);
}

static void tag(struct js *J, struct jsobject *o, const char *name)
{
    obj_define_value(J, o, J->symbol[SYM_TO_STRING_TAG], str_val(atom_cstr(J, name)), PROP_CONFIGURABLE);
}

void init_map(struct js *J)
{
    struct jsobject *mp = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
    J->proto[P_MAP] = mp;
    struct jsobject *mc = make_ctor(J, "Map", map_ctor, 0, mp);
    J->ctor[C_MAP] = mc;
    obj_define_native(J, mc, "groupBy", map_group_by, 2, 0);
    struct jsobject *sg = make_native(J, "get [Symbol.species]", species_getter, 0, 0);
    obj_define_accessor(J, mc, J->symbol[SYM_SPECIES], obj_val(sg), JS_UNDEFINED, PROP_CONFIGURABLE);
    obj_define_native(J, mp, "clear", mp_clear, 0, 0);
    obj_define_native(J, mp, "delete", mp_delete, 1, 0);
    obj_define_native(J, mp, "entries", mp_entries, 0, 0);
    obj_define_native(J, mp, "forEach", mp_for_each, 1, 0);
    obj_define_native(J, mp, "get", mp_get, 1, 0);
    obj_define_native(J, mp, "has", mp_has, 1, 0);
    obj_define_native(J, mp, "keys", mp_keys, 0, 0);
    obj_define_native(J, mp, "set", mp_set, 2, 0);
    obj_define_native(J, mp, "values", mp_values, 0, 0);
    define_size(J, mp, mp_size);
    obj_define_value(J, mp, J->symbol[SYM_ITERATOR], obj_get(J, mp, key_cstr(J, "entries"), obj_val(mp)),
                     PROP_WRITABLE | PROP_CONFIGURABLE);
    tag(J, mp, "Map");

    struct jsobject *sp = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
    J->proto[P_SET] = sp;
    struct jsobject *sc = make_ctor(J, "Set", set_ctor, 0, sp);
    J->ctor[C_SET] = sc;
    struct jsobject *sg2 = make_native(J, "get [Symbol.species]", species_getter, 0, 0);
    obj_define_accessor(J, sc, J->symbol[SYM_SPECIES], obj_val(sg2), JS_UNDEFINED, PROP_CONFIGURABLE);
    obj_define_native(J, sp, "add", sp_add, 1, 0);
    obj_define_native(J, sp, "clear", sp_clear, 0, 0);
    obj_define_native(J, sp, "delete", sp_delete, 1, 0);
    obj_define_native(J, sp, "difference", sp_difference, 1, 0);
    obj_define_native(J, sp, "entries", sp_entries, 0, 0);
    obj_define_native(J, sp, "forEach", sp_for_each, 1, 0);
    obj_define_native(J, sp, "has", sp_has, 1, 0);
    obj_define_native(J, sp, "intersection", sp_intersection, 1, 0);
    obj_define_native(J, sp, "isDisjointFrom", sp_is_disjoint_from, 1, 0);
    obj_define_native(J, sp, "isSubsetOf", sp_is_subset_of, 1, 0);
    obj_define_native(J, sp, "isSupersetOf", sp_is_superset_of, 1, 0);
    obj_define_native(J, sp, "symmetricDifference", sp_symmetric_difference, 1, 0);
    obj_define_native(J, sp, "union", sp_union, 1, 0);
    obj_define_native(J, sp, "values", sp_values, 0, 0);
    jsval values = obj_get(J, sp, key_cstr(J, "values"), obj_val(sp));
    obj_define_value(J, sp, key_cstr(J, "keys"), values, PROP_WRITABLE | PROP_CONFIGURABLE);
    obj_define_value(J, sp, J->symbol[SYM_ITERATOR], values, PROP_WRITABLE | PROP_CONFIGURABLE);
    define_size(J, sp, sp_size);
    tag(J, sp, "Set");

    struct jsobject *wmp = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
    J->proto[P_WEAKMAP] = wmp;
    make_ctor(J, "WeakMap", weakmap_ctor, 0, wmp);
    obj_define_native(J, wmp, "delete", wm_delete, 1, 0);
    obj_define_native(J, wmp, "get", wm_get, 1, 0);
    obj_define_native(J, wmp, "has", wm_has, 1, 0);
    obj_define_native(J, wmp, "set", wm_set, 2, 0);
    tag(J, wmp, "WeakMap");

    struct jsobject *wsp = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
    J->proto[P_WEAKSET] = wsp;
    make_ctor(J, "WeakSet", weakset_ctor, 0, wsp);
    obj_define_native(J, wsp, "add", ws_add, 1, 0);
    obj_define_native(J, wsp, "delete", ws_delete, 1, 0);
    obj_define_native(J, wsp, "has", ws_has, 1, 0);
    tag(J, wsp, "WeakSet");

    struct jsobject *mip = obj_new(J, J->proto[P_ITERATOR], CLASS_OBJECT);
    J->proto[P_MAP_ITERATOR] = mip;
    obj_define_native(J, mip, "next", map_iter_next, 0, 0);
    tag(J, mip, "Map Iterator");
    struct jsobject *sip = obj_new(J, J->proto[P_ITERATOR], CLASS_OBJECT);
    J->proto[P_SET_ITERATOR] = sip;
    obj_define_native(J, sip, "next", set_iter_next, 0, 0);
    tag(J, sip, "Set Iterator");
}

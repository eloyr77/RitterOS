/*
 * Generators, Promises and async functions (27r). A generator's frame is
 * kept in its object while it is not running (interp.c: gen_save,
 * gen_resume); an async function runs on the same machinery, `await`
 * stopping it like `yield`, and goes on from a promise reaction. Promises
 * follow the specification's records: a reaction is an array [kind,
 * handler, capability promise, resolve, reject], run as a job on the
 * engine's queue (js_queue_job), as the specification's PromiseJobs.
 */
#include "jsint.h"

static jsval native_data(struct js *J)
{
    return J->native_callee ? J->native_callee->u.native.data : JS_UNDEFINED;
}

static struct jsobject *make_native_data(struct js *J, const char *name, js_native fn, int length, int magic,
                                         jsval data)
{
    struct jsobject *f = make_native(J, name, fn, length, magic);
    f->u.native.data = data;
    return f;
}

static void tag_object(struct js *J, struct jsobject *o, const char *name)
{
    obj_define_value(J, o, J->symbol[SYM_TO_STRING_TAG], js_cstring(J, name), PROP_CONFIGURABLE);
}

static jsval list2(struct js *J, jsval a, jsval b)
{
    jsval v[2] = { a, b };
    return js_create_from_list(J, v, 2);
}

/* --- generators -------------------------------------------------------------- */

void gen_free(struct genstate *st)
{
    if (!st)
        return;
    free(st->saved);
    free(st->handlers);
    free(st);
}

static struct jsobject *promise_new(struct js *J, struct jsobject *proto);

struct jsobject *gen_create(struct js *J, struct frame *F, jsval *sp, const uint8_t *pc, bool async)
{
    struct jsobject *proto = J->proto[P_GENERATOR];
    if (!async) {                               /* (its function's prototype, if an object) */
        jsval p = obj_get(J, F->callee, str_val(J->atom[A_prototype]), obj_val(F->callee));
        if (p == JS_EXCEPTION)
            return 0;
        if (js_is_object(p))
            proto = to_obj(p);
    }
    struct genstate *st = calloc(1, sizeof *st);
    if (!st) {
        throw_oom(J);
        return 0;
    }
    st->this_val = st->new_target = st->promise = JS_UNDEFINED;
    struct jsobject *g = obj_new(J, async ? 0 : proto, CLASS_GENERATOR);
    g->u.gen = st;
    st->callee = F->callee;
    st->this_val = F->this_val;
    st->new_target = F->new_target;
    st->async = async;
    gen_save(J, st, F, sp, pc);
    st->state = GEN_START;
    if (async)
        st->promise = obj_val(promise_new(J, J->proto[P_PROMISE]));
    return g;
}

static struct jsobject *this_generator(struct js *J, jsval this_val)
{
    if (!js_is_object(this_val) || obj_class(to_obj(this_val)) != CLASS_GENERATOR ||
        !to_obj(this_val)->u.gen || to_obj(this_val)->u.gen->async) {
        throw_type(J, "Not a generator");
        return 0;
    }
    return to_obj(this_val);
}

/* next (magic RESUME_NEXT), throw, return */
static jsval gen_method(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    int mode = J->native_magic;
    struct jsobject *g = this_generator(J, this_val);
    if (!g)
        return JS_EXCEPTION;
    bool done;
    jsval r = gen_resume(J, g, mode, argv[0], &done);
    if (r == JS_EXCEPTION)
        return r;
    if (J->yield_raw) {                         /* (yield*: the inner iterator's own result) */
        J->yield_raw = false;
        return r;
    }
    return iter_result(J, r, done);
}

/* --- promises --------------------------------------------------------------- */

void promise_free(struct promise *p)
{
    free(p);
}

static struct jsobject *promise_new(struct js *J, struct jsobject *proto)
{
    struct promise *p = calloc(1, sizeof *p);
    if (!p)
        abort();
    p->result = JS_UNDEFINED;
    p->reactions = JS_UNDEFINED;
    struct jsobject *o = obj_new(J, proto, CLASS_PROMISE);
    o->u.promise = p;
    p->reactions = js_new_array(J, 0);
    return o;
}

static struct promise *promise_of(jsval v)
{
    return js_is_object(v) && obj_class(to_obj(v)) == CLASS_PROMISE ? to_obj(v)->u.promise : 0;
}

/* the reactions of kind (0 fulfill, 1 reject) queued with the result */
static void settle(struct js *J, jsval pv, int state, jsval result)
{
    struct promise *p = promise_of(pv);
    if (!p || p->state != PROMISE_PENDING)
        return;
    jsval reactions = p->reactions;
    p->state = (uint8_t)state;
    p->result = result;
    p->reactions = JS_UNDEFINED;
    uint32_t n = js_array_length(reactions);
    for (uint32_t i = 0; i < n; i++) {
        jsval r = js_get_index(J, reactions, i);
        if (js_get_index(J, r, 0) == js_number(state == PROMISE_FULFILLED ? 0 : 1)) {
            jsval args[2] = { r, result };
            js_queue_job(J, J->reaction_job, 2, args);
        }
    }
}

/* the specification's promise resolve functions, after "already resolved" */
static void resolve_promise(struct js *J, jsval p, jsval resolution)
{
    if (resolution == p) {
        jsval e = js_new_error(J, "TypeError", "Cannot resolve a promise with itself");
        settle(J, p, PROMISE_REJECTED, e);
        return;
    }
    if (!js_is_object(resolution)) {
        settle(J, p, PROMISE_FULFILLED, resolution);
        return;
    }
    jsval then = js_get_value(J, resolution, str_val(J->atom[A_then]));
    if (then == JS_EXCEPTION) {
        settle(J, p, PROMISE_REJECTED, js_take_exception(J));
        return;
    }
    if (!js_is_callable(then)) {
        settle(J, p, PROMISE_FULFILLED, resolution);
        return;
    }
    jsval args[3] = { p, resolution, then };
    js_queue_job(J, J->thenable_job, 3, args);
}

/* resolve (magic 0) and reject (1) of a promise: data [promise, already resolved] */
static jsval resolving_fn(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    int which = J->native_magic;
    jsval rec = native_data(J);
    if (js_get_index(J, rec, 1) == JS_TRUE)
        return JS_UNDEFINED;
    js_set_index(J, rec, 1, JS_TRUE);
    jsval p = js_get_index(J, rec, 0);
    if (which)
        settle(J, p, PROMISE_REJECTED, argv[0]);
    else
        resolve_promise(J, p, argv[0]);
    return JS_UNDEFINED;
}

static void resolving_functions(struct js *J, jsval p, jsval *resolve, jsval *reject)
{
    jsval rec = list2(J, p, JS_FALSE);
    *resolve = obj_val(make_native_data(J, "", resolving_fn, 1, 0, rec));
    *reject = obj_val(make_native_data(J, "", resolving_fn, 1, 1, rec));
}

/* NewPromiseCapability's executor: data [resolve, reject] */
static jsval capability_executor(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    jsval rec = native_data(J);
    if (!js_is_undefined(js_get_index(J, rec, 0)) || !js_is_undefined(js_get_index(J, rec, 1)))
        return throw_type(J, "Promise executor has already been invoked with non-undefined arguments");
    js_set_index(J, rec, 0, argv[0]);
    js_set_index(J, rec, 1, argv[1]);
    return JS_UNDEFINED;
}

struct capability {
    jsval promise, resolve, reject;
};

static int new_capability(struct js *J, jsval c, struct capability *cap)
{
    if (js_is_object(c) && to_obj(c) == J->ctor[C_PROMISE]) {     /* (ours: no executor needed) */
        cap->promise = obj_val(promise_new(J, J->proto[P_PROMISE]));
        resolving_functions(J, cap->promise, &cap->resolve, &cap->reject);
        return 0;
    }
    if (!js_is_constructor(c)) {
        throw_type(J, "Not a constructor");
        return -1;
    }
    jsval rec = list2(J, JS_UNDEFINED, JS_UNDEFINED);
    jsval executor = obj_val(make_native_data(J, "", capability_executor, 2, 0, rec));
    jsval p = vm_call(J, c, JS_UNDEFINED, 1, &executor, c);
    if (p == JS_EXCEPTION)
        return -1;
    cap->resolve = js_get_index(J, rec, 0);
    cap->reject = js_get_index(J, rec, 1);
    if (!js_is_callable(cap->resolve) || !js_is_callable(cap->reject)) {
        throw_type(J, "Promise resolve or reject function is not callable");
        return -1;
    }
    cap->promise = p;
    return 0;
}

/* PerformPromiseThen; cap 0: no promise made (await) */
static void perform_then(struct js *J, jsval pv, jsval on_fulfilled, jsval on_rejected,
                         const struct capability *cap)
{
    struct promise *p = promise_of(pv);
    jsval parts[5];
    parts[2] = cap ? cap->promise : JS_UNDEFINED;
    parts[3] = cap ? cap->resolve : JS_UNDEFINED;
    parts[4] = cap ? cap->reject : JS_UNDEFINED;
    parts[0] = js_number(0);
    parts[1] = js_is_callable(on_fulfilled) ? on_fulfilled : JS_UNDEFINED;
    jsval fulfill = js_create_from_list(J, parts, 5);
    parts[0] = js_number(1);
    parts[1] = js_is_callable(on_rejected) ? on_rejected : JS_UNDEFINED;
    jsval reject = js_create_from_list(J, parts, 5);
    if (p->state == PROMISE_PENDING) {
        uint32_t n = js_array_length(p->reactions);
        js_set_index(J, p->reactions, n, fulfill);
        js_set_index(J, p->reactions, n + 1, reject);
    } else {
        jsval args[2] = { p->state == PROMISE_FULFILLED ? fulfill : reject, p->result };
        js_queue_job(J, J->reaction_job, 2, args);
    }
    p->handled = true;
}

/* PromiseReactionJob(reaction, argument) */
static jsval reaction_job(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    jsval reaction = argv[0], argument = argv[1];
    bool fulfill = js_get_index(J, reaction, 0) == js_number(0);
    jsval handler = js_get_index(J, reaction, 1);
    jsval result;
    bool ok;
    if (js_is_undefined(handler)) {
        result = argument;
        ok = fulfill;
    } else {
        result = js_call(J, handler, JS_UNDEFINED, 1, &argument);
        ok = result != JS_EXCEPTION;
        if (!ok)
            result = js_take_exception(J);
    }
    jsval cap_promise = js_get_index(J, reaction, 2);
    if (js_is_undefined(cap_promise))
        return JS_UNDEFINED;                    /* (an await's: nothing more) */
    jsval fn = js_get_index(J, reaction, ok ? 3 : 4);
    return js_call(J, fn, JS_UNDEFINED, 1, &result);
}

/* PromiseResolveThenableJob(promise, thenable, then) */
static jsval thenable_job(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    jsval fns[2];
    resolving_functions(J, argv[0], &fns[0], &fns[1]);
    jsval r = js_call(J, argv[2], argv[1], 2, fns);
    if (r == JS_EXCEPTION) {
        jsval e = js_take_exception(J);
        return js_call(J, fns[1], JS_UNDEFINED, 1, &e);
    }
    return JS_UNDEFINED;
}

/* PromiseResolve(C, x) */
static jsval promise_resolve(struct js *J, jsval c, jsval x)
{
    if (promise_of(x)) {
        jsval ctor = js_get_value(J, x, str_val(J->atom[A_constructor]));
        if (ctor == JS_EXCEPTION)
            return ctor;
        if (ctor == c)
            return x;
    }
    struct capability cap;
    if (new_capability(J, c, &cap) < 0)
        return JS_EXCEPTION;
    if (js_call(J, cap.resolve, JS_UNDEFINED, 1, &x) == JS_EXCEPTION)
        return JS_EXCEPTION;
    return cap.promise;
}

static jsval promise_ctor(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc;
    if (js_is_undefined(nt))
        return throw_type(J, "Promise constructor cannot be invoked without 'new'");
    if (!js_is_callable(argv[0]))
        return throw_type(J, "Promise resolver is not a function");
    struct jsobject *proto = js_proto_from_ctor(J, nt, J->proto[P_PROMISE]);
    if (!proto)
        return JS_EXCEPTION;
    jsval p = obj_val(promise_new(J, proto));
    jsval fns[2];
    resolving_functions(J, p, &fns[0], &fns[1]);
    if (js_call(J, argv[0], JS_UNDEFINED, 2, fns) == JS_EXCEPTION) {
        jsval e = js_take_exception(J);
        if (js_call(J, fns[1], JS_UNDEFINED, 1, &e) == JS_EXCEPTION)
            return JS_EXCEPTION;
    }
    return p;
}

static jsval p_then(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    if (!promise_of(this_val))
        return throw_type(J, "Promise.prototype.then called on an object that is not a promise");
    jsval c = js_species_constructor(J, to_obj(this_val), obj_val(J->ctor[C_PROMISE]));
    if (c == JS_EXCEPTION)
        return c;
    struct capability cap;
    if (new_capability(J, c, &cap) < 0)
        return JS_EXCEPTION;
    perform_then(J, this_val, argv[0], argv[1], &cap);
    return cap.promise;
}

static jsval p_catch(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    jsval args[2] = { JS_UNDEFINED, argv[0] };
    return js_call_method(J, this_val, "then", 2, args);
}

/* finally's: thenFinally (magic 0) and catchFinally (1), data [onFinally, C];
 * their value thunk (2) and thrower (3), data the value */
static jsval finally_fn(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    int which = J->native_magic;
    jsval data = native_data(J);
    if (which == 2)
        return data;
    if (which == 3)
        return js_throw(J, data);
    jsval on_finally = js_get_index(J, data, 0), c = js_get_index(J, data, 1);
    jsval result = js_call(J, on_finally, JS_UNDEFINED, 0, 0);
    if (result == JS_EXCEPTION)
        return result;
    jsval p = promise_resolve(J, c, result);
    if (p == JS_EXCEPTION)
        return p;
    jsval thunk = obj_val(make_native_data(J, "", finally_fn, 0, which == 0 ? 2 : 3, argv[0]));
    return js_call_method(J, p, "then", 1, &thunk);
}

static jsval p_finally(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    if (!js_is_object(this_val))
        return throw_type(J, "Promise.prototype.finally called on a non-object");
    jsval c = js_species_constructor(J, to_obj(this_val), obj_val(J->ctor[C_PROMISE]));
    if (c == JS_EXCEPTION)
        return c;
    jsval args[2] = { argv[0], argv[0] };
    if (js_is_callable(argv[0])) {
        jsval data = list2(J, argv[0], c);
        args[0] = obj_val(make_native_data(J, "", finally_fn, 1, 0, data));
        args[1] = obj_val(make_native_data(J, "", finally_fn, 1, 1, data));
    }
    return js_call_method(J, this_val, "then", 2, args);
}

static jsval s_resolve(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    if (!js_is_object(this_val))
        return throw_type(J, "Promise.resolve called on a non-object");
    return promise_resolve(J, this_val, argv[0]);
}

static jsval s_reject(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    struct capability cap;
    if (new_capability(J, this_val, &cap) < 0)
        return JS_EXCEPTION;
    if (js_call(J, cap.reject, JS_UNDEFINED, 1, &argv[0]) == JS_EXCEPTION)
        return JS_EXCEPTION;
    return cap.promise;
}

static jsval s_with_resolvers(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)argv, (void)nt;
    struct capability cap;
    if (new_capability(J, this_val, &cap) < 0)
        return JS_EXCEPTION;
    jsval o = js_new_object(J);
    js_set(J, o, "promise", cap.promise);
    js_set(J, o, "resolve", cap.resolve);
    js_set(J, o, "reject", cap.reject);
    return o;
}

/* all (magic 0), allSettled (1), any (2), race (3). An element's function
 * has data [state, index, already called]; state is [values, remaining,
 * resolve, reject]. */
enum { COMBINE_ALL, COMBINE_SETTLED, COMBINE_ANY, COMBINE_RACE };

static jsval settled_record(struct js *J, bool fulfilled, jsval v)
{
    jsval o = js_new_object(J);
    js_set(J, o, "status", js_cstring(J, fulfilled ? "fulfilled" : "rejected"));
    js_set(J, o, fulfilled ? "value" : "reason", v);
    return o;
}

/* magic: COMBINE_ALL's resolve element, COMBINE_SETTLED's (+0 fulfilled,
 * +4 rejected), COMBINE_ANY's reject element */
static jsval element_fn(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    int which = J->native_magic;
    jsval data = native_data(J);
    if (js_get_index(J, data, 2) == JS_TRUE)
        return JS_UNDEFINED;
    js_set_index(J, data, 2, JS_TRUE);
    jsval state = js_get_index(J, data, 0);
    uint32_t index = (uint32_t)js_number_value(js_get_index(J, data, 1));
    jsval values = js_get_index(J, state, 0);
    jsval v = argv[0];
    if ((which & 3) == COMBINE_SETTLED)
        v = settled_record(J, which < 4, v);
    js_set_index(J, values, index, v);
    double remaining = js_number_value(js_get_index(J, state, 1)) - 1;
    js_set_index(J, state, 1, js_number(remaining));
    if (remaining > 0)
        return JS_UNDEFINED;
    if ((which & 3) == COMBINE_ANY) {
        jsval e = error_create(J, J->proto[P_AGGREGATE_ERROR], js_cstring(J, "All promises were rejected"));
        if (e == JS_EXCEPTION)
            return e;
        js_define_value(J, e, "errors", values);
        return js_call(J, js_get_index(J, state, 3), JS_UNDEFINED, 1, &e);
    }
    return js_call(J, js_get_index(J, state, 2), JS_UNDEFINED, 1, &values);
}

static jsval combine(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)argc, (void)nt;
    int kind = J->native_magic;
    struct capability cap;
    if (new_capability(J, this_val, &cap) < 0)
        return JS_EXCEPTION;
    jsval c = this_val;
    jsval resolve_fn = js_get_value(J, c, key_cstr(J, "resolve"));
    struct iter it;
    bool opened = false;
    if (resolve_fn == JS_EXCEPTION)
        goto failed;
    if (!js_is_callable(resolve_fn)) {
        throw_type(J, "Promise resolve is not a function");
        goto failed;
    }
    if (iter_open(J, argv[0], &it) < 0)
        goto failed;
    opened = true;
    jsval state = js_new_array(J, 0);
    js_set_index(J, state, 0, js_new_array(J, 0));
    js_set_index(J, state, 1, js_number(1));
    js_set_index(J, state, 2, cap.resolve);
    js_set_index(J, state, 3, cap.reject);
    for (uint32_t index = 0;; index++) {
        jsval v;
        int got = iter_step(J, &it, &v);
        if (got < 0) {
            opened = false;                     /* (an iterator that throws is not closed) */
            goto failed;
        }
        if (!got)
            break;
        jsval next = js_call(J, resolve_fn, c, 1, &v);
        if (next == JS_EXCEPTION)
            goto failed;
        jsval args[2] = { cap.resolve, cap.reject };
        if (kind != COMBINE_RACE) {
            js_set_index(J, js_get_index(J, state, 0), index, JS_UNDEFINED);
            js_set_index(J, state, 1, js_number(js_number_value(js_get_index(J, state, 1)) + 1));
            jsval parts[3] = { state, js_number(index), JS_FALSE };
            jsval data = js_create_from_list(J, parts, 3);
            if (kind == COMBINE_ALL)
                args[0] = obj_val(make_native_data(J, "", element_fn, 1, COMBINE_ALL, data));
            else if (kind == COMBINE_SETTLED) {     /* (one "already called" for both) */
                args[0] = obj_val(make_native_data(J, "", element_fn, 1, COMBINE_SETTLED, data));
                args[1] = obj_val(make_native_data(J, "", element_fn, 1, COMBINE_SETTLED + 4, data));
            } else
                args[1] = obj_val(make_native_data(J, "", element_fn, 1, COMBINE_ANY, data));
        }
        if (js_call_method(J, next, "then", 2, args) == JS_EXCEPTION)
            goto failed;
    }
    if (kind != COMBINE_RACE) {
        double remaining = js_number_value(js_get_index(J, state, 1)) - 1;
        js_set_index(J, state, 1, js_number(remaining));
        if (remaining == 0) {
            jsval values = js_get_index(J, state, 0);
            if (kind == COMBINE_ANY) {
                jsval e = error_create(J, J->proto[P_AGGREGATE_ERROR], js_cstring(J, "All promises were rejected"));
                js_define_value(J, e, "errors", values);
                if (js_call(J, cap.reject, JS_UNDEFINED, 1, &e) == JS_EXCEPTION)
                    return JS_EXCEPTION;
            } else if (js_call(J, cap.resolve, JS_UNDEFINED, 1, &values) == JS_EXCEPTION)
                return JS_EXCEPTION;
        }
    }
    return cap.promise;
failed: {
        jsval e = js_take_exception(J);
        if (opened)
            iter_close(J, &it, true);
        if (js_call(J, cap.reject, JS_UNDEFINED, 1, &e) == JS_EXCEPTION)
            return JS_EXCEPTION;
        return cap.promise;
    }
}

static jsval species_self(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)J, (void)argc, (void)argv, (void)nt;
    return this_val;
}

/* --- async functions ------------------------------------------------------------ */

static void async_step(struct js *J, struct jsobject *g, int mode, jsval v);

/* an await's reactions: go on (magic RESUME_NEXT) or throw there; data the run */
static jsval await_reaction(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    int mode = J->native_magic;
    jsval g = native_data(J);
    async_step(J, to_obj(g), mode, argv[0]);
    return JS_UNDEFINED;
}

static void async_step(struct js *J, struct jsobject *g, int mode, jsval v)
{
    bool done;
    jsval r = gen_resume(J, g, mode, v, &done);
    jsval promise = g->u.gen->promise;
    if (r == JS_EXCEPTION) {
        settle(J, promise, PROMISE_REJECTED, js_take_exception(J));
        return;
    }
    if (done) {
        resolve_promise(J, promise, r);
        return;
    }
    jsval p = promise_resolve(J, obj_val(J->ctor[C_PROMISE]), r);     /* (await r) */
    if (p == JS_EXCEPTION) {
        async_step(J, g, RESUME_THROW, js_take_exception(J));
        return;
    }
    jsval on_fulfilled = obj_val(make_native_data(J, "", await_reaction, 1, RESUME_NEXT, obj_val(g)));
    jsval on_rejected = obj_val(make_native_data(J, "", await_reaction, 1, RESUME_THROW, obj_val(g)));
    perform_then(J, p, on_fulfilled, on_rejected, 0);
}

jsval async_begin(struct js *J, struct jsobject *g)
{
    jsval promise = g->u.gen->promise;
    async_step(J, g, RESUME_NEXT, JS_UNDEFINED);
    return promise;
}

/* --- the realm ----------------------------------------------------------------- */

void init_generator(struct js *J)
{
    struct jsobject *gp = obj_new(J, J->proto[P_ITERATOR], CLASS_OBJECT);
    J->proto[P_GENERATOR] = gp;
    obj_define_native(J, gp, "next", gen_method, 1, RESUME_NEXT);
    obj_define_native(J, gp, "return", gen_method, 1, RESUME_RETURN);
    obj_define_native(J, gp, "throw", gen_method, 1, RESUME_THROW);
    tag_object(J, gp, "Generator");

    struct jsobject *pp = obj_new(J, J->proto[P_OBJECT], CLASS_OBJECT);
    J->proto[P_PROMISE] = pp;
    struct jsobject *pc = make_ctor(J, "Promise", promise_ctor, 1, pp);
    J->ctor[C_PROMISE] = pc;
    obj_define_native(J, pp, "then", p_then, 2, 0);
    obj_define_native(J, pp, "catch", p_catch, 1, 0);
    obj_define_native(J, pp, "finally", p_finally, 1, 0);
    tag_object(J, pp, "Promise");
    obj_define_native(J, pc, "resolve", s_resolve, 1, 0);
    obj_define_native(J, pc, "reject", s_reject, 1, 0);
    obj_define_native(J, pc, "withResolvers", s_with_resolvers, 0, 0);
    obj_define_native(J, pc, "all", combine, 1, COMBINE_ALL);
    obj_define_native(J, pc, "allSettled", combine, 1, COMBINE_SETTLED);
    obj_define_native(J, pc, "any", combine, 1, COMBINE_ANY);
    obj_define_native(J, pc, "race", combine, 1, COMBINE_RACE);
    struct jsobject *sg = make_native(J, "get [Symbol.species]", species_self, 0, 0);
    obj_define_accessor(J, pc, J->symbol[SYM_SPECIES], obj_val(sg), JS_UNDEFINED, PROP_CONFIGURABLE);
    J->reaction_job = obj_val(make_native(J, "", reaction_job, 2, 0));
    J->thenable_job = obj_val(make_native(J, "", thenable_job, 3, 0));
}

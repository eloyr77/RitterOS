/*
 * RitterOS's JavaScript engine (27o, roadmap item 27; the user's: our own,
 * written from the ECMAScript specification, an interpreter). The language
 * is parsed into a tree, compiled into a small stack code per function and
 * run by one loop (interp.c); values, objects and the collector are in
 * gc.c, string.c and object.c, the standard objects in b_*.c.
 *
 * This header is what programs see: `js` (user/bin/js.c) and iBrowser
 * (27p). It is private to the tree (not in /system/include, not in the
 * SDK): the engine is linked into each program, not a shared library.
 *
 * One `struct js` is one world of objects (a realm with its global object)
 * and runs on one thread at a time. Values are 64-bit words (jsval): a
 * double, or a tag and a pointer in the space of negative quiet NaNs.
 * The collector finds the values a C function holds in its locals by
 * looking at the C stack (conservatively), so native code needs no
 * handles; values kept in memory of its own (malloc) must be given to
 * js_root_push while they live.
 */
#ifndef RITTEROS_JS_H
#define RITTEROS_JS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint64_t jsval;
struct js;

/* A function written in C: `this`, the arguments (argv[0..argc), always at
 * least as many as the function's declared length, padded with undefined),
 * and new.target (undefined for a plain call). Returns the result, or
 * JS_EXCEPTION after js_throw* (the exception is kept in the world). */
typedef jsval (*js_native)(struct js *J, jsval this_val, int argc, jsval *argv, jsval new_target);

/* --- worlds ------------------------------------------------------------- */

struct js *js_new(void);
void js_free(struct js *J);

/* Where the C stack of the thread running scripts begins (an address in
 * the outermost frame that will call the engine, e.g. a local of main):
 * the collector looks at the stack between there and its own frame. */
void js_set_stack_base(struct js *J, void *base);

/* Runs a script (UTF-8 text). Returns the value of its last expression
 * statement, or JS_EXCEPTION (the error is js_take_exception's). `file`
 * names it in messages and error objects. */
jsval js_eval(struct js *J, const char *text, size_t len, const char *file);

/* The pending exception (after JS_EXCEPTION), taken: none pending after. */
jsval js_take_exception(struct js *J);

/* An uncaught exception as one line ("TypeError: x is not a function at
 * FILE:LINE"), malloc'd UTF-8. */
char *js_exception_text(struct js *J, jsval exception);

/* --- values ------------------------------------------------------------- */

#define JS_TAG_SHIFT 47
#define JS_TAG_BASE  ((uint64_t)0x1FFF << 51)
#define JS_MAKE_TAG(t) (JS_TAG_BASE | ((uint64_t)(t) << JS_TAG_SHIFT))

enum {
    JS_T_NUMBER = 0,                            /* (not a tag: every other bit pattern) */
    JS_T_UNDEFINED = 1,
    JS_T_NULL = 2,
    JS_T_BOOL = 3,
    JS_T_EMPTY = 4,                             /* internal: an array hole, a binding not yet initialized */
    JS_T_EXCEPTION = 5,                         /* internal: "an exception is pending" */
    JS_T_OBJECT = 8,                            /* tags from here on carry a cell pointer */
    JS_T_STRING = 9,
    JS_T_SYMBOL = 10,
    JS_T_CELL = 11,                             /* internal cells (scopes, compiled functions) */
};

#define JS_UNDEFINED JS_MAKE_TAG(JS_T_UNDEFINED)
#define JS_NULL      JS_MAKE_TAG(JS_T_NULL)
#define JS_FALSE     JS_MAKE_TAG(JS_T_BOOL)
#define JS_TRUE      (JS_MAKE_TAG(JS_T_BOOL) | 1)
#define JS_EMPTY     JS_MAKE_TAG(JS_T_EMPTY)
#define JS_EXCEPTION JS_MAKE_TAG(JS_T_EXCEPTION)

static inline int js_tag(jsval v)
{
    return (v >> 51) == 0x1FFF ? (int)((v >> JS_TAG_SHIFT) & 15) : JS_T_NUMBER;
}
static inline bool js_is_number(jsval v) { return (v >> 51) != 0x1FFF; }
static inline bool js_is_undefined(jsval v) { return v == JS_UNDEFINED; }
static inline bool js_is_null(jsval v) { return v == JS_NULL; }
static inline bool js_is_nullish(jsval v) { return v == JS_UNDEFINED || v == JS_NULL; }
static inline bool js_is_bool(jsval v) { return js_tag(v) == JS_T_BOOL; }
static inline bool js_is_object(jsval v) { return js_tag(v) == JS_T_OBJECT; }
static inline bool js_is_string(jsval v) { return js_tag(v) == JS_T_STRING; }
static inline bool js_is_symbol(jsval v) { return js_tag(v) == JS_T_SYMBOL; }
static inline bool js_is_exception(jsval v) { return v == JS_EXCEPTION; }

static inline double js_number_value(jsval v)
{
    double d;
    memcpy(&d, &v, 8);
    return d;
}
static inline jsval js_number(double d)
{
    jsval v;
    if (d != d)
        return (jsval)0x7FF8000000000000ull;    /* every NaN is this one (the others are tags) */
    memcpy(&v, &d, 8);
    return v;
}
static inline jsval js_bool(bool b) { return b ? JS_TRUE : JS_FALSE; }
static inline void *js_pointer(jsval v) { return (void *)(uintptr_t)(v & (((uint64_t)1 << JS_TAG_SHIFT) - 1)); }
static inline jsval js_tagged(int tag, const void *p) { return JS_MAKE_TAG(tag) | (uint64_t)(uintptr_t)p; }

/* A string from UTF-8 (bytes that are not UTF-8 become U+FFFD). */
jsval js_string(struct js *J, const char *utf8, size_t len);
jsval js_cstring(struct js *J, const char *utf8);
/* A value as UTF-8 by JavaScript's ToString (malloc'd; 0 and an exception
 * pending if the conversion threw). */
char *js_to_utf8(struct js *J, jsval v, size_t *len);

/* ToString / ToNumber / ToBoolean (JS_EXCEPTION or a string/number). */
jsval js_to_string(struct js *J, jsval v);
double js_to_number(struct js *J, jsval v, bool *threw);
bool js_to_boolean(jsval v);

/* --- objects ------------------------------------------------------------ */

jsval js_global(struct js *J);
jsval js_new_object(struct js *J);
jsval js_new_array(struct js *J, uint32_t length);
jsval js_new_function(struct js *J, const char *name, js_native fn, int length);
jsval js_new_error(struct js *J, const char *kind, const char *message);

/* Properties by UTF-8 name: JS_EXCEPTION on a throw. */
jsval js_get(struct js *J, jsval obj, const char *name);
int js_set(struct js *J, jsval obj, const char *name, jsval value);   /* -1 on a throw */
jsval js_get_index(struct js *J, jsval obj, uint32_t index);
int js_set_index(struct js *J, jsval obj, uint32_t index, jsval value);
/* A property that scripts see as the standard objects' own: writable and
 * configurable but not enumerable (functions on a prototype). */
void js_define_method(struct js *J, jsval obj, const char *name, js_native fn, int length);
void js_define_value(struct js *J, jsval obj, const char *name, jsval value);

jsval js_call(struct js *J, jsval f, jsval this_val, int argc, jsval *argv);
jsval js_construct(struct js *J, jsval f, int argc, jsval *argv);
bool js_is_callable(jsval v);

/* Throwing from native code: sets the pending exception and returns
 * JS_EXCEPTION, so `return js_throw_type(J, "...")`. */
jsval js_throw(struct js *J, jsval exception);
jsval js_throw_type(struct js *J, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
jsval js_throw_range(struct js *J, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
jsval js_throw_reference(struct js *J, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
jsval js_throw_syntax(struct js *J, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* (27p) Objects of a program's own: a script object carrying a C pointer.
 * The class names it (Object.prototype.toString shows nothing of it) and
 * may free the data when the object is collected, or mark the values the
 * data holds (js_mark from trace). */
struct js_host_class {
    const char *name;
    void (*finalize)(void *data);
    void (*trace)(struct js *J, void *data);
};
jsval js_new_host(struct js *J, jsval proto, void *data, const struct js_host_class *cls);
void *js_host_data(jsval v, const struct js_host_class *cls);   /* 0 if v is not one of cls */
void js_mark(struct js *J, jsval v);
/* An object with this prototype (an object, or JS_NULL). */
jsval js_new_object_proto(struct js *J, jsval proto);
jsval js_get_proto(struct js *J, jsval obj);
/* An accessor property (as the web's interfaces define theirs: enumerable
 * and configurable); set may be 0 (read-only). */
void js_define_accessor(struct js *J, jsval obj, const char *name, js_native get, js_native set);
void js_define_accessor_magic(struct js *J, jsval obj, const char *name, js_native get, js_native set, int magic);
/* Native functions sharing one C function: js_magic is the number of the
 * function being called (read it first: a call it makes changes it). */
jsval js_new_function_magic(struct js *J, const char *name, js_native fn, int length, int magic);
void js_define_method_magic(struct js *J, jsval obj, const char *name, js_native fn, int length, int magic);
int js_magic(struct js *J);
/* A constructor: its `prototype` is proto (non-writable), proto.constructor
 * is it. */
jsval js_new_constructor(struct js *J, const char *name, js_native fn, int length, jsval proto);
int js_delete(struct js *J, jsval obj, const char *name);     /* -1 on a throw */
bool js_is_array(jsval v);
uint32_t js_array_length(jsval v);
/* Throws an Error whose name is `name` (DOMException's kinds). */
jsval js_throw_named(struct js *J, const char *name, const char *message);

/* (27q) A key that no script can name or list (a private name): a
 * program's own data on script objects, read and written by key. */
jsval js_new_private_key(struct js *J, const char *description);
jsval js_get_key(struct js *J, jsval obj, jsval key);
int js_set_key(struct js *J, jsval obj, jsval key, jsval value);     /* -1 on a throw */

/* (27q) The job queue (the web's microtasks; Promise reactions in 27r):
 * fn(args) queued to run when the running code is done, in order (at most
 * 16 arguments). js_run_jobs runs them and what they queue: JS_UNDEFINED
 * when none is left, JS_EXCEPTION if one threw (the rest stay queued: call
 * it again after taking the exception). */
void js_queue_job(struct js *J, jsval fn, int argc, jsval *argv);
bool js_jobs_pending(struct js *J);
jsval js_run_jobs(struct js *J);

/* Values kept outside the stack and the engine's objects (a C array of
 * jsvals the caller owns) are roots while pushed; pops in reverse order. */
void js_root_push(struct js *J, jsval *values, size_t count);
void js_root_pop(struct js *J);

/* The collector, now (normally it runs by itself as memory is used). */
void js_gc(struct js *J);

#endif

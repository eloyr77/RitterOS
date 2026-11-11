/*
 * The engine's inside (27o): cells, strings, objects, compiled functions
 * and the interpreter's state, shared by the files of user/lib/js/.
 *
 * Every value that is not a number, boolean, null or undefined points to a
 * cell: a block of the collector's heap (gc.c) that starts with struct
 * cell. Strings are UTF-16 code units, as the language counts them; a
 * long concatenation is a rope until its characters are needed. Property
 * names are atoms (strings kept once each, compared by pointer) or
 * symbols. Objects keep their properties in insertion order (struct
 * propmap) and arrays their elements densely while they can.
 */
#ifndef RITTEROS_JSINT_H
#define RITTEROS_JSINT_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "js.h"

#define JS_NOINLINE __attribute__((noinline))

/* --- cells -------------------------------------------------------------- */

enum cell_kind {
    CELL_FREE = 0,
    CELL_STRING,
    CELL_SYMBOL,
    CELL_OBJECT,
    CELL_SCOPE,
    CELL_FUNCDEF,
    CELL_ACCESSOR,
};

struct cell {
    uint8_t kind;
    uint8_t mark;
    uint16_t flags;                             /* the kind's own */
    uint32_t extra;                             /* the kind's own (a string's length, an object's class) */
};

void *gc_alloc(struct js *J, enum cell_kind kind, size_t size);   /* zeroed */
void gc_note_external(struct js *J, size_t bytes);  /* malloc'd memory a cell owns, counted toward the next run */

/* --- strings ------------------------------------------------------------ */

enum {
    STR_ATOM = 1,                               /* in the atom table */
    STR_ROPE = 2,                               /* left + right, or `flat` once flattened */
    STR_FLATTENED = 4,                          /* a rope whose characters are in `flat` */
    STR_INDEX = 8,                              /* an atom that is an array index ("0".."4294967294") */
    STR_HASHED = 16,
};

struct jsstring {
    struct cell h;                              /* h.extra = the length in code units */
    uint32_t hash;
    uint32_t index;                             /* the index's value (STR_INDEX) */
    struct jsstring *left, *right, *flat;       /* ropes only */
    uint16_t chars[];                           /* flat strings only */
};

#define STR_MAX_LEN ((uint32_t)1 << 30)

static inline uint32_t str_len(const struct jsstring *s) { return s->h.extra; }
static inline struct jsstring *to_str(jsval v) { return (struct jsstring *)js_pointer(v); }
static inline jsval str_val(const struct jsstring *s) { return js_tagged(JS_T_STRING, s); }

struct jsstring *str_alloc(struct js *J, uint32_t len);          /* flat, characters uninitialized */
struct jsstring *str_flat(struct js *J, struct jsstring *s);     /* the flat string with s's characters */
const uint16_t *str_chars(struct js *J, struct jsstring *s);
struct jsstring *str_from_utf8(struct js *J, const char *s, size_t n);
struct jsstring *str_from_ascii(struct js *J, const char *s);
struct jsstring *str_from_u16(struct js *J, const uint16_t *s, uint32_t n);
struct jsstring *str_concat(struct js *J, struct jsstring *a, struct jsstring *b);  /* 0: too long (thrown) */
struct jsstring *str_sub(struct js *J, struct jsstring *s, uint32_t from, uint32_t to);
bool str_equal(struct js *J, struct jsstring *a, struct jsstring *b);
int str_compare(struct js *J, struct jsstring *a, struct jsstring *b);
uint32_t str_hash(struct js *J, struct jsstring *s);
char *str_to_utf8(struct js *J, struct jsstring *s, size_t *len);  /* malloc'd; lone surrogates as U+FFFD */
uint32_t str_char_at(struct js *J, struct jsstring *s, uint32_t i);
int32_t str_index_of(struct js *J, struct jsstring *s, struct jsstring *what, uint32_t from);
bool str_is_ascii_cstr(struct js *J, struct jsstring *s, const char *c);
struct jsstring *str_from_char(struct js *J, uint32_t unit);    /* one code unit (cached below 256) */

/* Atoms: one string per text. */
struct jsstring *atom(struct js *J, struct jsstring *s);
struct jsstring *atom_cstr(struct js *J, const char *ascii);
struct jsstring *atom_u16(struct js *J, const uint16_t *s, uint32_t n);
struct jsstring *atom_index(struct js *J, uint32_t index);
bool parse_index(const uint16_t *s, uint32_t n, uint32_t *index);   /* "0".."4294967294", canonical */
void atoms_sweep(struct js *J);

/* A string under construction (code units, malloc'd). */
struct sbuf {
    uint16_t *s;
    uint32_t n, cap;
    bool failed;                                /* grew past STR_MAX_LEN */
};
void sb_unit(struct sbuf *b, uint32_t unit);
void sb_code_point(struct sbuf *b, uint32_t cp);
void sb_ascii(struct sbuf *b, const char *s);
void sb_units(struct sbuf *b, const uint16_t *s, uint32_t n);
void sb_str(struct js *J, struct sbuf *b, struct jsstring *s);
struct jsstring *sb_finish(struct js *J, struct sbuf *b);   /* 0 (a RangeError thrown) when too long */
void sb_free(struct sbuf *b);

/* Numbers and text (numconv.c). */
struct jsstring *number_to_string(struct js *J, double d, int radix);
int number_to_ascii(double d, char *buf);       /* radix 10, at most 32 bytes with the NUL */
double string_to_number(const uint16_t *s, uint32_t n);    /* StringToNumber: NaN if not a number */
double parse_float_prefix(const uint16_t *s, uint32_t n, uint32_t *used);
int shortest_digits(double d, char *digits, int *point);   /* d > 0 finite: digits, decimal exponent */
bool js_is_space(uint32_t c);                   /* WhiteSpace or LineTerminator */
bool js_is_line_terminator(uint32_t c);

/* --- symbols ------------------------------------------------------------ */

struct jssymbol {
    struct cell h;                              /* h.flags 1: in Symbol.for's registry */
    struct jsstring *description;               /* 0 for undefined */
    uint32_t hash;
};
static inline struct jssymbol *to_sym(jsval v) { return (struct jssymbol *)js_pointer(v); }

/* --- objects ------------------------------------------------------------ */

enum {
    PROP_WRITABLE = 1,
    PROP_ENUMERABLE = 2,
    PROP_CONFIGURABLE = 4,
    PROP_ACCESSOR = 8,                          /* value is an accessor cell */
    PROP_DEFAULT = 7,
};

struct accessor {
    struct cell h;
    jsval get, set;                             /* undefined when missing */
};

struct prop {
    jsval key;                                  /* an atom or a symbol; JS_EMPTY when deleted */
    jsval value;
    uint32_t flags;
    uint32_t pad;
};

struct propmap {
    struct prop *e;
    uint32_t n, live, cap;                      /* entries used (with deleted ones), live, allocated */
    uint32_t *index;                            /* open hash of entry numbers + 1, from 9 entries on */
    uint32_t index_cap;
};

enum obj_class {
    CLASS_OBJECT,
    CLASS_ARRAY,
    CLASS_FUNCTION,                             /* a compiled function's closure */
    CLASS_NATIVE,
    CLASS_BOUND,
    CLASS_ERROR,
    CLASS_BOOLEAN,
    CLASS_NUMBER,
    CLASS_STRING,
    CLASS_SYMBOL,
    CLASS_DATE,
    CLASS_REGEXP,
    CLASS_MAP,
    CLASS_SET,
    CLASS_WEAKMAP,
    CLASS_WEAKSET,
    CLASS_ARGUMENTS,
    CLASS_ITERATOR,                             /* the built-in iterators (u.iter) */
    CLASS_FOR_IN,                               /* internal: a for-in walk */
    CLASS_GENERATOR,                            /* (27r) a generator, or an async function's run (u.gen) */
    CLASS_PROMISE,                              /* (27r) u.promise */
    CLASS_HOST,
    CLASS_PROXY,                                /* (64r) u.proxy, b_proxy.c */
};

enum {
    OBJ_EXTENSIBLE = 1,
    OBJ_CALLABLE = 2,
    OBJ_CONSTRUCTOR = 4,
    OBJ_SPARSE = 8,                             /* an array whose elements are properties */
    OBJ_LENGTH_FIXED = 16,                      /* an array whose length is not writable */
    OBJ_CLASS_CTOR = 32,                        /* a class: [[Call]] throws */
    OBJ_DERIVED = 64,                           /* a class with extends: `this` comes from super() */
    OBJ_HAS_INDEX = 128,                        /* some property key is an array index (key order) */
    OBJ_ERROR_DATA = 256,                       /* (errors) the file and line were recorded */
};

struct mapentry {
    jsval key, value;                           /* key JS_EMPTY: deleted */
    uint32_t hash;
    uint32_t next;                              /* the hash chain (entry number + 1) */
};
struct mapdata {
    struct mapentry *e;
    uint32_t n, live, cap;
    uint32_t *buckets;
    uint32_t nbuckets;
    uint32_t iterators;                         /* live iterators: no compaction while any */
};

enum iter_kind {
    ITER_ARRAY_VALUES, ITER_ARRAY_KEYS, ITER_ARRAY_ENTRIES,
    ITER_STRING,
    ITER_MAP_KEYS, ITER_MAP_VALUES, ITER_MAP_ENTRIES,
    ITER_SET_VALUES, ITER_SET_ENTRIES,
    ITER_REGEXP_STRING,                         /* matchAll */
};

struct funcdef;
struct scope;
struct regex;

struct jsobject {
    struct cell h;                              /* h.extra = enum obj_class; h.flags = OBJ_* */
    struct jsobject *proto;
    struct propmap props;
    jsval *elems;                               /* arrays and arguments: [0, length) while not sparse */
    uint32_t length, cap;
    union {
        struct {
            struct funcdef *def;
            struct scope *env;
            struct jsobject *home;              /* the object a method's `super` looks above */
            jsval fields;                       /* a class's field initializer (a function) or undefined */
        } fn;
        struct {
            js_native fn;
            int16_t length;
            int16_t magic;
            jsval data;
        } native;
        struct {
            jsval target, this_val;
            jsval *args;
            uint32_t argc;
        } bound;
        jsval prim;                             /* Boolean, Number, String, Symbol, Date (its time) */
        struct {
            struct jsstring *source, *flags;
            struct regex *prog;
        } re;
        struct mapdata *map;
        struct {
            jsval target;
            uint32_t index;
            uint8_t kind;
            bool done;
            jsval extra;                        /* matchAll: the string */
        } iter;
        struct {
            jsval *keys;
            uint32_t n, at;
            jsval object;
        } forin;
        struct {
            struct jsstring *file;
            int line;
        } err;
        struct {
            struct scope *scope;                /* sloppy arguments: elements that are the parameters */
            int32_t *map;                       /* map[i]: the parameter's slot, -1 when unmapped */
            uint32_t nmap;
        } args;
        struct {
            void *data;
            const struct js_host_class *cls;
        } host;
        struct {
            jsval target, handler;              /* both null once revoked */
        } proxy;                                /* (64r) */
        struct genstate *gen;                   /* (27r) */
        struct promise *promise;                /* (27r) */
    } u;
};

/* (27r) A generator's frame while it is not running: its arguments, then
 * its registers, then its operand stack, copied off the value stack; its
 * place and its open try handlers (as offsets: they go back elsewhere). An
 * async function runs on one too: `await` suspends it like `yield`. */
enum { GEN_START, GEN_SUSPENDED, GEN_RUNNING, GEN_DONE };
enum { RESUME_NEXT, RESUME_THROW, RESUME_RETURN };

struct savedhandler {
    uint32_t pc, sp;                            /* offsets: into the code, above the operand stack's base */
    struct scope *scope;
};

struct genstate {
    struct jsobject *callee;
    jsval this_val, new_target;
    jsval *saved;
    uint32_t argc, nregs, depth;
    struct scope *scope;
    uint32_t pc;
    struct savedhandler *handlers;
    uint32_t nhandlers;
    uint8_t state;
    bool async;
    jsval promise;                              /* async: the promise the call returned */
};

/* (27r) A promise: its state and result, and the reactions waiting for it
 * (an array of [kind, handler, capability promise, resolve, reject]). */
enum { PROMISE_PENDING, PROMISE_FULFILLED, PROMISE_REJECTED };

struct promise {
    uint8_t state;
    bool handled;
    jsval result;
    jsval reactions;
};

static inline struct jsobject *to_obj(jsval v) { return (struct jsobject *)js_pointer(v); }
static inline jsval obj_val(const struct jsobject *o) { return js_tagged(JS_T_OBJECT, o); }
static inline int obj_class(const struct jsobject *o) { return (int)o->h.extra; }

/* Keys */
static inline bool key_is_index(jsval key)
{
    return js_is_string(key) && (to_str(key)->h.flags & STR_INDEX);
}
static inline uint32_t key_index(jsval key) { return to_str(key)->index; }
jsval key_from_value(struct js *J, jsval v);   /* ToPropertyKey: an atom or a symbol, or JS_EXCEPTION */
jsval key_cstr(struct js *J, const char *ascii);
uint32_t key_hash(jsval key);

struct jsobject *obj_new(struct js *J, struct jsobject *proto, int cls);
struct jsobject *array_new(struct js *J, uint32_t length);
bool array_grow(struct js *J, struct jsobject *a, uint32_t cap);
void array_make_sparse(struct js *J, struct jsobject *a);
struct prop *prop_find(struct propmap *m, jsval key);
struct prop *prop_add(struct js *J, struct jsobject *o, jsval key, jsval value, uint32_t flags);
void prop_remove(struct propmap *m, struct prop *p);
void propmap_free(struct propmap *m);

/* A property's description (ordinary [[GetOwnProperty]] / [[DefineOwnProperty]]). */
struct pdesc {
    jsval value, get, set;
    uint32_t flags;                             /* PROP_* of what is there */
    uint32_t has;                               /* which fields a definition gives: DESC_* */
};
enum {
    DESC_VALUE = 1, DESC_WRITABLE = 2, DESC_GET = 4, DESC_SET = 8,
    DESC_ENUMERABLE = 16, DESC_CONFIGURABLE = 32,
};
int obj_get_own(struct js *J, struct jsobject *o, jsval key, struct pdesc *d);   /* 1 found, 0 not, -1 threw */
int obj_define(struct js *J, struct jsobject *o, jsval key, const struct pdesc *d, bool throw_on_fail);
jsval obj_get(struct js *J, struct jsobject *o, jsval key, jsval receiver);
int obj_set(struct js *J, struct jsobject *o, jsval key, jsval value, jsval receiver, bool strict);
int obj_has(struct js *J, struct jsobject *o, jsval key);
int obj_delete(struct js *J, struct jsobject *o, jsval key, bool strict);
/* (64r) [[GetPrototypeOf]] (0, or -1 on a throw), [[IsExtensible]] (1, 0 or
 * -1): a proxy's traps may run. */
int obj_get_proto(struct js *J, struct jsobject *o, struct jsobject **proto);
int obj_is_extensible(struct js *J, struct jsobject *o);
int value_is_array(struct js *J, jsval v);         /* IsArray: 1, 0, -1 (a revoked proxy) */
/* Property descriptors to and from their objects (b_object.c). */
int to_desc(struct js *J, jsval v, struct pdesc *d);
jsval from_desc(struct js *J, const struct pdesc *d);
/* (64r) The internal methods of a proxy (b_proxy.c); object.c sends a
 * CLASS_PROXY object to these. */
int proxy_get_own(struct js *J, struct jsobject *o, jsval key, struct pdesc *d);
int proxy_define(struct js *J, struct jsobject *o, jsval key, const struct pdesc *d, bool throw_on_fail);
int proxy_has(struct js *J, struct jsobject *o, jsval key);
jsval proxy_get(struct js *J, struct jsobject *o, jsval key, jsval receiver);
int proxy_set(struct js *J, struct jsobject *o, jsval key, jsval v, jsval receiver, bool strict);
int proxy_delete(struct js *J, struct jsobject *o, jsval key, bool strict);
int proxy_own_keys(struct js *J, struct jsobject *o, jsval **keys, uint32_t *n);
int proxy_get_proto(struct js *J, struct jsobject *o, struct jsobject **proto);
int proxy_set_proto(struct js *J, struct jsobject *o, struct jsobject *proto);
int proxy_is_extensible(struct js *J, struct jsobject *o);
int proxy_prevent_extensions(struct js *J, struct jsobject *o);
jsval proxy_call(struct js *J, struct jsobject *o, jsval this_val, int argc, jsval *argv, jsval new_target);
int proxy_is_array(struct js *J, struct jsobject *o);
/* Own keys in the language's order (indices, strings, symbols), as a malloc'd
 * array (rooted by the caller while used); flags: 1 strings, 2 symbols,
 * 4 enumerable only. Returns -1 on a throw. */
int obj_own_keys(struct js *J, struct jsobject *o, int flags, jsval **keys, uint32_t *n);
void obj_define_value(struct js *J, struct jsobject *o, jsval key, jsval v, uint32_t flags);
void obj_define_native(struct js *J, struct jsobject *o, const char *name, js_native fn, int length, int magic);
void obj_define_accessor(struct js *J, struct jsobject *o, jsval key, jsval get, jsval set, uint32_t flags);
int obj_prevent_extensions(struct js *J, struct jsobject *o);
int obj_set_level(struct js *J, struct jsobject *o, bool frozen);  /* seal / freeze */
int obj_test_level(struct js *J, struct jsobject *o, bool frozen);
int obj_set_proto(struct js *J, struct jsobject *o, struct jsobject *proto);   /* 1 ok, 0 refused */
bool obj_is_array(struct jsobject *o);
int array_set_length(struct js *J, struct jsobject *a, uint32_t len, bool strict);

/* Generic operations on values (ToObject first where the language does). */
jsval js_get_value(struct js *J, jsval v, jsval key);
int js_set_value(struct js *J, jsval v, jsval key, jsval value, bool strict);
jsval js_get_cstr(struct js *J, jsval v, const char *name);
struct jsobject *js_to_object(struct js *J, jsval v);   /* 0 on a throw */
jsval js_to_primitive(struct js *J, jsval v, int hint);  /* 0 default, 1 number, 2 string */
jsval js_to_numeric(struct js *J, jsval v);
double js_to_integer(struct js *J, jsval v, bool *threw);   /* ToIntegerOrInfinity */
int32_t js_to_int32(double d);
uint32_t js_to_uint32(double d);
int js_to_length(struct js *J, jsval v, double *len);   /* -1 threw */
struct jsstring *js_to_str(struct js *J, jsval v);      /* 0 on a throw */
struct jsstring *js_to_str_noexcept(struct js *J, jsval v);   /* for messages */
bool js_strict_equal(struct js *J, jsval a, jsval b);
int js_loose_equal(struct js *J, jsval a, jsval b);     /* -1 threw */
bool js_same_value(struct js *J, jsval a, jsval b, bool zero);
struct jsstring *js_typeof(struct js *J, jsval v);
jsval js_instanceof(struct js *J, jsval v, jsval target);
int js_ordinary_has_instance(struct js *J, jsval f, jsval v);
bool js_is_constructor(jsval v);
jsval js_species_constructor(struct js *J, struct jsobject *o, jsval def);
jsval js_get_method(struct js *J, jsval v, jsval key);   /* undefined if nullish, else callable or a throw */
jsval js_call_method(struct js *J, jsval obj, const char *name, int argc, jsval *argv);
jsval js_invoke(struct js *J, jsval f, jsval this_val, int argc, jsval *argv, jsval new_target);
struct jsobject *js_proto_from_ctor(struct js *J, jsval new_target, struct jsobject *fallback);
jsval js_create_from_list(struct js *J, jsval *v, uint32_t n);
int js_list_from_array_like(struct js *J, jsval v, jsval **list, uint32_t *n);
char *js_describe(struct js *J, jsval v);      /* a short text of a value for messages (malloc'd) */
jsval js_error_value(struct js *J, struct jsobject *proto, const char *fmt, va_list ap);

/* Iteration (the iterator protocol). */
struct iter {
    jsval it, next;
    bool fast;                                  /* an untouched array iterator: by index */
    struct jsobject *arr;
    uint32_t at;
};
int iter_open(struct js *J, jsval v, struct iter *it);   /* -1 threw */
int iter_step(struct js *J, struct iter *it, jsval *value);  /* 1 a value, 0 done, -1 threw */
int iter_close(struct js *J, struct iter *it, bool on_throw);   /* -1 threw */
jsval iter_result(struct js *J, jsval value, bool done);
jsval make_iterator(struct js *J, struct jsobject *proto, jsval target, int kind);

/* --- scopes and compiled functions ---------------------------------------- */

struct scope {
    struct cell h;                              /* h.extra = number of slots */
    struct scope *parent;
    jsval vars[];
};
struct scope *scope_new(struct js *J, struct scope *parent, uint32_t n);

enum {
    FD_STRICT = 1,
    FD_ARROW = 2,
    FD_METHOD = 4,                              /* object/class methods, getters, setters */
    FD_CLASS_CTOR = 8,
    FD_DERIVED = 16,
    FD_GENERATOR = 32,
    FD_ASYNC = 64,
    FD_SCRIPT = 128,
    FD_NO_CTOR = 256,                           /* methods and arrows cannot be new'd */
    FD_FIELD_INIT = 512,
};

struct linepc { uint32_t pc, line; };

struct funcdef {
    struct cell h;
    uint8_t *code;
    uint32_t code_len;
    jsval *consts;
    uint32_t nconsts;
    uint16_t nregs, nparams;                    /* registers; formal parameters before a default or rest */
    uint16_t flags;
    uint16_t stack_max;                         /* operand stack depth needed */
    struct jsstring *name;
    struct jsstring *file;
    struct jsstring *source;                    /* the whole script, for toString */
    uint32_t src_start, src_end;
    struct linepc *lines;
    uint32_t nlines;
};
static inline jsval cell_val(const void *c) { return js_tagged(JS_T_CELL, c); }

/* compile.c: a script's text to its top-level function (0, a SyntaxError
 * thrown, on errors). */
struct funcdef *js_compile(struct js *J, const char *text, size_t len, const char *file, bool eval_code);
/* `new Function(params, body)`. */
struct funcdef *js_compile_function(struct js *J, struct jsstring *params, struct jsstring *body, int kind);

/* --- the interpreter -------------------------------------------------- */

struct frame {
    struct funcdef *def;
    struct jsobject *callee;
    jsval this_val, new_target;
    jsval *argv;
    uint32_t argc;
    jsval *regs;                                /* nregs registers, then the operand stack */
    jsval *sp_base;
    struct scope *scope;
    const uint8_t *pc;
    uint32_t handlers;                          /* the handler stack's height at entry */
    uint8_t entry;                              /* return to the C caller */
    uint8_t construct;
    struct jsobject *gen;                       /* (27r) the generator it runs, or 0 */
};

struct handler {
    const uint8_t *pc;
    jsval *sp;
    struct scope *scope;
};

struct root_range { jsval *v; size_t n; };

#define VALUE_STACK (1u << 20)
#define FRAME_MAX 10000
#define NATIVE_DEPTH_MAX 600

/* Realm intrinsics kept as roots. */
enum {
    P_OBJECT, P_FUNCTION, P_ARRAY, P_STRING, P_NUMBER, P_BOOLEAN, P_SYMBOL,
    P_ERROR, P_TYPE_ERROR, P_RANGE_ERROR, P_REFERENCE_ERROR, P_SYNTAX_ERROR,
    P_EVAL_ERROR, P_URI_ERROR, P_AGGREGATE_ERROR,
    P_DATE, P_REGEXP, P_MAP, P_SET, P_WEAKMAP, P_WEAKSET,
    P_ITERATOR, P_ARRAY_ITERATOR, P_STRING_ITERATOR, P_MAP_ITERATOR, P_SET_ITERATOR,
    P_REGEXP_STRING_ITERATOR,
    P_GENERATOR, P_PROMISE,                     /* (27r) */
    P_COUNT
};
enum {
    C_OBJECT, C_FUNCTION, C_ARRAY, C_STRING, C_NUMBER, C_BOOLEAN, C_SYMBOL, C_ERROR,
    C_REGEXP, C_PROMISE, C_MAP, C_SET,
    C_COUNT
};

/* Well-known symbols. */
enum {
    SYM_ITERATOR, SYM_ASYNC_ITERATOR, SYM_HAS_INSTANCE, SYM_TO_PRIMITIVE,
    SYM_TO_STRING_TAG, SYM_SPECIES, SYM_IS_CONCAT_SPREADABLE, SYM_UNSCOPABLES,
    SYM_MATCH, SYM_MATCH_ALL, SYM_REPLACE, SYM_SEARCH, SYM_SPLIT,
    SYM_COUNT
};

/* Atoms the engine uses by name (atoms.def). */
enum {
#define A(name, text) A_##name,
#include "atoms.def"
#undef A
    A_COUNT
};

struct block;
struct bigcell;

struct js {
    /* the heap */
    struct block **blocks;
    uint32_t nblocks, blocks_cap;
    struct cell *free_lists[16];
    struct bigcell **bigs;
    uint32_t nbigs, bigs_cap;
    size_t allocated, threshold, live_after;
    struct cell **mark_stack;
    uint32_t mark_n, mark_cap;
    int gc_inhibit;
    void *stack_base;
    struct root_range *roots;
    uint32_t nroots, roots_cap;

    /* atoms */
    struct jsstring **atoms;
    uint32_t natoms, atoms_cap;
    struct jsstring *atom[A_COUNT];
    struct jsstring *char_strings[256];
    uint32_t symbol_count;

    /* the realm */
    struct jsobject *global;
    struct jsobject *proto[P_COUNT];
    struct jsobject *ctor[C_COUNT];
    jsval symbol[SYM_COUNT];
    struct jsobject *global_lex;                /* top-level let/const/class of scripts (a plain map) */
    struct jsobject *symbol_registry;
    struct jsobject *throw_type_error;          /* %ThrowTypeError% */
    jsval array_proto_values;                   /* the original Array.prototype.values */
    jsval array_iter_next;                      /* the original %ArrayIteratorPrototype%.next */
    jsval eval_fn;
    jsval jobs;                                 /* (27q) the job queue: an array of [fn, args...], or undefined */
    uint32_t jobs_head;                         /* the next one to run */
    jsval intl;                                 /* (27s) b_intl.c's api object, once loaded */

    /* the interpreter */
    jsval *stack, *stack_end, *sp_high;
    struct frame *frames;
    uint32_t nframes;
    struct handler *handlers;
    uint32_t nhandlers, handlers_cap;
    int native_depth;
    int native_magic;                           /* the magic of the native being called (js_magic) */
    struct jsobject *native_callee;             /* (27r) the native being called (its data) */
    jsval *resume_sp;                           /* (27r) run() starts a resumed frame with this sp */
    bool yielded;                               /* (27r) run() returned at a yield or await */
    bool yield_raw;                             /* (27r) ... a yield*'s: the inner result as it is */
    jsval reaction_job, thenable_job;           /* (27r) the promise jobs' natives */
    jsval exception;
    bool has_exception;
    struct jsstring *current_file;

    /* JSON.stringify's and join's cycle checks */
    struct jsobject **visiting;
    uint32_t nvisiting, visiting_cap;
};

jsval vm_call(struct js *J, jsval f, jsval this_val, int argc, jsval *argv, jsval new_target);
jsval vm_run_script(struct js *J, struct funcdef *def);
struct jsobject *closure_new(struct js *J, struct funcdef *def, struct scope *env, struct jsobject *home);
int vm_current_line(struct js *J, struct jsstring **file);

/* The collector's marking (for host classes and the files that own cells). */
void gc_mark_value(struct js *J, jsval v);
void gc_mark_cell(struct js *J, void *cell);

/* Throwing (object.c). */
jsval throw_error(struct js *J, int proto, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define throw_type(J, ...) throw_error(J, P_TYPE_ERROR, __VA_ARGS__)
#define throw_range(J, ...) throw_error(J, P_RANGE_ERROR, __VA_ARGS__)
#define throw_reference(J, ...) throw_error(J, P_REFERENCE_ERROR, __VA_ARGS__)
#define throw_syntax(J, ...) throw_error(J, P_SYNTAX_ERROR, __VA_ARGS__)
jsval throw_oom(struct js *J);
bool visiting_push(struct js *J, struct jsobject *o);   /* false: already being visited */
void visiting_pop(struct js *J);

/* The standard objects (b_*.c), set up by js_new in this order. */
void init_object(struct js *J);
void init_function(struct js *J);
void init_error(struct js *J);
void init_symbol(struct js *J);
void init_iterator(struct js *J);
void init_array(struct js *J);
void init_string(struct js *J);
void init_number(struct js *J);
void init_math(struct js *J);
void init_json(struct js *J);
void init_regexp(struct js *J);
void init_map(struct js *J);
void init_date(struct js *J);
void init_generator(struct js *J);              /* (27r) generators, promises, async functions */
void init_proxy(struct js *J);                  /* (64r) */
void init_intl(struct js *J);                   /* (27s) Intl, loaded at its first use */
jsval intl_number_to_locale(struct js *J, jsval x, jsval locales, jsval options);
jsval intl_date_to_locale(struct js *J, double t, jsval locales, jsval options, const char *required, const char *defaults);
jsval intl_locale_compare(struct js *J, jsval a, jsval b, jsval locales, jsval options);
struct jsobject *gen_create(struct js *J, struct frame *F, jsval *sp, const uint8_t *pc, bool async);
void gen_save(struct js *J, struct genstate *st, struct frame *F, jsval *sp, const uint8_t *pc);
/* Runs generator g on from where it stopped (next, throw or return with
 * v): the value it yielded (*done false), or what it returned (*done
 * true), or JS_EXCEPTION. */
jsval gen_resume(struct js *J, struct jsobject *g, int mode, jsval v, bool *done);
jsval async_begin(struct js *J, struct jsobject *g);
void gen_free(struct genstate *st);
void promise_free(struct promise *p);
void init_global(struct js *J);

/* Helpers shared by the built-ins. */
struct jsobject *make_native(struct js *J, const char *name, js_native fn, int length, int magic);
struct jsobject *make_ctor(struct js *J, const char *name, js_native fn, int length, struct jsobject *proto);
void set_function_name(struct js *J, struct jsobject *f, jsval key, const char *prefix);
jsval this_number(struct js *J, jsval v, const char *method);
struct jsstring *this_string(struct js *J, jsval v, const char *method);   /* RequireObjectCoercible + ToString */
double arg_integer(struct js *J, jsval v, bool *threw);
jsval regexp_create(struct js *J, jsval pattern, jsval flags, jsval new_target);
jsval regexp_exec(struct js *J, jsval re, struct jsstring *s);
jsval error_create(struct js *J, struct jsobject *proto, jsval message);
void error_capture(struct js *J, struct jsobject *e);
jsval json_quote(struct js *J, struct jsstring *s);
struct jsstring *get_substitution(struct js *J, struct jsstring *matched, struct jsstring *str, uint32_t pos,
                                  jsval *captures, uint32_t ncap, jsval groups, struct jsstring *repl);
void regex_free(struct regex *re);
struct jsstring *date_to_string(struct js *J, double t);
double js_pow(double x, double y);
uint32_t js_case_upper(uint32_t c);
uint32_t js_case_lower(uint32_t c);
uint32_t js_case_fold(uint32_t c);
int js_case_full(uint32_t c, bool upper, uint32_t out[3]);
bool js_case_cased(uint32_t c);
bool js_case_ignorable(uint32_t c);
bool js_case_changing(size_t i, uint32_t *lo, uint32_t *hi);

/* The instructions (opcodes.h). */
enum opcode {
#define OP(name, n, e) OP_##name,
#include "opcodes.h"
#undef OP
    OP_COUNT
};

#endif

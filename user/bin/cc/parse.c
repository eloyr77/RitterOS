/*
 * The parser (60b): C11 with the GNU additions the system's sources use
 * (__attribute__, statement expressions, __typeof__, case ranges, the
 * __builtin_ and __atomic_ functions), by recursive descent. Declarations
 * are read as declaration specifiers, then declarators that wrap the type
 * from the outside in (a nested declarator's suffix belongs to the type
 * outside it: the inner declarator is read again once that type is known).
 *
 * The tree it makes is typed and explicit (parse.h): conversions are
 * casts, arrays used as values are their address, and the operators
 * without a direct instruction are spelled out with temporaries.
 */
#include "parse.h"

/* --- scopes -------------------------------------------------------------- */

struct map_entry {
    char *key;
    int len;
    void *value;
};

struct map {
    struct map_entry *entries;
    int cap, used;
};

static unsigned map_hash(const char *s, int n)
{
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++)
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

static void *map_get(struct map *m, const char *key, int len)
{
    if (!m->cap)
        return 0;
    for (unsigned i = map_hash(key, len);; i++) {
        struct map_entry *e = &m->entries[i & (unsigned)(m->cap - 1)];
        if (!e->key)
            return 0;
        if (e->len == len && memcmp(e->key, key, (size_t)len) == 0)
            return e->value;
    }
}

static void map_put(struct map *m, const char *key, int len, void *value)
{
    if ((m->used + 1) * 4 >= m->cap * 3) {
        struct map old = *m;
        m->cap = m->cap ? m->cap * 2 : 16;
        m->entries = calloc((size_t)m->cap, sizeof(struct map_entry));
        m->used = 0;
        for (int i = 0; i < old.cap; i++)
            if (old.entries[i].key)
                map_put(m, old.entries[i].key, old.entries[i].len, old.entries[i].value);
        free(old.entries);
    }
    for (unsigned i = map_hash(key, len);; i++) {
        struct map_entry *e = &m->entries[i & (unsigned)(m->cap - 1)];
        if (!e->key) {
            e->key = (char *)key;
            e->len = len;
            e->value = value;
            m->used++;
            return;
        }
        if (e->len == len && memcmp(e->key, key, (size_t)len) == 0) {
            e->value = value;
            return;
        }
    }
}

/* What a name means in a scope: a variable or function, a typedef, or an
 * enumeration constant. */
struct name_entry {
    struct obj *var;
    struct type *type_def;
    struct type *enum_type;
    int64_t enum_value;
};

struct scope {
    struct scope *outer;
    struct map names, tags;
};

static struct scope file_scope;
static struct scope *scope = &file_scope;

static void enter_scope(void)
{
    struct scope *s = arena_alloc(sizeof(*s));
    s->outer = scope;
    scope = s;
}

static void leave_scope(void)
{
    scope = scope->outer;
}

static struct name_entry *find_name(struct token *t)
{
    for (struct scope *s = scope; s; s = s->outer) {
        struct name_entry *e = map_get(&s->names, t->text, t->len);
        if (e)
            return e;
    }
    return 0;
}

static struct name_entry *declare_name(struct token *t)
{
    struct name_entry *e = arena_alloc(sizeof(*e));
    map_put(&scope->names, t->text, t->len, e);
    return e;
}

static struct type *find_tag(struct token *t)
{
    for (struct scope *s = scope; s; s = s->outer) {
        struct type *ty = map_get(&s->tags, t->text, t->len);
        if (ty)
            return ty;
    }
    return 0;
}

/* --- the state of the parse ---------------------------------------------- */

static struct obj *globals_head, *globals_tail;
static struct obj *locals;                      /* the current function's */
static struct obj *current_fn;
static struct node *gotos, *labels;
static struct node *current_switch;
static struct node *break_target, *continue_target;
static struct node *current_block;              /* (marked when it declares a VLA) */
static int unique_count;
static struct type *builtin_va_list;

/* References between top-level objects, so that only the static
 * functions something uses are written. */
struct ref {
    struct ref *next;
    struct obj *to;
};
static struct map fn_refs;                      /* function name -> struct ref list */

static char *new_unique_name(void)
{
    return format(".L.anon.%d", unique_count++);
}

static char *new_label(void)
{
    return format(".L.label.%d", unique_count++);
}

static void add_global(struct obj *o)
{
    if (globals_tail)
        globals_tail->next = o;
    else
        globals_head = o;
    globals_tail = o;
}

static struct obj *new_local(char *name, struct type *type, struct token *tok)
{
    struct obj *o = arena_alloc(sizeof(*o));
    o->name = name;
    o->type = type;
    o->tok = tok;
    o->is_local = true;
    o->align = type->align;
    o->next = locals;
    locals = o;
    return o;
}

static struct obj *new_global(char *name, struct type *type, struct token *tok)
{
    struct obj *o = arena_alloc(sizeof(*o));
    o->name = name;
    o->type = type;
    o->tok = tok;
    o->align = type->align;
    o->is_definition = true;
    add_global(o);
    return o;
}

static struct obj *new_string_literal(char *data, int len, struct type *type)
{
    struct obj *o = new_global(new_unique_name(), type, 0);
    o->init_data = data;
    o->is_static = true;
    o->is_literal = true;
    (void)len;
    return o;
}

static void note_reference(struct obj *o)
{
    if (!current_fn || o->is_local)
        return;
    struct ref *r = arena_alloc(sizeof(*r));
    r->to = o;
    r->next = map_get(&fn_refs, current_fn->name, (int)strlen(current_fn->name));
    map_put(&fn_refs, current_fn->name, (int)strlen(current_fn->name), r);
}

/* --- tokens -------------------------------------------------------------- */

static bool equal(struct token *t, const char *s)
{
    return (t->kind == TK_PUNCT || t->kind == TK_KEYWORD || t->kind == TK_IDENT) && token_is(t, s);
}

static struct token *skip(struct token *t, const char *s)
{
    if (!equal(t, s))
        error_at(t, "expected '%s'", s);
    return t->next;
}

static bool consume(struct token **rest, struct token *t, const char *s)
{
    if (equal(t, s)) {
        *rest = t->next;
        return true;
    }
    *rest = t;
    return false;
}

static char *name_of(struct token *t)
{
    return token_text(t);
}

/* --- nodes --------------------------------------------------------------- */

static struct node *new_node(enum node_kind kind, struct token *tok)
{
    struct node *n = arena_alloc(sizeof(*n));
    n->kind = kind;
    n->tok = tok;
    return n;
}

static struct node *new_num(int64_t v, struct type *type, struct token *tok)
{
    struct node *n = new_node(ND_NUM, tok);
    n->value = v;
    n->type = type;
    return n;
}

static struct node *new_fnum(double v, struct type *type, struct token *tok)
{
    struct node *n = new_node(ND_NUM, tok);
    n->fvalue = v;
    n->type = type;
    return n;
}

static struct node *new_var_node(struct obj *o, struct token *tok)
{
    struct node *n = new_node(ND_VAR, tok);
    n->var = o;
    n->type = o->type;
    note_reference(o);
    if (o->is_function)
        o->refs++;                              /* (63c: only called, or also its address?) */
    return n;
}

static struct node *new_unary(enum node_kind kind, struct node *expr, struct type *type, struct token *tok)
{
    struct node *n = new_node(kind, tok);
    n->lhs = expr;
    n->type = type;
    return n;
}

static struct node *new_bin(enum node_kind kind, struct node *a, struct node *b, struct type *type,
                            struct token *tok)
{
    struct node *n = new_node(kind, tok);
    n->lhs = a;
    n->rhs = b;
    n->type = type;
    return n;
}

static struct node *new_cast(struct node *expr, struct type *type)
{
    if (expr->type == type)
        return expr;
    struct node *n = new_node(ND_CAST, expr->tok);
    n->lhs = expr;
    n->type = type;
    return n;
}

/* --- attributes ---------------------------------------------------------- */

struct attributes {
    bool packed, weak, used, noreturn, is_inline;
    bool hidden;                                /* (62d) visibility("hidden"/"internal") */
    bool constructor, destructor;               /* (62d) called at load and at exit: .init_array, .fini_array */
    bool ms_abi;                                /* (62f) the Microsoft calling convention (UEFI's) */
    bool noinline;                              /* (63c) */
    bool avx;                                   /* (32h) target("avx...", "fma"): AVX code */
    int aligned;
    int vector_size;                            /* (32d) */
    char *section;
    char *asm_name;
};

static struct node *assign(struct token **rest, struct token *t);
static struct node *conditional(struct token **rest, struct token *t);
static int64_t const_expr(struct token **rest, struct token *t);
static int64_t eval(struct node *n);

static struct token *skip_balanced(struct token *t)
{
    int depth = 0;
    do {
        if (equal(t, "("))
            depth++;
        else if (equal(t, ")"))
            depth--;
        else if (t->kind == TK_EOF)
            error_at(t, "unbalanced parentheses");
        t = t->next;
    } while (depth > 0);
    return t;
}

/* __attribute__((a, b(x), ...)): the ones that change the code are kept;
 * the rest (format, unused, nonnull, ...) only help diagnostics. */
static struct token *attribute_list(struct token *t, struct attributes *attr)
{
    while (equal(t, "__attribute__") || equal(t, "__attribute")) {
        t = skip(t->next, "(");
        t = skip(t, "(");
        while (!equal(t, ")")) {
            if (t->kind != TK_IDENT && t->kind != TK_KEYWORD)
                error_at(t, "expected an attribute name");
            char *name = name_of(t);
            /* __name__ is name */
            size_t n = strlen(name);
            if (n > 4 && strncmp(name, "__", 2) == 0 && strcmp(name + n - 2, "__") == 0) {
                name[n - 2] = 0;
                name += 2;
            }
            t = t->next;
            if (strcmp(name, "packed") == 0)
                attr->packed = true;
            else if (strcmp(name, "weak") == 0)
                attr->weak = true;
            else if (strcmp(name, "used") == 0)
                attr->used = true;
            else if (strcmp(name, "noreturn") == 0)
                attr->noreturn = true;
            else if (strcmp(name, "constructor") == 0 || strcmp(name, "destructor") == 0) {
                if (name[0] == 'c')
                    attr->constructor = true;
                else
                    attr->destructor = true;
                if (equal(t, "("))
                    error_at(t, "constructor and destructor priorities are not supported");
            } else if (strcmp(name, "noinline") == 0)
                attr->noinline = true;
            else if (strcmp(name, "always_inline") == 0 || strcmp(name, "gnu_inline") == 0)
                ;
            else if (strcmp(name, "target") == 0 && equal(t, "(") && t->next->kind == TK_STR) {
                /* (32h) the instruction sets a function may use: AVX's mark it */
                const char *f = t->next->str;
                attr->avx |= strstr(f, "avx") || strstr(f, "fma");
                t = skip(t->next->next, ")");
            }
            else if (strcmp(name, "vector_size") == 0) {
                t = skip(t, "(");
                attr->vector_size = (int)const_expr(&t, t);
                t = skip(t, ")");
            }
            else if (strcmp(name, "aligned") == 0) {
                if (equal(t, "(")) {
                    int64_t v = const_expr(&t, t->next);
                    t = skip(t, ")");
                    if (v > attr->aligned)
                        attr->aligned = (int)v;
                } else
                    attr->aligned = 16;
            } else if (strcmp(name, "section") == 0) {
                t = skip(t, "(");
                if (t->kind != TK_STR)
                    error_at(t, "expected a section name");
                attr->section = t->str;
                t = skip(t->next, ")");
            } else if (strcmp(name, "visibility") == 0) {
                t = skip(t, "(");
                if (t->kind != TK_STR)
                    error_at(t, "expected a visibility");
                attr->hidden = strcmp(t->str, "hidden") == 0 || strcmp(t->str, "internal") == 0;
                t = skip(t->next, ")");
            } else if (strcmp(name, "ms_abi") == 0) {
                if (target.arch != ARCH_X86_64)
                    error_at(t, "ms_abi is x86-64's");
                attr->ms_abi = true;
            }
            else if (equal(t, "("))
                t = skip_balanced(t);
            if (!equal(t, ","))
                break;
            t = t->next;
        }
        t = skip(t, ")");
        t = skip(t, ")");
    }
    return t;
}

/* (32d) typedef T NAME __attribute__((vector_size(N))): GCC's vector types,
 * as far as <immintrin.h> needs them: 16 or 32 bytes of an arithmetic type,
 * aligned to their size, copied, passed and returned whole (in one xmm or
 * ymm register, as the System V ABI's SSE and SSEUP classes), without
 * operators, subscripts or brace initializers: the header's functions work
 * on them. */
static struct type *vector_type(struct type *elem, int size, struct token *tok)
{
    struct type *e = unqual(elem);
    if (target.arch != ARCH_X86_64)
        error_at(tok, "vector types are x86-64's here");
    if (!(is_integer(e) && e->kind != TY_BOOL && e->kind != TY_INT128 && e->kind != TY_ENUM) &&
        e->kind != TY_FLOAT && e->kind != TY_DOUBLE)
        error_at(tok, "a vector's elements are integers, float or double");
    if ((size != 16 && size != 32) || size % e->size)
        error_at(tok, "vectors of 16 or 32 bytes only");
    struct type *t = struct_type();
    t->size = t->align = size;
    t->base = e;
    t->is_vector = true;
    return t;
}

/* __asm__("name") after a declarator: the symbol's name */
static struct token *asm_label(struct token *t, struct attributes *attr)
{
    if (equal(t, "__asm__") || equal(t, "__asm") || equal(t, "asm")) {
        t = skip(t->next, "(");
        if (t->kind != TK_STR)
            error_at(t, "expected a string");
        attr->asm_name = t->str;
        t = skip(t->next, ")");
    }
    return t;
}

/* --- declaration specifiers ---------------------------------------------- */

struct var_attr {
    bool is_typedef, is_static, is_extern, is_inline, is_tls, is_register;
    int align;
    struct attributes attr;
};

static struct type *declspec(struct token **rest, struct token *t, struct var_attr *va);
static struct type *declarator(struct token **rest, struct token *t, struct type *ty, struct token **name,
                               struct attributes *attr);
static struct type *abstract_declarator(struct token **rest, struct token *t, struct type *ty);
static struct type *struct_decl(struct token **rest, struct token *t, bool is_union);
static struct type *enum_specifier(struct token **rest, struct token *t);
static struct type *typeof_specifier(struct token **rest, struct token *t);

static struct type *find_typedef(struct token *t)
{
    if (t->kind != TK_IDENT)
        return 0;
    struct name_entry *e = find_name(t);
    return e ? e->type_def : 0;
}

static bool is_typename(struct token *t)
{
    static const char *names[] = {
        "void", "_Bool", "char", "short", "int", "long", "struct", "union", "typedef", "float",
        "double", "enum", "static", "extern", "_Alignas", "signed", "unsigned", "const",
        "volatile", "auto", "register", "restrict", "__restrict", "__restrict__", "_Noreturn",
        "inline", "__inline", "__inline__", "_Thread_local", "__thread", "_Atomic", "__attribute__",
        "__attribute", "__const", "__const__", "__volatile", "__volatile__", "__signed",
        "__signed__", "__typeof__", "__typeof", "typeof", "__int128", "__builtin_va_list",
        "__extension__", "_Complex",
    };
    if (t->kind == TK_KEYWORD) {
        /* (62a: __extension__ is a type's start only before one: complex.h's
         * I is (__extension__ 1.0iF)) */
        if (token_is(t, "__extension__"))
            return is_typename(t->next);
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
            if (token_is(t, names[i]))
                return true;
        return false;
    }
    return find_typedef(t) != 0;
}

enum {
    VOID = 1 << 0, BOOL = 1 << 2, CHAR = 1 << 4, SHORT = 1 << 6, INT = 1 << 8, LONG = 1 << 10,
    FLOAT = 1 << 12, DOUBLE = 1 << 14, OTHER = 1 << 16, SIGNED = 1 << 17, UNSIGNED = 1 << 18,
    INT128 = 1 << 19,
};

static struct type *declspec(struct token **rest, struct token *t, struct var_attr *va)
{
    struct type *ty = ty_int;
    int counter = 0, qual = 0;
    bool complex_spec = false;
    while (is_typename(t)) {
        if (equal(t, "typedef") || equal(t, "static") || equal(t, "extern") || equal(t, "inline") ||
            equal(t, "__inline") || equal(t, "__inline__") || equal(t, "_Thread_local") ||
            equal(t, "__thread") || equal(t, "auto") || equal(t, "register") || equal(t, "_Noreturn")) {
            if (!va)
                error_at(t, "storage class specifier is not allowed in this context");
            if (equal(t, "typedef"))
                va->is_typedef = true;
            else if (equal(t, "static"))
                va->is_static = true;
            else if (equal(t, "extern"))
                va->is_extern = true;
            else if (equal(t, "_Thread_local") || equal(t, "__thread"))
                va->is_tls = true;
            else if (equal(t, "register"))
                va->is_register = true;
            else if (equal(t, "_Noreturn"))
                va->attr.noreturn = true;
            else if (!equal(t, "auto"))
                va->is_inline = true;
            if (va->is_typedef && (va->is_static || va->is_extern || va->is_inline || va->is_tls))
                error_at(t, "typedef may not be used together with static, extern, inline or _Thread_local");
            t = t->next;
            continue;
        }
        if (equal(t, "const") || equal(t, "__const") || equal(t, "__const__")) {
            qual |= Q_CONST;
            t = t->next;
            continue;
        }
        if (equal(t, "volatile") || equal(t, "__volatile") || equal(t, "__volatile__")) {
            qual |= Q_VOLATILE;
            t = t->next;
            continue;
        }
        if (equal(t, "restrict") || equal(t, "__restrict") || equal(t, "__restrict__") ||
            equal(t, "__extension__")) {
            t = t->next;
            continue;
        }
        if (equal(t, "_Atomic")) {
            t = t->next;
            if (equal(t, "(")) {
                /* (65e-52: in one call the declarator began at the type's
                 * first token again, as its argument was read first) */
                struct type *base = declspec(&t, t->next, 0);
                ty = abstract_declarator(&t, t, base);
                t = skip(t, ")");
                counter += OTHER;
            }
            qual |= Q_ATOMIC;
            continue;
        }
        if (equal(t, "_Complex")) {
            complex_spec = true;
            t = t->next;
            continue;
        }
        if (equal(t, "__attribute__") || equal(t, "__attribute")) {
            struct attributes dummy = { 0 };
            t = attribute_list(t, va ? &va->attr : &dummy);
            continue;
        }
        if (equal(t, "_Alignas")) {
            if (!va)
                error_at(t, "_Alignas is not allowed in this context");
            t = skip(t->next, "(");
            int a;
            if (is_typename(t)) {
                struct type *at = abstract_declarator(&t, t, declspec(&t, t, 0));
                a = at->align;
            } else
                a = (int)const_expr(&t, t);
            if (a > va->align)
                va->align = a;
            t = skip(t, ")");
            continue;
        }
        /* user-defined types */
        struct type *td = find_typedef(t);
        if (equal(t, "struct") || equal(t, "union") || equal(t, "enum") || equal(t, "__typeof__") ||
            equal(t, "__typeof") || equal(t, "typeof") || equal(t, "__builtin_va_list") || td) {
            if (counter)
                break;
            if (equal(t, "struct"))
                ty = struct_decl(&t, t->next, false);
            else if (equal(t, "union"))
                ty = struct_decl(&t, t->next, true);
            else if (equal(t, "enum"))
                ty = enum_specifier(&t, t->next);
            else if (equal(t, "__builtin_va_list")) {
                ty = builtin_va_list;
                t = t->next;
            } else if (td) {
                ty = td;
                t = t->next;
            } else
                ty = typeof_specifier(&t, t->next);
            counter += OTHER;
            continue;
        }
        if (equal(t, "void"))
            counter += VOID;
        else if (equal(t, "_Bool"))
            counter += BOOL;
        else if (equal(t, "char"))
            counter += CHAR;
        else if (equal(t, "short"))
            counter += SHORT;
        else if (equal(t, "int"))
            counter += INT;
        else if (equal(t, "long"))
            counter += LONG;
        else if (equal(t, "float"))
            counter += FLOAT;
        else if (equal(t, "double"))
            counter += DOUBLE;
        else if (equal(t, "signed") || equal(t, "__signed") || equal(t, "__signed__"))
            counter |= SIGNED;
        else if (equal(t, "unsigned"))
            counter |= UNSIGNED;
        else if (equal(t, "__int128"))
            counter += INT128;
        else
            error_at(t, "internal error: type specifier");
        switch (counter) {
        case VOID: ty = ty_void; break;
        case BOOL: ty = ty_bool; break;
        case CHAR: ty = ty_char; break;
        case SIGNED + CHAR: ty = ty_schar; break;
        case UNSIGNED + CHAR: ty = ty_uchar; break;
        case SHORT: case SHORT + INT: case SIGNED + SHORT: case SIGNED + SHORT + INT: ty = ty_short; break;
        case UNSIGNED + SHORT: case UNSIGNED + SHORT + INT: ty = ty_ushort; break;
        case INT: case SIGNED: case SIGNED + INT: ty = ty_int; break;
        case UNSIGNED: case UNSIGNED + INT: ty = ty_uint; break;
        case LONG: case LONG + INT: case SIGNED + LONG: case SIGNED + LONG + INT: ty = ty_long; break;
        case UNSIGNED + LONG: case UNSIGNED + LONG + INT: ty = ty_ulong; break;
        case LONG + LONG: case LONG + LONG + INT: case SIGNED + LONG + LONG: case SIGNED + LONG + LONG + INT:
            ty = ty_llong; break;
        case UNSIGNED + LONG + LONG: case UNSIGNED + LONG + LONG + INT: ty = ty_ullong; break;
        case INT128: case SIGNED + INT128: ty = ty_int128; break;
        case UNSIGNED + INT128: ty = ty_uint128; break;
        case FLOAT: ty = ty_float; break;
        case DOUBLE: ty = ty_double; break;
        case LONG + DOUBLE: ty = ty_ldouble; break;
        default: error_at(t, "invalid combination of type specifiers");
        }
        t = t->next;
    }
    *rest = t;
    if (complex_spec) {
        /* (62a; _Complex alone is double _Complex, as GCC takes it) */
        if (!counter)
            ty = ty_double;
        if (!is_flonum(ty))
            error_at(t, "complex integer types are not supported");
        ty = complex_of(ty);
    }
    if (qual)
        ty = qualified(ty, qual);
    return ty;
}

static struct type *typeof_specifier(struct token **rest, struct token *t)
{
    t = skip(t, "(");
    struct type *ty;
    if (is_typename(t))
        ty = abstract_declarator(&t, t, declspec(&t, t, 0));
    else {
        struct node *n = assign(&t, t);   /* (the expression is not evaluated) */
        while (equal(t, ",")) {
            n = assign(&t, t->next);
        }
        ty = n->type;
    }
    *rest = skip(t, ")");
    return ty;
}

/* --- declarators --------------------------------------------------------- */

static struct type *type_suffix(struct token **rest, struct token *t, struct type *ty);

static struct type *pointers(struct token **rest, struct token *t, struct type *ty)
{
    while (consume(&t, t, "*")) {
        ty = pointer_to(ty);
        int qual = 0;
        for (;;) {
            if (equal(t, "const") || equal(t, "__const") || equal(t, "__const__"))
                qual |= Q_CONST;
            else if (equal(t, "volatile") || equal(t, "__volatile") || equal(t, "__volatile__"))
                qual |= Q_VOLATILE;
            else if (equal(t, "restrict") || equal(t, "__restrict") || equal(t, "__restrict__"))
                qual |= Q_RESTRICT;
            else if (equal(t, "_Atomic"))
                qual |= Q_ATOMIC;
            else if (equal(t, "__attribute__") || equal(t, "__attribute")) {
                struct attributes dummy = { 0 };
                t = attribute_list(t, &dummy);
                continue;
            } else
                break;
            t = t->next;
        }
        if (qual)
            ty = qualified(ty, qual);
    }
    *rest = t;
    return ty;
}

/* A parameter's type as the function sees it (6.7.6.3p7-8). */
static struct type *adjust_param(struct type *ty)
{
    if (ty->kind == TY_ARRAY || ty->kind == TY_VLA)
        return qualified(pointer_to(ty->base), ty->qual);
    if (ty->kind == TY_FUNC)
        return pointer_to(ty);
    return ty;
}

static struct type *func_params(struct token **rest, struct token *t, struct type *ty)
{
    struct type *fn = func_type(ty);
    if (equal(t, "void") && equal(t->next, ")")) {
        fn->has_prototype = true;
        *rest = t->next->next;
        return fn;
    }
    if (equal(t, ")")) {
        *rest = t->next;                        /* f(): no prototype */
        return fn;
    }
    fn->has_prototype = true;
    struct param head = { 0 }, *cur = &head;
    while (!equal(t, ")")) {
        if (cur != &head)
            t = skip(t, ",");
        if (equal(t, "...")) {
            fn->is_variadic = true;
            t = t->next;
            break;
        }
        if (!is_typename(t)) {
            /* K&R parameter names: their types come before the body */
            if (t->kind != TK_IDENT)
                error_at(t, "expected a parameter declaration");
            cur = cur->next = arena_alloc(sizeof(*cur));
            cur->type = ty_int;
            cur->name = t;
            fn->has_prototype = false;
            t = t->next;
            continue;
        }
        struct var_attr va = { 0 };
        struct type *pt = declspec(&t, t, &va);
        struct token *name = 0;
        struct attributes attr = { 0 };
        pt = declarator(&t, t, pt, &name, &attr);
        cur = cur->next = arena_alloc(sizeof(*cur));
        cur->type = adjust_param(pt);
        cur->name = name;
    }
    fn->params = head.next;
    *rest = skip(t, ")");
    return fn;
}

static struct type *array_dims(struct token **rest, struct token *t, struct type *ty)
{
    /* static and qualifiers in a parameter's [] change nothing here */
    while (equal(t, "static") || equal(t, "const") || equal(t, "restrict") || equal(t, "volatile") ||
           equal(t, "__restrict") || equal(t, "__restrict__"))
        t = t->next;
    if (equal(t, "]") || (equal(t, "*") && equal(t->next, "]"))) {
        if (equal(t, "*"))
            t = t->next;
        ty = type_suffix(rest, t->next, ty);
        return array_of(ty, -1);
    }
    struct node *len = conditional(&t, t);
    t = skip(t, "]");
    ty = type_suffix(rest, t, ty);
    bool constant = len->kind == ND_NUM && is_integer(len->type);
    if (!constant) {
        /* (a constant expression folds to a number) */
        extern bool is_const_expr(struct node * n);
        if (is_const_expr(len))
            constant = true;
    }
    if (ty->kind == TY_VLA || !constant)
        return vla_of(ty, len);
    int64_t n = eval(len);
    if (n < 0)
        error_at(len->tok, "array has negative size");
    if (ty->size < 0)
        error_at(len->tok, "array has incomplete element type '%s'", type_name(ty));
    return array_of(ty, (int)n);
}

static struct type *type_suffix(struct token **rest, struct token *t, struct type *ty)
{
    if (equal(t, "("))
        return func_params(rest, t->next, ty);
    if (equal(t, "["))
        return array_dims(rest, t->next, ty);
    *rest = t;
    return ty;
}

/* (62f) ms_abi given in a declarator: the function type it names, also
 * through pointers ("EFI_STATUS (EFIAPI *GetTime)(...)") */
static struct type *mark_ms_abi(struct type *ty, struct attributes *attr)
{
    if (!attr->ms_abi)
        return ty;
    for (struct type *t = ty; t; t = t->base)
        if (t->kind == TY_FUNC) {
            t->is_ms_abi = true;
            break;
        } else if (t->kind != TY_PTR && t->kind != TY_ARRAY)
            break;
    return ty;
}

static struct type *declarator(struct token **rest, struct token *t, struct type *ty, struct token **name,
                               struct attributes *attr)
{
    t = attribute_list(t, attr);                /* (62f: "(__attribute__((noreturn)) *fn)") */
    ty = pointers(&t, t, ty);
    t = attribute_list(t, attr);
    bool attribute_next = equal(t->next, "__attribute__") || equal(t->next, "__attribute");
    if (equal(t, "(") && (!is_typename(t->next) || attribute_next) && !equal(t->next, ")")) {
        /* ( declarator ): its suffix comes first, so read it after the
         * suffix outside, with the type made from that */
        struct token *start = t;
        struct type dummy = { 0 };
        struct token *ignored_name = 0;
        struct attributes ignored = { 0 };
        declarator(&t, start->next, &dummy, &ignored_name, &ignored);
        t = skip(t, ")");
        ty = type_suffix(rest, t, ty);
        struct token *end;
        ty = declarator(&end, start->next, ty, name, attr);
        (void)end;
        *rest = attribute_list(*rest, attr);
        return mark_ms_abi(ty, attr);
    }
    *name = 0;
    if (t->kind == TK_IDENT) {
        *name = t;
        t = t->next;
    }
    ty = type_suffix(&t, t, ty);
    t = asm_label(t, attr);
    *rest = attribute_list(t, attr);
    return mark_ms_abi(ty, attr);
}

static struct type *abstract_declarator(struct token **rest, struct token *t, struct type *ty)
{
    struct token *name = 0;
    struct attributes attr = { 0 };
    ty = declarator(rest, t, ty, &name, &attr);
    if (name)
        error_at(name, "unexpected name in a type name");
    return ty;
}

static struct type *typename(struct token **rest, struct token *t)
{
    struct type *ty = declspec(&t, t, 0);
    return abstract_declarator(rest, t, ty);
}

/* --- structures, unions and enumerations --------------------------------- */

static int align_to(int n, int a)
{
    return (n + a - 1) / a * a;
}

/* The members of a { } list. */
static struct member *struct_members(struct token **rest, struct token *t, struct type *ty)
{
    struct member head = { 0 }, *cur = &head;
    int index = 0;
    while (!equal(t, "}")) {
        if (equal(t, "_Static_assert")) {
            extern struct token *static_assertion(struct token * t);
            t = static_assertion(t->next);
            continue;
        }
        struct var_attr va = { 0 };
        struct type *base = declspec(&t, t, &va);
        bool first = true;
        /* an anonymous struct or union inside (C11 6.7.2.1p13) */
        if (is_struct_union(base) && consume(&t, t, ";")) {
            cur = cur->next = arena_alloc(sizeof(*cur));
            cur->type = base;
            cur->index = index++;
            cur->align = va.align ? va.align : base->align;
            continue;
        }
        while (!consume(&t, t, ";")) {
            if (!first)
                t = skip(t, ",");
            first = false;
            struct member *m = arena_alloc(sizeof(*m));
            struct token *name = 0;
            struct attributes attr = va.attr;
            if (equal(t, ":"))
                m->type = base;                 /* (an unnamed bit-field) */
            else
                m->type = declarator(&t, t, base, &name, &attr);
            m->name = name;
            m->pos = name ? name : t;
            m->index = index++;
            m->align = va.align ? va.align : attr.aligned ? attr.aligned : m->type->align;
            if (attr.packed)
                m->align = 1;
            if (consume(&t, t, ":")) {
                m->is_bitfield = true;
                m->bit_width = (int)const_expr(&t, t);
                if (!is_integer(m->type))
                    error_at(m->pos, "bit-field has non-integral type");
                if (m->bit_width > m->type->size * 8)
                    error_at(m->pos, "width of bit-field exceeds its type");
            }
            t = attribute_list(t, &attr);
            if (attr.aligned > m->align)
                m->align = attr.aligned;
            if (attr.vector_size)
                error_at(m->pos, "vector_size only in a typedef");
            cur = cur->next = m;
        }
    }
    /* A flexible array member: the last one, without a size */
    if (cur != &head && cur->type->kind == TY_ARRAY && cur->type->array_len < 0) {
        cur->type = array_of(cur->type->base, 0);
        ty->is_flexible = true;
    }
    *rest = t->next;
    return head.next;
}

/* The System V and AAPCS64 layout: each member at the next multiple of its
 * alignment; a bit-field in the storage unit of its type where it fits,
 * never across one; an unnamed bit-field of width 0 ends the unit. packed
 * structures put members next to each other, bit-fields bit by bit. */
static void struct_layout(struct type *ty)
{
    int64_t bits = 0;
    int align = 1;
    for (struct member *m = ty->members; m; m = m->next) {
        if (m->is_bitfield && m->bit_width == 0) {
            bits = align_to((int)((bits + 7) / 8), m->type->size) * 8LL;
            continue;
        }
        if (m->is_bitfield) {
            int unit = m->type->size * 8;
            if (!ty->is_packed && bits / unit != (bits + m->bit_width - 1) / unit)
                bits = align_to((int)((bits + unit - 1) / unit), 1) * (int64_t)unit;
            m->offset = (int)(bits / 8) / m->type->size * m->type->size;
            if (ty->is_packed)
                m->offset = (int)(bits / 8);
            m->bit_offset = (int)(bits - m->offset * 8LL);
            bits += m->bit_width;
            /* named bit-fields count for the alignment, unnamed ones not */
            if (m->name && !ty->is_packed && m->align > align)
                align = m->align;
            continue;
        }
        int ma = ty->is_packed ? 1 : m->align;
        bits = align_to((int)((bits + 7) / 8), ma) * 8LL;
        m->offset = (int)(bits / 8);
        if (m->type->size < 0)
            error_at(m->pos, "field has incomplete type '%s'", type_name(m->type));
        bits += m->type->size * 8LL;
        if (ma > align)
            align = ma;
    }
    ty->align = align;
    ty->size = align_to((int)((bits + 7) / 8), align);
}

static void union_layout(struct type *ty)
{
    int size = 0, align = 1;
    for (struct member *m = ty->members; m; m = m->next) {
        m->offset = 0;
        if (m->is_bitfield) {
            m->bit_offset = 0;
            int s = (m->bit_width + 7) / 8;
            if (s > size)
                size = s;
            if (m->name && m->type->align > align && !ty->is_packed)
                align = m->type->align;
            continue;
        }
        if (m->type->size < 0)
            error_at(m->pos, "field has incomplete type '%s'", type_name(m->type));
        if (m->type->size > size)
            size = m->type->size;
        int ma = ty->is_packed ? 1 : m->align;
        if (ma > align)
            align = ma;
    }
    ty->align = align;
    ty->size = align_to(size, align);
}

static struct type *struct_decl(struct token **rest, struct token *t, bool is_union)
{
    struct attributes attr = { 0 };
    t = attribute_list(t, &attr);
    struct token *tag = 0;
    if (t->kind == TK_IDENT) {
        tag = t;
        t = t->next;
    }
    if (tag && !equal(t, "{")) {
        /* a use: the visible one, or a new incomplete type */
        *rest = t;
        struct type *ty = find_tag(tag);
        if (ty) {
            if ((ty->kind == TY_UNION) != is_union || !is_struct_union(ty))
                error_at(tag, "use of '%.*s' with tag type that does not match previous declaration",
                         tag->len, tag->text);
            return ty;
        }
        ty = struct_type();
        if (is_union)
            ty->kind = TY_UNION;
        ty->tag = tag;
        map_put(&scope->tags, tag->text, tag->len, ty);
        return ty;
    }
    t = skip(t, "{");
    struct type *ty = 0;
    if (tag) {
        /* a definition completes a declaration of the same scope */
        struct type *same = map_get(&scope->tags, tag->text, tag->len);
        if (same && same->size >= 0)
            error_at(tag, "redefinition of '%.*s'", tag->len, tag->text);
        ty = same;
    }
    if (!ty) {
        ty = struct_type();
        if (is_union)
            ty->kind = TY_UNION;
        ty->tag = tag;
        if (tag)
            map_put(&scope->tags, tag->text, tag->len, ty);
    }
    ty->members = struct_members(&t, t, ty);
    struct attributes after = { 0 };
    t = attribute_list(t, &after);
    ty->is_packed = attr.packed || after.packed;
    if (is_union)
        union_layout(ty);
    else
        struct_layout(ty);
    int a = attr.aligned > after.aligned ? attr.aligned : after.aligned;
    if (a > ty->align) {
        ty->align = a;
        ty->size = align_to(ty->size, a);
    }
    complete_struct(ty);
    *rest = t;
    return ty;
}

static struct type *enum_specifier(struct token **rest, struct token *t)
{
    struct attributes attr = { 0 };
    t = attribute_list(t, &attr);
    struct token *tag = 0;
    if (t->kind == TK_IDENT) {
        tag = t;
        t = t->next;
    }
    if (tag && !equal(t, "{")) {
        struct type *ty = find_tag(tag);
        if (!ty) {
            ty = enum_type();
            ty->tag = tag;
            map_put(&scope->tags, tag->text, tag->len, ty);
        } else if (ty->kind != TY_ENUM)
            error_at(tag, "'%.*s' is not an enum tag", tag->len, tag->text);
        *rest = t;
        return ty;
    }
    struct type *ty = enum_type();
    ty->tag = tag;
    t = skip(t, "{");
    int64_t value = 0, min = 0, max = 0;
    struct name_entry *entries[4096];
    int count = 0;
    while (!consume(&t, t, "}")) {
        if (count)
            t = skip(t, ",");
        if (consume(&t, t, "}"))
            break;
        if (t->kind != TK_IDENT)
            error_at(t, "expected an identifier");
        struct token *name = t;
        t = t->next;
        struct attributes a = { 0 };
        t = attribute_list(t, &a);
        if (consume(&t, t, "="))
            value = const_expr(&t, t);
        struct name_entry *e = declare_name(name);
        e->enum_type = ty_int;
        e->enum_value = value;
        if (count < 4096)
            entries[count] = e;
        count++;
        if (value < min)
            min = value;
        if (value > max)
            max = value;
        value++;
    }
    /* The compatible integer type: unsigned int when nothing is negative
     * (as GCC and clang), larger ones when the values need them. */
    if (min < 0)
        ty->enum_int = min >= INT32_MIN && max <= INT32_MAX ? ty_int : ty_long;
    else
        ty->enum_int = max <= UINT32_MAX ? ty_uint : ty_ulong;
    ty->size = ty->align = ty->enum_int->size;
    ty->is_unsigned = ty->enum_int->is_unsigned;
    /* The constants are ints when every value fits one (6.7.2.2p2). */
    if (min < INT32_MIN || max > INT32_MAX)
        for (int i = 0; i < count && i < 4096; i++)
            entries[i]->enum_type = ty->enum_int;
    if (tag)
        map_put(&scope->tags, tag->text, tag->len, ty);
    *rest = attribute_list(t, &attr);
    return ty;
}

/* --- initializers -------------------------------------------------------- */

/* An initializer as a tree shaped like its type: a leaf holds the
 * expression for a scalar (or a whole structure); arrays, structures and
 * unions have a child per element or member. */
struct initializer {
    struct type *type;
    struct token *tok;
    bool is_flexible;                           /* an array whose length the initializer gives */
    struct node *expr;
    struct initializer **children;
    int nchildren;
    struct member *member;                      /* a union's initialized member */
};

static struct initializer *new_initializer(struct type *ty, bool flexible)
{
    struct initializer *init = arena_alloc(sizeof(*init));
    init->type = ty;
    if (ty->kind == TY_ARRAY) {
        if (flexible && ty->size < 0) {
            init->is_flexible = true;
            return init;
        }
        init->nchildren = ty->array_len;
        init->children = arena_alloc(sizeof(struct initializer *) * (size_t)(init->nchildren + 1));
        for (int i = 0; i < ty->array_len; i++)
            init->children[i] = new_initializer(ty->base, false);
        return init;
    }
    if (is_struct_union(ty)) {
        int n = 0;
        for (struct member *m = ty->members; m; m = m->next)
            n++;
        init->nchildren = n;
        init->children = arena_alloc(sizeof(struct initializer *) * (size_t)(n + 1));
        for (struct member *m = ty->members; m; m = m->next) {
            if (flexible && ty->is_flexible && !m->next) {
                /* (GNU: a static structure's flexible array may be initialized) */
                struct initializer *c = arena_alloc(sizeof(*c));
                c->type = m->type;
                c->is_flexible = true;
                init->children[m->index] = c;
            } else
                init->children[m->index] = new_initializer(m->type, false);
        }
        return init;
    }
    return init;
}

static void initializer2(struct token **rest, struct token *t, struct initializer *init);

static struct token *skip_excess_element(struct token *t)
{
    if (equal(t, "{")) {
        t = t->next;
        while (!equal(t, "}"))
            t = skip_excess_element(t);
        return t->next;
    }
    assign(&t, t);
    return t;
}

static bool is_end(struct token *t)
{
    return equal(t, "}") || (equal(t, ",") && equal(t->next, "}"));
}

static bool consume_end(struct token **rest, struct token *t)
{
    if (equal(t, "}")) {
        *rest = t->next;
        return true;
    }
    if (equal(t, ",") && equal(t->next, "}")) {
        *rest = t->next->next;
        return true;
    }
    return false;
}

static void string_initializer(struct token **rest, struct token *t, struct initializer *init)
{
    struct type *elem = init->type->base;
    int n = t->str_len / elem->size;            /* (the terminating zero included) */
    if (init->is_flexible) {
        struct initializer *fresh = new_initializer(array_of(elem, n), false);
        *init = *fresh;
    }
    int len = n < init->type->array_len ? n : init->type->array_len;
    for (int i = 0; i < len; i++) {
        int64_t v;
        switch (elem->size) {
        case 1: v = elem->is_unsigned ? (uint8_t)t->str[i] : (int8_t)t->str[i]; break;
        case 2: v = ((uint16_t *)(void *)t->str)[i]; break;
        default: v = elem->is_unsigned ? ((uint32_t *)(void *)t->str)[i] : ((int32_t *)(void *)t->str)[i];
        }
        init->children[i]->expr = new_num(v, elem, t);
    }
    *rest = t->next;
}

static bool is_char_array_for(struct type *ty, struct token *t)
{
    if (ty->kind != TY_ARRAY || t->kind != TK_STR)
        return false;
    struct type *e = unqual(ty->base), *s = unqual(t->type->base);
    if (!is_integer(e))
        return false;
    if (e->size != s->size)
        return false;
    return true;
}

/* The index range of an array designator: [i] or GNU's [i ... j]. */
static void array_designator(struct token **rest, struct token *t, struct type *ty, int *begin, int *end)
{
    *begin = (int)const_expr(&t, t->next);
    if (ty->array_len >= 0 && *begin >= ty->array_len)
        error_at(t, "array designator index exceeds array bounds");
    if (equal(t, "...")) {
        *end = (int)const_expr(&t, t->next);
        if (ty->array_len >= 0 && *end >= ty->array_len)
            error_at(t, "array designator index exceeds array bounds");
        if (*end < *begin)
            error_at(t, "array designator range is empty");
    } else
        *end = *begin;
    *rest = skip(t, "]");
}

static struct member *struct_designator(struct token **rest, struct token *t, struct type *ty)
{
    struct token *start = t;
    t = skip(t, ".");
    if (t->kind != TK_IDENT)
        error_at(t, "expected a field designator");
    for (struct member *m = ty->members; m; m = m->next) {
        if (!m->name && is_struct_union(m->type)) {
            /* a member of an anonymous struct or union: the anonymous one
             * is designated, and the rest is read again inside it */
            extern struct member *find_member(struct type * ty, struct token * name, int64_t *offset_out);
            int64_t ignored;
            if (find_member(m->type, t, &ignored)) {
                *rest = start;
                return m;
            }
            continue;
        }
        if (m->name && m->name->len == t->len && memcmp(m->name->text, t->text, (size_t)t->len) == 0) {
            *rest = t->next;
            return m;
        }
    }
    error_at(t, "field designator '%.*s' does not refer to any field", t->len, t->text);
}

static void designation(struct token **rest, struct token *t, struct initializer *init);

static void grow_flexible(struct initializer *init, int len)
{
    if (!init->is_flexible || len <= init->nchildren)
        return;
    struct initializer **c = arena_alloc(sizeof(struct initializer *) * (size_t)(len + 1));
    for (int i = 0; i < init->nchildren; i++)
        c[i] = init->children[i];
    for (int i = init->nchildren; i < len; i++)
        c[i] = new_initializer(init->type->base, false);
    init->children = c;
    init->nchildren = len;
}

/* { initializer, ... } for an array */
static void array_initializer1(struct token **rest, struct token *t, struct initializer *init)
{
    t = skip(t, "{");
    bool first = true;
    int i = 0, max = 0;
    while (!consume_end(rest, t)) {
        if (!first)
            t = skip(t, ",");
        first = false;
        if (equal(t, "[")) {
            int begin, end;
            array_designator(&t, t, init->type, &begin, &end);
            grow_flexible(init, end + 1);
            struct token *start = t;
            for (int j = begin; j <= end; j++)
                designation(&t, start, init->children[j]);
            i = end + 1;
            if (i > max)
                max = i;
            continue;
        }
        grow_flexible(init, i + 1);
        if (i < init->nchildren)
            initializer2(&t, t, init->children[i]);
        else
            t = skip_excess_element(t);
        i++;
        if (i > max)
            max = i;
    }
    if (init->is_flexible)
        init->type = array_of(init->type->base, max), init->is_flexible = false;
}

/* an array's elements without braces (brace elision, 6.7.9p20) */
static void array_initializer2(struct token **rest, struct token *t, struct initializer *init, int i)
{
    if (init->is_flexible) {
        /* (only inside braces of an outer aggregate: count what is there) */
        int len = 0;
        for (struct token *x = t; !is_end(x); len++) {
            if (len)
                x = skip(x, ",");
            x = skip_excess_element(x);
        }
        grow_flexible(init, len);
        init->type = array_of(init->type->base, len);
        init->is_flexible = false;
    }
    for (; i < init->nchildren && !is_end(t); i++) {
        struct token *start = t;
        if (i > 0)
            t = skip(t, ",");
        if (equal(t, "[") || equal(t, ".")) {
            *rest = start;
            return;
        }
        initializer2(&t, t, init->children[i]);
    }
    *rest = t;
}

static struct member *member_after(struct type *ty, struct member *m)
{
    (void)ty;
    m = m->next;
    while (m && m->is_bitfield && !m->name)
        m = m->next;                            /* (unnamed bit-fields are not initialized) */
    return m;
}

static struct member *first_member(struct type *ty)
{
    struct member *m = ty->members;
    while (m && m->is_bitfield && !m->name)
        m = m->next;
    return m;
}

static void struct_initializer1(struct token **rest, struct token *t, struct initializer *init)
{
    t = skip(t, "{");
    struct member *m = first_member(init->type);
    bool first = true;
    while (!consume_end(rest, t)) {
        if (!first)
            t = skip(t, ",");
        first = false;
        if (equal(t, ".")) {
            m = struct_designator(&t, t, init->type);
            designation(&t, t, init->children[m->index]);
            m = member_after(init->type, m);
            continue;
        }
        if (m) {
            initializer2(&t, t, init->children[m->index]);
            m = member_after(init->type, m);
        } else
            t = skip_excess_element(t);
    }
}

static void struct_initializer2(struct token **rest, struct token *t, struct initializer *init,
                                struct member *m)
{
    bool first = true;
    for (; m && !is_end(t); m = member_after(init->type, m)) {
        struct token *start = t;
        if (!first)
            t = skip(t, ",");
        first = false;
        if (equal(t, "[") || equal(t, ".")) {
            *rest = start;
            return;
        }
        initializer2(&t, t, init->children[m->index]);
    }
    *rest = t;
}

static void union_initializer(struct token **rest, struct token *t, struct initializer *init)
{
    if (equal(t, "{") && equal(t->next, ".")) {
        struct member *m = struct_designator(&t, t->next, init->type);
        init->member = m;
        designation(&t, t, init->children[m->index]);
        if (!consume_end(rest, t)) {
            /* (further designators for other members: the last one wins) */
            while (equal(t, ",") && equal(t->next, ".")) {
                m = struct_designator(&t, t->next, init->type);
                init->member = m;
                designation(&t, t, init->children[m->index]);
            }
            if (!consume_end(rest, t))
                error_at(t, "expected '}'");
        }
        return;
    }
    init->member = first_member(init->type);
    if (!init->member) {
        if (equal(t, "{"))
            *rest = skip(t->next, "}");
        else
            *rest = t;
        return;
    }
    if (equal(t, "{")) {
        initializer2(&t, t->next, init->children[init->member->index]);
        consume(&t, t, ",");
        *rest = skip(t, "}");
    } else
        initializer2(rest, t, init->children[init->member->index]);
}

/* A designation's rest: more designators, then = and an initializer. */
static void designation(struct token **rest, struct token *t, struct initializer *init)
{
    if (equal(t, "[")) {
        if (init->type->kind != TY_ARRAY)
            error_at(t, "array designator cannot initialize non-array type");
        int begin, end;
        array_designator(&t, t, init->type, &begin, &end);
        grow_flexible(init, end + 1);
        struct token *start = t;
        for (int i = begin; i <= end; i++)
            designation(&t, start, init->children[i]);
        array_initializer2(rest, t, init, begin + 1);
        return;
    }
    if (equal(t, ".") && is_struct_union(init->type)) {
        struct member *m = struct_designator(&t, t, init->type);
        if (init->type->kind == TY_UNION)
            init->member = m;
        designation(&t, t, init->children[m->index]);
        init->expr = 0;
        if (init->type->kind == TY_STRUCT)
            struct_initializer2(rest, t, init, member_after(init->type, m));
        else
            *rest = t;
        return;
    }
    if (equal(t, "."))
        error_at(t, "field designator cannot initialize a non-struct, non-union type");
    if (equal(t, "="))
        t = t->next;
    initializer2(rest, t, init);
}

static struct node *value(struct node *n);
static struct node *atomic_read(struct node *n);
static struct node *convert_for_assign(struct node *expr, struct type *ty, struct token *where);
static struct node *new_add(struct node *a, struct node *b, struct token *tok);

static void initializer2(struct token **rest, struct token *t, struct initializer *init)
{
    struct type *ty = init->type;
    if (ty->kind == TY_ARRAY && is_char_array_for(ty, t)) {
        string_initializer(rest, t, init);
        return;
    }
    if (ty->kind == TY_ARRAY && equal(t, "{") && is_char_array_for(ty, t->next) &&
        (equal(t->next->next, "}") || (equal(t->next->next, ",") && equal(t->next->next->next, "}")))) {
        /* char s[] = { "text" } */
        string_initializer(&t, t->next, init);
        consume(&t, t, ",");
        *rest = skip(t, "}");
        return;
    }
    if (ty->kind == TY_ARRAY) {
        if (equal(t, "{"))
            array_initializer1(rest, t, init);
        else
            array_initializer2(rest, t, init, 0);
        return;
    }
    if (ty->kind == TY_STRUCT) {
        if (ty->is_vector) {
            /* (32d) a vector only from another one */
            struct node *expr = equal(t, "{") ? 0 : assign(rest, t);
            if (!expr || unqual(expr->type) != unqual(ty))
                error_at(t, "a vector is set from a vector: <immintrin.h>'s _mm..._set functions");
            init->expr = expr;
            return;
        }
        if (equal(t, "{")) {
            struct_initializer1(rest, t, init);
            return;
        }
        /* a structure value, or its members without braces */
        struct token *start = t;
        struct node *expr = assign(rest, t);
        if (is_struct_union(expr->type)) {
            init->expr = expr;
            return;
        }
        struct_initializer2(rest, start, init, first_member(ty));
        return;
    }
    if (ty->kind == TY_UNION) {
        if (!equal(t, "{")) {
            struct token *start = t;
            struct node *expr = assign(rest, t);
            if (is_struct_union(expr->type)) {
                init->expr = expr;
                return;
            }
            t = start;
        }
        union_initializer(rest, t, init);
        return;
    }
    if (equal(t, "{")) {
        /* a scalar in braces: int x = { 3 }; */
        initializer2(&t, t->next, init);
        consume(&t, t, ",");
        *rest = skip(t, "}");
        return;
    }
    init->tok = t;
    init->expr = assign(rest, t);
}

static struct initializer *initializer(struct token **rest, struct token *t, struct type *ty,
                                       struct type **new_ty)
{
    struct initializer *init = new_initializer(ty, true);
    initializer2(rest, t, init);
    if (is_struct_union(ty) && ty->is_flexible) {
        /* (the size grows by the flexible array's initializer) */
        struct type *c = arena_alloc(sizeof(*c));
        *c = *unqual(ty);
        struct member head = { 0 }, *cur = &head;
        for (struct member *m = ty->members; m; m = m->next) {
            struct member *mm = arena_alloc(sizeof(*mm));
            *mm = *m;
            cur = cur->next = mm;
        }
        c->members = head.next;
        struct initializer *last = init->children[cur->index];
        if (last->type->kind == TY_ARRAY && last->type->array_len > 0) {
            cur->type = last->type;
            c->size = align_to(cur->offset + last->type->size, c->align);
        }
        c->variants = 0;
        c->origin = 0;
        *new_ty = c;
        init->type = c;
        return init;
    }
    *new_ty = init->type;
    return init;
}

/* A local's initializer: assignments, after the whole is zeroed (every
 * member without an initializer is 0, 6.7.9p21). */
static struct node *init_assignments(struct initializer *init, struct node *target, struct token *tok)
{
    struct type *ty = init->type;
    if (ty->kind == TY_ARRAY && !init->expr) {
        struct node *list = new_node(ND_NOP, tok);
        list->type = ty_void;
        for (int i = 0; i < init->nchildren; i++) {
            /* target[i]: *(base + i), the base being the array's address */
            struct node *base = new_unary(ND_ADDR, target, pointer_to(ty->base), tok);
            struct node *at = new_add(base, new_num(i, ty_long, tok), tok);
            struct node *elem = new_unary(ND_DEREF, at, ty->base, tok);
            struct node *rhs = init_assignments(init->children[i], elem, tok);
            list = new_bin(ND_COMMA, list, rhs, ty_void, tok);
        }
        return list;
    }
    if (ty->kind == TY_STRUCT && !init->expr) {
        struct node *list = new_node(ND_NOP, tok);
        list->type = ty_void;
        for (struct member *m = ty->members; m; m = m->next) {
            struct node *ref = new_node(ND_MEMBER, tok);
            ref->lhs = target;
            ref->member = m;
            ref->type = m->type;
            struct node *rhs = init_assignments(init->children[m->index], ref, tok);
            list = new_bin(ND_COMMA, list, rhs, ty_void, tok);
        }
        return list;
    }
    if (ty->kind == TY_UNION && !init->expr) {
        struct member *m = init->member ? init->member : first_member(ty);
        if (!m) {
            struct node *n = new_node(ND_NOP, tok);
            n->type = ty_void;
            return n;
        }
        struct node *ref = new_node(ND_MEMBER, tok);
        ref->lhs = target;
        ref->member = m;
        ref->type = m->type;
        return init_assignments(init->children[m->index], ref, tok);
    }
    if (!init->expr) {
        struct node *n = new_node(ND_NOP, tok);
        n->type = ty_void;
        return n;
    }
    struct node *rhs = convert_for_assign(value(init->expr), target->type, init->tok ? init->tok : tok);
    return new_bin(ND_ASSIGN, target, rhs, unqual(target->type), tok);
}

static struct node *local_initializer(struct token **rest, struct token *t, struct obj *var)
{
    struct type *new_ty;
    struct initializer *init = initializer(rest, t, var->type, &new_ty);
    var->type = new_ty;
    if (var->align < new_ty->align)
        var->align = new_ty->align;
    struct node *target = new_var_node(var, t);
    struct node *assigns = init_assignments(init, target, t);
    /* (61c) a scalar's initializer gives all of it: no zeros first */
    if (is_scalar(unqual(new_ty)) && init->expr)
        return assigns;
    struct node *zero = new_node(ND_MEMZERO, t);
    zero->var = var;
    zero->type = ty_void;
    return new_bin(ND_COMMA, zero, assigns, ty_void, t);
}

/* --- constant initializers ----------------------------------------------- */

static int64_t eval_reloc(struct node *n, char **label);
static double eval_double(struct node *n);

/* (63) a NaN that arithmetic here makes is the positive quiet NaN, as clang
 * folds it (negation and conversions keep a NaN's sign, as there):
 * the machine running cc gives its own (x86's 0.0/0.0 has the sign bit
 * set, ARM's not), and the programs it built on RitterOS x86 then differed
 * from the Mac's build in that bit */
static double quiet_nan(void)
{
    union { uint64_t u; double d; } q = { 0x7FF8000000000000ull };
    return q.d;
}

static double canonical(double d)
{
    return d == d ? d : quiet_nan();
}

static void eval_complex(struct node *n, double *re, double *im);
static unsigned __int128 eval128(struct node *n);

static void write_int(char *buf, int64_t v, int size)
{
    /* (62a: an __int128's upper half is v's sign) */
    for (int i = 0; i < size; i++)
        buf[i] = (char)(i < 8 ? v >> (8 * i) : v >> 63);
}

static void write_float(char *buf, double v, struct type *ty, struct token *tok)
{
    if (ty->kind == TY_FLOAT) {
        float f = (float)v;
        memcpy(buf, &f, 4);
    } else if (ty->kind == TY_DOUBLE)
        memcpy(buf, &v, 8);
    else {
        /* long double from a double: its exact value in the target's
         * format (x87's 80 bits, or binary128) */
        uint64_t bits;
        memcpy(&bits, &v, 8);
        int sign = (int)(bits >> 63);
        int exp = (int)((bits >> 52) & 0x7FF);
        uint64_t frac = bits & 0xFFFFFFFFFFFFFull;
        memset(buf, 0, 16);
        if (exp == 0 && frac == 0) {
            buf[target.arch == ARCH_X86_64 ? 9 : 15] = (char)(sign << 7);
            return;
        }
        int e;
        if (exp == 0x7FF)
            e = 0x7FFF;
        else if (exp == 0) {
            /* a subnormal double is a normal long double */
            int shift = 0;
            while (!(frac & (1ull << 52))) {
                frac <<= 1;
                shift++;
            }
            frac &= ~(1ull << 52);
            e = 1 - 1023 - shift + 16383;
        } else
            e = exp - 1023 + 16383;
        if (target.arch == ARCH_X86_64) {
            uint64_t mant = (exp == 0x7FF ? (1ull << 63) : (1ull << 63)) | (frac << 11);
            write_int(buf, (int64_t)mant, 8);
            write_int(buf + 8, (int64_t)(e | (sign << 15)), 2);
        } else {
            /* 112 fraction bits: the 52 of the double at the top */
            uint64_t lo = frac << 60, hi = (frac >> 4) | ((uint64_t)e << 48) | ((uint64_t)sign << 63);
            write_int(buf, (int64_t)lo, 8);
            write_int(buf + 8, (int64_t)hi, 8);
        }
        (void)tok;
    }
}

static struct reloc *write_static(struct initializer *init, struct type *ty, char *buf, int offset,
                                  struct reloc *relocs)
{
    if (ty->kind == TY_ARRAY && !init->expr) {
        int sz = ty->base->size;
        for (int i = 0; i < init->nchildren; i++)
            relocs = write_static(init->children[i], ty->base, buf, offset + sz * i, relocs);
        return relocs;
    }
    if (ty->kind == TY_STRUCT && !init->expr) {
        for (struct member *m = ty->members; m; m = m->next) {
            struct initializer *c = init->children[m->index];
            if (m->is_bitfield) {
                if (!c->expr)
                    continue;
                int64_t v = eval(c->expr);
                char *at = buf + offset + m->offset;
                uint64_t unit = 0;
                for (int i = 0; i < m->type->size; i++)
                    unit |= (uint64_t)(unsigned char)at[i] << (8 * i);
                uint64_t mask = m->bit_width == 64 ? ~0ull : ((1ull << m->bit_width) - 1);
                unit = (unit & ~(mask << m->bit_offset)) | (((uint64_t)v & mask) << m->bit_offset);
                write_int(at, (int64_t)unit, m->type->size);
                continue;
            }
            relocs = write_static(c, m->type, buf, offset + m->offset, relocs);
        }
        return relocs;
    }
    if (ty->kind == TY_UNION && !init->expr) {
        struct member *m = init->member ? init->member : first_member(ty);
        if (!m)
            return relocs;
        return write_static(init->children[m->index], m->type, buf, offset + m->offset, relocs);
    }
    if (!init->expr)
        return relocs;
    struct node *e = value(init->expr);
    if (is_struct_union(ty))
        error_at(init->tok ? init->tok : e->tok, "initializer element is not a compile-time constant");
    e = convert_for_assign(e, ty, init->tok ? init->tok : e->tok);
    if (is_complex(ty)) {
        double re, im;
        eval_complex(e, &re, &im);
        struct type *rt = unqual(ty)->base;
        write_float(buf + offset, re, rt, e->tok);
        write_float(buf + offset + rt->size, im, rt, e->tok);
        return relocs;
    }
    if (is_flonum(ty)) {
        write_float(buf + offset, eval_double(e), ty, e->tok);
        return relocs;
    }
    if (unqual(ty)->kind == TY_INT128) {
        unsigned __int128 w = eval128(e);
        write_int(buf + offset, (int64_t)(uint64_t)w, 8);
        write_int(buf + offset + 8, (int64_t)(uint64_t)(w >> 64), 8);
        return relocs;
    }
    char *label = 0;
    int64_t v = eval_reloc(e, &label);
    if (!label) {
        write_int(buf + offset, v, ty->size);
        return relocs;
    }
    if (ty->size != 8)
        error_at(e->tok, "initializer element is not a compile-time constant");
    struct reloc *r = arena_alloc(sizeof(*r));
    r->offset = offset;
    r->sym = label;
    r->addend = v;
    r->next = relocs;
    return r;
}

static void global_initializer(struct token **rest, struct token *t, struct obj *var)
{
    struct type *new_ty;
    struct initializer *init = initializer(rest, t, var->type, &new_ty);
    var->type = new_ty;
    if (new_ty->size < 0)
        error_at(t, "variable has incomplete type");
    char *buf = arena_alloc((size_t)new_ty->size + 1);
    var->relocs = write_static(init, new_ty, buf, 0, 0);
    var->init_data = buf;
}

/* --- conversions --------------------------------------------------------- */

static bool is_lvalue(struct node *n)
{
    return n->kind == ND_VAR || n->kind == ND_DEREF || n->kind == ND_MEMBER ||
           (n->kind == ND_COMMA && n->rhs && is_lvalue(n->rhs) && n->type == n->rhs->type &&
            n->rhs->kind == ND_VAR && n->rhs->var->is_local && n->rhs->var->name[0] == '.');
}

/* The size of a VLA type, computed when its declaration ran. */
static struct node *vla_size_of(struct type *ty, struct token *tok);

static struct node *size_of_type(struct type *ty, struct token *tok)
{
    if (ty->kind == TY_VLA)
        return vla_size_of(ty, tok);
    return new_num(ty->size, ty_ulong, tok);
}

/* An expression used for its value: arrays and functions become their
 * address (6.3.2.1p3-4), an lvalue loses its qualifiers, and a small
 * bit-field is an int. */
static struct node *value(struct node *n)
{
    struct type *ty = n->type;
    if (ty->kind == TY_ARRAY || ty->kind == TY_VLA)
        return new_unary(ND_ADDR, n, pointer_to(ty->base), n->tok);
    if (ty->kind == TY_FUNC)
        return new_unary(ND_ADDR, n, pointer_to(ty), n->tok);
    if (n->kind == ND_MEMBER && n->member->is_bitfield && integer_rank(unqual(ty)) <= 3 &&
        (n->member->bit_width < 32 || !unqual(ty)->is_unsigned)) {
        return new_cast(n, ty_int);
    }
    if ((ty->qual & Q_ATOMIC) && is_lvalue(n))
        return atomic_read(n);                  /* (65e-52) */
    if (ty->qual) {
        struct node *c = new_node(ND_CAST, n->tok);
        c->lhs = n;
        c->type = unqual(ty);
        c->value = -1;                          /* (only the qualifiers go: no conversion) */
        return c;
    }
    return n;
}

static bool is_null_pointer_constant(struct node *n)
{
    extern bool is_const_expr(struct node * n);
    while (n->kind == ND_CAST && n->type->kind == TY_PTR && unqual(n->type->base)->kind == TY_VOID)
        n = n->lhs;
    while (n->kind == ND_CAST && n->value == -1)
        n = n->lhs;
    return is_integer(n->type) && is_const_expr(n) && eval(n) == 0;
}

static struct node *convert_for_assign(struct node *expr, struct type *ty, struct token *where)
{
    struct type *to = unqual(ty), *from = unqual(expr->type);
    if (to->kind == TY_VOID)
        error_at(where, "cannot assign to void");
    if (is_struct_union(to)) {
        if (!is_struct_union(from) || (unqual(to->origin ? to->origin : to) != unqual(from->origin ? from->origin : from) &&
                                       to->members != from->members))
            error_at(where, "assigning to '%s' from incompatible type '%s'", type_name(to), type_name(from));
        return expr;
    }
    if (is_struct_union(from))
        error_at(where, "assigning to '%s' from incompatible type '%s'", type_name(to), type_name(from));
    if (from->kind == TY_VOID)
        error_at(where, "assigning to '%s' from incompatible type 'void'", type_name(to));
    if (to->kind == TY_PTR && is_flonum(from))
        error_at(where, "assigning to '%s' from incompatible type '%s'", type_name(to), type_name(from));
    if (is_flonum(to) && from->kind == TY_PTR)
        error_at(where, "assigning to '%s' from incompatible type '%s'", type_name(to), type_name(from));
    return new_cast(expr, to);
}

/* Pointer arithmetic: the integer counts elements (6.5.6p8). */
static struct node *scaled(struct node *index, struct type *elem, struct token *tok)
{
    index = new_cast(index, ty_long);
    if (elem->kind == TY_VLA)
        return new_bin(ND_MUL, index, new_cast(vla_size_of(elem, tok), ty_long), ty_long, tok);
    int size = unqual(elem)->kind == TY_VOID || elem->kind == TY_FUNC ? 1 : elem->size;
    if (size < 0)
        error_at(tok, "arithmetic on a pointer to an incomplete type '%s'", type_name(elem));
    if (size == 1)
        return index;
    return new_bin(ND_MUL, index, new_num(size, ty_long, tok), ty_long, tok);
}

/* (62a) An operand of a complex operation: a complex one made the result's
 * type, a real one only its real type (x * z is then done part by part,
 * as clang does: infinities and NaNs as C's Annex G) */
static struct node *complex_operand(struct node *x, struct type *ty)
{
    return new_cast(x, is_complex(x->type) ? ty : real_of(ty));
}

static struct node *arith(enum node_kind kind, struct node *a, struct node *b, struct token *tok)
{
    if (!is_arithmetic(a->type) || !is_arithmetic(b->type))
        error_at(tok, "invalid operands to binary expression ('%s' and '%s')", type_name(a->type),
                 type_name(b->type));
    if ((kind == ND_MOD || kind == ND_BITAND || kind == ND_BITOR || kind == ND_BITXOR) &&
        (!is_integer(a->type) || !is_integer(b->type)))
        error_at(tok, "invalid operands to binary expression ('%s' and '%s')", type_name(a->type),
                 type_name(b->type));
    struct type *ty = usual_arithmetic(a->type, b->type);
    if (is_complex(ty))
        return new_bin(kind, complex_operand(a, ty), complex_operand(b, ty), ty, tok);
    return new_bin(kind, new_cast(a, ty), new_cast(b, ty), ty, tok);
}

static struct node *new_add(struct node *a, struct node *b, struct token *tok)
{
    a = value(a);
    b = value(b);
    if (is_arithmetic(a->type) && is_arithmetic(b->type))
        return arith(ND_ADD, a, b, tok);
    if (a->type->kind == TY_PTR && b->type->kind == TY_PTR)
        error_at(tok, "invalid operands to binary expression");
    if (b->type->kind == TY_PTR) {
        struct node *t = a;
        a = b;
        b = t;
    }
    if (a->type->kind != TY_PTR || !is_integer(b->type))
        error_at(tok, "invalid operands to binary expression ('%s' and '%s')", type_name(a->type),
                 type_name(b->type));
    return new_bin(ND_ADD, a, scaled(b, a->type->base, tok), a->type, tok);
}

static struct node *new_sub(struct node *a, struct node *b, struct token *tok)
{
    a = value(a);
    b = value(b);
    if (is_arithmetic(a->type) && is_arithmetic(b->type))
        return arith(ND_SUB, a, b, tok);
    if (a->type->kind == TY_PTR && is_integer(b->type))
        return new_bin(ND_SUB, a, scaled(b, a->type->base, tok), a->type, tok);
    if (a->type->kind == TY_PTR && b->type->kind == TY_PTR) {
        /* the difference in elements, a ptrdiff_t */
        struct node *diff = new_bin(ND_SUB, new_cast(a, ty_long), new_cast(b, ty_long), ty_long, tok);
        struct type *elem = a->type->base;
        int size = unqual(elem)->kind == TY_VOID ? 1 : elem->size;
        if (elem->kind == TY_VLA)
            return new_bin(ND_DIV, diff, new_cast(vla_size_of(elem, tok), ty_long), ty_long, tok);
        if (size == 1)
            return diff;
        return new_bin(ND_DIV, diff, new_num(size, ty_long, tok), ty_long, tok);
    }
    error_at(tok, "invalid operands to binary expression ('%s' and '%s')", type_name(a->type), type_name(b->type));
}

static struct node *new_shift(enum node_kind kind, struct node *a, struct node *b, struct token *tok)
{
    a = value(a);
    b = value(b);
    if (!is_integer(a->type) || !is_integer(b->type))
        error_at(tok, "invalid operands to binary expression");
    struct type *ty = integer_promotion(a->type);
    return new_bin(kind, new_cast(a, ty), new_cast(b, integer_promotion(b->type)), ty, tok);
}

static struct node *new_compare(enum node_kind kind, struct node *a, struct node *b, struct token *tok)
{
    a = value(a);
    b = value(b);
    if (is_arithmetic(a->type) && is_arithmetic(b->type)) {
        struct type *ty = usual_arithmetic(a->type, b->type);
        if (is_complex(ty)) {
            if (kind != ND_EQ && kind != ND_NE)
                error_at(tok, "invalid operands to comparison ('%s' and '%s')", type_name(a->type),
                         type_name(b->type));
            return new_bin(kind, complex_operand(a, ty), complex_operand(b, ty), ty_int, tok);
        }
        return new_bin(kind, new_cast(a, ty), new_cast(b, ty), ty_int, tok);
    }
    if (a->type->kind == TY_PTR || b->type->kind == TY_PTR) {
        /* pointers compare as addresses; 0 and integers too (with GCC) */
        if (!(a->type->kind == TY_PTR || is_integer(a->type)) || !(b->type->kind == TY_PTR || is_integer(b->type)))
            error_at(tok, "invalid operands to comparison");
        return new_bin(kind, new_cast(a, ty_ulong), new_cast(b, ty_ulong), ty_int, tok);
    }
    error_at(tok, "invalid operands to comparison ('%s' and '%s')", type_name(a->type), type_name(b->type));
}

static struct node *to_condition(struct node *n)
{
    n = value(n);
    if (!is_scalar(n->type))
        error_at(n->tok, "statement requires expression of scalar type ('%s' invalid)", type_name(n->type));
    return n;
}

/* lhs op= rhs, as  tmp = &lhs, *tmp = *tmp op rhs  (lhs evaluated once);
 * a plain variable needs no temporary. A bit-field's structure is what the
 * temporary points to. */
static struct node *make_op(enum node_kind kind, struct node *a, struct node *b, struct token *tok)
{
    switch (kind) {
    case ND_ADD: return new_add(a, b, tok);
    case ND_SUB: return new_sub(a, b, tok);
    case ND_SHL: case ND_SHR: return new_shift(kind, a, b, tok);
    default: return arith(kind, value(a), value(b), tok);
    }
}

static struct node *assignment(struct node *lhs, struct node *rhs, struct token *tok);

static struct node *lvalue_once(struct node *lhs, struct node **setup, struct token *tok)
{
    if (lhs->kind == ND_VAR) {
        *setup = 0;
        return lhs;
    }
    if (lhs->kind == ND_MEMBER && lhs->member->is_bitfield) {
        struct node *inner = lvalue_once(lhs->lhs, setup, tok);
        struct node *m = new_node(ND_MEMBER, tok);
        *m = *lhs;
        m->lhs = inner;
        return m;
    }
    struct obj *tmp = new_local(".tmp", pointer_to(lhs->type), tok);
    struct node *addr = new_unary(ND_ADDR, lhs, pointer_to(lhs->type), tok);
    *setup = new_bin(ND_ASSIGN, new_var_node(tmp, tok), addr, tmp->type, tok);
    return new_unary(ND_DEREF, new_var_node(tmp, tok), lhs->type, tok);
}

static struct node *with_setup(struct node *setup, struct node *expr, struct token *tok)
{
    if (!setup)
        return expr;
    return new_bin(ND_COMMA, setup, expr, expr->type, tok);
}

/*
 * (65e-52) C11's atomic objects (6.2.6.1p9, 6.5.2.4p2, 6.5.16.2p3): a read
 * of an _Atomic lvalue is an atomic load, an assignment an atomic store,
 * and ++, --, op= one atomic read-modify-write, all sequentially
 * consistent (the __atomic_ functions' instructions). +=, -=, &=, |=, ^=
 * of an integer (+= and -= of a pointer too) with an integer are one
 * instruction's worth (fetch-and-op); every other one a compare-and-swap
 * loop over what the operator computes. Floating types go through an
 * integer of their size (the atomics work in integer registers). Types
 * other than scalars of 1, 2, 4 or 8 bytes would need a lock (clang calls
 * libatomic): refused.
 */
static struct node *once(struct node *x, struct node **setup, struct token *tok);
static struct node *builtin_node(enum builtin b, struct type *ty, struct token *tok);

static struct type *atomic_bits(struct type *ty, struct token *tok)
{
    ty = unqual(ty);
    if (!is_scalar(ty) || is_complex(ty) || (ty->size != 1 && ty->size != 2 && ty->size != 4 && ty->size != 8))
        error_at(tok, "atomic operations on '%s' are not supported", type_name(ty));
    if (is_flonum(ty))
        return ty->size == 4 ? ty_uint : ty_ulong;
    return ty;
}

/* the atomic object's address, as a pointer to its bits */
static struct node *atomic_address(struct node *lv, struct type *bits, struct token *tok)
{
    return new_cast(new_unary(ND_ADDR, lv, pointer_to(lv->type), tok), pointer_to(bits));
}

/* an __atomic_ function on addr: its value operands (0, 1 or 2) and the
 * memory orders, sequentially consistent */
static struct node *atomic_op(enum builtin b, struct node *addr, struct node *x, struct node *y,
                              struct type *ty, struct token *tok)
{
    struct node *n = builtin_node(b, ty, tok);
    struct node head = { 0 }, *cur = &head;
    cur = cur->next = addr;
    if (x)
        cur = cur->next = x;
    if (y) {
        cur = cur->next = y;
        cur = cur->next = new_num(0, ty_bool, tok);         /* (strong) */
        cur = cur->next = new_num(__ATOMIC_SEQ_CST, ty_int, tok);
    }
    cur = cur->next = new_num(__ATOMIC_SEQ_CST, ty_int, tok);
    cur->next = 0;
    n->args = head.next;
    return n;
}

/* x's bits seen as type `to` (both the same size): through a temporary */
static struct node *reinterpret(struct node *x, struct type *to, struct token *tok)
{
    if (unqual(x->type) == to)
        return x;
    struct obj *tmp = new_local(".tmp", unqual(x->type), tok);
    struct node *set = new_bin(ND_ASSIGN, new_var_node(tmp, tok), x, tmp->type, tok);
    struct node *as = new_unary(ND_DEREF, new_cast(new_unary(ND_ADDR, new_var_node(tmp, tok),
                                                             pointer_to(tmp->type), tok), pointer_to(to)), to, tok);
    return new_bin(ND_COMMA, set, as, to, tok);
}

static struct node *atomic_read(struct node *n)
{
    struct type *ty = unqual(n->type), *bits = atomic_bits(ty, n->tok);
    struct node *load = atomic_op(B_ATOMIC_LOAD, atomic_address(n, bits, n->tok), 0, 0, bits, n->tok);
    return reinterpret(load, ty, n->tok);
}

/* lhs = rhs (rhs converted already): the value stored */
static struct node *atomic_store(struct node *lhs, struct node *rhs, struct token *tok)
{
    struct node *setup, *vset;
    struct node *target = lvalue_once(lhs, &setup, tok);
    struct type *ty = unqual(target->type), *bits = atomic_bits(ty, tok);
    struct node *v = once(rhs, &vset, tok);
    struct node *store = atomic_op(B_ATOMIC_STORE, atomic_address(target, bits, tok),
                                   reinterpret(v, bits, tok), 0, ty_void, tok);
    struct node *e = new_bin(ND_COMMA, vset, new_bin(ND_COMMA, store, v, ty, tok), ty, tok);
    return with_setup(setup, e, tok);
}

static struct node *make_op(enum node_kind kind, struct node *a, struct node *b, struct token *tok);
static struct node *convert_for_assign(struct node *expr, struct type *ty, struct token *where);
static struct node *new_compare(enum node_kind kind, struct node *a, struct node *b, struct token *tok);

/* lhs op= rhs: the new value, or (post) the old one */
static struct node *atomic_update(enum node_kind kind, struct node *lhs, struct node *rhs, bool post,
                                  struct token *tok)
{
    struct node *setup;
    struct node *target = lvalue_once(lhs, &setup, tok);
    struct type *ty = unqual(target->type), *bits = atomic_bits(ty, tok);
    rhs = value(rhs);
    bool integer = is_integer(ty) && ty->kind != TY_BOOL && is_integer(rhs->type);
    bool pointer = ty->kind == TY_PTR && is_integer(rhs->type) && (kind == ND_ADD || kind == ND_SUB);
    if (pointer || (integer && (kind == ND_ADD || kind == ND_SUB || kind == ND_BITAND ||
                                kind == ND_BITOR || kind == ND_BITXOR))) {
        static const struct { enum node_kind kind; enum builtin new_value, old_value; } ops[] = {
            { ND_ADD, B_ATOMIC_ADD_FETCH, B_ATOMIC_FETCH_ADD }, { ND_SUB, B_ATOMIC_SUB_FETCH, B_ATOMIC_FETCH_SUB },
            { ND_BITAND, B_ATOMIC_AND_FETCH, B_ATOMIC_FETCH_AND }, { ND_BITOR, B_ATOMIC_OR_FETCH, B_ATOMIC_FETCH_OR },
            { ND_BITXOR, B_ATOMIC_XOR_FETCH, B_ATOMIC_FETCH_XOR },
        };
        enum builtin b = B_ATOMIC_LOAD;
        for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); k++)
            if (ops[k].kind == kind)
                b = post ? ops[k].old_value : ops[k].new_value;
        struct node *x = pointer ? new_cast(scaled(rhs, ty->base, tok), ty) : new_cast(rhs, ty);
        return with_setup(setup, atomic_op(b, atomic_address(target, bits, tok), x, 0, ty, tok), tok);
    }
    /* ({ r = rhs; old = load; do new = old op r; while (!cas(&old, new)); new or old; }) */
    struct node *rset;
    struct node *r = once(rhs, &rset, tok);
    struct obj *old = new_local(".tmp", bits, tok), *now = new_local(".tmp", ty, tok);
    struct node *load = atomic_op(B_ATOMIC_LOAD, atomic_address(target, bits, tok), 0, 0, bits, tok);
    struct node *first = new_bin(ND_ASSIGN, new_var_node(old, tok), load, bits, tok);
    struct node *op = make_op(kind, reinterpret(new_var_node(old, tok), ty, tok), r, tok);
    struct node *step = new_bin(ND_ASSIGN, new_var_node(now, tok), convert_for_assign(op, ty, tok), ty, tok);
    struct node *cas = atomic_op(B_ATOMIC_CAS, atomic_address(target, bits, tok),
                                 new_unary(ND_ADDR, new_var_node(old, tok), pointer_to(bits), tok),
                                 reinterpret(new_var_node(now, tok), bits, tok), ty_bool, tok);
    struct node *loop = new_node(ND_DO, tok);
    loop->then = new_unary(ND_EXPR_STMT, step, ty_void, tok);
    loop->cond = new_compare(ND_EQ, cas, new_num(0, ty_int, tok), tok);
    /* the new value, or the old: what the successful compare found */
    struct node *last = post ? reinterpret(new_var_node(old, tok), ty, tok) : new_var_node(now, tok);
    struct node *stmts[] = {
        new_unary(ND_EXPR_STMT, rset, ty_void, tok),
        new_unary(ND_EXPR_STMT, first, ty_void, tok),
        loop,
        0,
    };
    stmts[3] = new_unary(ND_EXPR_STMT, last, last->type, tok);
    struct node *e = new_node(ND_STMT_EXPR, tok);
    e->body = stmts[0];
    for (int k = 0; k < 3; k++)
        stmts[k]->next = stmts[k + 1];
    stmts[3]->next = 0;
    e->type = ty;
    return with_setup(setup, e, tok);
}

static struct node *compound_assign(enum node_kind kind, struct node *lhs, struct node *rhs, struct token *tok)
{
    if (!is_lvalue(lhs))
        error_at(tok, "expression is not assignable");
    if (lhs->type->qual & Q_ATOMIC) {
        if (lhs->type->qual & Q_CONST)
            error_at(tok, "cannot assign to a variable with const-qualified type '%s'", type_name(lhs->type));
        return atomic_update(kind, lhs, rhs, false, tok);
    }
    struct node *setup;
    struct node *target = lvalue_once(lhs, &setup, tok);
    struct node *op = make_op(kind, target, rhs, tok);
    return with_setup(setup, assignment(target, op, tok), tok);
}

/* x++ and x--: the old value, through a temporary */
static struct node *post_step(struct node *lhs, int step, struct token *tok)
{
    if (!is_lvalue(lhs))
        error_at(tok, "expression is not assignable");
    if (lhs->type->qual & Q_ATOMIC)
        return atomic_update(step > 0 ? ND_ADD : ND_SUB, lhs, new_num(1, ty_int, tok), true, tok);
    struct node *setup;
    struct node *target = lvalue_once(lhs, &setup, tok);
    struct type *ty = value(target)->type;
    if (target->kind == ND_MEMBER && target->member->is_bitfield)
        ty = unqual(target->type);
    struct obj *old = new_local(".tmp", ty, tok);
    struct node *save = new_bin(ND_ASSIGN, new_var_node(old, tok), value(target), ty, tok);
    struct node *op = make_op(step > 0 ? ND_ADD : ND_SUB, new_var_node(old, tok), new_num(1, ty_int, tok), tok);
    struct node *store = assignment(target, op, tok);
    struct node *e = new_bin(ND_COMMA, save, store, store->type, tok);
    e = new_bin(ND_COMMA, e, new_var_node(old, tok), ty, tok);
    return with_setup(setup, e, tok);
}

static struct node *assignment(struct node *lhs, struct node *rhs, struct token *tok)
{
    if (!is_lvalue(lhs))
        error_at(tok, "expression is not assignable");
    if (lhs->type->qual & Q_CONST)
        error_at(tok, "cannot assign to a variable with const-qualified type '%s'", type_name(lhs->type));
    if (lhs->type->kind == TY_ARRAY)
        error_at(tok, "array type '%s' is not assignable", type_name(lhs->type));
    rhs = convert_for_assign(value(rhs), lhs->type, tok);
    if (lhs->type->qual & Q_ATOMIC)
        return atomic_store(lhs, rhs, tok);
    return new_bin(ND_ASSIGN, lhs, rhs, unqual(lhs->type), tok);
}

/* --- members ------------------------------------------------------------- */

struct member *find_member(struct type *ty, struct token *name, int64_t *offset_out)
{
    for (struct member *m = ty->members; m; m = m->next) {
        if (!m->name && is_struct_union(m->type)) {
            if (find_member(m->type, name, offset_out))
                return m;
            continue;
        }
        if (m->name && m->name->len == name->len && memcmp(m->name->text, name->text, (size_t)name->len) == 0) {
            *offset_out = m->offset;
            return m;
        }
    }
    return 0;
}

/* n.name, through anonymous structures and unions */
static struct node *struct_ref(struct node *n, struct token *name)
{
    struct type *ty = n->type;
    if (!is_struct_union(ty))
        error_at(name, "member reference base type '%s' is not a structure or union", type_name(ty));
    if (ty->size < 0)
        error_at(name, "incomplete definition of type '%s'", type_name(ty));
    for (;;) {
        int64_t ignored;
        struct member *m = find_member(ty, name, &ignored);
        if (!m)
            error_at(name, "no member named '%.*s' in '%s'", name->len, name->text, type_name(ty));
        struct node *ref = new_node(ND_MEMBER, name);
        ref->lhs = n;
        ref->member = m;
        /* a member of a qualified structure is qualified the same way */
        ref->type = ty->qual ? qualified(m->type, ty->qual) : m->type;
        if (m->name)
            return ref;
        n = ref;
        ty = m->type;
    }
}

/* --- builtins ------------------------------------------------------------ */

static struct node *expr(struct token **rest, struct token *t);
static struct node *cast(struct token **rest, struct token *t);
static struct node *unary(struct token **rest, struct token *t);
static struct node *postfix(struct token **rest, struct token *t);
static struct node *primary(struct token **rest, struct token *t);
static struct node *compound_stmt(struct token **rest, struct token *t);
static int block_seq, stmt_expr_depth;          /* (61d) block numbers, ({ ... }) depth */
static struct node *funcall(struct token **rest, struct token *t, struct node *fn);

/* The address of a va_list's structure: x86-64's va_list is an array of
 * one (so the value is it), AArch64's the structure itself. */
static struct node *va_pointer(struct node *ap)
{
    if (ap->type->kind == TY_ARRAY || ap->type->kind == TY_PTR)
        return value(ap);
    return new_unary(ND_ADDR, ap, pointer_to(ap->type), ap->tok);
}

static struct node *builtin_node(enum builtin b, struct type *ty, struct token *tok)
{
    struct node *n = new_node(ND_BUILTIN, tok);
    n->builtin = b;
    n->type = ty;
    return n;
}

static struct node *call_args(struct token **rest, struct token *t, int count)
{
    t = skip(t, "(");
    struct node head = { 0 }, *cur = &head;
    for (int i = 0; i < count; i++) {
        if (i)
            t = skip(t, ",");
        cur = cur->next = value(assign(&t, t));
    }
    *rest = skip(t, ")");
    return head.next;
}

/* A function the library provides, declared if the program did not. */
static struct obj *library_function(const char *name, struct type *ret, struct type *arg)
{
    struct token fake = { .kind = TK_IDENT, .text = name, .len = (int)strlen(name) };
    for (struct scope *s = &file_scope; s; s = 0) {
        struct name_entry *e = map_get(&s->names, name, (int)strlen(name));
        if (e && e->var)
            return e->var;
    }
    struct type *fn = func_type(ret);
    fn->has_prototype = true;
    fn->params = arena_alloc(sizeof(struct param));
    fn->params->type = arg;
    struct obj *o = arena_alloc(sizeof(*o));
    o->name = arena_strndup(name, strlen(name));
    o->type = fn;
    o->is_function = true;
    o->tok = 0;
    add_global(o);
    struct name_entry *e = arena_alloc(sizeof(*e));
    e->var = o;
    map_put(&file_scope.names, o->name, (int)strlen(o->name), e);
    (void)fake;
    return o;
}

static struct node *call_library(const char *name, struct type *ty, struct node *arg, struct token *tok)
{
    struct obj *fn = library_function(name, ty, ty);
    struct node *call = new_node(ND_CALL, tok);
    call->lhs = value(new_var_node(fn, tok));
    call->func_type = fn->type;
    call->args = new_cast(arg, ty);
    call->type = ty;
    return call;
}

/* A temporary holding x, so x is evaluated once. */
static struct node *once(struct node *x, struct node **setup, struct token *tok)
{
    struct obj *tmp = new_local(".tmp", x->type, tok);
    *setup = new_bin(ND_ASSIGN, new_var_node(tmp, tok), x, x->type, tok);
    return new_var_node(tmp, tok);
}

static struct node *float_constant(double v, struct type *ty, struct token *tok)
{
    return new_fnum(v, ty, tok);
}

static struct type *float_arg_type(struct node *x)
{
    struct type *ty = unqual(x->type);
    return is_flonum(ty) ? ty : ty_double;
}

static struct node *logical(enum node_kind kind, struct node *a, struct node *b, struct token *tok)
{
    return new_bin(kind, to_condition(a), to_condition(b), ty_int, tok);
}

static struct node *classify(enum builtin b, struct node *x, struct token *tok)
{
    struct type *ty = float_arg_type(x);
    struct node *setup;
    struct node *v = once(new_cast(x, ty), &setup, tok);
    double inf = 1.0 / 0.0;
    struct node *r;
    switch (b) {
    case B_ISNAN:
        r = new_compare(ND_NE, v, v, tok);
        break;
    case B_ISINF:
        r = logical(ND_LOGOR, new_compare(ND_EQ, v, float_constant(inf, ty, tok), tok),
                    new_compare(ND_EQ, v, float_constant(-inf, ty, tok), tok), tok);
        break;
    case B_ISFINITE:
        /* x - x is 0 for finite x, NaN otherwise */
        r = new_compare(ND_EQ, new_sub(v, v, tok), float_constant(0, ty, tok), tok);
        break;
    case B_ISNORMAL: {
        double min = ty->kind == TY_FLOAT ? 1.17549435e-38 : 2.2250738585072014e-308;
        double max = ty->kind == TY_FLOAT ? 3.40282347e+38 : 1.7976931348623157e+308;
        if (ty->kind == TY_LDOUBLE)
            min = 2.2250738585072014e-308;      /* (at double precision) */
        struct node *pos = logical(ND_LOGAND, new_compare(ND_LE, float_constant(min, ty, tok), v, tok),
                                   new_compare(ND_LE, v, float_constant(max, ty, tok), tok), tok);
        struct node *neg = logical(ND_LOGAND, new_compare(ND_LE, float_constant(-max, ty, tok), v, tok),
                                   new_compare(ND_LE, v, float_constant(-min, ty, tok), tok), tok);
        if (ty->kind == TY_LDOUBLE) {
            /* any finite nonzero value at least the smallest normal long double */
            pos = new_compare(ND_LT, float_constant(0, ty, tok), v, tok);
            pos = logical(ND_LOGAND, pos, new_compare(ND_LT, v, float_constant(inf, ty, tok), tok), tok);
            neg = new_compare(ND_LT, v, float_constant(0, ty, tok), tok);
            neg = logical(ND_LOGAND, neg, new_compare(ND_LT, float_constant(-inf, ty, tok), v, tok), tok);
        }
        r = logical(ND_LOGOR, pos, neg, tok);
        break;
    }
    case B_SIGNBIT: {
        /* the top bit of the most significant byte */
        int byte = ty->kind == TY_FLOAT ? 3 : ty->kind == TY_DOUBLE ? 7 : target.arch == ARCH_X86_64 ? 9 : 15;
        struct node *addr = new_unary(ND_ADDR, v, pointer_to(ty), tok);
        struct node *bytes = new_cast(addr, pointer_to(ty_uchar));
        struct node *at = new_add(bytes, new_num(byte, ty_int, tok), tok);
        struct node *top = new_unary(ND_DEREF, at, ty_uchar, tok);
        r = new_shift(ND_SHR, top, new_num(7, ty_int, tok), tok);
        break;
    }
    default:
        error_at(tok, "internal error: classify");
    }
    return new_bin(ND_COMMA, setup, r, r->type, tok);
}

static struct node *atomic_builtin(enum builtin b, struct token **rest, struct token *t, int nargs, bool steps)
{
    struct token *tok = t;
    struct node *args = call_args(rest, t->next, nargs);
    if (args->type->kind != TY_PTR)
        error_at(tok, "address argument to atomic builtin must be a pointer");
    struct type *ty = unqual(args->type->base);
    if (steps && ty->kind == TY_PTR)            /* (65e-52) clang's __c11_: a pointer moves by elements */
        args->next = scaled(args->next, ty->base, tok);
    struct node *n = builtin_node(b, ty, tok);
    n->args = args;
    switch (b) {
    case B_ATOMIC_STORE: case B_ATOMIC_CLEAR: case B_ATOMIC_FENCE:
        n->type = ty_void;
        break;
    case B_ATOMIC_CAS: case B_ATOMIC_TEST_AND_SET:
        n->type = ty_bool;
        break;
    default:
        break;
    }
    /* the value operands in the pointed type */
    if (b != B_ATOMIC_LOAD && b != B_ATOMIC_TEST_AND_SET && b != B_ATOMIC_CLEAR && b != B_ATOMIC_CAS)
        args->next = new_cast(args->next, ty);
    if (b == B_ATOMIC_CAS)
        args->next->next = new_cast(args->next->next, ty);
    return n;
}

static struct node *builtin(struct token **rest, struct token *t)
{
    struct token *tok = t;
    if (equal(t, "__builtin_va_start")) {
        t = skip(t->next, "(");
        struct node *ap = assign(&t, t);
        t = skip(t, ",");
        assign(&t, t);                          /* (the last named parameter) */
        *rest = skip(t, ")");
        struct node *n = new_node(ND_VA_START, tok);
        n->lhs = va_pointer(ap);
        n->type = ty_void;
        if (!current_fn || !current_fn->type->is_variadic)
            error_at(tok, "'va_start' used in function with fixed args");
        return n;
    }
    if (equal(t, "__builtin_va_end")) {
        struct node *ap;
        t = skip(t->next, "(");
        ap = assign(&t, t);
        *rest = skip(t, ")");
        struct node *n = new_node(ND_VA_END, tok);
        n->lhs = va_pointer(ap);
        n->type = ty_void;
        return n;
    }
    if (equal(t, "__builtin_va_copy")) {
        t = skip(t->next, "(");
        struct node *d = assign(&t, t);
        t = skip(t, ",");
        struct node *s = assign(&t, t);
        *rest = skip(t, ")");
        struct node *n = new_node(ND_VA_COPY, tok);
        n->lhs = va_pointer(d);
        n->rhs = va_pointer(s);
        n->type = ty_void;
        return n;
    }
    if (equal(t, "__builtin_va_arg")) {
        t = skip(t->next, "(");
        struct node *ap = assign(&t, t);
        t = skip(t, ",");
        struct type *ty = typename(&t, t);
        *rest = skip(t, ")");
        struct node *n = new_node(ND_VA_ARG, tok);
        n->lhs = va_pointer(ap);
        n->type = pointer_to(ty);
        return new_unary(ND_DEREF, n, ty, tok);
    }
    if (equal(t, "__builtin_offsetof")) {
        t = skip(t->next, "(");
        struct type *ty = typename(&t, t);
        t = skip(t, ",");
        int64_t offset = 0;
        for (bool first = true;; first = false) {
            if (first || consume(&t, t, ".")) {
                if (t->kind != TK_IDENT)
                    error_at(t, "expected a member name");
                if (!is_struct_union(ty))
                    error_at(t, "offsetof requires a structure or union type");
                int64_t mo;
                struct member *m;
                /* (through anonymous members) */
                for (;;) {
                    m = find_member(ty, t, &mo);
                    if (!m)
                        error_at(t, "no member named '%.*s'", t->len, t->text);
                    offset += m->offset;
                    if (m->name)
                        break;
                    ty = m->type;
                }
                ty = m->type;
                t = t->next;
            } else if (equal(t, "[")) {
                int64_t i = const_expr(&t, t->next);
                t = skip(t, "]");
                if (ty->kind != TY_ARRAY)
                    error_at(t, "subscript of a non-array in offsetof");
                ty = ty->base;
                offset += i * ty->size;
            } else
                break;
        }
        *rest = skip(t, ")");
        return new_num(offset, ty_ulong, tok);
    }
    if (equal(t, "__builtin_types_compatible_p")) {
        t = skip(t->next, "(");
        struct type *a = typename(&t, t);
        t = skip(t, ",");
        struct type *b = typename(&t, t);
        *rest = skip(t, ")");
        return new_num(types_compatible(unqual(a), unqual(b)), ty_int, tok);
    }
    static const struct {
        const char *name;
        enum builtin b;
        int nargs;
    } simple[] = {
        { "__builtin_clz", B_CLZ, 1 }, { "__builtin_clzl", B_CLZL, 1 }, { "__builtin_clzll", B_CLZL, 1 },
        { "__builtin_ctz", B_CTZ, 1 }, { "__builtin_ctzl", B_CTZL, 1 }, { "__builtin_ctzll", B_CTZL, 1 },
        { "__builtin_popcount", B_POPCOUNT, 1 }, { "__builtin_popcountl", B_POPCOUNTL, 1 },
        { "__builtin_popcountll", B_POPCOUNTL, 1 },
        { "__builtin_bswap16", B_BSWAP16, 1 }, { "__builtin_bswap32", B_BSWAP32, 1 },
        { "__builtin_bswap64", B_BSWAP64, 1 },
        { "__builtin_trap", B_TRAP, 0 }, { "__builtin_unreachable", B_UNREACHABLE, 0 },
        { "__builtin_alloca", B_ALLOCA, 1 },
    };
    for (size_t i = 0; i < sizeof(simple) / sizeof(simple[0]); i++) {
        if (!equal(t, simple[i].name))
            continue;
        struct node *args = call_args(rest, t->next, simple[i].nargs);
        struct type *ty;
        switch (simple[i].b) {
        case B_CLZ: case B_CTZ: case B_POPCOUNT: case B_CLZL: case B_CTZL: case B_POPCOUNTL:
            ty = ty_int;
            args = new_cast(args, simple[i].b == B_CLZ || simple[i].b == B_CTZ || simple[i].b == B_POPCOUNT
                                      ? ty_uint : ty_ulong);
            break;
        case B_BSWAP16: ty = ty_ushort; args = new_cast(args, ty_ushort); break;
        case B_BSWAP32: ty = ty_uint; args = new_cast(args, ty_uint); break;
        case B_BSWAP64: ty = ty_ulong; args = new_cast(args, ty_ulong); break;
        case B_ALLOCA: ty = pointer_to(ty_void); args = new_cast(args, ty_ulong); break;
        default: ty = ty_void;
        }
        struct node *n = builtin_node(simple[i].b, ty, tok);
        n->args = args;
        return n;
    }
    if (equal(t, "__builtin_complex")) {
        /* (62a) CMPLX: the parts of one real floating type */
        struct node *args = call_args(rest, t->next, 2), *a = args, *b = args->next;
        if (!is_flonum(a->type) || !is_flonum(b->type))
            error_at(tok, "__builtin_complex needs two floating arguments");
        struct type *rt = usual_arithmetic(a->type, b->type);
        a->next = 0;
        a = new_cast(a, rt);
        b = new_cast(b, rt);
        a->next = b;
        b->next = 0;
        struct node *n = builtin_node(B_COMPLEX, complex_of(rt), tok);
        n->args = a;
        return n;
    }
    if (equal(t, "__builtin_expect")) {
        struct node *args = call_args(rest, t->next, 2);
        return args;                            /* (the hint is not used) */
    }
    if (equal(t, "__builtin_constant_p")) {
        extern bool is_const_expr(struct node * n);
        t = skip(t->next, "(");
        struct node *e = assign(&t, t);
        *rest = skip(t, ")");
        return new_num(is_const_expr(e), ty_int, tok);
    }
    static const struct {
        const char *name;
        double value;
        struct type **type;
    } constants[] = {
        { "__builtin_inf", 1.0 / 0.0, &ty_double }, { "__builtin_inff", 1.0 / 0.0, &ty_float },
        { "__builtin_infl", 1.0 / 0.0, &ty_ldouble }, { "__builtin_huge_val", 1.0 / 0.0, &ty_double },
        { "__builtin_huge_valf", 1.0 / 0.0, &ty_float }, { "__builtin_huge_vall", 1.0 / 0.0, &ty_ldouble },
    };
    for (size_t i = 0; i < sizeof(constants) / sizeof(constants[0]); i++)
        if (equal(t, constants[i].name)) {
            *rest = skip(skip(t->next, "("), ")");
            return new_fnum(constants[i].value, *constants[i].type, tok);
        }
    if (equal(t, "__builtin_nan") || equal(t, "__builtin_nanf") || equal(t, "__builtin_nanl")) {
        struct type *ty = equal(t, "__builtin_nanf") ? ty_float : equal(t, "__builtin_nanl") ? ty_ldouble : ty_double;
        t = skip(t->next, "(");
        if (t->kind != TK_STR)
            error_at(t, "expected a string");
        *rest = skip(t->next, ")");
        return new_fnum(quiet_nan(), ty, tok);  /* (its bits: 0.0 / 0.0 is the machine's own) */
    }
    static const struct {
        const char *name;
        enum builtin b;
    } classes[] = {
        { "__builtin_isnan", B_ISNAN }, { "__builtin_isinf", B_ISINF }, { "__builtin_isfinite", B_ISFINITE },
        { "__builtin_isnormal", B_ISNORMAL }, { "__builtin_signbit", B_SIGNBIT },
        { "__builtin_signbitf", B_SIGNBIT }, { "__builtin_signbitl", B_SIGNBIT },
    };
    for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); i++)
        if (equal(t, classes[i].name)) {
            struct node *x = call_args(rest, t->next, 1);
            return classify(classes[i].b, x, tok);
        }
    if (equal(t, "__builtin_fpclassify")) {
        t = skip(t->next, "(");
        struct node *k[5];
        for (int i = 0; i < 5; i++) {
            k[i] = new_cast(value(assign(&t, t)), ty_int);
            t = skip(t, ",");
        }
        struct node *x = value(assign(&t, t));
        *rest = skip(t, ")");
        struct type *ty = float_arg_type(x);
        struct node *setup;
        struct node *v = once(new_cast(x, ty), &setup, tok);
        /* NaN, infinite, normal, subnormal, zero (the order of the arguments) */
        struct node *r = new_node(ND_COND, tok);
        r->cond = new_compare(ND_EQ, v, new_fnum(0, ty, tok), tok);
        r->then = k[4];
        r->els = k[3];
        r->type = ty_int;
        struct node *normal = new_node(ND_COND, tok);
        normal->cond = classify(B_ISNORMAL, v, tok);
        normal->then = k[2];
        normal->els = r;
        normal->type = ty_int;
        struct node *inf = new_node(ND_COND, tok);
        inf->cond = classify(B_ISINF, v, tok);
        inf->then = k[1];
        inf->els = normal;
        inf->type = ty_int;
        struct node *nan = new_node(ND_COND, tok);
        nan->cond = classify(B_ISNAN, v, tok);
        nan->then = k[0];
        nan->els = inf;
        nan->type = ty_int;
        return new_bin(ND_COMMA, setup, nan, ty_int, tok);
    }
    static const struct {
        const char *name;
        enum node_kind kind;
        bool swap, negate;
    } compares[] = {
        { "__builtin_isgreater", ND_LT, true, false }, { "__builtin_isgreaterequal", ND_LE, true, false },
        { "__builtin_isless", ND_LT, false, false }, { "__builtin_islessequal", ND_LE, false, false },
    };
    for (size_t i = 0; i < sizeof(compares) / sizeof(compares[0]); i++)
        if (equal(t, compares[i].name)) {
            struct node *a = call_args(rest, t->next, 2), *b = a->next;
            a->next = 0;
            return compares[i].swap ? new_compare(compares[i].kind, b, a, tok) : new_compare(compares[i].kind, a, b, tok);
        }
    if (equal(t, "__builtin_islessgreater") || equal(t, "__builtin_isunordered")) {
        bool unordered = equal(t, "__builtin_isunordered");
        struct node *a = call_args(rest, t->next, 2), *b = a->next;
        a->next = 0;
        struct node *sa, *sb;
        struct type *ty = usual_arithmetic(a->type, b->type);
        struct node *va = once(new_cast(a, ty), &sa, tok), *vb = once(new_cast(b, ty), &sb, tok);
        struct node *r = unordered
            ? logical(ND_LOGOR, new_compare(ND_NE, va, va, tok), new_compare(ND_NE, vb, vb, tok), tok)
            : logical(ND_LOGOR, new_compare(ND_LT, va, vb, tok), new_compare(ND_LT, vb, va, tok), tok);
        return new_bin(ND_COMMA, new_bin(ND_COMMA, sa, sb, sa->type, tok), r, ty_int, tok);
    }
    static const struct {
        const char *name;
        struct type **type;
    } math[] = {
        { "sqrt", &ty_double }, { "sqrtf", &ty_float }, { "sqrtl", &ty_ldouble },
        { "fabs", &ty_double }, { "fabsf", &ty_float }, { "fabsl", &ty_ldouble },
        { "floor", &ty_double }, { "floorf", &ty_float }, { "ceil", &ty_double }, { "ceilf", &ty_float },
        { "trunc", &ty_double }, { "truncf", &ty_float }, { "round", &ty_double }, { "roundf", &ty_float },
    };
    if (t->len > 10 && strncmp(t->text, "__builtin_", 10) == 0)
        for (size_t i = 0; i < sizeof(math) / sizeof(math[0]); i++)
            if (t->len - 10 == (int)strlen(math[i].name) && memcmp(t->text + 10, math[i].name, (size_t)t->len - 10) == 0) {
                struct node *x = call_args(rest, t->next, 1);
                struct type *ty = *math[i].type;
                /* (62a) sqrt and fabs of a float or double are instructions, as
                 * clang's: math.c defines sqrt as __builtin_sqrt */
                bool sq = strncmp(math[i].name, "sqrt", 4) == 0, fa = strncmp(math[i].name, "fabs", 4) == 0;
                if ((sq || fa) && ty->kind != TY_LDOUBLE) {
                    struct node *n = builtin_node(sq ? B_SQRT : B_FABS, ty, tok);
                    n->args = new_cast(value(x), ty);
                    return n;
                }
                return call_library(math[i].name, ty, x, tok);
            }
    static const struct {
        const char *name;
        enum builtin b;
        int nargs;
    } atomics[] = {
        { "__atomic_load_n", B_ATOMIC_LOAD, 2 }, { "__atomic_store_n", B_ATOMIC_STORE, 3 },
        { "__atomic_exchange_n", B_ATOMIC_EXCHANGE, 3 }, { "__atomic_compare_exchange_n", B_ATOMIC_CAS, 6 },
        { "__atomic_add_fetch", B_ATOMIC_ADD_FETCH, 3 }, { "__atomic_sub_fetch", B_ATOMIC_SUB_FETCH, 3 },
        { "__atomic_and_fetch", B_ATOMIC_AND_FETCH, 3 }, { "__atomic_or_fetch", B_ATOMIC_OR_FETCH, 3 },
        { "__atomic_xor_fetch", B_ATOMIC_XOR_FETCH, 3 }, { "__atomic_fetch_add", B_ATOMIC_FETCH_ADD, 3 },
        { "__atomic_fetch_sub", B_ATOMIC_FETCH_SUB, 3 }, { "__atomic_fetch_and", B_ATOMIC_FETCH_AND, 3 },
        { "__atomic_fetch_or", B_ATOMIC_FETCH_OR, 3 }, { "__atomic_fetch_xor", B_ATOMIC_FETCH_XOR, 3 },
        { "__atomic_test_and_set", B_ATOMIC_TEST_AND_SET, 2 }, { "__atomic_clear", B_ATOMIC_CLEAR, 2 },
    };
    if (equal(t, "__c11_atomic_fetch_add") || equal(t, "__c11_atomic_fetch_sub"))    /* (65e-52, <stdatomic.h>'s) */
        return atomic_builtin(equal(t, "__c11_atomic_fetch_add") ? B_ATOMIC_FETCH_ADD : B_ATOMIC_FETCH_SUB,
                              rest, t, 3, true);
    for (size_t i = 0; i < sizeof(atomics) / sizeof(atomics[0]); i++)
        if (equal(t, atomics[i].name))
            return atomic_builtin(atomics[i].b, rest, t, atomics[i].nargs, false);
    if (equal(t, "__atomic_thread_fence") || equal(t, "__atomic_signal_fence") || equal(t, "__sync_synchronize")) {
        bool sync = equal(t, "__sync_synchronize");
        if (sync)
            *rest = skip(skip(t->next, "("), ")");
        else
            call_args(rest, t->next, 1);
        return builtin_node(B_ATOMIC_FENCE, ty_void, tok);
    }
    return 0;
}

/* --- expressions --------------------------------------------------------- */

static struct node *expr(struct token **rest, struct token *t)
{
    struct node *n = assign(&t, t);
    while (equal(t, ",")) {
        struct token *op = t;
        struct node *r = assign(&t, t->next);
        r = r->type->kind == TY_ARRAY || r->type->kind == TY_FUNC ? value(r) : r;
        n = new_bin(ND_COMMA, n, r, r->type, op);
    }
    *rest = t;
    return n;
}

static struct node *assign(struct token **rest, struct token *t)
{
    struct node *n = conditional(&t, t);
    static const struct {
        const char *op;
        enum node_kind kind;
    } ops[] = {
        { "+=", ND_ADD }, { "-=", ND_SUB }, { "*=", ND_MUL }, { "/=", ND_DIV }, { "%=", ND_MOD },
        { "&=", ND_BITAND }, { "|=", ND_BITOR }, { "^=", ND_BITXOR }, { "<<=", ND_SHL }, { ">>=", ND_SHR },
    };
    if (equal(t, "=")) {
        struct token *op = t;
        struct node *r = assign(rest, t->next);
        return assignment(n, r, op);
    }
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
        if (equal(t, ops[i].op)) {
            struct token *op = t;
            struct node *r = assign(rest, t->next);
            return compound_assign(ops[i].kind, n, r, op);
        }
    *rest = t;
    return n;
}

static struct node *logor(struct token **rest, struct token *t);

/* The type of a ?: from its two sides (6.5.15p3-6). */
static struct type *cond_type(struct node **a, struct node **b, struct token *tok)
{
    struct type *x = unqual((*a)->type), *y = unqual((*b)->type);
    if (is_arithmetic(x) && is_arithmetic(y)) {
        struct type *ty = usual_arithmetic(x, y);
        *a = new_cast(*a, ty);
        *b = new_cast(*b, ty);
        return ty;
    }
    if (x->kind == TY_VOID || y->kind == TY_VOID)
        return ty_void;
    if (is_struct_union(x) && is_struct_union(y))
        return x;
    if (x->kind == TY_PTR && y->kind == TY_PTR) {
        if (unqual(x->base)->kind == TY_VOID || unqual(y->base)->kind == TY_VOID) {
            int q = x->base->qual | y->base->qual;
            struct type *ty = pointer_to(qualified(ty_void, q));
            if (is_null_pointer_constant(*a))
                return y;
            if (is_null_pointer_constant(*b))
                return x;
            return ty;
        }
        int q = x->base->qual | y->base->qual;
        if (q != x->base->qual)
            return pointer_to(qualified(x->base, q));
        return x;
    }
    if (x->kind == TY_PTR && is_integer(y)) {
        *b = new_cast(*b, x);
        return x;
    }
    if (y->kind == TY_PTR && is_integer(x)) {
        *a = new_cast(*a, y);
        return y;
    }
    error_at(tok, "incompatible operand types ('%s' and '%s')", type_name(x), type_name(y));
}

static struct node *conditional(struct token **rest, struct token *t)
{
    struct node *c = logor(&t, t);
    if (!equal(t, "?")) {
        *rest = t;
        return c;
    }
    struct token *op = t;
    struct node *n = new_node(ND_COND, op);
    n->cond = to_condition(c);
    if (equal(t->next, ":")) {
        /* GNU's a ?: b, a evaluated once */
        struct node *setup;
        struct node *v = once(value(c), &setup, op);
        n->cond = to_condition(v);
        n->then = v;
        n->els = value(conditional(rest, t->next->next));
        n->type = cond_type(&n->then, &n->els, op);
        return new_bin(ND_COMMA, setup, n, n->type, op);
    }
    n->then = value(expr(&t, t->next));
    t = skip(t, ":");
    n->els = value(conditional(rest, t));
    n->type = cond_type(&n->then, &n->els, op);
    return n;
}

static struct node *logand(struct token **rest, struct token *t);
static struct node *bitor(struct token **rest, struct token *t);
static struct node *bitxor(struct token **rest, struct token *t);
static struct node *bitand(struct token **rest, struct token *t);
static struct node *equality(struct token **rest, struct token *t);
static struct node *relational(struct token **rest, struct token *t);
static struct node *shift(struct token **rest, struct token *t);
static struct node *add(struct token **rest, struct token *t);
static struct node *mul(struct token **rest, struct token *t);

static struct node *logor(struct token **rest, struct token *t)
{
    struct node *n = logand(&t, t);
    while (equal(t, "||")) {
        struct token *op = t;
        n = logical(ND_LOGOR, n, logand(&t, t->next), op);
    }
    *rest = t;
    return n;
}

static struct node *logand(struct token **rest, struct token *t)
{
    struct node *n = bitor(&t, t);
    while (equal(t, "&&")) {
        struct token *op = t;
        n = logical(ND_LOGAND, n, bitor(&t, t->next), op);
    }
    *rest = t;
    return n;
}

static struct node *bitor(struct token **rest, struct token *t)
{
    struct node *n = bitxor(&t, t);
    while (equal(t, "|")) {
        struct token *op = t;
        n = arith(ND_BITOR, value(n), value(bitxor(&t, t->next)), op);
    }
    *rest = t;
    return n;
}

static struct node *bitxor(struct token **rest, struct token *t)
{
    struct node *n = bitand(&t, t);
    while (equal(t, "^")) {
        struct token *op = t;
        n = arith(ND_BITXOR, value(n), value(bitand(&t, t->next)), op);
    }
    *rest = t;
    return n;
}

static struct node *bitand(struct token **rest, struct token *t)
{
    struct node *n = equality(&t, t);
    while (equal(t, "&")) {
        struct token *op = t;
        n = arith(ND_BITAND, value(n), value(equality(&t, t->next)), op);
    }
    *rest = t;
    return n;
}

static struct node *equality(struct token **rest, struct token *t)
{
    struct node *n = relational(&t, t);
    for (;;) {
        struct token *op = t;
        if (equal(t, "=="))
            n = new_compare(ND_EQ, n, relational(&t, t->next), op);
        else if (equal(t, "!="))
            n = new_compare(ND_NE, n, relational(&t, t->next), op);
        else {
            *rest = t;
            return n;
        }
    }
}

static struct node *relational(struct token **rest, struct token *t)
{
    struct node *n = shift(&t, t);
    for (;;) {
        struct token *op = t;
        if (equal(t, "<"))
            n = new_compare(ND_LT, n, shift(&t, t->next), op);
        else if (equal(t, "<="))
            n = new_compare(ND_LE, n, shift(&t, t->next), op);
        else if (equal(t, ">"))
            n = new_compare(ND_LT, shift(&t, t->next), n, op);
        else if (equal(t, ">="))
            n = new_compare(ND_LE, shift(&t, t->next), n, op);
        else {
            *rest = t;
            return n;
        }
    }
}

static struct node *shift(struct token **rest, struct token *t)
{
    struct node *n = add(&t, t);
    for (;;) {
        struct token *op = t;
        if (equal(t, "<<"))
            n = new_shift(ND_SHL, n, add(&t, t->next), op);
        else if (equal(t, ">>"))
            n = new_shift(ND_SHR, n, add(&t, t->next), op);
        else {
            *rest = t;
            return n;
        }
    }
}

static struct node *add(struct token **rest, struct token *t)
{
    struct node *n = mul(&t, t);
    for (;;) {
        struct token *op = t;
        if (equal(t, "+"))
            n = new_add(n, mul(&t, t->next), op);
        else if (equal(t, "-"))
            n = new_sub(n, mul(&t, t->next), op);
        else {
            *rest = t;
            return n;
        }
    }
}

static struct node *mul(struct token **rest, struct token *t)
{
    struct node *n = cast(&t, t);
    for (;;) {
        struct token *op = t;
        if (equal(t, "*"))
            n = arith(ND_MUL, value(n), value(cast(&t, t->next)), op);
        else if (equal(t, "/"))
            n = arith(ND_DIV, value(n), value(cast(&t, t->next)), op);
        else if (equal(t, "%"))
            n = arith(ND_MOD, value(n), value(cast(&t, t->next)), op);
        else {
            *rest = t;
            return n;
        }
    }
}

static struct node *compound_literal(struct token **rest, struct token *t, struct type *ty, struct token *start)
{
    if (!current_fn) {
        struct obj *var = new_global(new_unique_name(), ty, start);
        var->is_static = true;
        global_initializer(rest, t, var);
        return new_var_node(var, start);
    }
    struct obj *var = new_local(".compound", ty, start);
    struct node *init = local_initializer(rest, t, var);
    return new_bin(ND_COMMA, init, new_var_node(var, start), var->type, start);
}

static struct node *cast(struct token **rest, struct token *t)
{
    if (equal(t, "(") && is_typename(t->next)) {
        struct token *start = t;
        struct type *ty = typename(&t, t->next);
        t = skip(t, ")");
        if (equal(t, "{"))
            return postfix(rest, start);        /* a compound literal */
        struct node *e = cast(rest, t);
        if (unqual(ty)->kind == TY_VOID) {
            struct node *n = new_node(ND_CAST, start);
            n->lhs = e;
            n->type = ty_void;
            return n;
        }
        e = value(e);
        if (!is_scalar(unqual(ty)))
            error_at(start, "used type '%s' where arithmetic or pointer type is required", type_name(ty));
        if (!is_scalar(e->type))
            error_at(start, "operand of type '%s' where arithmetic or pointer type is required", type_name(e->type));
        if ((unqual(ty)->kind == TY_PTR && is_flonum(e->type)) || (is_flonum(ty) && e->type->kind == TY_PTR))
            error_at(start, "pointer cannot be cast to or from a floating type");
        struct node *n = new_node(ND_CAST, start);
        n->lhs = e;
        n->type = unqual(ty);
        return n;
    }
    return unary(rest, t);
}

static struct node *unary(struct token **rest, struct token *t)
{
    struct token *op = t;
    if (equal(t, "__real__") || equal(t, "__imag__")) {
        /* (62a) a part of a complex value, an lvalue as its member is */
        bool real = t->text[2] == 'r';
        struct node *e = cast(rest, t->next);
        if (!is_complex(e->type)) {
            e = value(e);
            if (!is_arithmetic(e->type))
                error_at(op, "invalid argument type '%s' to '%.*s'", type_name(e->type), op->len, op->text);
            if (real)
                return e;
            return new_bin(ND_COMMA, e, new_cast(new_num(0, ty_int, op), unqual(e->type)), unqual(e->type), op);
        }
        struct member *m = unqual(e->type)->members;
        if (!real)
            m = m->next;
        struct node *ref = new_node(ND_MEMBER, op);
        ref->lhs = e;
        ref->member = m;
        ref->type = e->type->qual ? qualified(m->type, e->type->qual) : m->type;
        return ref;
    }
    if (equal(t, "+")) {
        struct node *e = value(cast(rest, t->next));
        if (!is_arithmetic(e->type))
            error_at(op, "invalid argument type '%s' to unary expression", type_name(e->type));
        return is_integer(e->type) ? new_cast(e, integer_promotion(e->type)) : e;
    }
    if (equal(t, "-")) {
        struct node *e = value(cast(rest, t->next));
        if (!is_arithmetic(e->type))
            error_at(op, "invalid argument type '%s' to unary expression", type_name(e->type));
        struct type *ty = is_integer(e->type) ? integer_promotion(e->type) : e->type;
        return new_unary(ND_NEG, new_cast(e, ty), ty, op);
    }
    if (equal(t, "~")) {
        struct node *e = value(cast(rest, t->next));
        if (!is_integer(e->type))
            error_at(op, "invalid argument type '%s' to unary expression", type_name(e->type));
        struct type *ty = integer_promotion(e->type);
        return new_unary(ND_BITNOT, new_cast(e, ty), ty, op);
    }
    if (equal(t, "!")) {
        struct node *e = to_condition(cast(rest, t->next));
        return new_unary(ND_NOT, e, ty_int, op);
    }
    if (equal(t, "&")) {
        struct node *e = cast(rest, t->next);
        if (e->kind == ND_MEMBER && e->member->is_bitfield)
            error_at(op, "address of bit-field requested");
        if (!is_lvalue(e) && e->type->kind != TY_FUNC && !(e->kind == ND_COMMA && is_lvalue(e->rhs)))
            error_at(op, "cannot take the address of an rvalue of type '%s'", type_name(e->type));
        return new_unary(ND_ADDR, e, pointer_to(e->type), op);
    }
    if (equal(t, "*")) {
        struct node *e = value(cast(rest, t->next));
        if (e->type->kind != TY_PTR)
            error_at(op, "indirection requires pointer operand ('%s' invalid)", type_name(e->type));
        if (e->type->base->kind == TY_FUNC)
            return e;                           /* (*f is f) */
        return new_unary(ND_DEREF, e, e->type->base, op);
    }
    if (equal(t, "++"))
        return compound_assign(ND_ADD, unary(rest, t->next), new_num(1, ty_int, op), op);
    if (equal(t, "--"))
        return compound_assign(ND_SUB, unary(rest, t->next), new_num(1, ty_int, op), op);
    if (equal(t, "sizeof")) {
        if (equal(t->next, "(") && is_typename(t->next->next)) {
            struct type *ty = typename(&t, t->next->next);
            t = skip(t, ")");
            if (equal(t, "{")) {
                /* sizeof (type){ ... } */
                struct node *e = compound_literal(rest, t, ty, op);
                return size_of_type(e->type, op);
            }
            *rest = t;
            if (ty->size < 0)
                error_at(op, "invalid application of 'sizeof' to an incomplete type '%s'", type_name(ty));
            return size_of_type(ty, op);
        }
        struct node *e = unary(rest, t->next);
        if (e->type->kind == TY_VLA)
            return vla_size_of(e->type, op);
        if (e->type->size < 0)
            error_at(op, "invalid application of 'sizeof' to an incomplete type '%s'", type_name(e->type));
        return new_num(e->type->size, ty_ulong, op);
    }
    if (equal(t, "_Alignof") || equal(t, "__alignof__") || equal(t, "__alignof")) {
        if (equal(t->next, "(") && is_typename(t->next->next)) {
            struct type *ty = typename(&t, t->next->next);
            *rest = skip(t, ")");
            return new_num(ty->align, ty_ulong, op);
        }
        struct node *e = unary(rest, t->next);
        int a = e->type->align;
        if (e->kind == ND_VAR && e->var->align > a)
            a = e->var->align;
        return new_num(a, ty_ulong, op);
    }
    if (equal(t, "__extension__"))
        return cast(rest, t->next);
    return postfix(rest, t);
}

static struct node *postfix(struct token **rest, struct token *t)
{
    struct node *n;
    if (equal(t, "(") && is_typename(t->next)) {
        /* a compound literal */
        struct token *start = t;
        struct type *ty = typename(&t, t->next);
        t = skip(t, ")");
        n = compound_literal(&t, t, ty, start);
    } else
        n = primary(&t, t);
    for (;;) {
        struct token *op = t;
        if (equal(t, "(")) {
            n = funcall(&t, t->next, n);
            continue;
        }
        if (equal(t, "[")) {
            struct node *index = expr(&t, t->next);
            t = skip(t, "]");
            struct node *a = value(n), *b = value(index);
            if (a->type->kind != TY_PTR && b->type->kind != TY_PTR)
                error_at(op, "subscripted value is not an array, pointer, or vector");
            struct node *sum = new_add(a, b, op);
            n = new_unary(ND_DEREF, sum, sum->type->base, op);
            continue;
        }
        if (equal(t, ".")) {
            n = struct_ref(n, t->next);
            t = t->next->next;
            continue;
        }
        if (equal(t, "->")) {
            struct node *p = value(n);
            if (p->type->kind != TY_PTR)
                error_at(op, "member reference type '%s' is not a pointer", type_name(p->type));
            n = struct_ref(new_unary(ND_DEREF, p, p->type->base, op), t->next);
            t = t->next->next;
            continue;
        }
        if (equal(t, "++")) {
            n = post_step(n, 1, op);
            t = t->next;
            continue;
        }
        if (equal(t, "--")) {
            n = post_step(n, -1, op);
            t = t->next;
            continue;
        }
        *rest = t;
        return n;
    }
}

/* The default argument promotions (6.5.2.2p6). */
static struct node *promote_arg(struct node *a)
{
    if (unqual(a->type)->kind == TY_FLOAT)
        return new_cast(a, ty_double);
    if (is_integer(a->type))
        return new_cast(a, integer_promotion(a->type));
    return a;
}

static struct node *funcall(struct token **rest, struct token *t, struct node *fn)
{
    struct token *start = t;
    fn = value(fn);
    if (fn->type->kind != TY_PTR || fn->type->base->kind != TY_FUNC)
        error_at(start, "called object type '%s' is not a function or function pointer", type_name(fn->type));
    struct type *ft = fn->type->base;
    struct param *p = ft->params;
    struct node head = { 0 }, *cur = &head;
    int count = 0;
    while (!equal(t, ")")) {
        if (count++)
            t = skip(t, ",");
        struct token *at = t;
        struct node *arg = value(assign(&t, t));
        if (unqual(arg->type)->kind == TY_VOID)
            error_at(at, "argument may not have 'void' type");
        if (p && ft->has_prototype) {
            arg = convert_for_assign(arg, p->type, at);
            p = p->next;
        } else {
            if (ft->has_prototype && !ft->is_variadic)
                error_at(at, "too many arguments to function call");
            arg = promote_arg(arg);
        }
        cur = cur->next = arg;
    }
    if (p && ft->has_prototype)
        error_at(t, "too few arguments to function call");
    *rest = t->next;
    struct node *n = new_node(ND_CALL, start);
    n->block_at = block_seq;                    /* (63c: where an inlined body's locals live) */
    if (fn->kind == ND_ADDR && fn->lhs->kind == ND_VAR && fn->lhs->var->is_function)
        fn->lhs->var->calls++;                  /* (63c: one call only: inlined whatever its size) */
    n->lhs = fn;
    n->func_type = ft;
    n->args = head.next;
    n->type = unqual(ft->return_type);
    if (is_memory_value(n->type)) {
        if (current_fn)
            n->ret_buffer = new_local(".ret", n->type, start);
    }
    return n;
}

/* _Generic(controlling, type: expr, ..., default: expr) */
static struct node *generic_selection(struct token **rest, struct token *t)
{
    struct token *start = t;
    t = skip(t, "(");
    struct type *ctl = value(assign(&t, t))->type;
    ctl = unqual(ctl);
    struct node *chosen = 0, *fallback = 0;
    while (!consume(rest, t, ")")) {
        t = skip(t, ",");
        if (equal(t, "default")) {
            t = skip(t->next, ":");
            fallback = assign(&t, t);
            continue;
        }
        struct type *ty = typename(&t, t);
        t = skip(t, ":");
        struct node *e = assign(&t, t);
        if (types_compatible(ctl, unqual(ty)))
            chosen = e;
    }
    if (!chosen)
        chosen = fallback;
    if (!chosen)
        error_at(start, "controlling expression type '%s' not compatible with any generic association type",
                 type_name(ctl));
    return chosen;
}

static struct node *primary(struct token **rest, struct token *t)
{
    struct token *start = t;
    if (equal(t, "(") && equal(t->next, "{")) {
        /* a statement expression: its value is the last expression statement's */
        if (!current_fn)
            error_at(t, "statement expression not allowed at file scope");
        struct node *n = new_node(ND_STMT_EXPR, t);
        stmt_expr_depth++;
        struct node *block = compound_stmt(&t, t->next->next);
        stmt_expr_depth--;
        n->body = block->body;
        *rest = skip(t, ")");
        struct node *last = n->body;
        while (last && last->next)
            last = last->next;
        if (last && last->kind == ND_EXPR_STMT) {
            last->lhs = last->lhs->type->kind == TY_ARRAY ? value(last->lhs) : last->lhs;
            n->type = last->lhs->type->qual ? unqual(last->lhs->type) : last->lhs->type;
        } else
            n->type = ty_void;
        return n;
    }
    if (equal(t, "(")) {
        struct node *n = expr(&t, t->next);
        *rest = skip(t, ")");
        return n;
    }
    if (equal(t, "_Generic"))
        return generic_selection(rest, t->next);
    if (t->kind == TK_IDENT || t->kind == TK_KEYWORD) {
        struct node *b = builtin(rest, t);
        if (b)
            return b;
    }
    if (equal(t, "__func__") || equal(t, "__FUNCTION__") || equal(t, "__PRETTY_FUNCTION__")) {
        if (!current_fn)
            error_at(t, "'__func__' outside of a function");
        int len = (int)strlen(current_fn->name);
        struct obj *s = new_string_literal(arena_strndup(current_fn->name, (size_t)len), len + 1,
                                           array_of(ty_char, len + 1));
        *rest = t->next;
        return new_var_node(s, t);
    }
    if (t->kind == TK_IDENT) {
        struct name_entry *e = find_name(t);
        *rest = t->next;
        if (e && e->var) {
            e->var->referenced = true;
            return new_var_node(e->var, t);
        }
        if (e && e->enum_type)
            return new_num(e->enum_value, e->enum_type, t);
        if (equal(t->next, "("))
            error_at(t, "call to undeclared function '%.*s'; ISO C99 and later do not support implicit function declarations",
                     t->len, t->text);
        error_at(t, "use of undeclared identifier '%.*s'", t->len, t->text);
    }
    if (t->kind == TK_STR) {
        struct obj *s = new_string_literal(t->str, t->str_len, t->type);
        *rest = t->next;
        return new_var_node(s, t);
    }
    if (t->kind == TK_NUM) {
        *rest = t->next;
        if (is_flonum(t->type) || is_complex(t->type))
            return new_fnum(t->fvalue, t->type, t);  /* (complex: the imaginary part) */
        return new_num(t->value, t->type, t);
    }
    (void)start;
    error_at(t, "expected expression");
}

/* --- constant expressions ------------------------------------------------ */

static bool eval_quiet;                         /* (failure is noted, not an error) */
static bool eval_failed;

static int64_t fail_eval(struct node *n, const char *what)
{
    if (eval_quiet) {
        eval_failed = true;
        return 0;
    }
    error_at(n->tok, "%s", what);
}

static int64_t truncate_to(int64_t v, struct type *ty)
{
    ty = unqual(ty);
    if (ty->kind == TY_BOOL)
        return v != 0;
    /* (each arm made int64_t itself: in `u ? (uint32_t)v : (int32_t)v` the
     * signed arm became unsigned first, so -5 was 4294967291) */
    switch (ty->size) {
    case 1: return ty->is_unsigned ? (int64_t)(uint8_t)v : (int64_t)(int8_t)v;
    case 2: return ty->is_unsigned ? (int64_t)(uint16_t)v : (int64_t)(int16_t)v;
    case 4: return ty->is_unsigned ? (int64_t)(uint32_t)v : (int64_t)(int32_t)v;
    default: return v;
    }
}

static int64_t eval_address(struct node *n, char **label);

/* (62a) A constant of type __int128 at its full width; other operands are
 * extended as their types say. */
static unsigned __int128 eval128(struct node *n)
{
    struct type *ty = unqual(n->type);
    if (ty->kind == TY_ENUM)
        ty = ty->enum_int;
    if (ty->kind != TY_INT128) {
        int64_t v = eval(n);
        if (ty->is_unsigned || ty->kind == TY_PTR)
            return (unsigned __int128)(uint64_t)v;
        return (unsigned __int128)(__int128)v;
    }
    bool u = ty->is_unsigned;
    unsigned __int128 a, b;
    switch (n->kind) {
    case ND_NUM: return (unsigned __int128)(__int128)n->value;
    case ND_ADD: return eval128(n->lhs) + eval128(n->rhs);
    case ND_SUB: return eval128(n->lhs) - eval128(n->rhs);
    case ND_MUL: return eval128(n->lhs) * eval128(n->rhs);
    case ND_BITAND: return eval128(n->lhs) & eval128(n->rhs);
    case ND_BITOR: return eval128(n->lhs) | eval128(n->rhs);
    case ND_BITXOR: return eval128(n->lhs) ^ eval128(n->rhs);
    case ND_NEG: return 0 - eval128(n->lhs);
    case ND_BITNOT: return ~eval128(n->lhs);
    case ND_SHL: return eval128(n->lhs) << (eval(n->rhs) & 127);
    case ND_SHR:
        a = eval128(n->lhs);
        return u ? a >> (eval(n->rhs) & 127) : (unsigned __int128)((__int128)a >> (eval(n->rhs) & 127));
    case ND_DIV: case ND_MOD:
        a = eval128(n->lhs);
        b = eval128(n->rhs);
        if (b == 0)
            return (unsigned __int128)fail_eval(n, "division by zero in a constant expression");
        if (u)
            return n->kind == ND_DIV ? a / b : a % b;
        if ((__int128)b == -1)
            return n->kind == ND_DIV ? 0 - a : 0;
        return n->kind == ND_DIV ? (unsigned __int128)((__int128)a / (__int128)b)
                                 : (unsigned __int128)((__int128)a % (__int128)b);
    case ND_CAST:
        if (is_flonum(n->lhs->type)) {
            double d = eval_double(n->lhs);
            return u ? (unsigned __int128)d : (unsigned __int128)(__int128)d;
        }
        return eval128(n->lhs);
    case ND_COND:
        return eval(n->cond) ? eval128(n->then) : eval128(n->els);
    case ND_COMMA:
        eval128(n->lhs);
        return eval128(n->rhs);
    default:
        return (unsigned __int128)fail_eval(n, "initializer element is not a compile-time constant");
    }
}

/* (62a) A complex constant (or a real one as complex), at double precision
 * like the other floating constants. */
static void eval_complex(struct node *n, double *re, double *im)
{
    if (!is_complex(n->type)) {
        *re = eval_double(n);
        *im = 0;
        return;
    }
    double ar, ai, br, bi;
    switch (n->kind) {
    case ND_NUM:
        *re = 0;
        *im = n->fvalue;
        return;
    case ND_CAST:
        eval_complex(n->lhs, re, im);
        return;
    case ND_ADD: case ND_SUB:
        eval_complex(n->lhs, &ar, &ai);
        eval_complex(n->rhs, &br, &bi);
        *re = canonical(n->kind == ND_ADD ? ar + br : ar - br);
        *im = canonical(n->kind == ND_ADD ? ai + bi : ai - bi);
        return;
    case ND_MUL:
        eval_complex(n->lhs, &ar, &ai);
        eval_complex(n->rhs, &br, &bi);
        if (!is_complex(n->lhs->type) || !is_complex(n->rhs->type)) {
            /* (part by part, as at run time) */
            *re = canonical(ar * br);
            *im = canonical(is_complex(n->lhs->type) ? ai * br : ar * bi);
            return;
        }
        *re = canonical(ar * br - ai * bi);
        *im = canonical(ar * bi + ai * br);
        return;
    case ND_DIV: {
        eval_complex(n->lhs, &ar, &ai);
        eval_complex(n->rhs, &br, &bi);
        if (!is_complex(n->rhs->type)) {
            *re = canonical(ar / br);
            *im = canonical(ai / br);
            return;
        }
        double d = br * br + bi * bi;
        *re = canonical((ar * br + ai * bi) / d);
        *im = canonical((ai * br - ar * bi) / d);
        return;
    }
    case ND_NEG:
        eval_complex(n->lhs, re, im);
        *re = -*re;
        *im = -*im;
        return;
    case ND_BUILTIN:
        if (n->builtin == B_COMPLEX) {
            *re = eval_double(n->args);
            *im = eval_double(n->args->next);
            return;
        }
        break;
    case ND_COMMA:
        eval_complex(n->rhs, re, im);
        return;
    default:
        break;
    }
    fail_eval(n, "initializer element is not a compile-time constant");
    *re = *im = 0;
}

static bool is_int128_node(struct node *n)
{
    return unqual(n->type)->kind == TY_INT128;
}

/* An integer constant, or an address constant (label set) plus an offset. */
static int64_t eval_reloc(struct node *n, char **label)
{
    struct type *ty = unqual(n->type);
    if (is_flonum(ty))
        return (int64_t)eval_double(n);
    if (ty->kind == TY_INT128)
        return (int64_t)(uint64_t)eval128(n);    /* (the low half; casts take what they need) */
    switch (n->kind) {
    case ND_NUM:
        if (is_flonum(n->type))
            return (int64_t)n->fvalue;
        return n->value;
    case ND_ADD: {
        int64_t a = eval_reloc(n->lhs, label);
        return a + eval_reloc(n->rhs, label);
    }
    case ND_SUB: {
        char *l2 = 0;
        int64_t a = eval_reloc(n->lhs, label);
        int64_t b = eval_reloc(n->rhs, &l2);
        if (l2) {
            /* (the difference of two addresses of the same symbol) */
            if (!*label || strcmp(*label, l2) != 0)
                return fail_eval(n, "initializer element is not a compile-time constant");
            *label = 0;
        }
        return a - b;
    }
    case ND_MUL: return eval(n->lhs) * eval(n->rhs);
    case ND_DIV: {
        int64_t b = eval(n->rhs), a = eval(n->lhs);
        if (b == 0)
            return fail_eval(n, "division by zero in a constant expression");
        if (ty->is_unsigned)
            return truncate_to((int64_t)((uint64_t)a / (uint64_t)b), ty);
        if (b == -1)
            return truncate_to(-a, ty);
        return a / b;
    }
    case ND_MOD: {
        int64_t b = eval(n->rhs), a = eval(n->lhs);
        if (b == 0)
            return fail_eval(n, "division by zero in a constant expression");
        if (ty->is_unsigned)
            return truncate_to((int64_t)((uint64_t)a % (uint64_t)b), ty);
        if (b == -1)
            return 0;
        return a % b;
    }
    case ND_BITAND: return eval(n->lhs) & eval(n->rhs);
    case ND_BITOR: return eval(n->lhs) | eval(n->rhs);
    case ND_BITXOR: return eval(n->lhs) ^ eval(n->rhs);
    case ND_SHL: return truncate_to((int64_t)((uint64_t)eval(n->lhs) << (eval(n->rhs) & 63)), ty);
    case ND_SHR: {
        int64_t a = eval(n->lhs), b = eval(n->rhs) & 63;
        if (ty->is_unsigned)
            return truncate_to((int64_t)((uint64_t)truncate_to(a, ty) >> b), ty);
        return a >> b;
    }
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE: {
        if (is_flonum(n->lhs->type)) {
            double a = eval_double(n->lhs), b = eval_double(n->rhs);
            switch (n->kind) {
            case ND_EQ: return a == b;
            case ND_NE: return a != b;
            case ND_LT: return a < b;
            default: return a <= b;
            }
        }
        if (is_int128_node(n->lhs)) {
            unsigned __int128 x = eval128(n->lhs), y = eval128(n->rhs);
            bool xu = unqual(n->lhs->type)->is_unsigned;
            switch (n->kind) {
            case ND_EQ: return x == y;
            case ND_NE: return x != y;
            case ND_LT: return xu ? x < y : (__int128)x < (__int128)y;
            default: return xu ? x <= y : (__int128)x <= (__int128)y;
            }
        }
        int64_t a = eval(n->lhs), b = eval(n->rhs);
        bool u = unqual(n->lhs->type)->is_unsigned;
        switch (n->kind) {
        case ND_EQ: return a == b;
        case ND_NE: return a != b;
        case ND_LT: return u ? (uint64_t)a < (uint64_t)b : a < b;
        default: return u ? (uint64_t)a <= (uint64_t)b : a <= b;
        }
    }
    case ND_NEG: return truncate_to(-eval(n->lhs), ty);
    case ND_NOT: return is_flonum(n->lhs->type) ? !eval_double(n->lhs) : !eval(n->lhs);
    case ND_BITNOT: return truncate_to(~eval(n->lhs), ty);
    case ND_LOGAND: return eval(n->lhs) && eval(n->rhs);
    case ND_LOGOR: return eval(n->lhs) || eval(n->rhs);
    case ND_COND:
        if (is_flonum(n->cond->type) ? eval_double(n->cond) != 0 : eval(n->cond) != 0)
            return eval_reloc(n->then, label);
        return eval_reloc(n->els, label);
    case ND_COMMA:
        eval_reloc(n->lhs, label);
        return eval_reloc(n->rhs, label);
    case ND_CAST: {
        if (unqual(n->type)->kind == TY_VOID)
            return fail_eval(n, "not a constant expression");
        if (is_flonum(n->lhs->type)) {
            double d = eval_double(n->lhs);
            if (ty->kind == TY_BOOL)
                return d != 0;
            if (ty->is_unsigned && ty->size == 8)
                return (int64_t)(uint64_t)d;
            return truncate_to((int64_t)d, ty);
        }
        int64_t v = eval_reloc(n->lhs, label);
        if (*label) {
            if (ty->size != 8)
                return fail_eval(n, "initializer element is not a compile-time constant");
            return v;
        }
        if (is_integer(ty) && !unqual(n->lhs->type)->is_unsigned && n->lhs->type->size < ty->size)
            return truncate_to(v, n->lhs->type);
        return truncate_to(is_integer(n->lhs->type) ? truncate_to(v, n->lhs->type) : v, ty);
    }
    case ND_ADDR:
        return eval_address(n->lhs, label);
    case ND_MEMBER:
    case ND_DEREF:
    case ND_VAR:
        if (n->type->kind == TY_ARRAY || n->type->kind == TY_FUNC)
            return eval_address(n, label);
        return fail_eval(n, "initializer element is not a compile-time constant");
    default:
        return fail_eval(n, "initializer element is not a compile-time constant");
    }
}

/* The address of an lvalue made of a static object, members and indexes. */
static int64_t eval_address(struct node *n, char **label)
{
    switch (n->kind) {
    case ND_VAR:
        if (n->var->is_local)
            return fail_eval(n, "initializer element is not a compile-time constant");
        *label = n->var->name;
        return 0;
    case ND_DEREF:
        return eval_reloc(n->lhs, label);
    case ND_MEMBER:
        return eval_address(n->lhs, label) + n->member->offset;
    case ND_COMMA:
        return eval_address(n->rhs, label);
    default:
        return fail_eval(n, "initializer element is not a compile-time constant");
    }
}

static int64_t eval(struct node *n)
{
    char *label = 0;
    int64_t v = eval_reloc(n, &label);
    if (label)
        return fail_eval(n, "expression is not an integer constant expression");
    return v;
}

static double eval_double(struct node *n)
{
    struct type *ty = unqual(n->type);
    if (ty->kind == TY_INT128) {
        unsigned __int128 v = eval128(n);
        return ty->is_unsigned ? (double)v : (double)(__int128)v;
    }
    if (is_integer(ty)) {
        int64_t v = eval(n);
        return ty->is_unsigned ? (double)(uint64_t)v : (double)v;
    }
    switch (n->kind) {
    case ND_NUM: return n->fvalue;
    case ND_ADD: return canonical(eval_double(n->lhs) + eval_double(n->rhs));
    case ND_SUB: return canonical(eval_double(n->lhs) - eval_double(n->rhs));
    case ND_MUL: return canonical(eval_double(n->lhs) * eval_double(n->rhs));
    case ND_DIV: return canonical(eval_double(n->lhs) / eval_double(n->rhs));
    case ND_NEG: return -eval_double(n->lhs);
    case ND_COND:
        return eval(n->cond) ? eval_double(n->then) : eval_double(n->els);
    case ND_COMMA: return eval_double(n->rhs);
    case ND_CAST: {
        double d = is_flonum(n->lhs->type) ? eval_double(n->lhs)
                   : unqual(n->lhs->type)->is_unsigned ? (double)(uint64_t)eval(n->lhs) : (double)eval(n->lhs);
        if (ty->kind == TY_FLOAT)
            return (float)d;
        return d;
    }
    default:
        if (eval_quiet) {
            eval_failed = true;
            return 0;
        }
        error_at(n->tok, "initializer element is not a compile-time constant");
    }
}

bool is_const_expr(struct node *n)
{
    bool saved_quiet = eval_quiet, saved_failed = eval_failed;
    eval_quiet = true;
    eval_failed = false;
    if (is_flonum(n->type))
        eval_double(n);
    else if (is_integer(n->type))
        eval(n);
    else
        eval_failed = true;
    bool ok = !eval_failed;
    eval_quiet = saved_quiet;
    eval_failed = saved_failed;
    return ok;
}

static int64_t const_expr(struct token **rest, struct token *t)
{
    struct node *n = conditional(rest, t);
    if (!is_integer(n->type))
        error_at(t, "integer constant expression must have integer type, not '%s'", type_name(n->type));
    return eval(n);
}

/* --- VLAs ---------------------------------------------------------------- */

/* Code that computes the sizes of the VLA types in ty (inner ones first),
 * each into a variable of its own, when the declaration runs. */
static struct node *compute_vla_sizes(struct type *ty, struct token *tok)
{
    struct node *n = new_node(ND_NOP, tok);
    n->type = ty_void;
    if (ty->kind == TY_FUNC)
        return n;
    if (ty->base)
        n = compute_vla_sizes(ty->base, tok);
    if (ty->kind != TY_VLA)
        return n;
    struct node *base_size = ty->base->kind == TY_VLA ? new_var_node(ty->base->vla_size, tok)
                                                       : new_num(ty->base->size, ty_ulong, tok);
    ty->vla_size = new_local(".vla.size", ty_ulong, tok);
    struct node *len = new_cast(value(ty->vla_len), ty_ulong);
    struct node *size = new_bin(ND_MUL, len, base_size, ty_ulong, tok);
    struct node *set = new_bin(ND_ASSIGN, new_var_node(ty->vla_size, tok), size, ty_ulong, tok);
    return new_bin(ND_COMMA, n, set, ty_void, tok);
}

static struct node *vla_size_of(struct type *ty, struct token *tok)
{
    if (!ty->vla_size)
        error_at(tok, "the size of this variable length array is not known here");
    return new_var_node(ty->vla_size, tok);
}

static bool has_vla(struct type *ty)
{
    for (; ty; ty = ty->kind == TY_FUNC ? 0 : ty->base)
        if (ty->kind == TY_VLA)
            return true;
    return false;
}

/* --- statements ---------------------------------------------------------- */

static struct node *stmt(struct token **rest, struct token *t);
static struct node *declaration(struct token **rest, struct token *t, struct type *base, struct var_attr *va);
static struct token *global_declaration(struct token *t, struct type *base, struct var_attr *va);

struct token *static_assertion(struct token *t)
{
    struct token *start = t;
    t = skip(t, "(");
    int64_t v = const_expr(&t, t);
    char *msg = 0;
    if (consume(&t, t, ",")) {
        if (t->kind != TK_STR)
            error_at(t, "expected a string literal");
        msg = t->str;
        t = t->next;
    }
    t = skip(t, ")");
    t = skip(t, ";");
    if (!v)
        error_at(start, "static assertion failed%s%s", msg ? ": " : "", msg ? msg : "");
    return t;
}

static struct node *expr_stmt(struct node *e, struct token *tok)
{
    struct node *n = new_node(ND_EXPR_STMT, tok);
    n->lhs = e;
    return n;
}

static struct node *compound_stmt(struct token **rest, struct token *t)
{
    struct node *block = new_node(ND_BLOCK, t);
    struct node *saved_block = current_block;
    current_block = block;
    struct node head = { 0 }, *cur = &head;
    struct obj *outer_locals = locals;
    int block_lo = ++block_seq;
    enter_scope();
    while (!equal(t, "}")) {
        if (t->kind == TK_EOF)
            error_at(t, "expected '}'");
        if (equal(t, "_Static_assert")) {
            t = static_assertion(t->next);
            continue;
        }
        if (is_typename(t) && !equal(t->next, ":")) {
            struct var_attr va = { 0 };
            struct type *base = declspec(&t, t, &va);
            if (va.is_typedef) {
                struct node *n = declaration(&t, t, base, &va);
                if (n)
                    cur = cur->next = n;
                continue;
            }
            cur = cur->next = declaration(&t, t, base, &va);
            continue;
        }
        cur = cur->next = stmt(&t, t);
    }
    leave_scope();
    /* (61d) the locals this block made (inner blocks' have theirs); not
     * inside a statement expression, whose result may still point into its
     * locals after the braces: those take the block around it */
    int block_hi = ++block_seq;
    if (!stmt_expr_depth)
        for (struct obj *o = locals; o && o != outer_locals; o = o->next)
            if (!o->block_hi) {
                o->block_lo = block_lo;
                o->block_hi = block_hi;
            }
    current_block = saved_block;
    block->body = head.next;
    *rest = t->next;
    return block;
}

static struct obj *static_local(struct token *name, struct type *ty, struct var_attr *va)
{
    char *sym = format("%s.%.*s.%d", current_fn->name, name->len, name->text, unique_count++);
    struct obj *o = new_global(sym, ty, name);
    o->is_static = true;
    o->is_tls = va->is_tls;
    if (va->align)
        o->align = va->align;
    if (va->attr.section)
        o->section = va->attr.section;
    struct name_entry *e = declare_name(name);
    e->var = o;
    return o;
}

static struct obj *declare_global(struct token *name, struct type *ty, struct var_attr *va, struct attributes *attr);

static struct node *declaration(struct token **rest, struct token *t, struct type *base, struct var_attr *va)
{
    struct node head = { 0 }, *cur = &head;
    bool first = true;
    struct token *start = t;
    while (!equal(t, ";")) {
        if (!first)
            t = skip(t, ",");
        first = false;
        struct token *name = 0;
        struct attributes attr = va->attr;
        struct type *ty = declarator(&t, t, base, &name, &attr);
        if (!name)
            error_at(t, "declaration does not declare anything");
        if (va->is_typedef) {
            if (has_vla(ty))
                cur = cur->next = expr_stmt(compute_vla_sizes(ty, name), name);
            if (attr.vector_size)
                ty = vector_type(ty, attr.vector_size, name);
            struct name_entry *e = declare_name(name);
            e->type_def = ty;
            continue;
        }
        if (attr.vector_size)
            error_at(name, "vector_size only in a typedef");
        if (ty->kind == TY_FUNC || va->is_extern) {
            /* a block-scope declaration of something defined elsewhere */
            struct obj *o = declare_global(name, ty, va, &attr);
            struct name_entry *e = declare_name(name);
            e->var = o;
            continue;
        }
        if (unqual(ty)->kind == TY_VOID)
            error_at(name, "variable has incomplete type 'void'");
        if (va->is_static) {
            struct obj *o = static_local(name, ty, va);
            if (consume(&t, t, "="))
                global_initializer(&t, t, o);
            else if (ty->size < 0)
                error_at(name, "variable has incomplete type '%s'", type_name(ty));
            continue;
        }
        if (va->is_tls)
            error_at(name, "a thread-local variable must be static or extern");
        if (has_vla(ty)) {
            cur = cur->next = expr_stmt(compute_vla_sizes(ty, name), name);
        }
        struct obj *var = new_local(name_of(name), ty, name);
        if (attr.asm_name)
            var->asm_reg = attr.asm_name;       /* (62c) a register variable's register */
        if (va->align)
            var->align = va->align;
        if (attr.aligned > var->align)
            var->align = attr.aligned;
        struct name_entry *e = declare_name(name);
        e->var = var;
        if (ty->kind == TY_VLA) {
            if (equal(t, "="))
                error_at(t, "variable-sized object may not be initialized");
            struct node *alloc = new_node(ND_VLA_ALLOC, name);
            alloc->var = var;
            alloc->lhs = vla_size_of(ty, name);
            alloc->type = ty_void;
            cur = cur->next = expr_stmt(alloc, name);
            if (current_block)
                current_block->has_vla = true;
            continue;
        }
        if (consume(&t, t, "=")) {
            struct node *init = local_initializer(&t, t, var);
            cur = cur->next = expr_stmt(init, name);
        }
        if (var->type->size < 0)
            error_at(name, "variable has incomplete type '%s'", type_name(var->type));
    }
    *rest = t->next;
    struct node *block = new_node(ND_BLOCK, start);
    block->body = head.next;
    return block;
}

static struct node *for_stmt(struct token **rest, struct token *t)
{
    struct node *n = new_node(ND_FOR, t);
    t = skip(t->next, "(");
    enter_scope();
    struct node *saved_break = break_target, *saved_continue = continue_target;
    break_target = continue_target = n;
    if (is_typename(t)) {
        struct var_attr va = { 0 };
        struct type *base = declspec(&t, t, &va);
        n->init = declaration(&t, t, base, &va);
    } else if (equal(t, ";")) {
        t = t->next;
    } else {
        n->init = expr_stmt(expr(&t, t), t);
        t = skip(t, ";");
    }
    if (!equal(t, ";"))
        n->cond = to_condition(expr(&t, t));
    t = skip(t, ";");
    if (!equal(t, ")"))
        n->inc = expr(&t, t);
    t = skip(t, ")");
    n->then = stmt(rest, t);
    leave_scope();
    break_target = saved_break;
    continue_target = saved_continue;
    return n;
}

/* (62c) the bytes of one or more adjacent string literals */
static char *asm_string(struct token **rest, struct token *t)
{
    if (t->kind != TK_STR)
        error_at(t, "expected a string literal");
    /* (the lexer joined adjacent literals into the first one) */
    char *s = arena_strndup(t->str, t->str_len > 0 ? (size_t)t->str_len - 1 : 0);
    while (t->kind == TK_STR)
        t = t->next;
    *rest = t;
    return s;
}

static int asm_operands(struct token **rest, struct token *t, struct asm_stmt *a, bool outputs)
{
    int n = 0;
    while (t->kind == TK_STR || equal(t, "[")) {
        char *name = 0;
        if (consume(&t, t, "[")) {
            if (t->kind != TK_IDENT)
                error_at(t, "expected an operand name");
            name = name_of(t);
            t = skip(t->next, "]");
        }
        char *c = asm_string(&t, t);
        t = skip(t, "(");
        struct node *e = expr(&t, t);
        t = skip(t, ")");
        a->ops = realloc(a->ops, sizeof(*a->ops) * (size_t)(a->nops + 1));
        struct asm_operand *op = &a->ops[a->nops++];
        memset(op, 0, sizeof(*op));
        op->name = name;
        op->is_output = outputs;
        while (*c == '=' || *c == '+' || *c == '&') {
            if (*c == '+')
                op->is_inout = true;
            c++;
        }
        op->constraint = c;
        if (!outputs && (strchr(c, 'i') || strchr(c, 'n')) && !strchr(c, 'r') && !strchr(c, 'm')) {
            op->has_imm = true;
            op->imm = eval(value(e));
        } else if (!outputs && strchr(c, 'N')) {
            /* (x86's port number: a constant 0-255, else the dx register) */
            struct node *x = e;
            while (x->kind == ND_CAST)
                x = x->lhs;
            if (x->kind == ND_NUM && x->value >= 0 && x->value <= 255) {
                op->has_imm = true;
                op->imm = x->value;
            }
        }
        bool memory = strchr(c, 'm') && !strchr(c, 'r');
        if (outputs || memory) {
            if (!is_lvalue(e))
                error_at(e->tok, "an asm output or memory operand must be an lvalue");
            op->expr = e;
        } else
            op->expr = value(e);
        n++;
        if (!consume(&t, t, ","))
            break;
    }
    *rest = t;
    return n;
}

/* asm [volatile] ("template" [: outputs [: inputs [: clobbers]]]) */
static struct node *asm_stmt(struct token **rest, struct token *t)
{
    struct node *n = new_node(ND_ASM, t);
    struct asm_stmt *a = arena_alloc(sizeof(*a));
    n->asm_info = a;
    t = t->next;
    for (;;) {
        if (equal(t, "volatile") || equal(t, "__volatile__") || equal(t, "__volatile"))
            a->is_volatile = true;
        else if (equal(t, "inline") || equal(t, "__inline__"))
            ;
        else if (equal(t, "goto"))
            error_at(t, "asm goto is not supported");
        else
            break;
        t = t->next;
    }
    t = skip(t, "(");
    a->text = asm_string(&t, t);
    a->is_basic = equal(t, ")");
    if (consume(&t, t, ":")) {
        a->noutputs = asm_operands(&t, t, a, true);
        if (consume(&t, t, ":")) {
            asm_operands(&t, t, a, false);
            if (consume(&t, t, ":")) {
                while (t->kind == TK_STR) {
                    a->clobbers = realloc(a->clobbers, sizeof(char *) * (size_t)(a->nclobbers + 1));
                    a->clobbers[a->nclobbers++] = asm_string(&t, t);
                    if (!consume(&t, t, ","))
                        break;
                }
                if (equal(t, ":"))
                    error_at(t, "asm goto is not supported");
            }
        }
    }
    t = skip(t, ")");
    *rest = skip(t, ";");
    n->type = ty_void;
    return n;
}

static struct node *stmt(struct token **rest, struct token *t)
{
    if (equal(t, "asm") || equal(t, "__asm__") || equal(t, "__asm"))
        return asm_stmt(rest, t);
    if (equal(t, "return")) {
        struct node *n = new_node(ND_RETURN, t);
        if (consume(rest, t->next, ";"))
            return n;
        struct node *e = expr(&t, t->next);
        *rest = skip(t, ";");
        struct type *rt = unqual(current_fn->type->return_type);
        if (rt->kind == TY_VOID) {
            if (unqual(e->type)->kind != TY_VOID)
                error_at(n->tok, "void function '%s' should not return a value", current_fn->name);
            n->lhs = e;
            return n;
        }
        n->lhs = convert_for_assign(value(e), rt, n->tok);
        return n;
    }
    if (equal(t, "if")) {
        struct node *n = new_node(ND_IF, t);
        t = skip(t->next, "(");
        n->cond = to_condition(expr(&t, t));
        t = skip(t, ")");
        n->then = stmt(&t, t);
        if (equal(t, "else"))
            n->els = stmt(&t, t->next);
        *rest = t;
        return n;
    }
    if (equal(t, "switch")) {
        struct node *n = new_node(ND_SWITCH, t);
        t = skip(t->next, "(");
        struct node *c = value(expr(&t, t));
        if (!is_integer(c->type))
            error_at(n->tok, "statement requires expression of integer type ('%s' invalid)", type_name(c->type));
        n->cond = new_cast(c, integer_promotion(c->type));
        t = skip(t, ")");
        struct node *saved_switch = current_switch, *saved_break = break_target;
        current_switch = break_target = n;
        n->then = stmt(rest, t);
        current_switch = saved_switch;
        break_target = saved_break;
        return n;
    }
    if (equal(t, "case")) {
        if (!current_switch)
            error_at(t, "'case' statement not in switch statement");
        struct node *n = new_node(ND_CASE, t);
        struct type *ty = current_switch->cond->type;
        int64_t begin = truncate_to(const_expr(&t, t->next), ty), end = begin;
        if (equal(t, "..."))
            end = truncate_to(const_expr(&t, t->next), ty);
        t = skip(t, ":");
        n->label = new_label();
        n->case_begin = begin;
        n->case_end = end;
        for (struct node *c = current_switch->case_next; c; c = c->case_next)
            if (c->case_begin <= end && begin <= c->case_end)
                error_at(n->tok, "duplicate case value");
        n->case_next = current_switch->case_next;
        current_switch->case_next = n;
        n->lhs = equal(t, "}") ? new_node(ND_BLOCK, t) : stmt(rest, t);
        if (equal(t, "}"))
            *rest = t;
        return n;
    }
    if (equal(t, "default")) {
        if (!current_switch)
            error_at(t, "'default' statement not in switch statement");
        if (current_switch->default_case)
            error_at(t, "multiple default labels in one switch");
        struct node *n = new_node(ND_CASE, t);
        t = skip(t->next, ":");
        n->label = new_label();
        current_switch->default_case = n;
        n->lhs = equal(t, "}") ? new_node(ND_BLOCK, t) : stmt(rest, t);
        if (equal(t, "}"))
            *rest = t;
        return n;
    }
    if (equal(t, "for"))
        return for_stmt(rest, t);
    if (equal(t, "while")) {
        struct node *n = new_node(ND_FOR, t);
        t = skip(t->next, "(");
        n->cond = to_condition(expr(&t, t));
        t = skip(t, ")");
        struct node *saved_break = break_target, *saved_continue = continue_target;
        break_target = continue_target = n;
        n->then = stmt(rest, t);
        break_target = saved_break;
        continue_target = saved_continue;
        return n;
    }
    if (equal(t, "do")) {
        struct node *n = new_node(ND_DO, t);
        struct node *saved_break = break_target, *saved_continue = continue_target;
        break_target = continue_target = n;
        n->then = stmt(&t, t->next);
        break_target = saved_break;
        continue_target = saved_continue;
        t = skip(t, "while");
        t = skip(t, "(");
        n->cond = to_condition(expr(&t, t));
        t = skip(t, ")");
        *rest = skip(t, ";");
        return n;
    }
    if (equal(t, "goto")) {
        if (equal(t->next, "*"))
            error_at(t, "computed goto is not supported");
        struct node *n = new_node(ND_GOTO, t);
        if (t->next->kind != TK_IDENT)
            error_at(t->next, "expected a label");
        n->label = name_of(t->next);
        n->goto_next = gotos;
        gotos = n;
        *rest = skip(t->next->next, ";");
        return n;
    }
    if (equal(t, "break")) {
        if (!break_target)
            error_at(t, "'break' statement not in loop or switch statement");
        struct node *n = new_node(ND_BREAK, t);
        n->target = break_target;
        *rest = skip(t->next, ";");
        return n;
    }
    if (equal(t, "continue")) {
        if (!continue_target)
            error_at(t, "'continue' statement not in loop statement");
        struct node *n = new_node(ND_CONTINUE, t);
        n->target = continue_target;
        *rest = skip(t->next, ";");
        return n;
    }
    if (t->kind == TK_IDENT && equal(t->next, ":")) {
        struct node *n = new_node(ND_LABEL, t);
        n->label = name_of(t);
        n->unique_label = new_label();
        for (struct node *l = labels; l; l = l->goto_next)
            if (strcmp(l->label, n->label) == 0)
                error_at(t, "redefinition of label '%s'", n->label);
        n->goto_next = labels;
        labels = n;
        t = t->next->next;
        struct attributes ignored = { 0 };
        t = attribute_list(t, &ignored);
        if (equal(t, "}")) {
            /* (C23: a label before the end of a block) */
            n->lhs = new_node(ND_BLOCK, t);
            *rest = t;
            return n;
        }
        n->lhs = stmt(rest, t);
        return n;
    }
    if (equal(t, "{"))
        return compound_stmt(rest, t->next);
    if (equal(t, ";")) {
        *rest = t->next;
        return new_node(ND_BLOCK, t);
    }
    if (equal(t, "__asm__") || equal(t, "__asm") || equal(t, "asm"))
        error_at(t, "inline assembly is not supported");
    struct node *n = expr_stmt(expr(&t, t), t);
    *rest = skip(t, ";");
    return n;
}

static void resolve_gotos(void)
{
    for (struct node *g = gotos; g; g = g->goto_next) {
        struct node *l = labels;
        for (; l; l = l->goto_next)
            if (strcmp(g->label, l->label) == 0)
                break;
        if (!l)
            error_at(g->tok, "use of undeclared label '%s'", g->label);
        g->unique_label = l->unique_label;
        g->target = l;
    }
    gotos = labels = 0;
}

/* --- top level ----------------------------------------------------------- */

static struct map symbols;                      /* every file-scope object by name */

static struct obj *declare_global(struct token *name, struct type *ty, struct var_attr *va, struct attributes *attr)
{
    char *sym = attr->asm_name ? attr->asm_name : name_of(name);
    struct obj *o = map_get(&symbols, sym, (int)strlen(sym));
    if (o) {
        if (o->is_function != (ty->kind == TY_FUNC))
            error_at(name, "redefinition of '%s' as different kind of symbol", sym);
        if (!types_compatible(unqual(o->type), unqual(ty)) &&
            !(o->type->kind == TY_ARRAY && ty->kind == TY_ARRAY))
            error_at(name, "conflicting types for '%s'", sym);
        o->type = composite_type(o->type, ty);
    } else {
        o = arena_alloc(sizeof(*o));
        o->name = sym;
        o->type = ty;
        o->tok = name;
        o->align = ty->align;
        o->is_function = ty->kind == TY_FUNC;
        add_global(o);
        map_put(&symbols, sym, (int)strlen(sym), o);
        if (va->is_static && scope == &file_scope)
            o->is_static = true;
    }
    if (va->is_inline)
        o->is_inline = true;
    if (va->is_extern || !va->is_inline)
        o->is_extern_inline = true;             /* (some declaration is not inline-only) */
    if (va->is_tls)
        o->is_tls = true;
    if (attr->weak)
        o->is_weak = true;
    if (attr->hidden)
        o->is_hidden = true;
    if (attr->used || attr->constructor || attr->destructor)
        o->is_used = true;                      /* (a constructor is called though nothing names it) */
    if (attr->constructor)
        o->is_constructor = true;
    if (attr->noinline)
        o->is_noinline = true;
    if (attr->avx)
        o->is_avx = true;
    if (attr->destructor)
        o->is_destructor = true;
    if (attr->noreturn)
        o->is_noreturn = true;
    if (attr->section)
        o->section = attr->section;
    if (va->align > o->align)
        o->align = va->align;
    if (attr->aligned > o->align)
        o->align = attr->aligned;
    return o;
}

static struct token *function_definition(struct token *t, struct obj *fn, struct type *ty)
{
    if (fn->is_definition)
        error_at(fn->tok, "redefinition of '%s'", fn->name);
    fn->is_definition = true;
    fn->type = ty;
    current_fn = fn;
    locals = 0;
    enter_scope();
    /* K&R: the parameters' types before the body */
    if (!ty->has_prototype && ty->params) {
        while (!equal(t, "{")) {
            struct var_attr va = { 0 };
            struct type *base = declspec(&t, t, &va);
            bool first = true;
            while (!consume(&t, t, ";")) {
                if (!first)
                    t = skip(t, ",");
                first = false;
                struct token *name;
                struct attributes attr = { 0 };
                struct type *pt = declarator(&t, t, base, &name, &attr);
                for (struct param *p = ty->params; p; p = p->next)
                    if (p->name && name && p->name->len == name->len &&
                        memcmp(p->name->text, name->text, (size_t)name->len) == 0)
                        p->type = adjust_param(pt);
            }
        }
    }
    struct obj head = { 0 }, *cur = &head;
    struct node *param_vlas = 0;
    for (struct param *p = ty->params; p; p = p->next) {
        struct type *pt = p->type;
        if (unqual(pt)->kind == TY_FLOAT && !ty->has_prototype)
            pt = ty_double;                     /* (K&R: promoted) */
        struct obj *o = arena_alloc(sizeof(*o));
        o->name = p->name ? name_of(p->name) : "";
        o->type = pt;
        o->tok = p->name;
        o->is_local = true;
        o->align = pt->align;
        if (pt->size < 0)
            error_at(p->name ? p->name : t, "variable has incomplete type '%s'", type_name(pt));
        cur = cur->next = o;
        if (p->name) {
            struct name_entry *e = declare_name(p->name);
            e->var = o;
        }
        if (has_vla(pt)) {
            struct node *c = compute_vla_sizes(pt, p->name ? p->name : t);
            param_vlas = param_vlas ? new_bin(ND_COMMA, param_vlas, c, ty_void, t) : c;
        }
    }
    fn->params = head.next;
    struct node *body = compound_stmt(&t, skip(t, "{"));
    if (param_vlas) {
        struct node *s = expr_stmt(param_vlas, t);
        s->next = body->body;
        body->body = s;
    }
    fn->body = body;
    fn->locals = locals;
    leave_scope();
    resolve_gotos();
    current_fn = 0;
    return t;
}

static struct token *global_declaration(struct token *t, struct type *base, struct var_attr *va)
{
    bool first = true;
    while (!consume(&t, t, ";")) {
        if (!first)
            t = skip(t, ",");
        first = false;
        struct token *name = 0;
        struct attributes attr = va->attr;
        struct type *ty = declarator(&t, t, base, &name, &attr);
        if (!name)
            error_at(t, "declaration does not declare anything");
        if (va->is_typedef) {
            if (attr.vector_size)
                ty = vector_type(ty, attr.vector_size, name);
            struct name_entry *e = declare_name(name);
            e->type_def = ty;
            continue;
        }
        if (attr.vector_size)
            error_at(name, "vector_size only in a typedef");
        struct obj *o = declare_global(name, ty, va, &attr);
        struct name_entry *e = declare_name(name);
        e->var = o;
        if (ty->kind == TY_FUNC) {
            if (equal(t, "{") || (!ty->has_prototype && ty->params && is_typename(t)))
                return function_definition(t, o, ty);
            continue;
        }
        if (consume(&t, t, "=")) {
            if (o->is_definition && !o->is_tentative)
                error_at(name, "redefinition of '%s'", o->name);
            o->is_definition = true;
            o->is_tentative = false;
            global_initializer(&t, t, o);
            continue;
        }
        if (!va->is_extern && !o->is_definition) {
            /* a tentative definition: zeros unless a later one initializes it */
            o->is_definition = true;
            o->is_tentative = true;
        }
    }
    return t;
}

static bool is_function_definition(struct token *t)
{
    (void)t;
    return false;
}

static void mark_live(struct obj *o)
{
    if (o->referenced && o->is_used)
        return;
    o->referenced = true;
    o->is_used = true;
    for (struct reloc *r = o->relocs; r; r = r->next) {
        struct obj *to = map_get(&symbols, r->sym, (int)strlen(r->sym));
        if (to && !to->is_used)
            mark_live(to);
    }
    if (o->is_function) {
        for (struct ref *r = map_get(&fn_refs, o->name, (int)strlen(o->name)); r; r = r->next)
            if (!r->to->is_used)
                mark_live(r->to);
    }
}

static void make_va_list_type(void)
{
    if (target.arch == ARCH_X86_64) {
        /* struct { unsigned gp_offset, fp_offset; void *overflow_arg_area, *reg_save_area; }[1] */
        struct type *s = struct_type();
        s->size = 24;
        s->align = 8;
        builtin_va_list = array_of(s, 1);
    } else {
        /* struct { void *__stack, *__gr_top, *__vr_top; int __gr_offs, __vr_offs; } */
        struct type *s = struct_type();
        s->size = 32;
        s->align = 8;
        builtin_va_list = s;
    }
}

struct obj *parse(struct token *t)
{
    make_va_list_type();
    while (t->kind != TK_EOF) {
        if (equal(t, "_Static_assert")) {
            t = static_assertion(t->next);
            continue;
        }
        if (equal(t, ";")) {
            t = t->next;
            continue;
        }
        if (equal(t, "__asm__") || equal(t, "asm") || equal(t, "__asm"))
            error_at(t, "top-level inline assembly is not supported");
        struct var_attr va = { 0 };
        struct type *base = declspec(&t, t, &va);
        (void)is_function_definition;
        t = global_declaration(t, base, &va);
    }
    /* What is written: every variable defined, and the functions that are
     * external definitions or used by something written. */
    for (struct obj *o = globals_head; o; o = o->next) {
        if (!o->is_definition)
            continue;
        if (!o->is_function || (!o->is_static && !(o->is_inline && !o->is_extern_inline)) || o->is_used)
            mark_live(o);
    }
    struct obj head = { 0 }, *cur = &head;
    for (struct obj *o = globals_head; o; o = o->next) {
        if (o->is_function && o->is_inline && !o->is_extern_inline && !o->is_static)
            o->is_static = true;                /* (an inline definition: kept to this file) */
        /* (62d: also a weak declaration that is used: emit.c writes its .weak) */
        if ((o->is_definition && (o->is_used || !o->is_function)) ||
            (!o->is_definition && (o->is_weak || o->is_hidden) && o->referenced)) {
            struct obj *c = o;
            cur = cur->next = c;
        }
    }
    cur->next = 0;
    return head.next;
}

#ifdef CC_TESTING
/* For the tests: checks of every file-scope structure's layout, as C that
 * another compiler can judge (_Static_assert of sizes and offsets). */
static void dump_members(FILE *out, const char *ref, struct type *ty, const char *prefix)
{
    for (struct member *m = ty->members; m; m = m->next) {
        if (!m->name) {
            if (is_struct_union(m->type))
                dump_members(out, ref, m->type, prefix);
            continue;
        }
        if (m->is_bitfield)
            continue;
        fprintf(out, "_Static_assert(__builtin_offsetof(%s, %s%.*s) == %d, \"%s.%.*s\");\n", ref, prefix,
                m->name->len, m->name->text, m->offset, ref, m->name->len, m->name->text);
    }
}

static void dump_type(FILE *out, const char *ref, struct type *ty)
{
    if (ty->size < 0 || !is_struct_union(ty))
        return;
    fprintf(out, "_Static_assert(sizeof(%s) == %d, \"size of %s\");\n", ref, ty->size, ref);
    fprintf(out, "_Static_assert(_Alignof(%s) == %d, \"align of %s\");\n", ref, ty->align, ref);
    dump_members(out, ref, ty, "");
}

void dump_layouts(FILE *out)
{
    for (int i = 0; i < file_scope.tags.cap; i++) {
        struct map_entry *e = &file_scope.tags.entries[i];
        if (!e->key)
            continue;
        struct type *ty = e->value;
        char *ref = format("%s %.*s", ty->kind == TY_UNION ? "union" : "struct", e->len, e->key);
        dump_type(out, ref, ty);
    }
    for (int i = 0; i < file_scope.names.cap; i++) {
        struct map_entry *e = &file_scope.names.entries[i];
        if (!e->key)
            continue;
        struct name_entry *n = e->value;
        if (n->type_def) {
            char *ref = format("%.*s", e->len, e->key);
            dump_type(out, ref, n->type_def);
            if (!is_struct_union(n->type_def) && n->type_def->kind != TY_FUNC && n->type_def->size > 0 &&
                n->type_def->kind != TY_VLA)
                fprintf(out, "_Static_assert(sizeof(%s) == %d, \"size of %s\");\n", ref, n->type_def->size, ref);
        }
    }
}
#endif

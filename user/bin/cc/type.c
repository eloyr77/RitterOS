/*
 * C's types (60b): the basic types of the targets, and the rules that
 * combine them (6.2.5, 6.2.7, 6.3.1).
 */
#include "type.h"

static struct type basic[32];
static int nbasic;

struct type *ty_void, *ty_bool;
struct type *ty_char, *ty_schar, *ty_uchar, *ty_short, *ty_ushort, *ty_int, *ty_uint;
struct type *ty_long, *ty_ulong, *ty_llong, *ty_ullong, *ty_int128, *ty_uint128;
struct type *ty_float, *ty_double, *ty_ldouble;
struct type *ty_cfloat, *ty_cdouble, *ty_cldouble;
struct type *ty_wchar, *ty_char16, *ty_char32;
struct type *ty_size, *ty_ptrdiff;

static struct type *make(enum type_kind kind, int size, int align, bool is_unsigned)
{
    struct type *t = &basic[nbasic++];
    memset(t, 0, sizeof(*t));
    t->kind = kind;
    t->size = size;
    t->align = align;
    t->is_unsigned = is_unsigned;
    return t;
}

static void int128_members(struct type *t);
static struct type *complex_type(struct type *real);

void types_init(void)
{
    nbasic = 0;
    ty_void = make(TY_VOID, 1, 1, false);
    ty_bool = make(TY_BOOL, 1, 1, true);
    ty_char = make(TY_CHAR, 1, 1, !target.char_signed);
    ty_char->plain_char = true;
    ty_schar = make(TY_CHAR, 1, 1, false);
    ty_uchar = make(TY_CHAR, 1, 1, true);
    ty_short = make(TY_SHORT, 2, 2, false);
    ty_ushort = make(TY_SHORT, 2, 2, true);
    ty_int = make(TY_INT, 4, 4, false);
    ty_uint = make(TY_INT, 4, 4, true);
    ty_long = make(TY_LONG, 8, 8, false);
    ty_ulong = make(TY_LONG, 8, 8, true);
    ty_llong = make(TY_LLONG, 8, 8, false);
    ty_ullong = make(TY_LLONG, 8, 8, true);
    ty_int128 = make(TY_INT128, 16, 16, false);
    ty_uint128 = make(TY_INT128, 16, 16, true);
    int128_members(ty_int128);
    int128_members(ty_uint128);
    ty_float = make(TY_FLOAT, 4, 4, false);
    ty_double = make(TY_DOUBLE, 8, 8, false);
    ty_ldouble = make(TY_LDOUBLE, target.long_double_size, target.long_double_align, false);
    ty_cfloat = complex_type(ty_float);
    ty_cdouble = complex_type(ty_double);
    ty_cldouble = complex_type(ty_ldouble);
    ty_wchar = target.arch == ARCH_X86_64 ? ty_int : ty_uint;
    ty_char16 = ty_ushort;
    ty_char32 = ty_uint;
    ty_size = ty_ulong;
    ty_ptrdiff = ty_long;
}

struct type *copy_type(struct type *t)
{
    struct type *c = arena_alloc(sizeof(*c));
    *c = *t;
    c->origin = t->origin ? t->origin : t;
    return c;
}

struct type *pointer_to(struct type *base)
{
    struct type *t = arena_alloc(sizeof(*t));
    t->kind = TY_PTR;
    t->size = t->align = 8;
    t->is_unsigned = true;
    t->base = base;
    return t;
}

struct type *array_of(struct type *base, int len)
{
    struct type *t = arena_alloc(sizeof(*t));
    t->kind = TY_ARRAY;
    t->size = len < 0 || base->size < 0 ? -1 : base->size * len;
    t->align = base->align;
    t->base = base;
    t->array_len = len;
    return t;
}

bool is_integer(struct type *t)
{
    switch (t->kind) {
    case TY_BOOL: case TY_CHAR: case TY_SHORT: case TY_INT: case TY_LONG: case TY_LLONG:
    case TY_INT128: case TY_ENUM:
        return true;
    default:
        return false;
    }
}

bool is_flonum(struct type *t)
{
    return t->kind == TY_FLOAT || t->kind == TY_DOUBLE || t->kind == TY_LDOUBLE;
}

bool is_arithmetic(struct type *t)
{
    return is_integer(t) || is_flonum(t) || t->kind == TY_COMPLEX;
}

bool is_complex(struct type *t)
{
    return unqual(t)->kind == TY_COMPLEX;
}

struct type *complex_of(struct type *real)
{
    switch (unqual(real)->kind) {
    case TY_FLOAT: return ty_cfloat;
    case TY_LDOUBLE: return ty_cldouble;
    default: return ty_cdouble;
    }
}

struct type *real_of(struct type *t)
{
    t = unqual(t);
    return t->kind == TY_COMPLEX ? t->base : t;
}

bool is_scalar(struct type *t)
{
    return is_arithmetic(t) || t->kind == TY_PTR;
}

bool is_pointer_like(struct type *t)
{
    return t->kind == TY_PTR || t->kind == TY_ARRAY || t->kind == TY_VLA;
}

bool is_struct_union(struct type *t)
{
    return t->kind == TY_STRUCT || t->kind == TY_UNION;
}

/* (62a) A value the IR and the backends keep in memory and pass by its
 * address, like a structure; __int128 has the members lo and hi for the
 * calling conventions (two INTEGER eightbytes; AAPCS64's 16-aligned pair). */
bool is_memory_value(struct type *t)
{
    t = unqual(t);
    if (t->is_vector)
        return false;                           /* (32h: a value of its own class, in a register) */
    return t->kind == TY_STRUCT || t->kind == TY_UNION || t->kind == TY_INT128 || t->kind == TY_COMPLEX;
}

/* (62a) a complex type: its real and imaginary parts as members, for the
 * calling conventions (SysV classes them as the real type's; AAPCS64 sees
 * an HFA of two) */
static struct type *complex_type(struct type *real)
{
    struct type *t = make(TY_COMPLEX, 2 * real->size, real->align, false);
    t->base = real;
    struct member *re = calloc(1, sizeof(*re)), *im = calloc(1, sizeof(*im));
    if (!re || !im)
        fatal("out of memory");
    re->type = im->type = real;
    re->align = im->align = real->align;
    im->offset = real->size;
    im->index = 1;
    re->next = im;
    t->members = re;
    return t;
}

static void int128_members(struct type *t)
{
    struct member *lo = calloc(1, sizeof(*lo)), *hi = calloc(1, sizeof(*hi));
    if (!lo || !hi)
        fatal("out of memory");
    lo->type = hi->type = ty_ulong;
    lo->align = hi->align = 8;
    hi->offset = 8;
    hi->index = 1;
    lo->next = hi;
    t->members = lo;
}

struct type *unqual(struct type *t)
{
    return t->qual ? (t->origin ? t->origin : t) : t;
}

/* A copy with more qualifiers; a structure's copies are kept on it, so a
 * copy made while it is incomplete gets its layout too. */
struct type *qualified(struct type *t, int qual)
{
    if ((t->qual | qual) == t->qual)
        return t;
    struct type *base = t->origin ? t->origin : t;
    struct type *c = arena_alloc(sizeof(*c));
    *c = *t;
    c->origin = base;
    c->qual = t->qual | qual;
    if (is_struct_union(base)) {
        c->variants = base->variants;
        base->variants = c;
    }
    return c;
}

void complete_struct(struct type *t)
{
    for (struct type *v = t->variants; v; v = v->variants) {
        v->size = t->size;
        v->align = t->align;
        v->members = t->members;
        v->is_flexible = t->is_flexible;
        v->is_packed = t->is_packed;
    }
}

struct type *vla_of(struct type *base, struct node *len)
{
    struct type *t = arena_alloc(sizeof(*t));
    t->kind = TY_VLA;
    t->size = 8;                                /* (sizeof is computed at run time) */
    t->align = base->align;
    t->base = base;
    t->vla_len = len;
    return t;
}

struct type *func_type(struct type *return_type)
{
    struct type *t = arena_alloc(sizeof(*t));
    t->kind = TY_FUNC;
    t->size = 1;                                /* (GNU: sizeof of a function is 1) */
    t->align = 1;
    t->return_type = return_type;
    return t;
}

struct type *enum_type(void)
{
    struct type *t = arena_alloc(sizeof(*t));
    t->kind = TY_ENUM;
    t->size = 4;
    t->align = 4;
    t->is_unsigned = true;
    t->enum_int = ty_uint;
    return t;
}

struct type *struct_type(void)
{
    struct type *t = arena_alloc(sizeof(*t));
    t->kind = TY_STRUCT;
    t->size = -1;
    t->align = 1;
    return t;
}

static struct type *root(struct type *t)
{
    return t->origin ? t->origin : t;
}

bool types_compatible(struct type *a, struct type *b)
{
    if (a == b)
        return true;
    if (a->qual != b->qual)
        return false;
    a = root(a);
    b = root(b);
    if (a == b)
        return true;
    if (a->kind == TY_ENUM && b->kind != TY_ENUM)
        return types_compatible(a->enum_int, b);
    if (b->kind == TY_ENUM && a->kind != TY_ENUM)
        return types_compatible(a, b->enum_int);
    if (a->kind != b->kind)
        return false;
    switch (a->kind) {
    case TY_VOID:
        return true;
    case TY_CHAR:
        return a->is_unsigned == b->is_unsigned && a->plain_char == b->plain_char;
    case TY_BOOL: case TY_SHORT: case TY_INT: case TY_LONG: case TY_LLONG: case TY_INT128:
        return a->is_unsigned == b->is_unsigned;
    case TY_FLOAT: case TY_DOUBLE: case TY_LDOUBLE:
        return true;
    case TY_COMPLEX:
        return a->base->kind == b->base->kind;
    case TY_PTR:
        return types_compatible(a->base, b->base);
    case TY_ARRAY:
        if (!types_compatible(a->base, b->base))
            return false;
        return a->array_len < 0 || b->array_len < 0 || a->array_len == b->array_len;
    case TY_VLA:
        return types_compatible(a->base, b->base);
    case TY_FUNC: {
        if (!types_compatible(a->return_type, b->return_type))
            return false;
        if (!a->has_prototype || !b->has_prototype)
            return true;
        if (a->is_variadic != b->is_variadic)
            return false;
        struct param *p = a->params, *q = b->params;
        for (; p && q; p = p->next, q = q->next)
            if (!types_compatible(unqual(p->type), unqual(q->type)))
                return false;
        return !p && !q;
    }
    default:
        return false;                           /* (structures: the same type only) */
    }
}

struct type *composite_type(struct type *a, struct type *b)
{
    if (a->kind == TY_ARRAY && b->kind == TY_ARRAY) {
        if (a->array_len < 0)
            return b;
        return a;
    }
    if (a->kind == TY_FUNC && b->kind == TY_FUNC) {
        if (!a->has_prototype)
            return b;
        return a;
    }
    return a;
}

int integer_rank(struct type *t)
{
    switch (t->kind) {
    case TY_BOOL: return 0;
    case TY_CHAR: return 1;
    case TY_SHORT: return 2;
    case TY_INT: return 3;
    case TY_ENUM: return integer_rank(t->enum_int);
    case TY_LONG: return 4;
    case TY_LLONG: return 5;
    case TY_INT128: return 6;
    default: return -1;
    }
}

/* 6.3.1.1p2: below int, an int (every value fits). */
struct type *integer_promotion(struct type *t)
{
    t = unqual(t);
    if (t->kind == TY_ENUM)
        t = t->enum_int;
    if (integer_rank(t) < 3)
        return ty_int;
    return t;
}

static struct type *unsigned_of(struct type *t)
{
    switch (t->kind) {
    case TY_INT: return ty_uint;
    case TY_LONG: return ty_ulong;
    case TY_LLONG: return ty_ullong;
    case TY_INT128: return ty_uint128;
    default: return t;
    }
}

/* 6.3.1.8 */
struct type *usual_arithmetic(struct type *a, struct type *b)
{
    a = unqual(a);
    b = unqual(b);
    /* (62a) with a complex operand: complex of the real types' common one */
    if (a->kind == TY_COMPLEX || b->kind == TY_COMPLEX)
        return complex_of(usual_arithmetic(real_of(a), real_of(b)));
    if (a->kind == TY_LDOUBLE || b->kind == TY_LDOUBLE)
        return ty_ldouble;
    if (a->kind == TY_DOUBLE || b->kind == TY_DOUBLE)
        return ty_double;
    if (a->kind == TY_FLOAT || b->kind == TY_FLOAT)
        return ty_float;
    a = integer_promotion(a);
    b = integer_promotion(b);
    if (a->is_unsigned == b->is_unsigned)
        return integer_rank(a) >= integer_rank(b) ? a : b;
    struct type *u = a->is_unsigned ? a : b, *s = a->is_unsigned ? b : a;
    if (integer_rank(u) >= integer_rank(s))
        return u;
    if (s->size > u->size)
        return s;
    return unsigned_of(s);
}

static void name_into(struct buffer *b, struct type *t)
{
    if (t->qual & Q_CONST)
        buf_printf(b, "const ");
    if (t->qual & Q_VOLATILE)
        buf_printf(b, "volatile ");
    switch (t->kind) {
    case TY_VOID: buf_printf(b, "void"); return;
    case TY_BOOL: buf_printf(b, "_Bool"); return;
    case TY_CHAR: buf_printf(b, t->plain_char ? "char" : t->is_unsigned ? "unsigned char" : "signed char"); return;
    case TY_SHORT: buf_printf(b, t->is_unsigned ? "unsigned short" : "short"); return;
    case TY_INT: buf_printf(b, t->is_unsigned ? "unsigned int" : "int"); return;
    case TY_LONG: buf_printf(b, t->is_unsigned ? "unsigned long" : "long"); return;
    case TY_LLONG: buf_printf(b, t->is_unsigned ? "unsigned long long" : "long long"); return;
    case TY_INT128: buf_printf(b, t->is_unsigned ? "unsigned __int128" : "__int128"); return;
    case TY_FLOAT: buf_printf(b, "float"); return;
    case TY_DOUBLE: buf_printf(b, "double"); return;
    case TY_LDOUBLE: buf_printf(b, "long double"); return;
    case TY_COMPLEX: name_into(b, t->base); buf_printf(b, " _Complex"); return;
    case TY_ENUM: buf_printf(b, "enum %.*s", t->tag ? t->tag->len : 0, t->tag ? t->tag->text : ""); return;
    case TY_STRUCT: case TY_UNION:
        buf_printf(b, "%s %.*s", t->kind == TY_STRUCT ? "struct" : "union",
                   t->tag ? t->tag->len : 11, t->tag ? t->tag->text : "(anonymous)");
        return;
    case TY_PTR: name_into(b, t->base); buf_printf(b, " *"); return;
    case TY_ARRAY: name_into(b, t->base); buf_printf(b, "[%d]", t->array_len); return;
    case TY_VLA: name_into(b, t->base); buf_printf(b, "[*]"); return;
    case TY_FUNC: name_into(b, t->return_type); buf_printf(b, " (...)"); return;
    }
}

char *type_name(struct type *t)
{
    struct buffer b = { 0 };
    name_into(&b, t);
    char *s = arena_strndup(b.data ? b.data : "", b.len);
    free(b.data);
    return s;
}

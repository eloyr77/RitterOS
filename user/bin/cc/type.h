/*
 * C's types (60b), with the sizes and alignments of the targets' ABIs (the
 * System V AMD64 ABI and AAPCS64: LP64 on both, little-endian).
 */
#ifndef TYPE_H
#define TYPE_H

#include "cc.h"

enum type_kind {
    TY_VOID,
    TY_BOOL,
    TY_CHAR,
    TY_SHORT,
    TY_INT,
    TY_LONG,
    TY_LLONG,
    TY_INT128,
    TY_ENUM,
    TY_FLOAT,
    TY_DOUBLE,
    TY_LDOUBLE,
    TY_COMPLEX,                                 /* (62a) base: its real type (members re, im) */
    TY_PTR,
    TY_ARRAY,
    TY_VLA,                                     /* a variable length array */
    TY_FUNC,
    TY_STRUCT,
    TY_UNION,
};

/* qualifiers */
enum { Q_CONST = 1, Q_VOLATILE = 2, Q_RESTRICT = 4, Q_ATOMIC = 8 };

struct node;
struct member;
struct obj;

struct type {
    enum type_kind kind;
    int size;                                   /* sizeof; -1 while incomplete */
    int align;
    bool is_unsigned;
    bool plain_char;                            /* char, neither signed nor unsigned */
    int qual;
    struct type *origin;                        /* the unqualified type of a qualified copy */

    struct type *base;                          /* pointed to, element, or returned */

    /* arrays */
    int array_len;                              /* -1 for [] */
    struct node *vla_len;                       /* a VLA's length expression */
    struct obj *vla_size;                       /* the variable holding its sizeof */

    /* structures and unions */
    struct member *members;
    bool is_flexible, is_packed;
    bool is_vector;                             /* (32d) vector_size(16 or 32): a struct without
                                                 * members, passed in one xmm/ymm register */
    struct token *tag;
    struct type *variants;                      /* qualified copies, completed with it */

    /* enumerations: its compatible integer type */
    struct type *enum_int;

    /* functions */
    struct type *return_type;
    struct param *params;
    bool is_variadic, has_prototype;
    bool is_ms_abi;                             /* (62f) __attribute__((ms_abi)): UEFI's calling convention */
};

struct param {
    struct param *next;
    struct type *type;
    struct token *name;
};

struct member {
    struct member *next;
    struct type *type;
    struct token *name;                         /* 0 for an anonymous struct or union */
    struct token *pos;
    int index;
    int offset;
    int align;
    bool is_bitfield;
    int bit_offset, bit_width;
};

extern struct type *ty_void, *ty_bool;
extern struct type *ty_char, *ty_schar, *ty_uchar, *ty_short, *ty_ushort, *ty_int, *ty_uint;
extern struct type *ty_long, *ty_ulong, *ty_llong, *ty_ullong, *ty_int128, *ty_uint128;
extern struct type *ty_float, *ty_double, *ty_ldouble;
extern struct type *ty_cfloat, *ty_cdouble, *ty_cldouble;
extern struct type *ty_wchar, *ty_char16, *ty_char32;
extern struct type *ty_size, *ty_ptrdiff;

void types_init(void);                          /* after the target is known */
bool is_integer(struct type *t);
bool is_flonum(struct type *t);
bool is_arithmetic(struct type *t);
bool is_scalar(struct type *t);
bool is_pointer_like(struct type *t);           /* a pointer or an array */
bool is_struct_union(struct type *t);
bool is_memory_value(struct type *t);
bool is_complex(struct type *t);
struct type *complex_of(struct type *real);     /* float, double or long double _Complex */
struct type *real_of(struct type *t);           /* a complex type's real type, else t itself */           /* (62a) handled by its address: a struct, union, __int128 */
struct type *unqual(struct type *t);
struct type *qualified(struct type *t, int qual);
struct type *copy_type(struct type *t);
struct type *pointer_to(struct type *base);
struct type *array_of(struct type *base, int len);
struct type *vla_of(struct type *base, struct node *len);
struct type *func_type(struct type *return_type);
struct type *enum_type(void);
struct type *struct_type(void);
void complete_struct(struct type *t);           /* copies the layout to the variants */
bool types_compatible(struct type *a, struct type *b);
struct type *composite_type(struct type *a, struct type *b);
struct type *integer_promotion(struct type *t);
struct type *usual_arithmetic(struct type *a, struct type *b);
int integer_rank(struct type *t);
char *type_name(struct type *t);                /* for messages */

#endif

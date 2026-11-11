/*
 * The parser's tree (60b): every expression node has its type; implicit
 * conversions are explicit casts in the tree, compound assignments and
 * ++/-- are spelled out with temporaries, and string literals and compound
 * literals are objects. The code generator (ir.c) walks it.
 */
#ifndef PARSE_H
#define PARSE_H

#include "type.h"

enum node_kind {
    /* expressions */
    ND_NUM,                                     /* an integer or floating constant */
    ND_VAR,
    ND_ADD, ND_SUB, ND_MUL, ND_DIV, ND_MOD,
    ND_BITAND, ND_BITOR, ND_BITXOR, ND_SHL, ND_SHR,
    ND_EQ, ND_NE, ND_LT, ND_LE,
    ND_NEG, ND_NOT, ND_BITNOT,
    ND_LOGAND, ND_LOGOR,
    ND_ASSIGN,
    ND_COND,
    ND_COMMA,
    ND_MEMBER,                                  /* lhs.member */
    ND_ADDR,
    ND_DEREF,
    ND_CAST,
    ND_CALL,
    ND_STMT_EXPR,                               /* GNU's ({ ... }) */
    ND_MEMZERO,                                 /* zero the variable (before its initializer) */
    ND_VLA_ALLOC,                               /* make room for a VLA, its address in var */
    ND_VA_START, ND_VA_ARG, ND_VA_COPY, ND_VA_END,
    ND_BUILTIN,                                 /* a __builtin_ or __atomic_ function */
    ND_NOP,
    /* statements */
    ND_RETURN,
    ND_IF,
    ND_FOR,                                     /* also while */
    ND_DO,
    ND_SWITCH,
    ND_CASE,
    ND_BLOCK,
    ND_GOTO,
    ND_LABEL,
    ND_BREAK,
    ND_CONTINUE,
    ND_EXPR_STMT,
    ND_ASM,                                     /* (62c) inline assembly: asm_stmt */
};

/* (62c) GCC's extended asm: outputs first, then inputs */
struct asm_operand {
    char *constraint;                           /* without = + & (see is_output, is_inout) */
    char *name;                                 /* [name], or 0 */
    struct node *expr;                          /* an lvalue for outputs and memory operands */
    bool is_output, is_inout;
    bool has_imm;                               /* an "i", "n" (or "N" given a number): imm */
    int64_t imm;
};

struct asm_stmt {
    char *text;                                 /* the template */
    bool is_volatile, is_basic;                 /* (basic: no operands, % is not special) */
    struct asm_operand *ops;
    int nops, noutputs;
    char **clobbers;
    int nclobbers;
};

enum builtin {
    B_NONE,
    B_CLZ, B_CLZL, B_CTZ, B_CTZL, B_POPCOUNT, B_POPCOUNTL,
    B_BSWAP16, B_BSWAP32, B_BSWAP64,
    B_SQRT, B_FABS,
    B_COMPLEX,                                  /* (62a) __builtin_complex(re, im) */                             /* (62a: instructions, not calls: math.c defines sqrt with it) */
    B_EXPECT, B_UNREACHABLE, B_TRAP,
    B_CONSTANT_P, B_FRAME_ADDRESS, B_RETURN_ADDRESS, B_ALLOCA,
    B_ISNAN, B_ISINF, B_ISFINITE, B_ISNORMAL, B_SIGNBIT, B_FPCLASSIFY,
    B_ISGREATER, B_ISGREATEREQUAL, B_ISLESS, B_ISLESSEQUAL, B_ISLESSGREATER, B_ISUNORDERED,
    B_INF, B_NAN, B_HUGE_VAL,
    /* atomics: (ptr, val, order) and friends; one processor, so plain code
     * would do, but the loads and stores must not be torn or reordered */
    B_ATOMIC_LOAD, B_ATOMIC_STORE, B_ATOMIC_EXCHANGE, B_ATOMIC_CAS,
    B_ATOMIC_ADD_FETCH, B_ATOMIC_SUB_FETCH, B_ATOMIC_AND_FETCH, B_ATOMIC_OR_FETCH, B_ATOMIC_XOR_FETCH,
    B_ATOMIC_FETCH_ADD, B_ATOMIC_FETCH_SUB, B_ATOMIC_FETCH_AND, B_ATOMIC_FETCH_OR, B_ATOMIC_FETCH_XOR,
    B_ATOMIC_TEST_AND_SET, B_ATOMIC_CLEAR, B_ATOMIC_FENCE,
    B_MATH,                                     /* sqrt, fabs and the like: a call of the library's */
};

struct obj;

/* An address in a static initializer: sym's address plus addend. */
struct reloc {
    struct reloc *next;
    int offset;
    char *sym;
    int64_t addend;
};

struct node {
    enum node_kind kind;
    struct node *next;
    struct type *type;
    struct token *tok;

    struct node *lhs, *rhs;
    struct node *cond, *then, *els, *init, *inc;
    struct node *body;                          /* a block's statements, a statement expression's */

    struct member *member;
    struct obj *var;
    int64_t value;
    double fvalue;

    /* calls */
    struct type *func_type;
    struct node *args;
    struct obj *ret_buffer;                     /* where a returned structure goes */
    int block_at;                               /* (63c) a call: the block numbering's place there */

    /* goto and labels */
    char *label;
    char *unique_label;
    struct node *goto_next;

    /* switch and case */
    struct node *case_next, *default_case;
    int64_t case_begin, case_end;

    /* break and continue: the statement they belong to */
    struct node *target;
    struct asm_stmt *asm_info;                  /* (62c) ND_ASM */
    bool has_vla;                               /* a block that declares a VLA (ir.c restores the stack) */

    /* the code generator's (ir.c): labels of loops, switches, cases and
     * labels, and the depth of blocks with VLAs around a label */
    int ir_break, ir_continue, ir_label;
    int ir_vla_depth;

    /* builtins */
    enum builtin builtin;
    char *math_name;
};

struct obj {
    struct obj *next;
    char *name;                                 /* the symbol's name */
    struct type *type;
    struct token *tok;
    bool is_local;
    int align;

    /* locals */
    int offset;                                 /* (the backend's) */
    bool is_vla_base;                           /* (saves the stack pointer for VLAs) */

    /* globals and functions */
    bool is_function, is_definition, is_static, is_tentative, is_weak, is_used, is_tls;
    bool is_inline, is_extern_inline, referenced;
    bool is_hidden;                             /* (62d) visibility("hidden"): in this module, never exported */
    bool is_constructor, is_destructor;         /* (62d) its address in .init_array / .fini_array */
    bool is_noinline;                           /* (63c) __attribute__((noinline)): never inlined */
    bool is_avx;                                /* (32h) target("avx2") and the like: AVX code, VEX
                                                 * encodings; inlined only into such functions */
    int calls;                                  /* (63c) direct calls of this function in the file */
    int refs;                                   /* (63c) every use of its name (the calls among them) */
    bool call_emitted, emitted;                 /* (63c) a call was not inlined; its code is written */
    struct obj *inline_copy;                    /* (63c, ir.c) this local's copy in the inlined call being made */
    bool inline_pending;                        /* (63c, ir.c) a local of the callee being inlined */
    int block_lo, block_hi;                     /* (61d) a local's block, as numbers in the order of its
                                                 * braces (0, 0: the whole function): locals of blocks
                                                 * that never overlap share frame space */
    bool is_literal;                            /* a string literal (read-only data) */
    bool address_taken;                         /* (ir.c: &x somewhere) */
    int ir_value;                               /* (ir.c: a promoted local's value, else 0) */
    char *asm_reg;                              /* (62c) register ... __asm__("x0"): asm operands' register */
    char *section;
    char *init_data;                            /* the initial bytes; 0 for zeros */
    struct reloc *relocs;

    /* functions */
    struct obj *params;
    struct obj *locals;
    struct node *body;
    struct obj *va_area;                        /* a variadic function's register save area */
    int stack_size;
    bool is_noreturn;
};

struct obj *parse(struct token *t);             /* the translation unit's objects */

#endif

/*
 * The intermediate code (60c): a function is a list of instructions on
 * numbered virtual registers ("values"), each of one class: a 32- or 64-bit
 * integer, a float, a double or a long double. A value may be set more
 * than once (the two arms of a ?: set the same one); it is not SSA. ir.c
 * makes it from the tree; a backend gives each value a place and writes
 * each instruction as its machine's.
 *
 * (39f) An IR_BIN whose dst is V_V16 works lane by lane, size the lanes'
 * bytes (4): its sub says int or float, as for scalars.
 *
 * Integers narrower than 32 bits live in 32-bit values: loads extend them,
 * and a conversion to a wider type extends from the narrow width again
 * (CONV_SEXT/CONV_ZEXT with from_size), so only the low bytes of a value
 * matter until then. Structures are values only as their address; (32h)
 * vectors (vector_size, x86-64) are values of 16 or 32 bytes.
 */
#ifndef IR_H
#define IR_H

#include "parse.h"

enum ir_class { V_NONE, V_I32, V_I64, V_F32, V_F64, V_LD, V_V16, V_V32 };   /* (32h: vectors of 16, 32 bytes) */

enum ir_op {
    IR_LABEL,                                   /* label */
    IR_JMP,                                     /* goto label */
    IR_BR,                                      /* if (a) goto label else goto label2 */
    IR_IMM,                                     /* dst = imm */
    IR_FIMM,                                    /* dst = fimm (in dst's class) */
    IR_LOCAL,                                   /* dst = the address of obj */
    IR_GLOBAL,                                  /* dst = the address of sym + imm */
    IR_LOAD,                                    /* dst = *a, size bytes (is_signed: sign-extended) */
    IR_STORE,                                   /* *a = b, size bytes */
    IR_COPY,                                    /* dst = a */
    IR_BIN,                                     /* dst = a sub b */
    IR_UN,                                      /* dst = sub a */
    IR_CMP,                                     /* dst = a sub b, 0 or 1 */
    IR_CONV,                                    /* dst = conversion sub of a */
    IR_CALL,                                    /* dst = (sym, or a)(args): see struct ir_call */
    IR_RET,                                     /* return a (none: a = 0); a structure's address */
    IR_MEMCPY,                                  /* memcpy(a, b, imm) */
    IR_MEMZERO,                                 /* memset(a, 0, imm) */
    IR_VA_START,                                /* va_start(*a) */
    IR_VA_ARG,                                  /* dst = the address of the next argument of type */
    IR_ALLOCA,                                  /* dst = imm-aligned room for a bytes on the stack */
    IR_GET_SP,                                  /* dst = the stack pointer (blocks with VLAs) */
    IR_SET_SP,                                  /* the stack pointer = a */
    IR_BUILTIN,                                 /* dst = builtin sub of a (clz, bswap, ...) */
    IR_ATOMIC,                                  /* dst = atomic sub on *a with b (and c) */
    IR_TRAP,
    IR_NOP,                                     /* (61b: a copy made unneeded) */
    IR_TLS_ADDR,                                /* (62b) dst = this thread's address of sym (a call) */
    IR_ASM,                                     /* (62c) asm_info; its operands as call's args */
    IR_VSPLAT,                                  /* (39f) dst = a in every lane (V_V16, size: a lane's bytes) */
    IR_VHADD,                                   /* (39f) dst = the sum of a's 4 int lanes */
    IR_VMADD,                                   /* (39f-2) dst = a * b on 8 int16 lanes, pairs added into 4 ints */
    IR_VREVW,                                   /* (39f-2) dst = a's 8 int16 lanes in reverse order */
};

enum ir_sub {
    /* IR_BIN */
    OP_ADD, OP_SUB, OP_MUL, OP_SDIV, OP_UDIV, OP_SMOD, OP_UMOD, OP_AND, OP_OR, OP_XOR,
    OP_SHL, OP_SHR, OP_SAR, OP_FADD, OP_FSUB, OP_FMUL, OP_FDIV,
    OP_UMULH,                                   /* (62a) the high 64 bits of a * b, unsigned (I64) */
    /* IR_UN */
    OP_NEG, OP_NOT, OP_FNEG,
    /* IR_CMP: integer ones signed (S) or unsigned (U); float ones ordered,
     * except FNE (true when unordered, as C's != is) */
    OP_EQ, OP_NE, OP_SLT, OP_SLE, OP_ULT, OP_ULE, OP_FEQ, OP_FNE, OP_FLT, OP_FLE,
    /* IR_CONV */
    CONV_SEXT, CONV_ZEXT,                       /* from from_size bytes to dst's class */
    CONV_TRUNC,                                 /* to dst's (smaller) integer class */
    CONV_S2F, CONV_U2F,                         /* integer (from_size 4 or 8) to floating */
    CONV_F2S, CONV_F2U,                         /* floating to integer (dst's class) */
    CONV_F2F,                                   /* between floating classes */
    /* IR_BUILTIN */
    BI_CLZ, BI_CTZ, BI_POPCOUNT, BI_BSWAP16, BI_BSWAP32, BI_BSWAP64,
    BI_SQRT, BI_FABS,                           /* (62a) of a float or double */
    /* IR_ATOMIC */
    AT_LOAD, AT_STORE, AT_XCHG, AT_CAS, AT_ADD, AT_SUB, AT_AND, AT_OR, AT_XOR, AT_FENCE,
};

struct ir_arg {
    int value;                                  /* (a structure's address) */
    struct type *type;
};

struct ir_call {
    struct type *func_type;
    struct ir_arg *args;
    int nargs;
    int nfixed;                                 /* arguments before the "..." */
    int ret_buffer;                             /* the value holding a returned structure's place */
};

struct ir {
    enum ir_op op;
    int sub;
    int dst, a, b, c;
    int size;                                   /* memory width; IR_CONV: the source's width */
    bool is_signed;
    bool is_volatile;                           /* (40c) IR_LOAD, IR_STORE: a volatile object's (never merged) */
    bool fetch_new;                             /* IR_ATOMIC: the new value (op_fetch), not the old */
    int64_t imm;
    double fimm;
    struct obj *obj;
    char *sym;
    int label, label2;
    struct type *type;                          /* IR_VA_ARG, IR_RET: the C type */
    struct ir_call *call;
    struct asm_stmt *asm_info;                  /* (62c) IR_ASM */
    struct token *tok;
};

struct ir_value {
    enum ir_class cls;
};

struct ir_func {
    struct obj *fn;
    struct ir *code;
    int ncode, capcode;
    struct ir_value *values;                    /* index 0 is no value */
    int nvalues, capvalues;
    int nlabels;
};

struct ir_func *ir_function(struct obj *fn);
int value_size(enum ir_class cls);              /* the bytes a value needs: 4, 8, 16 or 32 */
/* (32h) inline assembly given registers like an instruction (x86-64): one
 * output at most, "x"/"r" operands and immediates, no memory */
bool asm_in_registers(struct asm_stmt *a);
void ir_dump(FILE *out, struct ir_func *f);     /* (for the tests) */

/* The backends: write the program as assembly text. */
void emit_x86_64(FILE *out, struct obj *program);
void emit_aarch64(FILE *out, struct obj *program);
/* shared by both: data, strings and names */
void emit_data(FILE *out, struct obj *program, bool aarch64);
bool symbol_is_local(struct obj *o);            /* defined in this file (no GOT needed) */
int place_locals(struct obj *fn, int top, bool down);  /* (61d) the frame's room for locals */
/* (63c) a static function used only by its one call, which is inlined: its
 * own code is written only if that call was not (call_emitted) */
bool inline_only(struct obj *fn);

#endif

/*
 * as (60e): RitterOS's assembler. It reads GNU assembler syntax (AT&T for
 * x86-64), the subset cc writes and the C library's .S files use, and
 * writes an ELF relocatable object. A section's contents are fragments:
 * fixed bytes, a jump whose size is not known yet (x86-64's short and long
 * forms, decided by relaxation as LLVM's assembler does), or alignment.
 */
#ifndef AS_H
#define AS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct as_section;

struct as_symbol {
    struct as_symbol *next;                     /* (hash chain) */
    char *name;
    struct as_section *section;                 /* 0: undefined (or absolute, with is_absolute) */
    int64_t value;                              /* (the offset; set once the layout is final) */
    struct as_fragment *frag;                   /* where it is defined, and the offset in it */
    int64_t frag_offset;
    bool defined, is_global, is_weak, is_hidden, is_absolute, used_in_reloc;
    int type;                                   /* STT_ */
    char *size_expr;                            /* .size's expression, evaluated at the end */
    int64_t size;
    int index;                                  /* in .symtab */
};

/* An expression: symbol + addend, or a constant, or a difference of two
 * symbols (folded when both are in one section). */
struct as_expr {
    struct as_symbol *sym, *minus;
    int64_t addend;
    int modifier;                               /* (see as_x86.c, as_arm.c: @PLT, :lo12:, ...) */
};

/* A place in a fragment the end result has to fill in. */
struct as_fixup {
    struct as_fixup *next;
    int64_t offset;                             /* in the fragment */
    struct as_expr expr;
    int kind;                                   /* the architecture's; 0 = a plain address (data) */
    int size;                                   /* bytes */
    bool pcrel;                                 /* resolved here when the target is a label of this section */
    int64_t pc_base;                            /* (PC-relative: the fragment offset the PC counts from) */
};

enum frag_kind { FRAG_DATA, FRAG_JUMP, FRAG_ALIGN };

struct as_fragment {
    struct as_fragment *next;
    enum frag_kind kind;
    unsigned char *data;
    int64_t len, cap;
    struct as_fixup *fixups;
    int64_t address;                            /* in the section, during the layout */
    /* FRAG_JUMP: the short and long forms */
    struct as_expr target;
    int jump_op;                                /* -1 jmp, -2 call (never short), else a condition code */
    bool is_long;
    /* FRAG_ALIGN */
    int align;
    int64_t max_skip;
    bool code;                                  /* (pad with NOPs) */
};

struct as_reloc {
    struct as_reloc *next;
    int64_t offset;
    struct as_symbol *sym;
    int type;
    int64_t addend;
};

struct as_section {
    struct as_section *next;
    char *name;
    int type;                                   /* SHT_ */
    uint64_t flags;                             /* SHF_ */
    int align;
    struct as_fragment *first, *last;
    int64_t size;
    struct as_reloc *relocs;
    struct as_symbol *symbol;                   /* the section's own symbol */
    int index;                                  /* in the section headers */
    int rela_index;
    int entsize;
};

/* The architecture's part */
struct as_arch {
    const char *name;
    int machine;                                /* EM_ */
    void (*instruction)(char *mnemonic, char *operands);
    int (*jump_size)(struct as_fragment *f, bool is_long);
    void (*write_jump)(struct as_fragment *f, unsigned char *out);
    void (*apply)(struct as_fixup *fx, struct as_fragment *f, unsigned char *out, int64_t value, bool resolved);
    /* the relocation for a fixup that is not resolved here (and a change
     * of its addend); false if none fits */
    bool (*relocate)(struct as_fixup *fx, int *type, int64_t *addend);
    void (*nops)(unsigned char *out, int64_t n);
    char comment;                               /* '#' or 0 (only "//") */
};

extern struct as_arch *arch;
extern struct as_section *current_section;
extern int as_line;
extern const char *as_file;

_Noreturn void as_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void *as_alloc(size_t n);
char *as_strdup(const char *s, size_t n);

/* bytes into the current section */
void emit_byte(int b);
void emit_bytes(const void *p, int n);
void emit_le(uint64_t v, int n);
void emit_fixup(struct as_expr *e, int kind, int size, bool pcrel, int64_t pc_base_from_here);
void emit_jump(struct as_expr *target, int op);
int64_t here(void);                             /* offset in the current fragment */

struct as_symbol *symbol(const char *name, size_t len);
/* an expression from text; *end after it */
bool parse_expr(const char **p, struct as_expr *e);
bool expr_constant(struct as_expr *e, int64_t *v);
const char *skip_space(const char *p);

/* the architectures */
extern struct as_arch arch_x86_64, arch_aarch64;

#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHT_NOTE 7
#define SHT_INIT_ARRAY 14
#define SHT_FINI_ARRAY 15
#define SHF_WRITE 1
#define SHF_ALLOC 2
#define SHF_EXECINSTR 4
#define SHF_MERGE 0x10
#define SHF_STRINGS 0x20
#define SHF_INFO_LINK 0x40
#define SHF_TLS 0x400
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2
#define STT_SECTION 3
#define STT_FILE 4
#define STT_TLS 6

#endif

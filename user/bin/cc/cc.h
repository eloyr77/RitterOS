/*
 * cc (60): RitterOS's own C compiler. One frontend (the preprocessor, the
 * parser and its types) and one backend per architecture (x86-64 and
 * AArch64), joined by a small intermediate code (struct ir). cc writes
 * assembly text; `as` makes objects of it and `ld` links them, each a
 * program of its own, started by cc like on Unix.
 *
 * Memory is never given back: a compilation is one short run, and every
 * structure lives until its end (arena_alloc).
 */
#ifndef CC_H
#define CC_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- the run ------------------------------------------------------------- */

enum target_arch { ARCH_X86_64, ARCH_AARCH64 };

struct target {
    enum target_arch arch;
    const char *name;                           /* "x86_64" or "aarch64" */
    bool char_signed;                           /* x86-64: signed; AArch64 (ELF): unsigned */
    int long_double_size, long_double_align;    /* 16, 16 on both (80-bit x87 or binary128) */
};
extern struct target target;
extern int opt_level;                           /* -O0: 0 (every value on the stack); else 1 */
extern bool opt_no_pic;                         /* (62f) -fno-pic: every symbol reached directly (the kernel) */
extern bool opt_general_regs;                   /* (62f) -mgeneral-regs-only: no FP/SIMD register anywhere */

void *arena_alloc(size_t n);                    /* zeroed, never freed */
char *arena_strndup(const char *s, size_t n);
char *format(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* A growing byte buffer (assembly text, string contents). */
struct buffer {
    char *data;
    size_t len, cap;
};
void buf_add(struct buffer *b, const void *data, size_t n);
void buf_byte(struct buffer *b, int c);
void buf_printf(struct buffer *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* --- tokens -------------------------------------------------------------- */

struct source {
    char *name;                                 /* as written in #include or on the command line */
    char *display_name;                         /* changed by #line */
    char *text;                                 /* NUL-terminated, lines spliced (see lex.c) */
    int *line_of;                               /* (spliced text offset -> line, for a few lines) */
    int number;
};

enum token_kind {
    TK_IDENT,
    TK_PUNCT,
    TK_KEYWORD,                                 /* (an identifier the parser knows) */
    TK_PPNUM,                                   /* a preprocessing number, converted by the parser */
    TK_NUM,
    TK_STR,
    TK_CHAR,                                    /* a character constant, converted to TK_NUM */
    TK_OTHER,                                   /* a stray character (allowed in skipped groups, -E) */
    TK_EOF,
};

struct hideset;
struct type;

struct token {
    enum token_kind kind;
    struct token *next;
    const char *text;                           /* the token's spelling, not NUL-terminated */
    int len;
    struct source *src;
    int line, col;
    int line_delta;                             /* (#line's adjustment) */
    bool at_line_start, space_before;
    struct hideset *hideset;                    /* macros not to expand again (Prosser) */
    struct token *origin;                       /* the macro use this came from, for messages */
    /* Literals, after conversion */
    int64_t value;
    double fvalue;                              /* (long double too: at double precision) */
    struct type *type;
    char *str;                                  /* a string literal's bytes (str_len of them) */
    int str_len;
};

struct source *source_read(const char *path);  /* 0 if it cannot be read */
struct source *source_from_text(const char *name, char *text);
struct token *tokenize(struct source *src);
struct token *tokenize_text(const char *name, const char *text);
struct token *token_copy(struct token *t);
bool token_is(struct token *t, const char *s);
char *token_text(struct token *t);              /* a NUL-terminated copy */
struct token *convert_pp_tokens(struct token *t);        /* keywords, numbers, strings, characters */

_Noreturn void error_at(struct token *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void warn_at(struct token *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
_Noreturn void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int error_count;
extern bool lex_assembly;                       /* (62e) preprocessing a .S file */

/* --- the preprocessor ---------------------------------------------------- */

void cpp_add_include(const char *dir, bool system);
void cpp_define(const char *name_eq_value);     /* -D NAME or NAME=VALUE */
void cpp_undefine(const char *name);
void cpp_predefine(void);                       /* the target's macros */
struct token *preprocess(struct token *t);
void print_preprocessed(FILE *out, struct token *t, bool assembly);
extern struct buffer cpp_dependencies;          /* the files included (63g: -MD, -MMD) */
extern bool cpp_dependencies_all;               /* (the system's headers too) */

#endif

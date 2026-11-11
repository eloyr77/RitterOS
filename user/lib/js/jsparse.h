/*
 * The compiler's inside (27o): tokens (lex.c), the syntax tree (parse.c)
 * and what compile.c adds to it (scopes and bindings). A compilation runs
 * with the collector held off; its nodes live in an arena freed at the end,
 * and an error anywhere longjmps back to the start with its message.
 */
#ifndef RITTEROS_JSPARSE_H
#define RITTEROS_JSPARSE_H

#include <setjmp.h>
#include "jsint.h"

/* --- tokens --------------------------------------------------------------- */

enum tok_type {
    T_EOF,
    T_IDENT,                                    /* tok.atom; tok.escaped when it had \u escapes */
    T_PRIVATE,                                  /* #name: tok.atom */
    T_KEYWORD,                                  /* tok.op = KW_* */
    T_PUNCT,                                    /* tok.op = P_* */
    T_NUM,                                      /* tok.num */
    T_STR,                                      /* tok.atom (the value) */
    T_TEMPLATE,                                 /* a template's part up to ${ or `: tok.atom cooked (0 if invalid), tok.raw; tok.tail */
    T_REGEXP,                                   /* tok.atom body, tok.raw flags */
};

enum keyword {
    KW_BREAK = 1, KW_CASE, KW_CATCH, KW_CLASS, KW_CONST, KW_CONTINUE, KW_DEBUGGER,
    KW_DEFAULT, KW_DELETE, KW_DO, KW_ELSE, KW_EXPORT, KW_EXTENDS, KW_FALSE, KW_FINALLY,
    KW_FOR, KW_FUNCTION, KW_IF, KW_IMPORT, KW_IN, KW_INSTANCEOF, KW_NEW, KW_NULL,
    KW_RETURN, KW_SUPER, KW_SWITCH, KW_THIS, KW_THROW, KW_TRUE, KW_TRY, KW_TYPEOF,
    KW_VAR, KW_VOID, KW_WHILE, KW_WITH, KW_ENUM,
};

enum punct {
    P_LBRACE = 1, P_RBRACE, P_LPAREN, P_RPAREN, P_LBRACKET, P_RBRACKET, P_DOT, P_ELLIPSIS,
    P_SEMI, P_COMMA, P_LT, P_GT, P_LE, P_GE, P_EQ, P_NE, P_SEQ, P_SNE, P_PLUS, P_MINUS,
    P_STAR, P_SLASH, P_PERCENT, P_STARSTAR, P_INC, P_DEC, P_SHL, P_SAR, P_SHR, P_AMP,
    P_BAR, P_CARET, P_BANG, P_TILDE, P_AND, P_OR, P_NULLISH, P_QUESTION, P_OPTIONAL,
    P_COLON, P_ASSIGN, P_ADD_ASSIGN, P_SUB_ASSIGN, P_MUL_ASSIGN, P_DIV_ASSIGN,
    P_MOD_ASSIGN, P_POW_ASSIGN, P_SHL_ASSIGN, P_SAR_ASSIGN, P_SHR_ASSIGN, P_AND_ASSIGN,
    P_OR_ASSIGN, P_XOR_ASSIGN, P_LAND_ASSIGN, P_LOR_ASSIGN, P_NULLISH_ASSIGN, P_ARROW,
    P_HASH,
};

struct token {
    enum tok_type type;
    int op;
    double num;
    struct jsstring *atom, *raw;
    bool tail;                                  /* (templates) the last part */
    bool escaped;
    bool nl_before;                             /* a line terminator before it */
    uint32_t pos, end;                          /* offsets in the source */
    int line, col;
};

struct lexer {
    struct js *J;
    const uint16_t *s;
    uint32_t n, pos;
    int line;
    uint32_t line_start;
    struct token tok;                           /* the current token */
    bool strict;
    jmp_buf *fail;
    char *error;                                /* the message (malloc'd) after a longjmp */
    int error_line, error_col;
};

void lex_init(struct lexer *L, struct js *J, const uint16_t *s, uint32_t n, jmp_buf *fail);
void lex_next(struct lexer *L);                 /* the next token ('/' as an operator) */
void lex_regexp(struct lexer *L);               /* the current '/' or '/=' token read again as a regular expression */
void lex_template_continue(struct lexer *L);    /* after a substitution's '}': the next template part */
void lex_save(struct lexer *L, struct lexer *saved);
void lex_restore(struct lexer *L, const struct lexer *saved);
_Noreturn void lex_error(struct lexer *L, const struct token *at, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
const char *punct_text(int op);
const char *keyword_text(int kw);

/* --- the tree ---------------------------------------------------------- */

enum node_type {
    /* expressions */
    N_NUM, N_STR, N_TEMPLATE, N_TAGGED, N_REGEXP, N_IDENT, N_THIS, N_SUPER_MEMBER, N_SUPER_INDEX,
    N_SUPER_CALL, N_NEW_TARGET, N_NULL, N_TRUE, N_FALSE, N_ARRAY, N_OBJECT, N_PROPERTY,
    N_FUNCTION, N_CLASS, N_MEMBER, N_INDEX, N_CALL, N_NEW, N_CHAIN, N_UNARY, N_UPDATE,
    N_BINARY, N_LOGICAL, N_ASSIGN, N_COND, N_SEQ, N_SPREAD, N_HOLE, N_YIELD, N_AWAIT,
    N_PRIVATE_MEMBER, N_PRIVATE_IN,
    /* patterns */
    N_ARRAY_PATTERN, N_OBJECT_PATTERN, N_ASSIGN_PATTERN, N_REST,
    /* statements */
    N_VAR, N_DECLARATOR, N_EXPR_STMT, N_BLOCK, N_IF, N_FOR, N_FOR_IN, N_FOR_OF, N_WHILE,
    N_DO_WHILE, N_RETURN, N_BREAK, N_CONTINUE, N_THROW, N_TRY, N_SWITCH, N_CASE,
    N_LABELED, N_EMPTY, N_DEBUGGER, N_FUNC_DECL, N_CLASS_DECL, N_SCRIPT,
    N_CLASS_MEMBER, N_STATIC_BLOCK,
};

/* node flags */
enum {
    NF_OPTIONAL = 1,                            /* ?. on this member or call */
    NF_PREFIX = 2,                              /* ++x */
    NF_COMPUTED = 4,                            /* [key] in literals and classes */
    NF_SHORTHAND = 8,                           /* { a } */
    NF_COVER_INIT = 16,                         /* { a = 1 } before it becomes a pattern */
    NF_STATIC = 32,
    NF_PARENS = 64,                             /* written in parentheses */
    NF_ASYNC_ARROW_COVER = 128,
};

/* property kinds (N_PROPERTY, N_CLASS_MEMBER .op) */
enum { PK_INIT, PK_GET, PK_SET, PK_METHOD, PK_SPREAD, PK_FIELD, PK_PROTO };

/* var kinds (N_VAR .op) */
enum { VK_VAR, VK_LET, VK_CONST };

struct cscope;
struct binding;
struct cfunc;

struct node {
    uint16_t type;
    uint16_t op;
    uint32_t flags;
    int line, col;
    uint32_t pos, end;
    struct node *a, *b, *c, *d;
    struct node **list;
    int n;
    struct jsstring *str;                       /* identifiers, strings, member names, labels */
    struct jsstring *raw;                       /* templates: the raw parts' text; regexps: flags */
    double num;
    struct cscope *scope;                       /* nodes that make a scope */
    struct binding *binding;                    /* identifiers, once resolved (0: a global) */
    struct binding *binding2;                   /* super: the `this` binding next to the home object */
    struct binding *binding3;                   /* super(): new.target */
    struct cfunc *fn;                           /* N_FUNCTION; N_CLASS: the instance fields' initializer */
    struct cfunc *fn2;                          /* N_CLASS: the static fields and blocks */
    struct node **raws;                         /* templates: the cooked strings' nodes (n of them) */
    int site;                                   /* tagged templates: their constant */
};

/* A function's facts from the parser. */
enum {
    FN_ARROW = 1, FN_ASYNC = 2, FN_GENERATOR = 4, FN_METHOD = 8, FN_GETTER = 16, FN_SETTER = 32,
    FN_CTOR = 64, FN_DERIVED = 128, FN_STRICT = 256, FN_EXPRESSION = 512, FN_DECLARATION = 1024,
    FN_SIMPLE_PARAMS = 2048, FN_CLASS_CTOR = 4096, FN_FIELD_INIT = 8192, FN_SCRIPT = 16384,
    FN_STATIC_BLOCK = 32768, FN_HAS_NAME_BINDING = 65536,
};

struct cfunc {
    uint32_t flags;
    struct jsstring *name;
    struct node **params;
    int nparams;
    struct node **body;
    int nbody;
    struct node *expr;                          /* an arrow's expression body */
    uint32_t start, end;                        /* the source text (toString) */
    int line;
    struct node *class_node;                    /* class constructors: their class (fields) */
    /* compile.c's */
    struct cscope *scope;
    struct cfunc *parent;
    bool uses_arguments, uses_this, uses_new_target, uses_home, has_eval;
    int nregs;
    int length;                                 /* params before the first default or rest */
};

/* --- scopes (compile.c) --------------------------------------------------- */

enum bind_kind { B_VAR, B_LET, B_CONST, B_CLASS, B_FUNCTION, B_PARAM, B_CATCH, B_THIS,
                 B_ARGUMENTS, B_NEW_TARGET, B_HOME, B_FUNC_NAME, B_CLASS_INNER };

struct binding {
    struct jsstring *name;
    uint8_t kind;
    bool captured;
    bool in_heap;
    bool tdz;                                   /* needs an initialized check */
    int slot;                                   /* a register, or the heap scope's slot */
    struct cscope *scope;
    uint32_t decl_pos;
    struct node *func;                          /* hoisted function declarations */
};

enum scope_kind { S_FUNCTION, S_BLOCK, S_CATCH, S_FOR, S_SWITCH, S_CLASS, S_SCRIPT, S_FUNC_NAME };

struct cscope {
    struct cscope *parent;
    struct cfunc *fn;
    enum scope_kind kind;
    struct binding **b;
    int nb, cap;
    bool heap;                                  /* has captured bindings: a scope cell at run time */
    int heap_size;
};

/* The parser: a script's text to its tree. */
struct node *parse_script(struct lexer *L, bool strict);
struct node *parse_function_text(struct lexer *L, int kind);
void *arena_new(size_t n);
void arena_free_all(void);

#endif

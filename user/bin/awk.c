/*
 * awk [-F FS] [-v NAME=VALUE]... 'PROGRAM' [ARGUMENT]...
 * awk [-F FS] [-v NAME=VALUE]... -f FILE... [ARGUMENT]... (58c): POSIX's awk.
 *
 * A program is a list of PATTERN { ACTION } pairs (BEGIN and END too, a
 * PATTERN, PATTERN range, a missing pattern for every record, a missing
 * action to print it) and functions. Each input record (a line; RS="" a
 * paragraph) is split into fields $1..$NF by FS (" ": blanks; one other
 * character: that one; more: an ERE). Values are numbers or strings, as
 * used; variables need no declaration; arrays are associative (a[i, j]:
 * the subscripts joined by SUBSEP). The language as POSIX describes it:
 * operators (with ^, ~ and !~, in, ?:, the assignments, ++ and --),
 * if/while/do/for/for-in, break, continue, next, nextfile, exit, return,
 * delete, getline in its six forms, print and printf with > >> and |, the
 * functions length substr index split sub gsub match sprintf sin cos
 * atan2 exp log sqrt int rand srand tolower toupper system close fflush,
 * and the variables NR NF FNR FS OFS ORS RS FILENAME SUBSEP RSTART RLENGTH
 * CONVFMT OFMT ENVIRON ARGC ARGV. ARGUMENTs are files ("-": the standard
 * input) or NAME=VALUE assignments made when reached.
 *
 * This file: values and variables, the lexer, the parser (a tree), the
 * tree's interpreter, input (records and fields) and output (streams by
 * name). Exit status: exit's, or 2 after an error.
 */
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <regex.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("awk [-F FS] [-v NAME=VALUE]... ['PROGRAM' | -f PROGFILE...] [FILE | NAME=VALUE]...",
             "scan and process text: patterns, fields, arrays (POSIX's awk)");

/* --- errors -------------------------------------------------------------------- */

static int source_line;                 /* the program's line, for messages */

static __attribute__((noreturn)) void fatal(const char *format, ...)
{
    fflush(stdout);
    va_list ap;
    va_start(ap, format);
    char text[512];
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    if (source_line)
        dprintf(STDERR_FILENO, "awk: %s (line %d)\n", text, source_line);
    else
        dprintf(STDERR_FILENO, "awk: %s\n", text);
    exit(2);
}

static void *must(void *p)
{
    if (!p)
        fatal("out of memory");
    return p;
}

static char *copy(const char *s)
{
    return must(strdup(s));
}

static char *copy_n(const char *s, size_t n)
{
    char *r = must(malloc(n + 1));
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

/* --- growing strings ---------------------------------------------------------------- */

struct text {
    char *s;
    size_t n, room;
};

static void text_add(struct text *t, const char *s, size_t n)
{
    if (t->n + n + 1 > t->room) {
        size_t room = t->room ? t->room : 64;
        while (t->n + n + 1 > room)
            room *= 2;
        t->s = must(realloc(t->s, room));
        t->room = room;
    }
    memcpy(t->s + t->n, s, n);
    t->n += n;
    t->s[t->n] = 0;
}

static void text_char(struct text *t, char c)
{
    text_add(t, &c, 1);
}

static void text_str(struct text *t, const char *s)
{
    text_add(t, s, strlen(s));
}

/* --- values ----------------------------------------------------------------------- */

enum { T_UNINIT, T_NUM, T_STR, T_STRNUM };

struct value {
    uint8_t type;
    bool has_num, has_str;
    double num;
    char *str;                          /* owned, when has_str */
};

static struct value uninit_value(void)
{
    struct value v = { T_UNINIT, true, false, 0, 0 };
    return v;
}

static struct value num_value(double d)
{
    struct value v = { T_NUM, true, false, d, 0 };
    return v;
}

static struct value str_value_owned(char *s)
{
    struct value v = { T_STR, false, true, 0, s };
    return v;
}

static struct value str_value(const char *s)
{
    return str_value_owned(copy(s));
}

static void free_value(struct value *v)
{
    if (v->has_str)
        free(v->str);
    v->has_str = false;
    v->str = 0;
}

static struct value copy_value(const struct value *v)
{
    struct value c = *v;
    if (v->has_str)
        c.str = copy(v->str);
    return c;
}

/* A number at the start of s as POSIX's awk reads one (no hex, no inf):
 * its value, and where it ended in *end (s if none). */
static double scan_number(const char *s, const char **end)
{
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v')
        p++;
    const char *start = p;
    if (*p == '+' || *p == '-')
        p++;
    bool digits = false;
    while (isdigit((unsigned char)*p))
        p++, digits = true;
    if (*p == '.') {
        p++;
        while (isdigit((unsigned char)*p))
            p++, digits = true;
    }
    if (!digits) {
        *end = s;
        return 0;
    }
    if (*p == 'e' || *p == 'E') {
        const char *e = p + 1;
        if (*e == '+' || *e == '-')
            e++;
        if (isdigit((unsigned char)*e)) {
            while (isdigit((unsigned char)*e))
                e++;
            p = e;
        }
    }
    *end = p;
    char buf[128];
    size_t n = (size_t)(p - start) < sizeof(buf) - 1 ? (size_t)(p - start) : sizeof(buf) - 1;
    memcpy(buf, start, n);
    buf[n] = 0;
    return strtod(buf, 0);
}

/* Does the text from input look like a number (blanks around it allowed)? */
static bool looks_numeric(const char *s)
{
    const char *end;
    scan_number(s, &end);
    if (end == s)
        return false;
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r' || *end == '\f' ||
           *end == '\v')
        end++;
    return *end == 0;
}

/* A string from input (a field, a getline variable, ARGV...): a "strnum"
 * when it looks like a number, else a string. */
static struct value input_value(const char *s, size_t n)
{
    struct value v = str_value_owned(copy_n(s, n));
    if (looks_numeric(v.str)) {
        v.type = T_STRNUM;
        const char *end;
        v.num = scan_number(v.str, &end);
        v.has_num = true;
    }
    return v;
}

static double to_num(struct value *v)
{
    if (!v->has_num) {
        const char *end;
        v->num = v->has_str ? scan_number(v->str, &end) : 0;
        v->has_num = true;
    }
    return v->num;
}

static const char *convfmt_text(void);
static const char *ofmt_text(void);

static void format_number(double d, const char *fmt, char *out, size_t size)
{
    if (isnan(d))
        snprintf(out, size, "%s", signbit(d) ? "-nan" : "nan");
    else if (isinf(d))
        snprintf(out, size, "%s", d < 0 ? "-inf" : "inf");
    else if (d == (double)(long long)d && fabs(d) < 1e16)
        snprintf(out, size, "%lld", (long long)d);
    else
        snprintf(out, size, fmt, d);
}

/* The string value (numbers through CONVFMT); valid while v is. */
static const char *to_str(struct value *v)
{
    if (!v->has_str) {
        char buf[64];
        if (v->type == T_UNINIT)
            buf[0] = 0;
        else
            format_number(v->num, convfmt_text(), buf, sizeof(buf));
        v->str = copy(buf);
        v->has_str = true;
    }
    return v->str;
}

/* For output: numbers through OFMT. The result is in out, or v's string. */
static const char *output_str(struct value *v, char *out, size_t size)
{
    if (v->type == T_NUM) {
        format_number(v->num, ofmt_text(), out, size);
        return out;
    }
    return to_str(v);
}

static bool truth(struct value *v)
{
    switch (v->type) {
    case T_NUM:
    case T_STRNUM:
        return to_num(v) != 0;
    case T_STR:
        return to_str(v)[0] != 0;
    }
    return false;
}

/* --- arrays ----------------------------------------------------------------------- */

struct entry {
    char *key;
    struct value v;
    struct entry *next;
};

struct array {
    struct entry **buckets;
    size_t size, count;
};

static struct array *new_array(void)
{
    struct array *a = must(calloc(1, sizeof(*a)));
    a->size = 64;
    a->buckets = must(calloc(a->size, sizeof(*a->buckets)));
    return a;
}

static size_t hash(const char *s)
{
    size_t h = 5381;
    while (*s)
        h = h * 33 + (unsigned char)*s++;
    return h;
}

static struct entry *array_find(struct array *a, const char *key)
{
    for (struct entry *e = a->buckets[hash(key) % a->size]; e; e = e->next)
        if (!strcmp(e->key, key))
            return e;
    return 0;
}

static struct value *array_get(struct array *a, const char *key)
{
    struct entry *e = array_find(a, key);
    if (e)
        return &e->v;
    if (a->count >= a->size * 2) {                  /* grow */
        size_t size = a->size * 4;
        struct entry **b = must(calloc(size, sizeof(*b)));
        for (size_t i = 0; i < a->size; i++)
            for (struct entry *x = a->buckets[i], *n; x; x = n) {
                n = x->next;
                size_t h = hash(x->key) % size;
                x->next = b[h];
                b[h] = x;
            }
        free(a->buckets);
        a->buckets = b;
        a->size = size;
    }
    e = must(malloc(sizeof(*e)));
    e->key = copy(key);
    e->v = uninit_value();
    size_t h = hash(key) % a->size;
    e->next = a->buckets[h];
    a->buckets[h] = e;
    a->count++;
    return &e->v;
}

static void array_delete(struct array *a, const char *key)
{
    for (struct entry **l = &a->buckets[hash(key) % a->size]; *l; l = &(*l)->next)
        if (!strcmp((*l)->key, key)) {
            struct entry *e = *l;
            *l = e->next;
            free(e->key);
            free_value(&e->v);
            free(e);
            a->count--;
            return;
        }
}

static void array_clear(struct array *a)
{
    for (size_t i = 0; i < a->size; i++) {
        for (struct entry *e = a->buckets[i], *n; e; e = n) {
            n = e->next;
            free(e->key);
            free_value(&e->v);
            free(e);
        }
        a->buckets[i] = 0;
    }
    a->count = 0;
}

/* The keys, for for-in (a copy: the loop may change the array). */
static char **array_keys(struct array *a, size_t *count)
{
    char **keys = must(malloc(sizeof(char *) * (a->count + 1)));
    size_t n = 0;
    for (size_t i = 0; i < a->size; i++)
        for (struct entry *e = a->buckets[i]; e; e = e->next)
            keys[n++] = copy(e->key);
    *count = n;
    return keys;
}

/* --- variables -------------------------------------------------------------------- */

enum { C_UNTYPED, C_SCALAR, C_ARRAY };

struct cell {
    int kind;
    struct value v;
    struct array *a;
};

static struct cell *globals;
static char **global_names;
static int global_count, global_room;

enum { V_NF, V_NR, V_FNR, V_FS, V_OFS, V_ORS, V_RS, V_FILENAME, V_SUBSEP, V_RSTART, V_RLENGTH,
       V_CONVFMT, V_OFMT, V_ENVIRON, V_ARGC, V_ARGV, V_SPECIALS };

static int global_index(const char *name)
{
    for (int i = 0; i < global_count; i++)
        if (!strcmp(global_names[i], name))
            return i;
    if (global_count == global_room) {
        global_room = global_room ? global_room * 2 : 64;
        globals = must(realloc(globals, sizeof(*globals) * (size_t)global_room));
        global_names = must(realloc(global_names, sizeof(char *) * (size_t)global_room));
    }
    globals[global_count] = (struct cell){ C_UNTYPED, uninit_value(), 0 };
    global_names[global_count] = copy(name);
    return global_count++;
}

static struct array *cell_array(struct cell *c)
{
    if (c->kind == C_SCALAR)
        fatal("a scalar used as an array");
    if (c->kind == C_UNTYPED) {
        c->kind = C_ARRAY;
        c->a = new_array();
    }
    return c->a;
}

static void set_scalar(struct cell *c, struct value v)
{
    if (c->kind == C_ARRAY)
        fatal("an array used as a scalar");
    c->kind = C_SCALAR;
    free_value(&c->v);
    c->v = v;
}

static const char *special_str(int i)
{
    struct cell *c = &globals[i];
    if (c->kind == C_ARRAY)
        return "";
    return to_str(&c->v);
}

static const char *convfmt_text(void)
{
    return globals ? special_str(V_CONVFMT) : "%.6g";
}

static const char *ofmt_text(void)
{
    return globals ? special_str(V_OFMT) : "%.6g";
}

/* --- the program's tree ------------------------------------------------------------- */

enum {
    /* expressions */
    N_NUM, N_STR, N_REGEX, N_VAR, N_LOCAL, N_FIELD, N_INDEX, N_ASSIGN, N_COND, N_OR, N_AND,
    N_IN, N_MATCH, N_NOMATCH, N_LT, N_LE, N_NE, N_EQ, N_GT, N_GE, N_CONCAT, N_ADD, N_SUB, N_MUL,
    N_DIV, N_MOD, N_POW, N_NEG, N_PLUS, N_NOT, N_PREINC, N_PREDEC, N_POSTINC, N_POSTDEC, N_CALL,
    N_BUILTIN, N_GETLINE, N_GROUP,
    /* statements */
    S_EXPR, S_PRINT, S_PRINTF, S_IF, S_WHILE, S_DO, S_FOR, S_FORIN, S_BLOCK, S_NEXT, S_NEXTFILE,
    S_EXIT, S_RETURN, S_BREAK, S_CONTINUE, S_DELETE,
};

enum { G_SIMPLE, G_FILE, G_COMMAND };                   /* getline's kinds */
enum { R_NONE, R_FILE, R_APPEND, R_PIPE };              /* print's redirections */

enum { B_LENGTH, B_SUBSTR, B_INDEX, B_SPLIT, B_SUB, B_GSUB, B_MATCH, B_SPRINTF, B_SIN, B_COS,
       B_ATAN2, B_EXP, B_LOG, B_SQRT, B_INT, B_RAND, B_SRAND, B_TOLOWER, B_TOUPPER, B_SYSTEM,
       B_CLOSE, B_FFLUSH };

static const char *const builtin_names[] = { "length", "substr", "index", "split", "sub",
    "gsub", "match", "sprintf", "sin", "cos", "atan2", "exp", "log", "sqrt", "int", "rand",
    "srand", "tolower", "toupper", "system", "close", "fflush" };

struct node {
    int op;
    int value;                  /* VAR/LOCAL: the index; ASSIGN: the operator (0 for =);
                                 * BUILTIN: which; CALL: the function; GETLINE: the kind;
                                 * PRINT: the redirection; N_STR etc. */
    int line;
    double num;
    char *str;
    regex_t *re;
    struct node *a, *b, *c, *d;  /* operands; a list's members by next */
    struct node *next;
};

static struct node *new_node(int op, struct node *a, struct node *b)
{
    struct node *n = must(calloc(1, sizeof(*n)));
    n->op = op;
    n->a = a;
    n->b = b;
    n->line = source_line;
    return n;
}

struct function {
    char *name;
    char **params;
    int param_count;
    struct node *body;
    bool defined;
};

static struct function *functions;
static int function_count, function_room;

static int function_index(const char *name)
{
    for (int i = 0; i < function_count; i++)
        if (!strcmp(functions[i].name, name))
            return i;
    if (function_count == function_room) {
        function_room = function_room ? function_room * 2 : 16;
        functions = must(realloc(functions, sizeof(*functions) * (size_t)function_room));
    }
    functions[function_count] = (struct function){ copy(name), 0, 0, 0, false };
    return function_count++;
}

struct item {                   /* PATTERN [, PATTERN] { ACTION } */
    struct node *pattern, *pattern2, *action;
    bool in_range;
};

static struct node *begin_actions, *end_actions;       /* lists of blocks */
static struct item *items;
static int item_count, item_room;

/* --- the lexer --------------------------------------------------------------------- */

enum {
    K_EOF = 256, K_NEWLINE, K_NUMBER, K_STRING, K_ERE, K_NAME, K_FUNC_NAME, K_BUILTIN,
    K_BEGIN, K_END, K_FUNCTION, K_IF, K_ELSE, K_WHILE, K_FOR, K_DO, K_BREAK, K_CONTINUE,
    K_NEXT, K_NEXTFILE, K_EXIT, K_RETURN, K_DELETE, K_IN, K_GETLINE, K_PRINT, K_PRINTF,
    K_AND, K_OR, K_EQ, K_NE, K_LE, K_GE, K_MATCH_NOT, K_INCR, K_DECR, K_APPEND,
    K_ADD_ASSIGN, K_SUB_ASSIGN, K_MUL_ASSIGN, K_DIV_ASSIGN, K_MOD_ASSIGN, K_POW_ASSIGN,
};

static const struct {
    const char *word;
    int token;
} keywords[] = {
    { "BEGIN", K_BEGIN }, { "END", K_END }, { "function", K_FUNCTION }, { "func", K_FUNCTION },
    { "if", K_IF }, { "else", K_ELSE }, { "while", K_WHILE }, { "for", K_FOR }, { "do", K_DO },
    { "break", K_BREAK }, { "continue", K_CONTINUE }, { "next", K_NEXT },
    { "nextfile", K_NEXTFILE }, { "exit", K_EXIT }, { "return", K_RETURN },
    { "delete", K_DELETE }, { "in", K_IN }, { "getline", K_GETLINE }, { "print", K_PRINT },
    { "printf", K_PRINTF },
};

static const char *src;                 /* the program's text, where the lexer is */
static int token, previous;
static double token_num;
static char *token_str;                 /* NAME, STRING, ERE: owned by the lexer until taken */
static int token_builtin;

/* Whether a '/' here starts an ERE: not after something a division follows. */
static bool ere_allowed(void)
{
    switch (previous) {
    case K_NAME: case K_NUMBER: case K_STRING: case K_ERE: case K_BUILTIN: case ')': case ']':
    case '$': case K_INCR: case K_DECR:
        return false;
    }
    return true;
}

static void string_escape(const char **p, struct text *t, bool in_ere)
{
    const char *s = *p;                             /* after the backslash */
    char c = *s++;
    switch (c) {
    case 'n': text_char(t, '\n'); break;
    case 't': text_char(t, '\t'); break;
    case 'r': text_char(t, '\r'); break;
    case 'f': text_char(t, '\f'); break;
    case 'v': text_char(t, '\v'); break;
    case 'a': text_char(t, '\a'); break;
    case 'b': text_char(t, '\b'); break;
    case '"': text_char(t, '"'); break;
    case '/': text_char(t, '/'); break;
    case '\\':
        text_str(t, in_ere ? "\\\\" : "\\");
        break;
    default:
        if (c >= '0' && c <= '7') {
            int v = c - '0';
            for (int k = 0; k < 2 && *s >= '0' && *s <= '7'; k++)
                v = v * 8 + (*s++ - '0');
            if (in_ere && strchr("\\^$.[]|()*+?{}", v)) {
                text_char(t, '\\');
            }
            text_char(t, (char)v);
        } else {
            text_char(t, '\\');                     /* (kept: \. in an ERE, \c elsewhere) */
            text_char(t, c);
        }
    }
    *p = s;
}

static void next_token(void)
{
    free(token_str);
    token_str = 0;
    previous = token;
    for (;;) {
        while (*src == ' ' || *src == '\t' || *src == '\r')
            src++;
        if (src[0] == '\\' && src[1] == '\n') {     /* a continued line */
            src += 2;
            source_line++;
            continue;
        }
        if (*src == '\\' && src[1] == '\r' && src[2] == '\n') {
            src += 3;
            source_line++;
            continue;
        }
        if (*src == '#') {
            while (*src && *src != '\n')
                src++;
            continue;
        }
        break;
    }
    char c = *src;
    if (!c) {
        token = K_EOF;
        return;
    }
    if (c == '\n') {
        src++;
        source_line++;
        token = K_NEWLINE;
        return;
    }
    if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)src[1]))) {
        const char *end;
        token_num = scan_number(src, &end);
        src = end;
        token = K_NUMBER;
        return;
    }
    if (isalpha((unsigned char)c) || c == '_') {
        const char *s = src;
        while (isalnum((unsigned char)*src) || *src == '_')
            src++;
        char *word = copy_n(s, (size_t)(src - s));
        for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++)
            if (!strcmp(word, keywords[i].word)) {
                free(word);
                token = keywords[i].token;
                return;
            }
        for (size_t i = 0; i < sizeof(builtin_names) / sizeof(builtin_names[0]); i++)
            if (!strcmp(word, builtin_names[i])) {
                free(word);
                token = K_BUILTIN;
                token_builtin = (int)i;
                return;
            }
        token_str = word;
        token = *src == '(' ? K_FUNC_NAME : K_NAME;
        return;
    }
    if (c == '"') {
        src++;
        struct text t = { 0 };
        text_add(&t, "", 0);
        while (*src != '"') {
            if (!*src || *src == '\n')
                fatal("a string without its end");
            if (*src == '\\' && src[1]) {
                src++;
                if (*src == '\n') {
                    src++;
                    source_line++;
                    continue;
                }
                string_escape(&src, &t, false);
                continue;
            }
            text_char(&t, *src++);
        }
        src++;
        token_str = t.s;
        token = K_STRING;
        return;
    }
    if (c == '/' && ere_allowed()) {
        src++;
        struct text t = { 0 };
        text_add(&t, "", 0);
        bool in_set = false;
        while (in_set || *src != '/') {
            if (!*src || *src == '\n')
                fatal("a regular expression without its end");
            if (*src == '\\' && src[1]) {
                src++;
                string_escape(&src, &t, true);
                continue;
            }
            if (*src == '[' && !in_set) {
                in_set = true;
                text_char(&t, *src++);
                if (*src == '^')
                    text_char(&t, *src++);
                if (*src == ']')
                    text_char(&t, *src++);
                continue;
            }
            if (*src == ']' && in_set)
                in_set = false;
            text_char(&t, *src++);
        }
        src++;
        token_str = t.s;
        token = K_ERE;
        return;
    }
    src++;
    char d = *src;
#define TWO(x, y, t) if (c == x && d == y) { src++; token = t; return; }
    TWO('&', '&', K_AND)
    TWO('|', '|', K_OR)
    TWO('=', '=', K_EQ)
    TWO('!', '=', K_NE)
    TWO('<', '=', K_LE)
    TWO('>', '=', K_GE)
    TWO('!', '~', K_MATCH_NOT)
    TWO('+', '+', K_INCR)
    TWO('-', '-', K_DECR)
    TWO('>', '>', K_APPEND)
    TWO('+', '=', K_ADD_ASSIGN)
    TWO('-', '=', K_SUB_ASSIGN)
    TWO('*', '=', K_MUL_ASSIGN)
    TWO('/', '=', K_DIV_ASSIGN)
    TWO('%', '=', K_MOD_ASSIGN)
    TWO('^', '=', K_POW_ASSIGN)
#undef TWO
    if (c == '*' && d == '*') {                     /* ** and **= as ^ and ^= (common) */
        src++;
        if (*src == '=') {
            src++;
            token = K_POW_ASSIGN;
        } else {
            token = '^';
        }
        return;
    }
    if (strchr("{}()[];,+-*/%^!><|?:~$=", c)) {
        token = c;
        return;
    }
    fatal("an unexpected character '%c'", c);
}

static __attribute__((noreturn)) void syntax(void)
{
    if (token == K_EOF)
        fatal("a syntax error at the program's end");
    fatal("a syntax error");
}

static void expect(int t)
{
    if (token != t)
        syntax();
    next_token();
}

static void newlines(void)
{
    while (token == K_NEWLINE)
        next_token();
}

static char *take_str(void)
{
    char *s = token_str;
    token_str = 0;
    return s;
}

/* --- the parser -------------------------------------------------------------------- */

static int current_function = -1;       /* while parsing a function: its index */

static struct node *expression(bool no_gt, bool no_in);
static struct node *statement(void);
static struct node *unary(bool no_gt, bool no_in);

static struct node *name_node(char *name)
{
    if (current_function >= 0) {
        struct function *f = &functions[current_function];
        for (int i = 0; i < f->param_count; i++)
            if (!strcmp(f->params[i], name)) {
                free(name);
                struct node *n = new_node(N_LOCAL, 0, 0);
                n->value = i;
                return n;
            }
    }
    struct node *n = new_node(N_VAR, 0, 0);
    n->value = global_index(name);
    free(name);
    return n;
}

static bool is_lvalue(const struct node *n)
{
    return n && (n->op == N_VAR || n->op == N_LOCAL || n->op == N_FIELD || n->op == N_INDEX);
}

static regex_t *compile_ere(const char *text)
{
    regex_t *re = must(malloc(sizeof(*re)));
    int e = regcomp(re, text, REG_EXTENDED);
    if (e) {
        char why[100];
        regerror(e, re, why, sizeof(why));
        fatal("%s: /%s/", why, text);
    }
    return re;
}

/* ( expr, expr ... ): a list (for in, print, or a call) */
static struct node *expression_list(int end, bool no_gt)
{
    struct node *first = 0, **tail = &first;
    newlines();
    while (token != end) {
        *tail = expression(no_gt, false);
        tail = &(*tail)->next;
        if (token != ',')
            break;
        next_token();
        newlines();
    }
    return first;
}

/* An lvalue for getline (optional): a name, a[..], or $expr. */
static struct node *getline_target(void)
{
    if (token == K_NAME || token == '$')
        return unary(true, true);                   /* (no > or in: getline var < file) */
    return 0;
}

static struct node *primary(bool no_gt, bool no_in)
{
    struct node *n;
    switch (token) {
    case K_NUMBER:
        n = new_node(N_NUM, 0, 0);
        n->num = token_num;
        next_token();
        return n;
    case K_STRING:
        n = new_node(N_STR, 0, 0);
        n->str = take_str();
        next_token();
        return n;
    case K_ERE:
        n = new_node(N_REGEX, 0, 0);
        n->str = take_str();
        n->re = compile_ere(n->str);
        next_token();
        return n;
    case '(': {
        next_token();
        struct node *list = expression_list(')', false);
        expect(')');
        if (!list)
            syntax();
        if (list->next || token == K_IN) {          /* (a, b) in arr; print (a, b) */
            n = new_node(N_GROUP, list, 0);
            return n;
        }
        return list;
    }
    case '$': {
        next_token();
        struct node *index;
        if (token == K_INCR || token == K_DECR || token == '-' || token == '+' || token == '!')
            index = unary(no_gt, no_in);
        else
            index = primary(no_gt, no_in);
        return new_node(N_FIELD, index, 0);
    }
    case K_INCR:
    case K_DECR: {
        int op = token == K_INCR ? N_PREINC : N_PREDEC;
        next_token();
        struct node *target = unary(no_gt, no_in);
        if (!is_lvalue(target))
            fatal("++ or -- needs a variable");
        return new_node(op, target, 0);
    }
    case K_NAME: {
        struct node *var = name_node(take_str());
        next_token();
        if (token == '[') {
            next_token();
            struct node *subs = expression_list(']', false);
            expect(']');
            if (!subs)
                syntax();
            return new_node(N_INDEX, var, subs);
        }
        return var;
    }
    case K_FUNC_NAME: {
        int f = function_index(token_str);
        next_token();
        expect('(');
        n = new_node(N_CALL, expression_list(')', false), 0);
        n->value = f;
        expect(')');
        return n;
    }
    case K_BUILTIN: {
        int which = token_builtin;
        next_token();
        n = new_node(N_BUILTIN, 0, 0);
        n->value = which;
        if (token == '(') {
            next_token();
            n->a = expression_list(')', false);
            expect(')');
        } else if (which != B_LENGTH) {
            syntax();                               /* (only length goes without) */
        }
        return n;
    }
    case K_GETLINE: {                               /* getline [var] [< file] */
        next_token();
        n = new_node(N_GETLINE, getline_target(), 0);
        n->value = G_SIMPLE;
        if (token == '<') {
            next_token();
            n->b = primary(true, true);
            n->value = G_FILE;
        }
        return n;
    }
    case '-':
    case '+':
    case '!':
        return unary(no_gt, no_in);
    }
    syntax();
}

static struct node *postfix(bool no_gt, bool no_in)
{
    struct node *n = primary(no_gt, no_in);
    if ((token == K_INCR || token == K_DECR) && is_lvalue(n)) {
        n = new_node(token == K_INCR ? N_POSTINC : N_POSTDEC, n, 0);
        next_token();
    }
    return n;
}

static struct node *power(bool no_gt, bool no_in)
{
    struct node *n = postfix(no_gt, no_in);
    if (token == '^') {
        next_token();
        return new_node(N_POW, n, unary(no_gt, no_in));    /* (right to left; 2^-1) */
    }
    return n;
}

static struct node *unary(bool no_gt, bool no_in)
{
    if (token == '!') {
        next_token();
        return new_node(N_NOT, unary(no_gt, no_in), 0);
    }
    if (token == '-') {
        next_token();
        return new_node(N_NEG, unary(no_gt, no_in), 0);
    }
    if (token == '+') {
        next_token();
        return new_node(N_PLUS, unary(no_gt, no_in), 0);
    }
    return power(no_gt, no_in);
}

static struct node *multiplicative(bool no_gt, bool no_in)
{
    struct node *n = unary(no_gt, no_in);
    while (token == '*' || token == '/' || token == '%') {
        int op = token == '*' ? N_MUL : token == '/' ? N_DIV : N_MOD;
        next_token();
        n = new_node(op, n, unary(no_gt, no_in));
    }
    return n;
}

static struct node *additive(bool no_gt, bool no_in)
{
    struct node *n = multiplicative(no_gt, no_in);
    while (token == '+' || token == '-') {
        int op = token == '+' ? N_ADD : N_SUB;
        next_token();
        n = new_node(op, n, multiplicative(no_gt, no_in));
    }
    return n;
}

static bool starts_operand(void)
{
    switch (token) {
    case K_NUMBER: case K_STRING: case K_ERE: case K_NAME: case K_FUNC_NAME: case K_BUILTIN:
    case '$': case '(': case K_INCR: case K_DECR: case '!': case '-': case '+':
        return true;
    }
    return false;
}

static struct node *concatenation(bool no_gt, bool no_in)
{
    struct node *n = additive(no_gt, no_in);
    /* (a '-' or '+' here is the additive's; a '!' would be != or !~) */
    while (starts_operand() && token != '-' && token != '+' && token != '!' &&
           !(token == K_IN && no_in))
        n = new_node(N_CONCAT, n, additive(no_gt, no_in));
    return n;
}

static struct node *comparison(bool no_gt, bool no_in)
{
    struct node *n = concatenation(no_gt, no_in);
    for (;;) {
        if (token == '|') {                         /* cmd | getline [var] */
            const char *at = src;
            int line = source_line, prev = previous;
            next_token();
            if (token != K_GETLINE) {               /* (print's | : not this one's) */
                free(token_str);
                token_str = 0;
                src = at;
                source_line = line;
                previous = prev;
                token = '|';
                return n;
            }
            next_token();
            struct node *g = new_node(N_GETLINE, getline_target(), n);
            g->value = G_COMMAND;
            n = g;
            continue;
        }
        int op;
        switch (token) {
        case '<': op = N_LT; break;
        case K_LE: op = N_LE; break;
        case K_NE: op = N_NE; break;
        case K_EQ: op = N_EQ; break;
        case K_GE: op = N_GE; break;
        case '>':
            if (no_gt)
                return n;
            op = N_GT;
            break;
        default:
            return n;
        }
        next_token();
        n = new_node(op, n, concatenation(no_gt, no_in));
    }
}

static struct node *match_expression(bool no_gt, bool no_in)
{
    struct node *n = comparison(no_gt, no_in);
    while (token == '~' || token == K_MATCH_NOT) {
        int op = token == '~' ? N_MATCH : N_NOMATCH;
        next_token();
        n = new_node(op, n, comparison(no_gt, no_in));
    }
    return n;
}

static struct node *in_expression(bool no_gt, bool no_in)
{
    struct node *n = match_expression(no_gt, no_in);
    while (token == K_IN && !no_in) {
        next_token();
        if (token != K_NAME)
            syntax();
        struct node *array = name_node(take_str());
        next_token();
        struct node *subs = n->op == N_GROUP ? n->a : n;
        n = new_node(N_IN, subs, array);
    }
    return n;
}

static struct node *and_expression(bool no_gt, bool no_in)
{
    struct node *n = in_expression(no_gt, no_in);
    while (token == K_AND) {
        next_token();
        newlines();
        n = new_node(N_AND, n, in_expression(no_gt, no_in));
    }
    return n;
}

static struct node *or_expression(bool no_gt, bool no_in)
{
    struct node *n = and_expression(no_gt, no_in);
    while (token == K_OR) {
        next_token();
        newlines();
        n = new_node(N_OR, n, and_expression(no_gt, no_in));
    }
    return n;
}

static struct node *ternary(bool no_gt, bool no_in)
{
    struct node *n = or_expression(no_gt, no_in);
    if (token == '?') {
        next_token();
        newlines();
        struct node *yes = ternary(no_gt, no_in);
        newlines();
        expect(':');
        newlines();
        struct node *no = ternary(no_gt, no_in);
        struct node *c = new_node(N_COND, n, yes);
        c->c = no;
        return c;
    }
    return n;
}

static int assign_op(int t)
{
    switch (t) {
    case '=': return 1;
    case K_ADD_ASSIGN: return N_ADD;
    case K_SUB_ASSIGN: return N_SUB;
    case K_MUL_ASSIGN: return N_MUL;
    case K_DIV_ASSIGN: return N_DIV;
    case K_MOD_ASSIGN: return N_MOD;
    case K_POW_ASSIGN: return N_POW;
    }
    return 0;
}

static struct node *expression(bool no_gt, bool no_in)
{
    struct node *n = ternary(no_gt, no_in);
    int op = assign_op(token);
    if (op && is_lvalue(n)) {
        next_token();
        newlines();
        struct node *a = new_node(N_ASSIGN, n, expression(no_gt, no_in));
        a->value = op == 1 ? 0 : op;
        return a;
    }
    if (op == 1 && n->op == N_GROUP)
        syntax();
    return n;
}

/* A statement's end: ';', a line break, or before '}' or the end. */
static void terminator(void)
{
    if (token == ';' || token == K_NEWLINE) {
        next_token();
        newlines();
        return;
    }
    if (token == '}' || token == K_EOF)
        return;
    syntax();
}

static struct node *simple_statement(void)
{
    struct node *n;
    switch (token) {
    case K_PRINT:
    case K_PRINTF: {
        int op = token == K_PRINT ? S_PRINT : S_PRINTF;
        next_token();
        struct node *args = 0;
        if (token != ';' && token != K_NEWLINE && token != '}' && token != '>' &&
            token != K_APPEND && token != '|' && token != K_EOF) {
            struct node **tail = &args;
            for (;;) {
                *tail = expression(true, false);
                if ((*tail)->op == N_GROUP && !args->next && tail == &args) {
                    struct node *group = *tail;
                    *tail = group->a;               /* print (a, b): the list */
                    free(group);
                    while (*tail)
                        tail = &(*tail)->next;
                } else {
                    tail = &(*tail)->next;
                }
                if (token != ',')
                    break;
                next_token();
                newlines();
            }
        }
        if (op == S_PRINTF && !args)
            fatal("printf needs a format");
        n = new_node(op, args, 0);
        n->value = R_NONE;
        if (token == '>' || token == K_APPEND || token == '|') {
            n->value = token == '>' ? R_FILE : token == K_APPEND ? R_APPEND : R_PIPE;
            next_token();
            n->b = concatenation(true, false);
        }
        return n;
    }
    case K_NEXT:
        next_token();
        return new_node(S_NEXT, 0, 0);
    case K_NEXTFILE:
        next_token();
        return new_node(S_NEXTFILE, 0, 0);
    case K_EXIT:
        next_token();
        n = new_node(S_EXIT, 0, 0);
        if (token != ';' && token != K_NEWLINE && token != '}' && token != K_EOF)
            n->a = expression(false, false);
        return n;
    case K_RETURN:
        if (current_function < 0)
            fatal("return outside a function");
        next_token();
        n = new_node(S_RETURN, 0, 0);
        if (token != ';' && token != K_NEWLINE && token != '}' && token != K_EOF)
            n->a = expression(false, false);
        return n;
    case K_BREAK:
        next_token();
        return new_node(S_BREAK, 0, 0);
    case K_CONTINUE:
        next_token();
        return new_node(S_CONTINUE, 0, 0);
    case K_DELETE:
        next_token();
        if (token != K_NAME)
            syntax();
        n = new_node(S_DELETE, name_node(take_str()), 0);
        next_token();
        if (token == '[') {
            next_token();
            n->b = expression_list(']', false);
            expect(']');
        }
        return n;
    }
    return new_node(S_EXPR, expression(false, false), 0);
}

static struct node *block(void)
{
    expect('{');
    newlines();
    struct node *first = 0, **tail = &first;
    while (token != '}') {
        if (token == K_EOF)
            syntax();
        if (token == ';') {
            next_token();
            newlines();
            continue;
        }
        *tail = statement();
        tail = &(*tail)->next;
    }
    next_token();
    return new_node(S_BLOCK, first, 0);
}

/* The body of if/while/for: a statement, a ';' alone being an empty one. */
static struct node *body(void)
{
    newlines();
    if (token == ';') {
        next_token();
        newlines();
        return new_node(S_BLOCK, 0, 0);
    }
    return statement();
}

static struct node *statement(void)
{
    struct node *n;
    switch (token) {
    case '{':
        n = block();
        newlines();
        if (token == ';') {
            next_token();
            newlines();
        }
        return n;
    case K_IF: {
        next_token();
        expect('(');
        struct node *cond = expression(false, false);
        expect(')');
        n = new_node(S_IF, cond, body());
        if (token == K_ELSE) {
            next_token();
            n->c = body();
        }
        return n;
    }
    case K_WHILE: {
        next_token();
        expect('(');
        struct node *cond = expression(false, false);
        expect(')');
        if (token == ';') {                         /* while (x); : an empty body */
            next_token();
            newlines();
            return new_node(S_WHILE, cond, new_node(S_BLOCK, 0, 0));
        }
        return new_node(S_WHILE, cond, body());
    }
    case K_DO: {
        next_token();
        struct node *what = body();
        newlines();
        expect(K_WHILE);
        expect('(');
        n = new_node(S_DO, what, expression(false, false));
        expect(')');
        terminator();
        return n;
    }
    case K_FOR: {
        next_token();
        expect('(');
        if (token == K_NAME) {                      /* for (k in a)? */
            const char *save_src = src;
            int save_line = source_line, save_prev = previous;
            char *name = take_str();
            next_token();
            if (token == K_IN) {
                next_token();
                if (token != K_NAME)
                    syntax();
                struct node *array = name_node(take_str());
                next_token();
                if (token == ')') {
                    next_token();
                    n = new_node(S_FORIN, name_node(name), array);
                    n->c = body();
                    return n;
                }
                fatal("for (... in ...) takes one name and one array");
            }
            /* Not for-in: start again at the name. */
            src = save_src;
            source_line = save_line;
            previous = save_prev;
            token = K_NAME;
            token_str = name;
        }
        n = new_node(S_FOR, 0, 0);
        if (token != ';')
            n->a = simple_statement();
        expect(';');
        newlines();
        if (token != ';')
            n->b = expression(false, false);
        expect(';');
        newlines();
        if (token != ')')
            n->c = simple_statement();
        expect(')');
        n->d = body();
        return n;
    }
    case ';':
        next_token();
        newlines();
        return new_node(S_BLOCK, 0, 0);
    }
    n = simple_statement();
    terminator();
    return n;
}

static void add_item(struct node *pattern, struct node *pattern2, struct node *action)
{
    if (item_count == item_room) {
        item_room = item_room ? item_room * 2 : 16;
        items = must(realloc(items, sizeof(*items) * (size_t)item_room));
    }
    items[item_count++] = (struct item){ pattern, pattern2, action, false };
}

static void append_block(struct node **list, struct node *b)
{
    while (*list)
        list = &(*list)->next;
    *list = b;
}

static void parse_program(const char *text)
{
    src = text;
    source_line = 1;
    token = K_NEWLINE;
    next_token();
    for (;;) {
        while (token == K_NEWLINE || token == ';')
            next_token();
        if (token == K_EOF)
            break;
        if (token == K_BEGIN || token == K_END) {
            bool begin = token == K_BEGIN;
            next_token();
            newlines();
            append_block(begin ? &begin_actions : &end_actions, block());
        } else if (token == K_FUNCTION) {
            next_token();
            if (token != K_NAME && token != K_FUNC_NAME)
                syntax();
            int f = function_index(token_str);
            if (functions[f].defined)
                fatal("function %s defined twice", token_str);
            next_token();
            expect('(');
            struct function *fn = &functions[f];
            while (token == K_NAME) {
                fn->params = must(realloc(fn->params, sizeof(char *) * (size_t)(fn->param_count + 1)));
                fn->params[fn->param_count++] = take_str();
                next_token();
                if (token != ',')
                    break;
                next_token();
                newlines();
            }
            expect(')');
            newlines();
            current_function = f;
            fn->body = block();
            fn->defined = true;
            current_function = -1;
        } else if (token == '{') {
            add_item(0, 0, block());
        } else {
            struct node *pattern = expression(false, false), *pattern2 = 0;
            if (token == ',') {
                next_token();
                newlines();
                pattern2 = expression(false, false);
            }
            struct node *action = token == '{' ? block() : 0;
            add_item(pattern, pattern2, action);
            if (!action && token != K_EOF && token != K_NEWLINE && token != ';')
                syntax();                           /* (after an action, the next item may follow) */
        }
    }
    for (int i = 0; i < function_count; i++)
        if (!functions[i].defined)
            fatal("calling undefined function %s", functions[i].name);
}

/* --- output streams ---------------------------------------------------------------- */

struct stream {
    char *name;
    FILE *f;
    int kind;                       /* R_FILE (also >>), R_PIPE, or 'r': getline < file,
                                     * 'p': cmd | getline */
    bool eof;
    struct text pending;            /* (input: bytes read past a record) */
};

static struct stream *streams;
static int stream_count, stream_room;

static void flush_all(void)
{
    fflush(stdout);
    for (int i = 0; i < stream_count; i++)
        if (streams[i].f && (streams[i].kind == R_FILE || streams[i].kind == R_PIPE))
            fflush(streams[i].f);
}

static struct stream *find_stream(const char *name, bool output)
{
    for (int i = 0; i < stream_count; i++)
        if (streams[i].f && !strcmp(streams[i].name, name) &&
            ((streams[i].kind == R_FILE || streams[i].kind == R_PIPE) == output))
            return &streams[i];
    return 0;
}

static struct stream *new_stream(const char *name, FILE *f, int kind)
{
    int i = 0;
    while (i < stream_count && streams[i].f)
        i++;
    if (i == stream_count) {
        if (stream_count == stream_room) {
            stream_room = stream_room ? stream_room * 2 : 8;
            streams = must(realloc(streams, sizeof(*streams) * (size_t)stream_room));
        }
        stream_count++;
    }
    streams[i] = (struct stream){ copy(name), f, kind, false, { 0 } };
    return &streams[i];
}

static FILE *output_stream(int redirect, const char *name)
{
    if (!strcmp(name, "/dev/stdout") || !strcmp(name, "-"))
        return stdout;
    if (!strcmp(name, "/dev/stderr")) {
        fflush(stdout);
        return stderr;
    }
    struct stream *s = find_stream(name, true);
    if (s)
        return s->f;
    FILE *f;
    if (redirect == R_PIPE) {
        flush_all();
        f = popen(name, "w");
    } else {
        f = fopen(name, redirect == R_APPEND ? "a" : "w");
    }
    if (!f)
        fatal("%s: %s", name, strerror(errno));
    return new_stream(name, f, redirect == R_PIPE ? R_PIPE : R_FILE)->f;
}

static int close_stream(const char *name)
{
    int result = -1;
    for (int i = 0; i < stream_count; i++) {
        struct stream *s = &streams[i];
        if (!s->f || strcmp(s->name, name))
            continue;
        if (s->f == stdin) {
            result = 0;                             /* (getline < "-": the standard input stays) */
        } else if (s->kind == R_PIPE || s->kind == 'p') {
            int status = pclose(s->f);
            result = status < 0 ? -1 : WIFEXITED(status) ? WEXITSTATUS(status)
                                                         : 256 + WTERMSIG(status);
        } else {
            result = fclose(s->f) == 0 ? 0 : -1;
        }
        free(s->name);
        free(s->pending.s);
        s->f = 0;
    }
    return result;
}

static void close_all(void)
{
    for (int i = 0; i < stream_count; i++)
        if (streams[i].f)
            close_stream(streams[i].name);
}

/* --- records and fields ------------------------------------------------------------- */

static struct text record;              /* $0 */
static struct value record_value;       /* (as a value, while valid) */
static bool record_value_valid;
static struct value *fields;            /* $1..$NF at fields[1..] */
static int field_count, field_room;
static bool fields_split;               /* fields reflect $0 */
static bool record_stale;               /* a field changed: $0 is to be joined again */

static void set_record(const char *s, size_t n)
{
    record.n = 0;
    text_add(&record, s, n);
    if (!record.s)
        text_add(&record, "", 0);
    if (record_value_valid)
        free_value(&record_value);
    record_value_valid = false;
    fields_split = false;
    record_stale = false;
}

static void ensure_fields(int n)
{
    if (n + 1 > field_room) {
        int room = field_room ? field_room : 16;
        while (n + 1 > room)
            room *= 2;
        fields = must(realloc(fields, sizeof(*fields) * (size_t)room));
        for (int i = field_room; i < room; i++)
            fields[i] = uninit_value();
        field_room = room;
    }
}

static void clear_fields(void)
{
    for (int i = 1; i <= field_count; i++) {
        free_value(&fields[i]);
        fields[i] = uninit_value();
    }
    field_count = 0;
}

static regex_t *dynamic_ere(const char *text);

/* Split text by fs the way FS does; each piece to add(piece, length, ctx). */
static void split_by(const char *s, const char *fs, bool paragraph,
                     void (*add)(const char *, size_t, void *), void *ctx)
{
    size_t n = strlen(s);
    if (!n)
        return;
    if (!strcmp(fs, " ")) {
        size_t i = 0;
        for (;;) {
            while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n'))
                i++;
            if (i >= n)
                break;
            size_t start = i;
            while (i < n && s[i] != ' ' && s[i] != '\t' && s[i] != '\n')
                i++;
            add(s + start, i - start, ctx);
        }
        return;
    }
    if (strlen(fs) == 1 && fs[0] != '\\') {
        char c = fs[0];
        size_t start = 0;
        for (size_t i = 0; i <= n; i++)
            if (i == n || s[i] == c || (paragraph && s[i] == '\n')) {
                add(s + start, i - start, ctx);
                start = i + 1;
            }
        return;
    }
    regex_t *re = dynamic_ere(fs);
    size_t at = 0;
    regmatch_t m;
    while (at <= n) {
        size_t cut = n, next = n + 1;
        const char *nl = paragraph ? memchr(s + at, '\n', n - at) : 0;
        if (regexec(re, s + at, 1, &m, at ? REG_NOTBOL : 0) == 0 && m.rm_eo > m.rm_so) {
            cut = at + (size_t)m.rm_so;
            next = at + (size_t)m.rm_eo;
        }
        if (nl && (size_t)(nl - s) < cut) {
            cut = (size_t)(nl - s);
            next = cut + 1;
        }
        add(s + at, cut - at, ctx);
        at = next;
    }
}

static void add_field(const char *s, size_t n, void *ctx)
{
    (void)ctx;
    ensure_fields(field_count + 1);
    fields[++field_count] = input_value(s, n);
}

static bool paragraph_mode(void)
{
    return special_str(V_RS)[0] == 0;
}

static void split_record(void)
{
    if (fields_split)
        return;
    clear_fields();
    split_by(record.s ? record.s : "", special_str(V_FS), paragraph_mode(), add_field, 0);
    fields_split = true;
}

/* $0 again from the fields, joined by OFS. */
static void join_record(void)
{
    if (!record_stale)
        return;
    struct text t = { 0 };
    text_add(&t, "", 0);
    const char *ofs = special_str(V_OFS);
    for (int i = 1; i <= field_count; i++) {
        if (i > 1)
            text_str(&t, ofs);
        text_str(&t, to_str(&fields[i]));
    }
    free(record.s);
    record = t;
    if (record_value_valid)
        free_value(&record_value);
    record_value_valid = false;
    record_stale = false;
}

static struct value *get_field(long i)
{
    if (i < 0)
        fatal("a field below $0: $%ld", i);
    if (i == 0) {
        join_record();
        if (!record_value_valid) {
            record_value = input_value(record.s ? record.s : "", record.n);
            record_value_valid = true;
        }
        return &record_value;
    }
    split_record();
    if (i > field_count) {
        static struct value none;
        free_value(&none);
        none = uninit_value();
        return &none;
    }
    return &fields[i];
}

static void set_nf(long n);

static void set_field(long i, struct value v)
{
    if (i < 0)
        fatal("a field below $0: $%ld", i);
    if (i == 0) {
        const char *s = to_str(&v);
        set_record(s, strlen(s));
        free_value(&v);
        return;
    }
    split_record();
    if (i > field_count)
        set_nf(i);
    free_value(&fields[i]);
    fields[i] = v;
    record_stale = true;
}

static void set_nf(long n)
{
    split_record();
    if (n < 0)
        n = 0;
    ensure_fields((int)n);
    for (int k = field_count + 1; k <= n; k++)
        fields[k] = str_value("");
    for (int k = (int)n + 1; k <= field_count; k++) {
        free_value(&fields[k]);
        fields[k] = uninit_value();
    }
    field_count = (int)n;
    record_stale = true;
}

/* --- input -------------------------------------------------------------------------- */

/* The next record of f into out (RS: one character, or "" for paragraphs);
 * false at the end. */
static bool read_record(FILE *f, struct text *out)
{
    out->n = 0;
    text_add(out, "", 0);
    const char *rs = special_str(V_RS);
    int ch;
    if (!rs[0]) {                                   /* paragraph mode */
        while ((ch = fgetc(f)) == '\n')
            ;
        if (ch == EOF)
            return false;
        for (;;) {
            if (ch == EOF)
                break;
            if (ch == '\n') {
                int next = fgetc(f);
                if (next == EOF)
                    break;                          /* (the file's last line break) */
                if (next == '\n') {
                    while ((next = fgetc(f)) == '\n')
                        ;
                    if (next != EOF)
                        ungetc(next, f);
                    break;
                }
                text_char(out, '\n');
                ch = next;
                continue;
            }
            text_char(out, (char)ch);
            ch = fgetc(f);
        }
        return true;
    }
    char sep = rs[0];
    bool any = false;
    while ((ch = fgetc(f)) != EOF) {
        any = true;
        if (ch == sep)
            return true;
        text_char(out, (char)ch);
    }
    return any;
}

static FILE *main_input;
static bool main_is_stdin, main_done, used_file_argument;
static int argv_at = 1;

static void assign_argument(const char *arg);

/* The next file of ARGV (assignments made on the way); false: none left. */
static bool open_main(void)
{
    while (!main_input) {
        struct cell *argc_cell = &globals[V_ARGC];
        long argc = (long)to_num(&argc_cell->v);
        if (argv_at >= argc) {
            if (used_file_argument)
                return false;
            used_file_argument = true;              /* (none: the standard input, once) */
            main_input = stdin;
            main_is_stdin = true;
            set_scalar(&globals[V_FILENAME], str_value(""));
            break;
        }
        char key[32];
        snprintf(key, sizeof(key), "%d", argv_at++);
        struct array *av = cell_array(&globals[V_ARGV]);
        struct entry *e = array_find(av, key);
        if (!e)
            continue;
        const char *arg = to_str(&e->v);
        if (!arg[0])
            continue;
        const char *eq = strchr(arg, '=');
        bool assignment = eq && (isalpha((unsigned char)arg[0]) || arg[0] == '_');
        for (const char *c = arg; assignment && c < eq; c++)
            if (!isalnum((unsigned char)*c) && *c != '_')
                assignment = false;
        if (assignment) {
            assign_argument(arg);
            continue;
        }
        used_file_argument = true;
        if (!strcmp(arg, "-")) {
            main_input = stdin;
            main_is_stdin = true;
        } else {
            main_input = fopen(arg, "r");
            main_is_stdin = false;
            if (!main_input) {
                fatal("can't open file %s", arg);
            }
        }
        set_scalar(&globals[V_FILENAME], str_value(arg));
        set_scalar(&globals[V_FNR], num_value(0));
    }
    return true;
}

static void close_main(void)
{
    if (main_input && !main_is_stdin)
        fclose(main_input);
    main_input = 0;
}

static void bump(int var)
{
    struct cell *c = &globals[var];
    double v = to_num(&c->v);
    set_scalar(c, num_value(v + 1));
}

/* The next main-input record into out; false at the end of all input. */
static bool next_main_record(struct text *out)
{
    if (main_done)
        return false;
    for (;;) {
        if (!open_main()) {
            main_done = true;
            return false;
        }
        if (read_record(main_input, out)) {
            bump(V_NR);
            bump(V_FNR);
            return true;
        }
        close_main();
    }
}

/* --- regular expressions from strings -------------------------------------------------- */

static struct {
    char *text;
    regex_t re;
} ere_cache[64];
static int ere_next;

static regex_t *dynamic_ere(const char *text)
{
    for (int i = 0; i < 64; i++)
        if (ere_cache[i].text && !strcmp(ere_cache[i].text, text))
            return &ere_cache[i].re;
    int slot = ere_next++ % 64;
    if (ere_cache[slot].text) {
        regfree(&ere_cache[slot].re);
        free(ere_cache[slot].text);
    }
    int e = regcomp(&ere_cache[slot].re, text, REG_EXTENDED);
    if (e) {
        char why[100];
        regerror(e, &ere_cache[slot].re, why, sizeof(why));
        ere_cache[slot].text = 0;
        fatal("%s: /%s/", why, text);
    }
    ere_cache[slot].text = copy(text);
    return &ere_cache[slot].re;
}

/* --- the interpreter ------------------------------------------------------------------- */

struct frame {
    struct cell *locals;
    struct cell **refs;             /* a parameter that is the caller's array (or untyped) */
    int count;
};

static struct frame *frame;
static int depth;
static struct value return_value;
static int exit_code;

enum { R_NORMAL, R_BREAK, R_CONTINUE, R_NEXT, R_NEXTFILE, R_EXIT, R_RETURN };

static struct value eval(struct node *n);
static int run(struct node *n);

/* A variable's cell; for a parameter that refers to the caller's (untyped)
 * variable, used as a scalar: a local of its own from now on. */
static struct cell *cell_of(struct node *n, bool as_array)
{
    if (n->op == N_VAR)
        return &globals[n->value];
    struct cell **ref = &frame->refs[n->value];
    if (*ref) {
        if (as_array || (*ref)->kind == C_ARRAY)
            return *ref;
        *ref = 0;                                   /* (a scalar: by value after all) */
    }
    return &frame->locals[n->value];
}

static char *subscript(struct node *list)
{
    struct text t = { 0 };
    text_add(&t, "", 0);
    const char *subsep = special_str(V_SUBSEP);
    for (struct node *s = list; s; s = s->next) {
        struct value v = eval(s);
        if (s != list)
            text_str(&t, subsep);
        text_str(&t, to_str(&v));
        free_value(&v);
    }
    return t.s;
}

static void nf_changed_check(int index, struct value *v)
{
    if (index == V_NF)
        set_nf((long)to_num(v));
}

/* Where an lvalue is: for a field its number, for an element its array and
 * key, worked out once (a[i++] = i: the subscript before the value, as the
 * One True AWK and gawk). */
struct place {
    struct node *n;
    long field;
    struct array *array;
    char *key;
};

static struct place resolve_place(struct node *n)
{
    struct place p = { n, 0, 0, 0 };
    if (n->op == N_FIELD) {
        struct value i = eval(n->a);
        p.field = (long)to_num(&i);
        free_value(&i);
    } else if (n->op == N_INDEX) {
        p.array = cell_array(cell_of(n->a, true));
        p.key = subscript(n->b);
    }
    return p;
}

/* The value there (a copy). */
static struct value place_get(struct place *p)
{
    struct node *n = p->n;
    if (n->op == N_FIELD)
        return copy_value(get_field(p->field));
    if (n->op == N_INDEX)
        return copy_value(array_get(p->array, p->key));
    struct cell *c = cell_of(n, false);
    if (c->kind == C_ARRAY)
        fatal("an array used as a scalar");
    if (n->op == N_VAR && n->value == V_NF) {
        split_record();
        return num_value(field_count);
    }
    return copy_value(&c->v);
}

static void place_set(struct place *p, struct value v)
{
    struct node *n = p->n;
    if (n->op == N_FIELD) {
        set_field(p->field, v);
    } else if (n->op == N_INDEX) {
        struct value *slot = array_get(p->array, p->key);
        free_value(slot);
        *slot = v;
    } else {
        struct cell *c = cell_of(n, false);
        if (n->op == N_VAR)
            nf_changed_check(n->value, &v);
        set_scalar(c, v);
    }
}

static void place_free(struct place *p)
{
    free(p->key);
    p->key = 0;
}

static struct value lvalue_get(struct node *n)
{
    struct place p = resolve_place(n);
    struct value v = place_get(&p);
    place_free(&p);
    return v;
}

static void lvalue_set(struct node *n, struct value v)
{
    struct place p = resolve_place(n);
    place_set(&p, v);
    place_free(&p);
}

static int compare(struct value *a, struct value *b)
{
    bool numeric = (a->type == T_NUM || a->type == T_STRNUM || a->type == T_UNINIT) &&
                   (b->type == T_NUM || b->type == T_STRNUM || b->type == T_UNINIT);
    if (numeric) {
        double x = to_num(a), y = to_num(b);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    return strcmp(to_str(a), to_str(b));
}

static regex_t *regex_of(struct node *n, struct value *keep)
{
    if (n->op == N_REGEX)
        return n->re;
    *keep = eval(n);
    return dynamic_ere(to_str(keep));
}

static bool matches(struct node *subject_node, struct node *re_node)
{
    struct value subject = eval(subject_node), keep = uninit_value();
    regex_t *re = regex_of(re_node, &keep);
    bool found = regexec(re, to_str(&subject), 0, 0, 0) == 0;
    free_value(&subject);
    free_value(&keep);
    return found;
}

/* printf's formatting into out. */
static void format(struct text *out, struct node *format_node, struct node *args)
{
    struct value fv = eval(format_node);
    const char *f = to_str(&fv);
    struct node *arg = args;
    while (*f) {
        if (*f != '%') {
            text_char(out, *f++);
            continue;
        }
        if (f[1] == '%') {
            text_char(out, '%');
            f += 2;
            continue;
        }
        char spec[64];
        size_t k = 0;
        spec[k++] = *f++;
        while (*f && strchr("-+ #0", *f) && k < 20)
            spec[k++] = *f++;
        for (int part = 0; part < 2; part++) {      /* width, then .precision */
            if (part == 1) {
                if (*f != '.')
                    break;
                spec[k++] = *f++;
            }
            if (*f == '*') {
                f++;
                long w = 0;
                if (arg) {
                    struct value v = eval(arg);
                    w = (long)to_num(&v);
                    free_value(&v);
                    arg = arg->next;
                }
                k += (size_t)snprintf(spec + k, sizeof(spec) - k, "%ld", w);
            } else {
                while (isdigit((unsigned char)*f) && k < 50)
                    spec[k++] = *f++;
            }
        }
        char conv = *f;
        if (!conv) {
            spec[k] = 0;
            text_str(out, spec);
            break;
        }
        f++;
        struct value v = arg ? eval(arg) : uninit_value();
        if (arg)
            arg = arg->next;
        char buf[512], *big = 0;
        int written = 0;
        switch (conv) {
        case 'd':
        case 'i': {
            spec[k++] = 'l';
            spec[k++] = 'l';
            spec[k++] = 'd';
            spec[k] = 0;
            double d = to_num(&v);
            long long x = d >= 9.2e18 ? INT64_MAX : d <= -9.2e18 ? INT64_MIN : (long long)d;
            written = snprintf(buf, sizeof(buf), spec, x);
            break;
        }
        case 'o': case 'x': case 'X': case 'u': {
            spec[k++] = 'l';
            spec[k++] = 'l';
            spec[k++] = conv;
            spec[k] = 0;
            double d = to_num(&v);
            unsigned long long x = d < 0 ? (unsigned long long)(long long)d : (unsigned long long)d;
            written = snprintf(buf, sizeof(buf), spec, x);
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
            spec[k++] = conv;
            spec[k] = 0;
            written = snprintf(buf, sizeof(buf), spec, to_num(&v));
            break;
        case 'c': {
            spec[k++] = 'c';
            spec[k] = 0;
            int ch = 0;
            if (v.type == T_NUM || v.type == T_STRNUM)
                ch = (int)to_num(&v) & 0xFF;
            else
                ch = (unsigned char)to_str(&v)[0];
            if (ch == 0 && v.type != T_NUM) {       /* (an empty string: nothing, padded) */
                spec[k - 1] = 's';
                written = snprintf(buf, sizeof(buf), spec, "");
            } else {
                written = snprintf(buf, sizeof(buf), spec, ch);
            }
            break;
        }
        case 's': {
            spec[k++] = 's';
            spec[k] = 0;
            const char *s = to_str(&v);
            written = snprintf(buf, sizeof(buf), spec, s);
            if (written >= (int)sizeof(buf)) {
                big = must(malloc((size_t)written + 1));
                snprintf(big, (size_t)written + 1, spec, s);
            }
            break;
        }
        default:
            spec[k++] = conv;
            spec[k] = 0;
            text_str(out, spec);                    /* (not a conversion: as written) */
            free_value(&v);
            continue;
        }
        if (big) {
            text_add(out, big, (size_t)written);
            free(big);
        } else if (written > 0) {
            text_add(out, buf, (size_t)written < sizeof(buf) ? (size_t)written : sizeof(buf) - 1);
        }
        free_value(&v);
    }
    free_value(&fv);
}

/* sub and gsub: the count of replacements; target gets the new string. */
static int substitute(struct node *re_node, struct node *repl_node, struct node *target,
                      bool global)
{
    struct value keep = uninit_value();
    regex_t *re = regex_of(re_node, &keep);
    struct value rv = eval(repl_node);
    const char *repl = to_str(&rv);
    struct place where = target ? resolve_place(target) : (struct place){ 0, 0, 0, 0 };
    struct value tv = target ? place_get(&where) : copy_value(get_field(0));
    const char *s = to_str(&tv);
    size_t n = strlen(s), at = 0, last_end = (size_t)-1;
    struct text out = { 0 };
    text_add(&out, "", 0);
    int count = 0;
    regmatch_t m;
    while (at <= n) {
        if (regexec(re, s + at, 1, &m, at ? REG_NOTBOL : 0) != 0)
            break;
        size_t so = at + (size_t)m.rm_so, eo = at + (size_t)m.rm_eo;
        if (eo == so && so == last_end) {           /* (empty, right after a match) */
            if (so >= n)
                break;
            text_char(&out, s[so]);
            at = so + 1;
            continue;
        }
        text_add(&out, s + at, so - at);
        for (const char *r = repl; *r; r++) {
            if (*r == '\\' && (r[1] == '&' || r[1] == '\\')) {
                text_char(&out, *++r);
            } else if (*r == '&') {
                text_add(&out, s + so, eo - so);
            } else {
                text_char(&out, *r);
            }
        }
        count++;
        last_end = eo;
        if (eo == so) {
            if (so < n)
                text_char(&out, s[so]);
            at = so + 1;
        } else {
            at = eo;
        }
        if (!global)
            break;
    }
    if (count) {
        if (at < n)
            text_add(&out, s + at, n - at);
        struct value result = str_value_owned(out.s);
        if (target)
            place_set(&where, result);
        else
            set_field(0, result);
    } else {
        free(out.s);
    }
    place_free(&where);
    free_value(&tv);
    free_value(&rv);
    free_value(&keep);
    return count;
}

static void add_split(const char *s, size_t n, void *ctx)
{
    struct array *a = ctx;
    char key[32];
    snprintf(key, sizeof(key), "%zu", a->count + 1);
    struct value *slot = array_get(a, key);
    free_value(slot);
    *slot = input_value(s, n);
}

static unsigned random_seed;
static double previous_seed;

static struct value builtin(struct node *n)
{
    struct node *a1 = n->a, *a2 = a1 ? a1->next : 0, *a3 = a2 ? a2->next : 0;
    struct value v, w;
    switch (n->value) {
    case B_LENGTH: {
        if (a1 && (a1->op == N_VAR || a1->op == N_LOCAL)) {
            struct cell *c = cell_of(a1, false);
            if (c->kind == C_ARRAY)
                return num_value((double)c->a->count);
        }
        v = a1 ? eval(a1) : copy_value(get_field(0));
        double len = (double)strlen(to_str(&v));
        free_value(&v);
        return num_value(len);
    }
    case B_SUBSTR: {
        v = eval(a1);
        const char *s = to_str(&v);
        double len = (double)strlen(s);
        w = eval(a2);
        double m = to_num(&w);
        free_value(&w);
        double from = floor(m + 0.5), count = INFINITY;
        if (a3) {
            w = eval(a3);
            count = floor(to_num(&w) + 0.5);
            free_value(&w);
        }
        /* POSIX: the characters from position from (1 first), at most count. */
        double end = isinf(count) ? len + 1 : from + count;
        if (from < 1)
            from = 1;
        if (end > len + 1)
            end = len + 1;
        struct value r = end > from ? str_value_owned(copy_n(s + (size_t)from - 1,
                                                             (size_t)(end - from)))
                                    : str_value("");
        free_value(&v);
        return r;
    }
    case B_INDEX: {
        v = eval(a1);
        w = eval(a2);
        const char *hit = strstr(to_str(&v), to_str(&w));
        double r = hit && v.str[0] ? (double)(hit - v.str + 1) : 0;   /* ("" is in no "") */
        free_value(&v);
        free_value(&w);
        return num_value(r);
    }
    case B_SPLIT: {
        if (!a2 || (a2->op != N_VAR && a2->op != N_LOCAL))
            fatal("split needs an array");
        v = eval(a1);
        struct array *arr = cell_array(cell_of(a2, true));
        array_clear(arr);
        struct value fsv = uninit_value();
        const char *fs;
        char literal[2] = { 0, 0 };
        if (!a3) {
            fs = special_str(V_FS);
        } else if (a3->op == N_REGEX) {
            fs = a3->str;                           /* (an ERE: / / is one blank, not FS's " ") */
            if (!strcmp(fs, " "))
                fs = "[ ]";
            else if (strlen(fs) == 1)
                literal[0] = fs[0], fs = literal;
        } else {
            fsv = eval(a3);
            fs = to_str(&fsv);
        }
        split_by(to_str(&v), fs, false, add_split, arr);
        double count = (double)arr->count;
        free_value(&v);
        free_value(&fsv);
        return num_value(count);
    }
    case B_SUB:
    case B_GSUB:
        if (!a1 || !a2)
            fatal("sub and gsub need a pattern and a replacement");
        if (a3 && !is_lvalue(a3))
            fatal("sub's or gsub's target must be a variable");
        return num_value(substitute(a1, a2, a3, n->value == B_GSUB));
    case B_MATCH: {
        v = eval(a1);
        struct value keep = uninit_value();
        regex_t *re = regex_of(a2, &keep);
        regmatch_t m;
        double start = 0, length = -1;
        if (regexec(re, to_str(&v), 1, &m, 0) == 0) {
            start = (double)m.rm_so + 1;
            length = (double)(m.rm_eo - m.rm_so);
        }
        set_scalar(&globals[V_RSTART], num_value(start));
        set_scalar(&globals[V_RLENGTH], num_value(length));
        free_value(&v);
        free_value(&keep);
        return num_value(start);
    }
    case B_SPRINTF: {
        if (!a1)
            return str_value("");
        struct text t = { 0 };
        text_add(&t, "", 0);
        format(&t, a1, a2);
        return str_value_owned(t.s);
    }
    case B_SIN: case B_COS: case B_EXP: case B_LOG: case B_SQRT: case B_INT: {
        v = a1 ? eval(a1) : copy_value(get_field(0));
        double x = to_num(&v), r;
        free_value(&v);
        switch (n->value) {
        case B_SIN: r = sin(x); break;
        case B_COS: r = cos(x); break;
        case B_EXP: r = exp(x); break;
        case B_LOG: r = log(x); break;
        case B_SQRT: r = sqrt(x); break;
        default: r = trunc(x);
        }
        return num_value(r);
    }
    case B_ATAN2: {
        v = eval(a1);
        w = eval(a2);
        double r = atan2(to_num(&v), to_num(&w));
        free_value(&v);
        free_value(&w);
        return num_value(r);
    }
    case B_RAND:
        random_seed = random_seed * 1103515245u + 12345u;
        return num_value((double)(random_seed >> 1) / 2147483648.0);
    case B_SRAND: {
        double old = previous_seed;
        if (a1) {
            v = eval(a1);
            previous_seed = to_num(&v);
            free_value(&v);
        } else {
            previous_seed = (double)time(0);
        }
        random_seed = (unsigned)(long long)previous_seed;
        return num_value(old);
    }
    case B_TOLOWER:
    case B_TOUPPER: {
        v = a1 ? eval(a1) : copy_value(get_field(0));
        char *s = copy(to_str(&v));
        for (char *c = s; *c; c++)
            *c = (char)(n->value == B_TOLOWER ? tolower((unsigned char)*c) : toupper((unsigned char)*c));
        free_value(&v);
        return str_value_owned(s);
    }
    case B_SYSTEM: {
        v = eval(a1);
        flush_all();
        int status = system(to_str(&v));
        free_value(&v);
        return num_value(status < 0 ? -1 : WIFEXITED(status) ? WEXITSTATUS(status)
                                                             : 256 + WTERMSIG(status));
    }
    case B_CLOSE: {
        v = eval(a1);
        int r = close_stream(to_str(&v));
        free_value(&v);
        return num_value(r);
    }
    case B_FFLUSH: {
        if (!a1) {
            flush_all();
            return num_value(0);
        }
        v = eval(a1);
        struct stream *s = find_stream(to_str(&v), true);
        free_value(&v);
        if (s)
            fflush(s->f);
        return num_value(s ? 0 : -1);
    }
    }
    return uninit_value();
}

static struct value call(struct node *n)
{
    struct function *f = &functions[n->value];
    int argc = 0;
    for (struct node *a = n->a; a; a = a->next)
        argc++;
    if (argc > f->param_count)
        fatal("function %s called with %d arguments, it takes %d", f->name, argc, f->param_count);
    if (depth > 10000)
        fatal("function calls nested too deep");
    struct frame fr;
    fr.count = f->param_count;
    fr.locals = must(calloc((size_t)fr.count + 1, sizeof(struct cell)));
    fr.refs = must(calloc((size_t)fr.count + 1, sizeof(struct cell *)));
    int i = 0;
    for (struct node *a = n->a; a; a = a->next, i++) {
        if (a->op == N_VAR || a->op == N_LOCAL) {
            struct cell *c = cell_of(a, true);
            if (c->kind == C_ARRAY || c->kind == C_UNTYPED) {
                fr.refs[i] = c;                     /* arrays by reference */
                continue;
            }
        }
        struct value v = eval(a);
        fr.locals[i].kind = C_SCALAR;
        fr.locals[i].v = v;
    }
    struct frame *saved = frame;
    frame = &fr;
    depth++;
    return_value = uninit_value();
    int r = run(f->body);
    depth--;
    frame = saved;
    struct value result = r == R_RETURN ? return_value : uninit_value();
    if (r != R_RETURN)
        free_value(&return_value);
    return_value = uninit_value();
    for (int k = 0; k < fr.count; k++) {
        if (fr.refs[k])
            continue;
        free_value(&fr.locals[k].v);
        if (fr.locals[k].kind == C_ARRAY) {
            array_clear(fr.locals[k].a);
            free(fr.locals[k].a->buckets);
            free(fr.locals[k].a);
        }
    }
    free(fr.locals);
    free(fr.refs);
    if (r == R_EXIT) {
        /* exit inside a function: carried out by the caller (see the exit flag) */
    }
    return result;
}

static bool exiting;

static struct value getline_value(struct node *n)
{
    struct text line = { 0 };
    int result;
    if (n->value == G_SIMPLE) {
        result = next_main_record(&line) ? 1 : 0;
    } else {
        struct value src_value = eval(n->b);
        const char *name = to_str(&src_value);
        struct stream *s = find_stream(name, false);
        if (!s) {
            FILE *f;
            if (n->value == G_COMMAND) {
                flush_all();
                f = popen(name, "r");
            } else {
                f = !strcmp(name, "-") || !strcmp(name, "/dev/stdin") ? stdin : fopen(name, "r");
            }
            s = f ? new_stream(name, f, n->value == G_COMMAND ? 'p' : 'r') : 0;
        }
        free_value(&src_value);
        if (!s) {
            free(line.s);
            return num_value(-1);
        }
        result = read_record(s->f, &line) ? 1 : 0;
        if (result && n->value == G_COMMAND)
            bump(V_NR);
    }
    if (result > 0) {
        if (n->a) {
            lvalue_set(n->a, input_value(line.s ? line.s : "", line.n));
        } else {
            set_record(line.s ? line.s : "", line.n);
            if (n->value == G_FILE) {
                /* getline < file: $0 and NF only */
            }
        }
    }
    free(line.s);
    return num_value(result);
}

static double arithmetic(int op, double x, double y)
{
    switch (op) {
    case N_ADD: return x + y;
    case N_SUB: return x - y;
    case N_MUL: return x * y;
    case N_DIV:
        if (y == 0)
            fatal("division by zero");
        return x / y;
    case N_MOD:
        if (y == 0)
            fatal("division by zero in %%");
        return fmod(x, y);
    case N_POW: return pow(x, y);
    }
    return 0;
}

static struct value eval(struct node *n)
{
    source_line = n->line;
    struct value a, b;
    switch (n->op) {
    case N_NUM:
        return num_value(n->num);
    case N_STR:
        return str_value(n->str);
    case N_REGEX:
        return num_value(regexec(n->re, to_str(get_field(0)), 0, 0, 0) == 0);
    case N_GROUP:
        return eval(n->a);                          /* (a, b) alone: the first */
    case N_VAR:
    case N_LOCAL:
    case N_FIELD:
    case N_INDEX:
        return lvalue_get(n);
    case N_ASSIGN: {
        struct place p = resolve_place(n->a);
        struct value v = eval(n->b);
        if (n->value) {
            struct value old = place_get(&p);
            double r = arithmetic(n->value, to_num(&old), to_num(&v));
            free_value(&old);
            free_value(&v);
            v = num_value(r);
        } else if (v.type == T_UNINIT) {
            v = uninit_value();
        }
        place_set(&p, copy_value(&v));
        place_free(&p);
        return v;
    }
    case N_COND:
        a = eval(n->a);
        {
            bool t = truth(&a);
            free_value(&a);
            return eval(t ? n->b : n->c);
        }
    case N_OR:
    case N_AND: {
        a = eval(n->a);
        bool t = truth(&a);
        free_value(&a);
        if (n->op == N_OR && t)
            return num_value(1);
        if (n->op == N_AND && !t)
            return num_value(0);
        b = eval(n->b);
        t = truth(&b);
        free_value(&b);
        return num_value(t);
    }
    case N_IN: {
        struct array *arr = cell_array(cell_of(n->b, true));
        char *key = subscript(n->a);
        bool found = array_find(arr, key) != 0;
        free(key);
        return num_value(found);
    }
    case N_MATCH:
    case N_NOMATCH: {
        bool m = matches(n->a, n->b);
        return num_value(n->op == N_MATCH ? m : !m);
    }
    case N_LT: case N_LE: case N_NE: case N_EQ: case N_GT: case N_GE: {
        a = eval(n->a);
        b = eval(n->b);
        int c = compare(&a, &b);
        free_value(&a);
        free_value(&b);
        bool r = n->op == N_LT ? c < 0 : n->op == N_LE ? c <= 0 : n->op == N_NE ? c != 0
               : n->op == N_EQ ? c == 0 : n->op == N_GT ? c > 0 : c >= 0;
        return num_value(r);
    }
    case N_CONCAT: {
        a = eval(n->a);
        b = eval(n->b);
        const char *x = to_str(&a), *y = to_str(&b);
        size_t lx = strlen(x), ly = strlen(y);
        char *s = must(malloc(lx + ly + 1));
        memcpy(s, x, lx);
        memcpy(s + lx, y, ly + 1);
        free_value(&a);
        free_value(&b);
        return str_value_owned(s);
    }
    case N_ADD: case N_SUB: case N_MUL: case N_DIV: case N_MOD: case N_POW: {
        a = eval(n->a);
        b = eval(n->b);
        double r = arithmetic(n->op, to_num(&a), to_num(&b));
        free_value(&a);
        free_value(&b);
        return num_value(r);
    }
    case N_NEG:
    case N_PLUS:
    case N_NOT: {
        a = eval(n->a);
        struct value r = n->op == N_NOT ? num_value(!truth(&a))
                       : num_value(n->op == N_NEG ? -to_num(&a) : to_num(&a));
        free_value(&a);
        return r;
    }
    case N_PREINC: case N_PREDEC: case N_POSTINC: case N_POSTDEC: {
        struct place p = resolve_place(n->a);
        a = place_get(&p);
        double old = to_num(&a);
        free_value(&a);
        double now = n->op == N_PREINC || n->op == N_POSTINC ? old + 1 : old - 1;
        place_set(&p, num_value(now));
        place_free(&p);
        return num_value(n->op == N_PREINC || n->op == N_PREDEC ? now : old);
    }
    case N_CALL:
        return call(n);
    case N_BUILTIN:
        return builtin(n);
    case N_GETLINE:
        return getline_value(n);
    }
    fatal("an expression of unknown kind");
}

static void print_items(FILE *out, struct node *args)
{
    const char *ofs = special_str(V_OFS);
    if (!args) {
        fputs(to_str(get_field(0)), out);
    }
    for (struct node *a = args; a; a = a->next) {
        struct value v = eval(a);
        char buf[64];
        if (a != args)
            fputs(ofs, out);
        fputs(output_str(&v, buf, sizeof(buf)), out);
        free_value(&v);
    }
    fputs(special_str(V_ORS), out);
}

static FILE *destination(struct node *n)
{
    if (n->value == R_NONE)
        return stdout;
    struct value v = eval(n->b);
    char *name = copy(to_str(&v));
    free_value(&v);
    FILE *f = output_stream(n->value, name);
    free(name);
    return f;
}

static int run(struct node *n)
{
    for (; n; n = n->next) {
        source_line = n->line;
        int r = R_NORMAL;
        switch (n->op) {
        case S_EXPR: {
            struct value v = eval(n->a);
            free_value(&v);
            break;
        }
        case S_PRINT: {
            FILE *out = destination(n);
            print_items(out, n->a);
            break;
        }
        case S_PRINTF: {
            FILE *out = destination(n);
            struct text t = { 0 };
            text_add(&t, "", 0);
            format(&t, n->a, n->a->next);
            fwrite(t.s, 1, t.n, out);
            free(t.s);
            break;
        }
        case S_IF: {
            struct value v = eval(n->a);
            bool t = truth(&v);
            free_value(&v);
            r = t ? run(n->b) : n->c ? run(n->c) : R_NORMAL;
            break;
        }
        case S_WHILE:
            for (;;) {
                struct value v = eval(n->a);
                bool t = truth(&v);
                free_value(&v);
                if (!t)
                    break;
                r = run(n->b);
                if (r == R_BREAK) {
                    r = R_NORMAL;
                    break;
                }
                if (r == R_CONTINUE)
                    r = R_NORMAL;
                if (r != R_NORMAL)
                    break;
            }
            break;
        case S_DO:
            for (;;) {
                r = run(n->a);
                if (r == R_BREAK) {
                    r = R_NORMAL;
                    break;
                }
                if (r == R_CONTINUE)
                    r = R_NORMAL;
                if (r != R_NORMAL)
                    break;
                struct value v = eval(n->b);
                bool t = truth(&v);
                free_value(&v);
                if (!t)
                    break;
            }
            break;
        case S_FOR:
            if (n->a)
                run(n->a);
            for (;;) {
                if (n->b) {
                    struct value v = eval(n->b);
                    bool t = truth(&v);
                    free_value(&v);
                    if (!t)
                        break;
                }
                r = run(n->d);
                if (r == R_BREAK) {
                    r = R_NORMAL;
                    break;
                }
                if (r == R_CONTINUE)
                    r = R_NORMAL;
                if (r != R_NORMAL)
                    break;
                if (n->c)
                    run(n->c);
            }
            break;
        case S_FORIN: {
            struct array *arr = cell_array(cell_of(n->b, true));
            size_t count;
            char **keys = array_keys(arr, &count);
            bool stop = false;
            for (size_t k = 0; k < count; k++) {
                if (!stop && array_find(arr, keys[k])) {    /* (not deleted meanwhile) */
                    lvalue_set(n->a, str_value(keys[k]));
                    r = run(n->c);
                    if (r == R_CONTINUE)
                        r = R_NORMAL;
                    stop = r != R_NORMAL;
                }
                free(keys[k]);
            }
            free(keys);
            if (r == R_BREAK)
                r = R_NORMAL;
            break;
        }
        case S_BLOCK:
            r = run(n->a);
            break;
        case S_NEXT:
            return R_NEXT;
        case S_NEXTFILE:
            return R_NEXTFILE;
        case S_EXIT:
            if (n->a) {
                struct value v = eval(n->a);
                exit_code = (int)to_num(&v);
                free_value(&v);
            }
            exiting = true;
            return R_EXIT;
        case S_RETURN:
            free_value(&return_value);
            return_value = n->a ? eval(n->a) : uninit_value();
            return R_RETURN;
        case S_BREAK:
            return R_BREAK;
        case S_CONTINUE:
            return R_CONTINUE;
        case S_DELETE: {
            struct array *arr = cell_array(cell_of(n->a, true));
            if (n->b) {
                char *key = subscript(n->b);
                array_delete(arr, key);
                free(key);
            } else {
                array_clear(arr);
            }
            break;
        }
        }
        if (exiting)
            return R_EXIT;
        if (r != R_NORMAL)
            return r;
    }
    return R_NORMAL;
}

/* --- the start ---------------------------------------------------------------------- */

static char *unescape(const char *s)
{
    struct text t = { 0 };
    text_add(&t, "", 0);
    while (*s) {
        if (*s == '\\' && s[1]) {
            s++;
            string_escape(&s, &t, false);
            continue;
        }
        text_char(&t, *s++);
    }
    return t.s;
}

static void assign_argument(const char *arg)
{
    const char *eq = strchr(arg, '=');
    char *name = copy_n(arg, (size_t)(eq - arg));
    char *value = unescape(eq + 1);
    int i = global_index(name);
    free(name);
    struct value v = input_value(value, strlen(value));
    free(value);
    if (i == V_NF)
        set_nf((long)to_num(&v));
    set_scalar(&globals[i], v);
}

static void set_special(int i, const char *value)
{
    set_scalar(&globals[i], str_value(value));
}

int main(int argc, char **argv)
{
    static const char *const specials[V_SPECIALS] = { "NF", "NR", "FNR", "FS", "OFS", "ORS",
        "RS", "FILENAME", "SUBSEP", "RSTART", "RLENGTH", "CONVFMT", "OFMT", "ENVIRON", "ARGC",
        "ARGV" };
    for (int i = 0; i < V_SPECIALS; i++)
        global_index(specials[i]);
    set_scalar(&globals[V_NR], num_value(0));
    set_scalar(&globals[V_FNR], num_value(0));
    set_special(V_FS, " ");
    set_special(V_OFS, " ");
    set_special(V_ORS, "\n");
    set_special(V_RS, "\n");
    set_special(V_SUBSEP, "\034");
    set_special(V_CONVFMT, "%.6g");
    set_special(V_OFMT, "%.6g");
    set_scalar(&globals[V_RSTART], num_value(0));
    set_scalar(&globals[V_RLENGTH], num_value(-1));
    struct array *env = cell_array(&globals[V_ENVIRON]);
    for (char **e = environ; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq)
            continue;
        char *key = copy_n(*e, (size_t)(eq - *e));
        struct value *slot = array_get(env, key);
        *slot = input_value(eq + 1, strlen(eq + 1));
        free(key);
    }

    struct text program = { 0 };
    bool have_program = false;
    char **assignments = must(calloc((size_t)argc + 1, sizeof(char *)));
    int assignment_count = 0;
    const char *field_separator = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) {
            i++;
            break;
        }
        if (a[0] != '-' || !a[1])
            break;
        const char *arg = a[2] ? a + 2 : 0;
        if (a[1] == 'F' || a[1] == 'v' || a[1] == 'f') {
            if (!arg) {
                if (i + 1 >= argc)
                    fatal("-%c needs an argument", a[1]);
                arg = argv[++i];
            }
            if (a[1] == 'F') {
                field_separator = arg;
            } else if (a[1] == 'v') {
                if (!strchr(arg, '='))
                    fatal("-v needs NAME=VALUE");
                assignments[assignment_count++] = (char *)arg;
            } else {
                FILE *f = !strcmp(arg, "-") ? stdin : fopen(arg, "r");
                if (!f)
                    fatal("can't open file %s", arg);
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                    text_add(&program, buf, n);
                text_char(&program, '\n');
                if (f != stdin)
                    fclose(f);
                have_program = true;
            }
            continue;
        }
        fatal("unknown option %s", a);
    }
    if (!have_program) {
        if (i >= argc) {
            dprintf(STDERR_FILENO, "usage: awk [-F FS] [-v NAME=VALUE] 'PROGRAM' [FILE...]\n");
            return 2;
        }
        text_str(&program, argv[i++]);
    }
    if (field_separator) {
        char *fs = !strcmp(field_separator, "t") ? copy("\t") : unescape(field_separator);
        set_scalar(&globals[V_FS], str_value_owned(fs));
    }
    for (int k = 0; k < assignment_count; k++)
        assign_argument(assignments[k]);
    struct array *av = cell_array(&globals[V_ARGV]);
    *array_get(av, "0") = str_value("awk");
    int count = 1;
    for (int k = i; k < argc; k++, count++) {
        char key[32];
        snprintf(key, sizeof(key), "%d", count);
        *array_get(av, key) = input_value(argv[k], strlen(argv[k]));
    }
    set_scalar(&globals[V_ARGC], num_value(count));

    parse_program(program.s ? program.s : "");
    source_line = 0;
    set_record("", 0);
    setvbuf(stdout, 0, _IOFBF, BUFSIZ);

    int r = run(begin_actions);
    bool skip_input = exiting || (!item_count && !end_actions);
    struct text line = { 0 };
    while (!skip_input && !exiting && next_main_record(&line)) {
        set_record(line.s, line.n);
        for (int k = 0; k < item_count; k++) {
            struct item *it = &items[k];
            bool selected;
            if (!it->pattern) {
                selected = true;
            } else if (!it->pattern2) {
                struct value v = eval(it->pattern);
                selected = truth(&v);
                free_value(&v);
            } else if (it->in_range) {
                selected = true;
                struct value v = eval(it->pattern2);
                if (truth(&v))
                    it->in_range = false;
                free_value(&v);
            } else {
                struct value v = eval(it->pattern);
                selected = truth(&v);
                free_value(&v);
                if (selected) {
                    struct value e = eval(it->pattern2);
                    it->in_range = !truth(&e);
                    free_value(&e);
                }
            }
            if (!selected)
                continue;
            if (!it->action) {
                print_items(stdout, 0);
                continue;
            }
            r = run(it->action);
            if (r == R_NEXT || r == R_EXIT)
                break;
            if (r == R_NEXTFILE) {
                close_main();
                break;
            }
        }
    }
    free(line.s);
    exiting = false;                                /* (END runs after an exit too) */
    run(end_actions);
    flush_all();
    close_all();
    fflush(stdout);
    return exit_code;
}

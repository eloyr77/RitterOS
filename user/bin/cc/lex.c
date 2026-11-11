/*
 * Source files and tokens (60a). A file is read whole; lines ending in a
 * backslash are joined first (translation phase 2), and the newlines they
 * lose are put back after the joined line, so every later line keeps its
 * number. The tokenizer then makes preprocessing tokens (phase 3): comments
 * become a space, digraphs their usual spelling. `convert_pp_tokens` turns
 * them into the parser's tokens after the preprocessor: keywords, numbers
 * with their types, character constants, and string literals with their
 * bytes (adjacent ones joined, phase 6).
 */
#include "cc.h"
#include "type.h"

int error_count;
static int sources;

/* --- memory -------------------------------------------------------------- */

void *arena_alloc(size_t n)
{
    static char *at, *end;
    n = (n + 15) & ~(size_t)15;
    if (n > 65536) {
        void *p = calloc(1, n);
        if (!p)
            fatal("out of memory");
        return p;
    }
    if ((size_t)(end - at) < n) {
        at = calloc(1, 1 << 20);
        if (!at)
            fatal("out of memory");
        end = at + (1 << 20);
    }
    void *p = at;
    at += n;
    return p;
}

char *arena_strndup(const char *s, size_t n)
{
    char *p = arena_alloc(n + 1);
    memcpy(p, s, n);
    return p;
}

char *format(const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(0, 0, fmt, ap);
    va_end(ap);
    char *p = arena_alloc((size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return p;
}

void buf_add(struct buffer *b, const void *data, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->len + n + 1)
            cap *= 2;
        char *d = realloc(b->data, cap);
        if (!d)
            fatal("out of memory");
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, n);
    b->len += n;
    b->data[b->len] = 0;
}

void buf_byte(struct buffer *b, int c)
{
    char ch = (char)c;
    buf_add(b, &ch, 1);
}

void buf_printf(struct buffer *b, const char *fmt, ...)
{
    char small[256];
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(small, sizeof(small), fmt, ap);
    va_end(ap);
    if (n < (int)sizeof(small))
        buf_add(b, small, (size_t)n);
    else {
        char *big = malloc((size_t)n + 1);
        if (!big)
            fatal("out of memory");
        vsnprintf(big, (size_t)n + 1, fmt, ap2);
        buf_add(b, big, (size_t)n);
        free(big);
    }
    va_end(ap2);
}

/* --- messages ------------------------------------------------------------ */

static void message(struct token *t, const char *kind, const char *fmt, va_list ap)
{
    if (t && t->src) {
        /* Inside a macro's expansion the place is the macro's use. */
        while (t->origin)
            t = t->origin;
        fprintf(stderr, "%s:%d:%d: %s: ", t->src->display_name, t->line + t->line_delta, t->col, kind);
    } else
        fprintf(stderr, "cc: %s: ", kind);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void error_at(struct token *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    message(t, "error", fmt, ap);
    va_end(ap);
    exit(1);
}

void warn_at(struct token *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    message(t, "warning", fmt, ap);
    va_end(ap);
}

void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "cc: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/* --- files --------------------------------------------------------------- */

/* Joins lines ending in a backslash; CR LF becomes LF; the text ends in a
 * newline. The newlines taken out come back after the joined line. */
static char *splice(const char *in, size_t n)
{
    char *out = arena_alloc(n + 2);
    size_t o = 0;
    int pending = 0;
    for (size_t i = 0; i < n;) {
        if (in[i] == '\\' && i + 1 < n && in[i + 1] == '\n') {
            i += 2, pending++;
            continue;
        }
        if (in[i] == '\\' && i + 2 < n && in[i + 1] == '\r' && in[i + 2] == '\n') {
            i += 3, pending++;
            continue;
        }
        if (in[i] == '\r' && i + 1 < n && in[i + 1] == '\n') {
            i++;
            continue;
        }
        if (in[i] == '\n') {
            /* (the output grows by at most the input's size: each pending
             * newline stands for a backslash and newline taken out) */
            out[o++] = '\n';
            for (; pending; pending--)
                out[o++] = '\n';
            i++;
            continue;
        }
        out[o++] = in[i++];
    }
    if (o == 0 || out[o - 1] != '\n')
        out[o++] = '\n';
    for (; pending && o < n + 1; pending--)
        out[o++] = '\n';
    out[o] = 0;
    return out;
}

struct source *source_from_text(const char *name, char *text)
{
    struct source *s = arena_alloc(sizeof(*s));
    s->name = s->display_name = arena_strndup(name, strlen(name));
    s->text = splice(text, strlen(text));
    s->number = ++sources;
    return s;
}

struct source *source_read(const char *path)
{
    FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
    if (!f)
        return 0;
    struct buffer b = { 0 };
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        buf_add(&b, chunk, n);
    if (f != stdin)
        fclose(f);
    struct source *s = arena_alloc(sizeof(*s));
    s->name = s->display_name = arena_strndup(path, strlen(path));
    s->text = splice(b.data ? b.data : "", b.len);
    s->number = ++sources;
    free(b.data);
    return s;
}

/* --- tokens -------------------------------------------------------------- */

struct token *token_copy(struct token *t)
{
    struct token *c = arena_alloc(sizeof(*c));
    *c = *t;
    c->next = 0;
    return c;
}

bool token_is(struct token *t, const char *s)
{
    return (int)strlen(s) == t->len && memcmp(t->text, s, (size_t)t->len) == 0;
}

char *token_text(struct token *t)
{
    return arena_strndup(t->text, (size_t)t->len);
}

bool lex_assembly;

static bool ident_start(unsigned char c)
{
    /* (in a .S file $ begins an immediate: "$USER_DATA" is two tokens, as clang's) */
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c == '$' && !lex_assembly) || c >= 0x80;
}

static bool ident_char(unsigned char c)
{
    return ident_start(c) || (c >= '0' && c <= '9');
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* Longest first: three-character punctuators before two-character ones. */
static const char *punctuators[] = {
    "<<=", ">>=", "...", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
    "*=", "/=", "%=", "+=", "-=", "&=", "^=", "|=", "##",
};

/* Digraphs and their spellings (C11 6.4.6p3). */
static const char *digraphs[][2] = {
    { "%:%:", "##" }, { "<:", "[" }, { ":>", "]" }, { "<%", "{" }, { "%>", "}" }, { "%:", "#" },
};

static struct source *current;
static const char *line_start;
static int line;

static struct token *new_token(enum token_kind kind, const char *start, const char *end)
{
    struct token *t = arena_alloc(sizeof(*t));
    t->kind = kind;
    t->text = start;
    t->len = (int)(end - start);
    t->src = current;
    t->line = line;
    t->col = (int)(start - line_start) + 1;
    return t;
}

/* The end of a string or character literal starting at the quote p. */
static const char *literal_end(const char *p, char quote, struct token *where)
{
    for (p++; *p != quote; p++) {
        if (*p == '\n' || *p == 0) {
            if (where)
                error_at(where, "missing terminating %c character", quote);
            return 0;
        }
        if (*p == '\\')
            p++;
    }
    return p + 1;
}

struct token *tokenize(struct source *src)
{
    current = src;
    line = 1;
    const char *p = src->text;
    line_start = p;
    struct token head = { 0 }, *cur = &head;
    bool at_start = true, space = false;
    while (*p) {
        if (*p == '\n') {
            p++;
            line++;
            line_start = p;
            at_start = true;
            space = false;
            continue;
        }
        if (*p == ' ' || *p == '\t' || *p == '\f' || *p == '\v' || *p == '\r') {
            p++;
            space = true;
            continue;
        }
        if (p[0] == '/' && p[1] == '/') {
            while (*p != '\n')
                p++;
            space = true;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            const char *start = p;
            int start_line = line;
            for (p += 2; !(p[0] == '*' && p[1] == '/'); p++) {
                if (*p == 0) {
                    line = start_line;
                    error_at(new_token(TK_OTHER, start, start + 2), "unterminated comment");
                }
                if (*p == '\n') {
                    line++;
                    line_start = p + 1;
                }
            }
            p += 2;
            space = true;
            continue;
        }
        struct token *t;
        const char *q = p;
        if (is_digit(*p) || (*p == '.' && is_digit(p[1]))) {
            /* A preprocessing number (6.4.8): digits, letters, '.', '_',
             * and a sign after e, E, p or P. */
            for (q++;;) {
                if (q[1] && (q[0] == 'e' || q[0] == 'E' || q[0] == 'p' || q[0] == 'P') &&
                    (q[1] == '+' || q[1] == '-'))
                    q += 2;
                else if (ident_char((unsigned char)*q) || *q == '.')
                    q++;
                else
                    break;
            }
            t = new_token(TK_PPNUM, p, q);
        } else if (*p == '"' || *p == '\'' ||
                   ((*p == 'L' || *p == 'U') && (p[1] == '"' || p[1] == '\'')) ||
                   (*p == 'u' && (p[1] == '"' || p[1] == '\'')) ||
                   (p[0] == 'u' && p[1] == '8' && (p[2] == '"' || p[2] == '\''))) {
            const char *quote = p;
            while (*quote != '"' && *quote != '\'')
                quote++;
            struct token *where = new_token(TK_OTHER, p, quote + 1);
            const char *end = literal_end(quote, *quote, 0);
            if (!end) {
                /* An unmatched quote is a stray character: allowed in a
                 * skipped group, and an error if the parser ever sees it. */
                t = where;
                q = quote + 1;
            } else {
                t = new_token(*quote == '"' ? TK_STR : TK_CHAR, p, end);
                q = end;
            }
        } else if (ident_start((unsigned char)*p)) {
            for (q++; ident_char((unsigned char)*q); q++)
                ;
            t = new_token(TK_IDENT, p, q);
        } else {
            t = 0;
            for (size_t i = 0; i < sizeof(digraphs) / sizeof(digraphs[0]) && !t; i++) {
                size_t n = strlen(digraphs[i][0]);
                if (strncmp(p, digraphs[i][0], n) == 0) {
                    t = new_token(TK_PUNCT, p, p + n);
                    t->text = digraphs[i][1];
                    t->len = (int)strlen(digraphs[i][1]);
                    q = p + n;
                }
            }
            for (size_t i = 0; i < sizeof(punctuators) / sizeof(punctuators[0]) && !t; i++) {
                size_t n = strlen(punctuators[i]);
                if (strncmp(p, punctuators[i], n) == 0) {
                    t = new_token(TK_PUNCT, p, p + n);
                    q = p + n;
                }
            }
            if (!t) {
                q = p + 1;
                t = new_token(strchr("[](){}.&*+-~!/%<>^|?:;=,#", *p) ? TK_PUNCT : TK_OTHER, p, q);
            }
        }
        t->at_line_start = at_start;
        t->space_before = space;
        at_start = space = false;
        cur = cur->next = t;
        p = q;
    }
    struct token *eof = new_token(TK_EOF, p, p);
    eof->at_line_start = true;
    cur->next = eof;
    return head.next;
}

struct token *tokenize_text(const char *name, const char *text)
{
    char *copy = arena_strndup(text, strlen(text));
    return tokenize(source_from_text(name, copy));
}

/* --- conversion for the parser ------------------------------------------- */

static const char *keywords[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else",
    "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long", "register",
    "restrict", "return", "short", "signed", "sizeof", "static", "struct", "switch",
    "typedef", "union", "unsigned", "void", "volatile", "while", "_Alignas", "_Alignof",
    "_Atomic", "_Bool", "_Complex", "_Generic", "_Imaginary", "_Noreturn", "_Static_assert",
    "_Thread_local",
    /* GNU spellings found in headers and programs */
    "__attribute__", "__attribute", "__asm__", "__asm", "asm", "__inline", "__inline__",
    "__restrict", "__restrict__", "__const", "__const__", "__volatile", "__volatile__",
    "__signed", "__signed__", "__thread", "__extension__", "__typeof__", "__typeof", "typeof",
    "__alignof__", "__alignof", "__int128", "__builtin_va_list", "__builtin_va_arg",
    "__builtin_offsetof", "__builtin_types_compatible_p", "__label__",
};

static bool is_keyword(struct token *t)
{
    static struct entry {
        const char *name;
        int len;
    } table[256];
    static bool made;
    if (!made) {
        /* A small open hash: few keywords, and identifiers are many. */
        for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
            unsigned h = 0;
            for (const char *c = keywords[i]; *c; c++)
                h = h * 31 + (unsigned char)*c;
            while (table[h & 255].name)
                h++;
            table[h & 255].name = keywords[i];
            table[h & 255].len = (int)strlen(keywords[i]);
        }
        made = true;
    }
    unsigned h = 0;
    for (int i = 0; i < t->len; i++)
        h = h * 31 + (unsigned char)t->text[i];
    for (; table[h & 255].name; h++)
        if (table[h & 255].len == t->len && memcmp(table[h & 255].name, t->text, (size_t)t->len) == 0)
            return true;
    return false;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* One character of a literal at *pp, an escape sequence included: its
 * value (a code point for \u and \U). */
static uint32_t read_char(struct token *t, const char **pp, bool *universal)
{
    const char *p = *pp;
    *universal = false;
    if (*p != '\\') {
        *pp = p + 1;
        return (unsigned char)*p;
    }
    p++;
    uint32_t c;
    if (*p >= '0' && *p <= '7') {
        c = (uint32_t)(*p++ - '0');
        for (int i = 0; i < 2 && *p >= '0' && *p <= '7'; i++)
            c = c * 8 + (uint32_t)(*p++ - '0');
    } else if (*p == 'x') {
        p++;
        if (hex_value(*p) < 0)
            error_at(t, "\\x used with no following hex digits");
        c = 0;
        for (; hex_value(*p) >= 0; p++)
            c = c * 16 + (uint32_t)hex_value(*p);
    } else if (*p == 'u' || *p == 'U') {
        int n = *p == 'u' ? 4 : 8;
        p++;
        c = 0;
        for (int i = 0; i < n; i++, p++) {
            if (hex_value(*p) < 0)
                error_at(t, "incomplete universal character name");
            c = c * 16 + (uint32_t)hex_value(*p);
        }
        *universal = true;
    } else {
        switch (*p) {
        case 'a': c = 7; break;
        case 'b': c = 8; break;
        case 'f': c = 12; break;
        case 'n': c = 10; break;
        case 'r': c = 13; break;
        case 't': c = 9; break;
        case 'v': c = 11; break;
        case 'e': c = 27; break;                /* (GNU) */
        case '\\': case '\'': case '"': case '?': c = (unsigned char)*p; break;
        default:
            warn_at(t, "unknown escape sequence '\\%c'", *p);
            c = (unsigned char)*p;
        }
        p++;
    }
    *pp = p;
    return c;
}

static int encode_utf8(char *out, uint32_t c)
{
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (c >> 18));
    out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

/* The next code point of UTF-8 text (a lone byte stands for itself). */
static uint32_t decode_utf8(const char **pp)
{
    const unsigned char *p = (const unsigned char *)*pp;
    uint32_t c = p[0];
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (n == 0) {
        *pp += 1;
        return c;
    }
    c &= 0x3F >> n;
    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *pp += 1;
            return p[0];
        }
        c = (c << 6) | (p[i] & 0x3F);
    }
    *pp += n + 1;
    return c;
}

enum encoding { ENC_CHAR, ENC_UTF8, ENC_WIDE, ENC_UTF16, ENC_UTF32 };

static enum encoding literal_encoding(struct token *t)
{
    if (t->text[0] == 'L')
        return ENC_WIDE;
    if (t->text[0] == 'U')
        return ENC_UTF32;
    if (t->text[0] == 'u')
        return t->text[1] == '8' ? ENC_UTF8 : ENC_UTF16;
    return ENC_CHAR;
}

static struct type *element_type(enum encoding e)
{
    switch (e) {
    case ENC_WIDE: return ty_wchar;
    case ENC_UTF16: return ty_char16;
    case ENC_UTF32: return ty_char32;
    default: return ty_char;
    }
}

static void convert_char(struct token *t)
{
    enum encoding e = literal_encoding(t);
    const char *p = memchr(t->text, '\'', (size_t)t->len) + 1;
    const char *end = t->text + t->len - 1;
    if (p == end)
        error_at(t, "empty character constant");
    int64_t value = 0;
    int count = 0;
    while (p < end) {
        bool universal = false;
        uint32_t c;
        if (*p == '\\')
            c = read_char(t, &p, &universal);
        else if (e != ENC_CHAR && e != ENC_UTF8)
            c = decode_utf8(&p);                /* L'é' is one wide character */
        else
            c = (unsigned char)*p++;
        if (e == ENC_CHAR && universal && c >= 0x80) {
            /* a narrow constant of a universal name: its UTF-8 bytes */
            char u[4];
            int n = encode_utf8(u, c);
            for (int i = 0; i < n; i++)
                value = (value << 8) | (unsigned char)u[i];
            count += n;
            continue;
        }
        value = e == ENC_CHAR || e == ENC_UTF8 ? (value << 8) | (c & 0xFF) : c;
        count++;
    }
    if (e == ENC_CHAR) {
        /* 'a' is an int with the value of a char; 'ab' is GNU's (a << 8) | b */
        if (count == 1)
            value = target.char_signed ? (int64_t)(int8_t)value : (int64_t)(uint8_t)value;
        else
            value = (int32_t)value;
        t->type = ty_int;
    } else if (e == ENC_UTF8) {
        t->type = ty_uchar;
        value &= 0xFF;
    } else {
        t->type = element_type(e);
        if (e == ENC_UTF16)
            value &= 0xFFFF;
        if (t->type == ty_wchar && !t->type->is_unsigned)
            value = (int32_t)value;
    }
    t->kind = TK_NUM;
    t->value = value;
}

/* A string literal's elements: char strings as bytes (UTF-8 for \u),
 * wide ones as code points (UTF-16 with surrogate pairs for u""). */
static void string_elements(struct token *t, enum encoding e, struct buffer *out)
{
    const char *p = memchr(t->text, '"', (size_t)t->len) + 1;
    const char *end = t->text + t->len - 1;
    while (p < end) {
        bool universal = false;
        uint32_t c;
        if (*p == '\\')
            c = read_char(t, &p, &universal);
        else if (e == ENC_CHAR || e == ENC_UTF8) {
            buf_byte(out, *p++);
            continue;
        } else
            c = decode_utf8(&p);
        if (e == ENC_CHAR || e == ENC_UTF8) {
            if (universal) {
                char u[4];
                buf_add(out, u, (size_t)encode_utf8(u, c));
            } else
                buf_byte(out, (int)c);
        } else if (e == ENC_UTF16) {
            uint16_t u[2];
            int n = 1;
            if (c >= 0x10000) {
                u[0] = (uint16_t)(0xD800 + ((c - 0x10000) >> 10));
                u[1] = (uint16_t)(0xDC00 + ((c - 0x10000) & 0x3FF));
                n = 2;
            } else
                u[0] = (uint16_t)c;
            buf_add(out, u, (size_t)n * 2);     /* (both targets are little-endian) */
        } else {
            uint32_t w = c;
            buf_add(out, &w, 4);
        }
    }
}

/* Adjacent string literals become one (6.4.5p5): a prefixed one gives the
 * whole its kind. */
static void convert_strings(struct token *t)
{
    enum encoding e = ENC_CHAR;
    for (struct token *s = t; s && s->kind == TK_STR; s = s->next) {
        enum encoding se = literal_encoding(s);
        if (se != ENC_CHAR) {
            if (e != ENC_CHAR && e != se)
                error_at(s, "unsupported non-standard concatenation of string literals");
            e = se;
        }
    }
    struct buffer b = { 0 };
    struct token *last = t;
    for (struct token *s = t; s && s->kind == TK_STR; s = s->next) {
        string_elements(s, e, &b);
        last = s;
    }
    struct type *elem = element_type(e);
    if (e == ENC_UTF8)
        elem = ty_char;                         /* (C11: u8"" is an array of char) */
    int size = elem->size;
    for (int i = 0; i < size; i++)
        buf_byte(&b, 0);
    t->str = arena_alloc(b.len + 1);
    memcpy(t->str, b.data ? b.data : "", b.len);
    t->str_len = (int)b.len;
    t->type = array_of(elem, (int)(b.len / (size_t)size));
    free(b.data);
    t->next = last->next;
}

static void convert_number(struct token *t)
{
    char *text = token_text(t);
    const char *p = text;
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && (hex_value(p[2]) >= 0 || p[2] == '.')) {
        base = 16;
        p += 2;
    } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B') && (p[2] == '0' || p[2] == '1')) {
        base = 2;
        p += 2;
    } else if (p[0] == '0')
        base = 8;
    /* An integer if only digits of its base and an integer suffix follow. */
    const char *d = p;
    uint64_t v = 0;
    bool overflow = false;
    for (;; d++) {
        int digit = hex_value(*d);
        if (digit < 0 || digit >= (base == 8 ? 10 : base))
            break;
        if (v > (UINT64_MAX - (uint64_t)digit) / (uint64_t)(base == 8 ? 8 : base))
            overflow = true;
        v = v * (uint64_t)base + (uint64_t)digit;
    }
    bool is_float = *d == '.' || (base != 16 && (*d == 'e' || *d == 'E')) ||
                    (base == 16 && (*d == 'p' || *d == 'P'));
    if (!is_float) {
        if (base == 8)
            for (const char *c = p; c < d; c++)
                if (*c > '7')
                    error_at(t, "invalid digit '%c' in octal constant", *c);
        bool u = false;
        int l = 0;
        const char *s = d;
        for (;;) {
            if ((*s == 'u' || *s == 'U') && !u) {
                u = true;
                s++;
            } else if ((s[0] == 'l' && s[1] == 'l') || (s[0] == 'L' && s[1] == 'L')) {
                if (l)
                    break;
                l = 2;
                s += 2;
            } else if ((*s == 'l' || *s == 'L') && !l) {
                l = 1;
                s++;
            } else
                break;
        }
        if (*s)
            error_at(t, "invalid suffix '%s' on integer constant", d);
        if (overflow)
            warn_at(t, "integer literal is too large to be represented in any integer type");
        /* The first type of C11's list that holds the value (6.4.4.1p5):
         * decimal ones without u stay signed, others may become unsigned. */
        struct type *ty;
        if (u) {
            if (l == 0 && v <= UINT32_MAX)
                ty = ty_uint;
            else
                ty = l == 2 ? ty_ullong : ty_ulong;
        } else if (base == 10) {
            if (l == 0 && v <= INT32_MAX)
                ty = ty_int;
            else if (v <= INT64_MAX)
                ty = l == 2 ? ty_llong : ty_long;
            else {
                warn_at(t, "integer literal is too large to be represented in a signed integer type");
                ty = l == 2 ? ty_ullong : ty_ulong;
            }
        } else {
            if (l == 0 && v <= INT32_MAX)
                ty = ty_int;
            else if (l == 0 && v <= UINT32_MAX)
                ty = ty_uint;
            else if (v <= INT64_MAX)
                ty = l == 2 ? ty_llong : ty_long;
            else
                ty = l == 2 ? ty_ullong : ty_ulong;
        }
        t->kind = TK_NUM;
        t->type = ty;
        t->value = (int64_t)v;
        return;
    }
    char *end;
    double f = strtod(text, &end);
    struct type *ty = ty_double;
    /* (62a) GNU's imaginary suffix i or j, before or after f or l: a
     * complex constant whose real part is 0 (fvalue is the imaginary one) */
    bool imaginary = false;
    if (*end == 'i' || *end == 'I' || *end == 'j' || *end == 'J') {
        imaginary = true;
        end++;
    }
    if (*end == 'f' || *end == 'F') {
        ty = ty_float;
        end++;
    } else if (*end == 'l' || *end == 'L') {
        ty = ty_ldouble;
        end++;
    }
    if (!imaginary && (*end == 'i' || *end == 'I' || *end == 'j' || *end == 'J')) {
        imaginary = true;
        end++;
    }
    if (*end)
        error_at(t, "invalid suffix '%s' on floating constant", end);
    t->kind = TK_NUM;
    t->type = imaginary ? complex_of(ty) : ty;
    t->fvalue = f;
}

struct token *convert_pp_tokens(struct token *t)
{
    /* (the preprocessor's marks for the ends of included files go) */
    while (t && t->kind == TK_OTHER && t->len == 0 && t->value == -1)
        t = t->next;
    struct token *first = t;
    for (struct token *prev = 0; t && t->kind != TK_EOF; prev = t, t = t->next) {
        if (t->kind == TK_OTHER && t->len == 0 && t->value == -1 && prev) {
            /* (the preprocessor's mark for the end of an included file) */
            prev->next = t->next;
            t = prev;
            continue;
        }
        switch (t->kind) {
        case TK_IDENT:
            if (is_keyword(t))
                t->kind = TK_KEYWORD;
            break;
        case TK_PPNUM:
            convert_number(t);
            break;
        case TK_CHAR:
            convert_char(t);
            break;
        case TK_STR:
            convert_strings(t);
            break;
        case TK_OTHER:
            if (t->len == 0 && t->value == -1)
                break;                          /* (only first: the parser skips it) */
            error_at(t, "stray '%.*s' in program", t->len, t->text);
        default:
            break;
        }
    }
    return first;
}

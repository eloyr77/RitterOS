/*
 * The tokenizer (27o): the source as UTF-16 code units, one token at a time,
 * with the parser asking again where the grammar decides (a '/' that starts
 * a regular expression, a template's part after its substitution's '}').
 * Line terminators before a token are noted for automatic semicolons.
 */
#include <math.h>
#include "jsparse.h"

static const char *const keywords[] = {
    0, "break", "case", "catch", "class", "const", "continue", "debugger", "default",
    "delete", "do", "else", "export", "extends", "false", "finally", "for", "function",
    "if", "import", "in", "instanceof", "new", "null", "return", "super", "switch",
    "this", "throw", "true", "try", "typeof", "var", "void", "while", "with", "enum",
};

static const char *const puncts[] = {
    0, "{", "}", "(", ")", "[", "]", ".", "...", ";", ",", "<", ">", "<=", ">=", "==",
    "!=", "===", "!==", "+", "-", "*", "/", "%", "**", "++", "--", "<<", ">>", ">>>",
    "&", "|", "^", "!", "~", "&&", "||", "?\?", "?", "?.", ":", "=", "+=", "-=", "*=",
    "/=", "%=", "**=", "<<=", ">>=", ">>>=", "&=", "|=", "^=", "&&=", "||=", "?\?=", "=>",
    "#",
};

const char *punct_text(int op) { return puncts[op]; }
const char *keyword_text(int kw) { return keywords[kw]; }

void lex_error(struct lexer *L, const struct token *at, const char *fmt, ...)
{
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    free(L->error);
    L->error = strdup(buf);
    L->error_line = at ? at->line : L->line;
    L->error_col = at ? at->col : (int)(L->pos - L->line_start) + 1;
    longjmp(*L->fail, 1);
}

void lex_init(struct lexer *L, struct js *J, const uint16_t *s, uint32_t n, jmp_buf *fail)
{
    memset(L, 0, sizeof *L);
    L->J = J;
    L->s = s;
    L->n = n;
    L->line = 1;
    L->fail = fail;
    if (n >= 2 && s[0] == '#' && s[1] == '!')   /* a hashbang line */
        while (L->pos < n && !js_is_line_terminator(s[L->pos]))
            L->pos++;
}

void lex_save(struct lexer *L, struct lexer *saved)
{
    *saved = *L;
}

void lex_restore(struct lexer *L, const struct lexer *saved)
{
    char *err = L->error;
    *L = *saved;
    L->error = err;
}

static uint32_t peekc(struct lexer *L, uint32_t ahead)
{
    return L->pos + ahead < L->n ? L->s[L->pos + ahead] : 0xFFFFFFFF;
}

static void newline(struct lexer *L, uint32_t c)
{
    if (c == '\r' && peekc(L, 0) == '\n')
        L->pos++;
    L->line++;
    L->line_start = L->pos;
}

/* Skips blanks and comments; true if a line terminator was among them. */
static bool skip_space(struct lexer *L)
{
    bool nl = false;
    bool line_start = L->pos == 0;
    while (L->pos < L->n) {
        uint32_t c = L->s[L->pos];
        if (js_is_line_terminator(c)) {
            L->pos++;
            newline(L, c);
            nl = line_start = true;
        } else if (js_is_space(c))
            L->pos++;
        else if (c == '/' && peekc(L, 1) == '/') {
            while (L->pos < L->n && !js_is_line_terminator(L->s[L->pos]))
                L->pos++;
        } else if (c == '/' && peekc(L, 1) == '*') {
            uint32_t start = L->pos;
            L->pos += 2;
            for (;;) {
                if (L->pos >= L->n) {
                    L->pos = start;
                    lex_error(L, 0, "Unterminated comment");
                }
                uint32_t d = L->s[L->pos++];
                if (d == '*' && peekc(L, 0) == '/') {
                    L->pos++;
                    break;
                }
                if (js_is_line_terminator(d)) {
                    newline(L, d);
                    nl = true;
                }
            }
        } else if (c == '<' && peekc(L, 1) == '!' && peekc(L, 2) == '-' && peekc(L, 3) == '-') {
            while (L->pos < L->n && !js_is_line_terminator(L->s[L->pos]))   /* <!-- (Annex B) */
                L->pos++;
        } else if (c == '-' && line_start && peekc(L, 1) == '-' && peekc(L, 2) == '>') {
            while (L->pos < L->n && !js_is_line_terminator(L->s[L->pos]))   /* --> at a line's start */
                L->pos++;
        } else
            break;
    }
    return nl;
}

static int hexval(uint32_t c)
{
    if (c >= '0' && c <= '9')
        return (int)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return (int)(c - 'A' + 10);
    return -1;
}

static bool id_start(uint32_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '$' || c == '_' ||
           (c >= 0x80 && !js_is_space(c) && c != 0xFFFFFFFF && c != 0x2028 && c != 0x2029);
}

static bool id_part(uint32_t c)
{
    return id_start(c) || (c >= '0' && c <= '9') || c == 0x200C || c == 0x200D;
}

/* \uXXXX or \u{X...} after the backslash (at L->pos, on the 'u'). -1: invalid. */
static int64_t read_unicode_escape(struct lexer *L)
{
    if (peekc(L, 0) != 'u')
        return -1;
    L->pos++;
    if (peekc(L, 0) == '{') {
        L->pos++;
        int64_t v = 0;
        int digits = 0;
        while (hexval(peekc(L, 0)) >= 0) {
            v = v * 16 + hexval(L->s[L->pos++]);
            digits++;
            if (v > 0x10FFFF)
                return -1;
        }
        if (!digits || peekc(L, 0) != '}')
            return -1;
        L->pos++;
        return v;
    }
    int64_t v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(peekc(L, 0));
        if (h < 0)
            return -1;
        v = v * 16 + h;
        L->pos++;
    }
    return v;
}

static void read_identifier(struct lexer *L, struct token *t)
{
    struct sbuf b = { 0 };
    bool escaped = false;
    for (bool first = true;; first = false) {
        uint32_t c = peekc(L, 0);
        if (c == '\\') {
            L->pos++;
            int64_t cp = read_unicode_escape(L);
            if (cp < 0 || !(first ? id_start((uint32_t)cp) : id_part((uint32_t)cp)))
                lex_error(L, t, "Invalid escape in an identifier");
            sb_code_point(&b, (uint32_t)cp);
            escaped = true;
            continue;
        }
        if (c >= 0xD800 && c <= 0xDBFF && peekc(L, 1) >= 0xDC00 && peekc(L, 1) <= 0xDFFF) {
            sb_unit(&b, c);
            sb_unit(&b, peekc(L, 1));
            L->pos += 2;
            continue;
        }
        if (first ? !id_start(c) : !id_part(c))
            break;
        sb_unit(&b, c);
        L->pos++;
    }
    t->atom = atom_u16(L->J, b.s, b.n);
    t->escaped = escaped;
    t->type = T_IDENT;
    if (!escaped && b.n >= 2 && b.n <= 10) {
        char kw[12];
        uint32_t i;
        for (i = 0; i < b.n && b.s[i] < 128; i++)
            kw[i] = (char)b.s[i];
        kw[i] = 0;
        if (i == b.n)
            for (int k = 1; k <= KW_ENUM; k++)
                if (!strcmp(kw, keywords[k])) {
                    t->type = T_KEYWORD;
                    t->op = k;
                    break;
                }
    }
    sb_free(&b);
}

static void read_number(struct lexer *L, struct token *t)
{
    uint32_t start = L->pos;
    uint32_t c = peekc(L, 0);
    char buf[512];
    int k = 0;
    if (c == '0' && (peekc(L, 1) == 'x' || peekc(L, 1) == 'X' || peekc(L, 1) == 'o' || peekc(L, 1) == 'O' ||
                     peekc(L, 1) == 'b' || peekc(L, 1) == 'B')) {
        uint32_t r = peekc(L, 1) | 0x20;
        int radix = r == 'x' ? 16 : r == 'o' ? 8 : 2;
        L->pos += 2;
        double v = 0;
        int digits = 0;
        uint16_t ds[1100];
        int nd = 0;
        for (;;) {
            uint32_t d = peekc(L, 0);
            if (d == '_' && digits && hexval(peekc(L, 1)) >= 0 && hexval(peekc(L, 1)) < radix) {
                L->pos++;
                continue;
            }
            int h = hexval(d);
            if (h < 0 || h >= radix)
                break;
            if (nd < 1099)
                ds[nd++] = (uint16_t)d;
            L->pos++;
            digits++;
        }
        if (!digits)
            lex_error(L, t, "No digits after the number's prefix");
        /* text "0x..." through string_to_number for one correct rounding */
        uint16_t full[1102];
        full[0] = '0';
        full[1] = (uint16_t)r;
        memcpy(full + 2, ds, (size_t)nd * 2);
        v = string_to_number(full, (uint32_t)nd + 2);
        t->num = v;
    } else if (c == '0' && peekc(L, 1) >= '0' && peekc(L, 1) <= '9') {
        /* a legacy octal (or decimal with 8 or 9) literal */
        if (L->strict)
            lex_error(L, t, "Octal literals are not allowed in strict mode");
        bool octal = true;
        uint32_t p = L->pos + 1;
        while (p < L->n && L->s[p] >= '0' && L->s[p] <= '9') {
            if (L->s[p] >= '8')
                octal = false;
            p++;
        }
        double v = 0;
        for (uint32_t i = L->pos + 1; i < p; i++)
            v = v * (octal ? 8 : 10) + (L->s[i] - '0');
        L->pos = p;
        t->num = v;
    } else {
        bool dot = false, exp = false;
        for (;;) {
            uint32_t d = peekc(L, 0);
            if (d >= '0' && d <= '9') {
                if (k < 500)
                    buf[k++] = (char)d;
                L->pos++;
            } else if (d == '_' && k && buf[k - 1] >= '0' && buf[k - 1] <= '9' &&
                       peekc(L, 1) >= '0' && peekc(L, 1) <= '9') {
                L->pos++;
            } else if (d == '.' && !dot && !exp) {
                dot = true;
                if (k < 500)
                    buf[k++] = '.';
                L->pos++;
            } else if ((d == 'e' || d == 'E') && !exp) {
                uint32_t e1 = peekc(L, 1);
                uint32_t e2 = (e1 == '+' || e1 == '-') ? peekc(L, 2) : e1;
                if (!(e2 >= '0' && e2 <= '9'))
                    break;
                exp = true;
                if (k < 500)
                    buf[k++] = 'e';
                L->pos++;
                if (e1 == '+' || e1 == '-') {
                    if (k < 500)
                        buf[k++] = (char)e1;
                    L->pos++;
                }
            } else
                break;
        }
        buf[k] = 0;
        t->num = strtod(buf, 0);
    }
    if (peekc(L, 0) == 'n')
        lex_error(L, t, "BigInt literals are not supported");
    if (id_start(peekc(L, 0)) || (peekc(L, 0) >= '0' && peekc(L, 0) <= '9'))
        lex_error(L, t, "No identifier allowed directly after a number");
    t->type = T_NUM;
    (void)start;
}

/* An escape after the backslash (at L->pos) into b; in templates an invalid
 * one makes *bad true instead of an error. */
static void read_escape(struct lexer *L, struct sbuf *b, struct token *t, bool templ, bool *bad)
{
    uint32_t c = peekc(L, 0);
    L->pos++;
    switch (c) {
    case 'n': sb_unit(b, '\n'); return;
    case 't': sb_unit(b, '\t'); return;
    case 'r': sb_unit(b, '\r'); return;
    case 'b': sb_unit(b, '\b'); return;
    case 'f': sb_unit(b, '\f'); return;
    case 'v': sb_unit(b, '\v'); return;
    case '\r':
        if (peekc(L, 0) == '\n')
            L->pos++;
        /* fall through */
    case '\n': case 0x2028: case 0x2029:
        L->line++;
        L->line_start = L->pos;
        return;
    case 'x': {
        int h1 = hexval(peekc(L, 0)), h2 = hexval(peekc(L, 1));
        if (h1 < 0 || h2 < 0) {
            if (templ) {
                *bad = true;
                return;
            }
            lex_error(L, t, "Invalid hexadecimal escape");
        }
        L->pos += 2;
        sb_unit(b, (uint32_t)(h1 * 16 + h2));
        return;
    }
    case 'u': {
        L->pos--;
        int64_t cp = read_unicode_escape(L);
        if (cp < 0) {
            if (templ) {
                *bad = true;
                return;
            }
            lex_error(L, t, "Invalid Unicode escape");
        }
        sb_code_point(b, (uint32_t)cp);
        return;
    }
    }
    if (c >= '0' && c <= '7') {
        if (c == '0' && !(peekc(L, 0) >= '0' && peekc(L, 0) <= '9')) {
            sb_unit(b, 0);
            return;
        }
        if (templ) {
            *bad = true;
            return;
        }
        if (L->strict)
            lex_error(L, t, "Octal escapes are not allowed in strict mode");
        uint32_t v = c - '0';
        if (peekc(L, 0) >= '0' && peekc(L, 0) <= '7') {
            v = v * 8 + (L->s[L->pos++] - '0');
            if (c <= '3' && peekc(L, 0) >= '0' && peekc(L, 0) <= '7')
                v = v * 8 + (L->s[L->pos++] - '0');
        }
        sb_unit(b, v);
        return;
    }
    if (c == '8' || c == '9') {
        if (templ) {
            *bad = true;
            return;
        }
        if (L->strict)
            lex_error(L, t, "\\8 and \\9 are not allowed in strict mode");
    }
    if (c == 0xFFFFFFFF)
        lex_error(L, t, "Unterminated string");
    sb_unit(b, c);
}

static void read_string(struct lexer *L, struct token *t)
{
    uint32_t q = L->s[L->pos++];
    struct sbuf b = { 0 };
    for (;;) {
        uint32_t c = peekc(L, 0);
        if (c == 0xFFFFFFFF || c == '\n' || c == '\r') {
            sb_free(&b);
            lex_error(L, t, "Unterminated string literal");
        }
        L->pos++;
        if (c == q)
            break;
        if (c == '\\')
            read_escape(L, &b, t, false, 0);
        else
            sb_unit(&b, c);
    }
    t->type = T_STR;
    t->atom = atom_u16(L->J, b.s, b.n);
    sb_free(&b);
}

/* A template part from L->pos (just after ` or }) up to ${ or `. */
static void read_template(struct lexer *L, struct token *t)
{
    struct sbuf cooked = { 0 }, raw = { 0 };
    bool bad = false;
    for (;;) {
        uint32_t c = peekc(L, 0);
        if (c == 0xFFFFFFFF) {
            sb_free(&cooked);
            sb_free(&raw);
            lex_error(L, t, "Unterminated template literal");
        }
        if (c == '`') {
            L->pos++;
            t->tail = true;
            break;
        }
        if (c == '$' && peekc(L, 1) == '{') {
            L->pos += 2;
            t->tail = false;
            break;
        }
        if (c == '\\') {
            uint32_t from = L->pos;
            L->pos++;
            read_escape(L, &cooked, t, true, &bad);
            for (uint32_t i = from; i < L->pos; i++) {
                uint32_t r = L->s[i];
                if (r == '\r') {
                    if (i + 1 < L->pos && L->s[i + 1] == '\n')
                        i++;
                    r = '\n';
                }
                sb_unit(&raw, r);
            }
            continue;
        }
        L->pos++;
        if (c == '\r') {
            if (peekc(L, 0) == '\n')
                L->pos++;
            c = '\n';
        }
        if (js_is_line_terminator(c)) {
            L->line++;
            L->line_start = L->pos;
        }
        sb_unit(&cooked, c);
        sb_unit(&raw, c);
    }
    t->type = T_TEMPLATE;
    t->atom = bad ? 0 : atom_u16(L->J, cooked.s, cooked.n);
    t->raw = atom_u16(L->J, raw.s, raw.n);
    sb_free(&cooked);
    sb_free(&raw);
}

static int read_punct(struct lexer *L)
{
    uint32_t c = peekc(L, 0), c1 = peekc(L, 1), c2 = peekc(L, 2), c3 = peekc(L, 3);
#define TAKE(n, op) do { L->pos += n; return op; } while (0)
    switch (c) {
    case '{': TAKE(1, P_LBRACE);
    case '}': TAKE(1, P_RBRACE);
    case '(': TAKE(1, P_LPAREN);
    case ')': TAKE(1, P_RPAREN);
    case '[': TAKE(1, P_LBRACKET);
    case ']': TAKE(1, P_RBRACKET);
    case ';': TAKE(1, P_SEMI);
    case ',': TAKE(1, P_COMMA);
    case ':': TAKE(1, P_COLON);
    case '~': TAKE(1, P_TILDE);
    case '#': TAKE(1, P_HASH);
    case '.':
        if (c1 == '.' && c2 == '.')
            TAKE(3, P_ELLIPSIS);
        TAKE(1, P_DOT);
    case '<':
        if (c1 == '<')
            TAKE(c2 == '=' ? 3 : 2, c2 == '=' ? P_SHL_ASSIGN : P_SHL);
        TAKE(c1 == '=' ? 2 : 1, c1 == '=' ? P_LE : P_LT);
    case '>':
        if (c1 == '>' && c2 == '>')
            TAKE(c3 == '=' ? 4 : 3, c3 == '=' ? P_SHR_ASSIGN : P_SHR);
        if (c1 == '>')
            TAKE(c2 == '=' ? 3 : 2, c2 == '=' ? P_SAR_ASSIGN : P_SAR);
        TAKE(c1 == '=' ? 2 : 1, c1 == '=' ? P_GE : P_GT);
    case '=':
        if (c1 == '=')
            TAKE(c2 == '=' ? 3 : 2, c2 == '=' ? P_SEQ : P_EQ);
        if (c1 == '>')
            TAKE(2, P_ARROW);
        TAKE(1, P_ASSIGN);
    case '!':
        if (c1 == '=')
            TAKE(c2 == '=' ? 3 : 2, c2 == '=' ? P_SNE : P_NE);
        TAKE(1, P_BANG);
    case '+':
        if (c1 == '+')
            TAKE(2, P_INC);
        TAKE(c1 == '=' ? 2 : 1, c1 == '=' ? P_ADD_ASSIGN : P_PLUS);
    case '-':
        if (c1 == '-')
            TAKE(2, P_DEC);
        TAKE(c1 == '=' ? 2 : 1, c1 == '=' ? P_SUB_ASSIGN : P_MINUS);
    case '*':
        if (c1 == '*')
            TAKE(c2 == '=' ? 3 : 2, c2 == '=' ? P_POW_ASSIGN : P_STARSTAR);
        TAKE(c1 == '=' ? 2 : 1, c1 == '=' ? P_MUL_ASSIGN : P_STAR);
    case '/':
        TAKE(c1 == '=' ? 2 : 1, c1 == '=' ? P_DIV_ASSIGN : P_SLASH);
    case '%':
        TAKE(c1 == '=' ? 2 : 1, c1 == '=' ? P_MOD_ASSIGN : P_PERCENT);
    case '&':
        if (c1 == '&')
            TAKE(c2 == '=' ? 3 : 2, c2 == '=' ? P_LAND_ASSIGN : P_AND);
        TAKE(c1 == '=' ? 2 : 1, c1 == '=' ? P_AND_ASSIGN : P_AMP);
    case '|':
        if (c1 == '|')
            TAKE(c2 == '=' ? 3 : 2, c2 == '=' ? P_LOR_ASSIGN : P_OR);
        TAKE(c1 == '=' ? 2 : 1, c1 == '=' ? P_OR_ASSIGN : P_BAR);
    case '^':
        TAKE(c1 == '=' ? 2 : 1, c1 == '=' ? P_XOR_ASSIGN : P_CARET);
    case '?':
        if (c1 == '?')
            TAKE(c2 == '=' ? 3 : 2, c2 == '=' ? P_NULLISH_ASSIGN : P_NULLISH);
        if (c1 == '.' && !(c2 >= '0' && c2 <= '9'))
            TAKE(2, P_OPTIONAL);
        TAKE(1, P_QUESTION);
    }
#undef TAKE
    return 0;
}

void lex_next(struct lexer *L)
{
    struct token *t = &L->tok;
    bool nl = skip_space(L);
    memset(t, 0, sizeof *t);
    t->nl_before = nl;
    t->pos = L->pos;
    t->line = L->line;
    t->col = (int)(L->pos - L->line_start) + 1;
    if (L->pos >= L->n) {
        t->type = T_EOF;
        t->end = L->pos;
        return;
    }
    uint32_t c = L->s[L->pos];
    if (id_start(c) || c == '\\')
        read_identifier(L, t);
    else if ((c >= '0' && c <= '9') || (c == '.' && peekc(L, 1) >= '0' && peekc(L, 1) <= '9'))
        read_number(L, t);
    else if (c == '"' || c == '\'')
        read_string(L, t);
    else if (c == '`') {
        L->pos++;
        read_template(L, t);
    } else if (c == '#' && (id_start(peekc(L, 1)) || peekc(L, 1) == '\\')) {
        L->pos++;
        read_identifier(L, t);
        t->type = T_PRIVATE;
    } else {
        int op = read_punct(L);
        if (!op)
            lex_error(L, t, "Invalid character '\\u%04x'", c);
        t->type = T_PUNCT;
        t->op = op;
    }
    t->end = L->pos;
}

void lex_regexp(struct lexer *L)
{
    struct token *t = &L->tok;
    L->pos = t->pos + 1;
    struct sbuf body = { 0 };
    bool in_class = false;
    for (;;) {
        uint32_t c = peekc(L, 0);
        if (c == 0xFFFFFFFF || js_is_line_terminator(c)) {
            sb_free(&body);
            lex_error(L, t, "Unterminated regular expression literal");
        }
        L->pos++;
        if (c == '\\') {
            uint32_t d = peekc(L, 0);
            if (d == 0xFFFFFFFF || js_is_line_terminator(d)) {
                sb_free(&body);
                lex_error(L, t, "Unterminated regular expression literal");
            }
            sb_unit(&body, c);
            sb_unit(&body, d);
            L->pos++;
            continue;
        }
        if (c == '[')
            in_class = true;
        else if (c == ']')
            in_class = false;
        else if (c == '/' && !in_class)
            break;
        sb_unit(&body, c);
    }
    struct sbuf flags = { 0 };
    while (id_part(peekc(L, 0)))
        sb_unit(&flags, L->s[L->pos++]);
    t->type = T_REGEXP;
    t->atom = atom_u16(L->J, body.s, body.n);
    t->raw = atom_u16(L->J, flags.s, flags.n);
    t->end = L->pos;
    sb_free(&body);
    sb_free(&flags);
}

void lex_template_continue(struct lexer *L)
{
    struct token *t = &L->tok;
    L->pos = t->pos + 1;                        /* just after the '}' */
    bool nl = t->nl_before;
    memset(t, 0, sizeof *t);
    t->nl_before = nl;
    t->pos = L->pos;
    t->line = L->line;
    t->col = (int)(L->pos - L->line_start) + 1;
    read_template(L, t);
    t->end = L->pos;
}

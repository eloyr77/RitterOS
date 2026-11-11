/*
 * The regular expression engine (27o): the language's patterns (ES2024
 * syntax with Annex B's leniency outside /u) compiled to a small program
 * and run by a backtracking matcher with its own stack (no C recursion
 * but for lookarounds). Jumps are relative, so a quantified atom's code is
 * copied as it is: x{2,4} is x x (x (x)?)?. A loop iteration that matched
 * nothing fails (MARK/CHECK, as the specification's RepeatMatcher wants),
 * and a quantified group's captures are cleared at each iteration (RESET).
 * Lookbehinds are compiled backwards: their terms in reverse order,
 * matching to the left.
 */
#include <setjmp.h>
#include "jsint.h"

enum {
    RF_G = 1, RF_I = 2, RF_M = 4, RF_S = 8, RF_U = 16, RF_Y = 32, RF_D = 64, RF_V = 128,
};

enum rop {
    R_CHAR, R_CHAR_I, R_ANY, R_ANY_ALL, R_CLASS, R_BOL, R_EOL, R_BOL_M, R_EOL_M, R_WORDB, R_NWORDB,
    R_SPLIT, R_JMP, R_SAVE, R_RESET, R_BACKREF, R_LOOK, R_LOOK_END, R_MARK, R_CHECK, R_MATCH,
    R_CHAR_B, R_CHAR_I_B, R_ANY_B, R_ANY_ALL_B, R_CLASS_B, R_BACKREF_B,
};

struct rclass {
    uint32_t *r;                                /* sorted, merged ranges: lo, hi pairs */
    uint32_t n;                                 /* pairs */
    bool neg;
};

struct regex {
    int32_t *code;
    uint32_t len;
    uint32_t flags;
    uint32_t ngroups;                           /* with group 0 */
    uint32_t nregs;
    struct rclass *classes;
    uint32_t nclasses;
    uint16_t **names;                           /* names[g] (0 for unnamed), length in name_len */
    uint32_t *name_len;
};

/* --- compiling ---------------------------------------------------------- */

struct rbuf {
    int32_t *w;
    uint32_t n, cap;
};

struct rparse {
    struct js *J;
    const uint16_t *s;
    uint32_t n, i;
    uint32_t flags;
    bool unicode;
    struct regex *re;
    uint32_t group;                             /* the next capture's number */
    uint32_t total_groups;
    bool has_names;
    const char *error;
    int depth;
    jmp_buf *fail;
};

static _Noreturn void rfail(struct rparse *P, const char *msg)
{
    P->error = msg;
    longjmp(*P->fail, 1);
}

static void put(struct rparse *P, struct rbuf *b, int32_t w)
{
    if (b->n == b->cap) {
        uint32_t cap = b->cap ? b->cap * 2 : 32;
        if (cap > (1u << 22))
            rfail(P, "regular expression too large");
        b->w = realloc(b->w, cap * sizeof *b->w);
        if (!b->w)
            abort();
        b->cap = cap;
    }
    b->w[b->n++] = w;
}

static void append(struct rparse *P, struct rbuf *to, const struct rbuf *from)
{
    for (uint32_t i = 0; i < from->n; i++)
        put(P, to, from->w[i]);
}

static uint32_t peek_u(struct rparse *P, uint32_t ahead)
{
    return P->i + ahead < P->n ? P->s[P->i + ahead] : 0xFFFFFFFF;
}

static bool at_end(struct rparse *P) { return P->i >= P->n; }

/* the next pattern character (a code point in unicode mode) */
static uint32_t take_char(struct rparse *P)
{
    uint32_t c = P->s[P->i++];
    if (P->unicode && c >= 0xD800 && c <= 0xDBFF && P->i < P->n && P->s[P->i] >= 0xDC00 && P->s[P->i] <= 0xDFFF)
        c = 0x10000 + ((c - 0xD800) << 10) + (P->s[P->i++] - 0xDC00);
    return c;
}

/* the standard's Canonicalize: simple case folding with /u or /v, else
 * the upper case when it is one character (and not ASCII from beyond) */
static uint32_t canon(uint32_t c, bool unicode)
{
    if (unicode)
        return js_case_fold(c);
    uint32_t m[3];
    if (js_case_full(c, true, m) != 1)
        return c;
    if (c >= 128 && m[0] < 128)
        return c;
    return m[0];
}

/* class building */
struct cbuild {
    uint32_t *r;
    uint32_t n, cap;
};

static void cadd(struct cbuild *c, uint32_t lo, uint32_t hi)
{
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 16;
        c->r = realloc(c->r, c->cap * 2 * sizeof *c->r);
        if (!c->r)
            abort();
    }
    c->r[2 * c->n] = lo;
    c->r[2 * c->n + 1] = hi;
    c->n++;
}

/* With /i a class matches a character whose canonical form is one of its
 * members' (the matcher canonicalizes the input): the class gets the
 * canonical form of every member some mapping changes. */
static void case_close(struct rparse *P, struct cbuild *c)
{
    struct cbuild extra = { 0 };
    uint32_t lo, hi;
    for (size_t i = 0; js_case_changing(i, &lo, &hi); i++)
        for (uint32_t k = 0; k < c->n; k++) {
            uint32_t a = c->r[2 * k] > lo ? c->r[2 * k] : lo, b = c->r[2 * k + 1] < hi ? c->r[2 * k + 1] : hi;
            for (uint32_t x = a; a <= b && x <= b; x++) {
                uint32_t m = canon(x, P->unicode);
                if (m != x)
                    cadd(&extra, m, m);
            }
        }
    for (uint32_t k = 0; k < extra.n; k++)
        cadd(c, extra.r[2 * k], extra.r[2 * k + 1]);
    free(extra.r);
}

static int range_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void cnormalize(struct cbuild *c)
{
    if (c->n < 2)
        return;
    qsort(c->r, c->n, 2 * sizeof *c->r, range_cmp);
    uint32_t k = 0;
    for (uint32_t i = 1; i < c->n; i++) {
        if (c->r[2 * i] <= c->r[2 * k + 1] + 1) {
            if (c->r[2 * i + 1] > c->r[2 * k + 1])
                c->r[2 * k + 1] = c->r[2 * i + 1];
        } else {
            k++;
            c->r[2 * k] = c->r[2 * i];
            c->r[2 * k + 1] = c->r[2 * i + 1];
        }
    }
    c->n = k + 1;
}

static void cadd_complement(struct cbuild *c, const uint32_t *r, uint32_t n)
{
    uint32_t lo = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (r[2 * i] > lo)
            cadd(c, lo, r[2 * i] - 1);
        lo = r[2 * i + 1] + 1;
    }
    if (lo <= 0x10FFFF)
        cadd(c, lo, 0x10FFFF);
}

static const uint32_t digit_r[] = { '0', '9' };
static const uint32_t word_r[] = { '0', '9', 'A', 'Z', '_', '_', 'a', 'z' };
/* with /iu also ſ and the Kelvin sign, which fold to s and k (the standard's WordCharacters) */
static const uint32_t word_iu_r[] = { '0', '9', 'A', 'Z', '_', '_', 'a', 'z', 0x17F, 0x17F, 0x212A, 0x212A };
static const uint32_t space_r[] = { 9, 13, 32, 32, 0xA0, 0xA0, 0x1680, 0x1680, 0x2000, 0x200A,
                                    0x2028, 0x2029, 0x202F, 0x202F, 0x205F, 0x205F, 0x3000, 0x3000,
                                    0xFEFF, 0xFEFF };

static void cadd_list(struct cbuild *c, const uint32_t *r, uint32_t n, bool neg)
{
    if (neg)
        cadd_complement(c, r, n);
    else
        for (uint32_t i = 0; i < n; i++)
            cadd(c, r[2 * i], r[2 * i + 1]);
}

/* \p{...}: approximations by ranges (no Unicode tables) */
static bool property_class(struct cbuild *c, const char *name, bool neg)
{
    struct cbuild t = { 0 };
    if (!strcmp(name, "L") || !strcmp(name, "Letter") || !strcmp(name, "Alphabetic") || !strcmp(name, "Alpha")) {
        cadd(&t, 'A', 'Z'); cadd(&t, 'a', 'z'); cadd(&t, 0xAA, 0xAA); cadd(&t, 0xB5, 0xB5); cadd(&t, 0xBA, 0xBA);
        cadd(&t, 0xC0, 0xD6); cadd(&t, 0xD8, 0xF6); cadd(&t, 0xF8, 0x2C1); cadd(&t, 0x370, 0x3FF);
        cadd(&t, 0x400, 0x52F); cadd(&t, 0x531, 0x587); cadd(&t, 0x5D0, 0x5EA); cadd(&t, 0x620, 0x64A);
        cadd(&t, 0x904, 0x939); cadd(&t, 0xE01, 0xE30); cadd(&t, 0x10A0, 0x10FF); cadd(&t, 0x1E00, 0x1FFF);
        cadd(&t, 0x3041, 0x3096); cadd(&t, 0x30A1, 0x30FA); cadd(&t, 0x4E00, 0x9FFF); cadd(&t, 0xAC00, 0xD7A3);
    } else if (!strcmp(name, "Lu") || !strcmp(name, "Uppercase_Letter") || !strcmp(name, "Uppercase")) {
        cadd(&t, 'A', 'Z'); cadd(&t, 0xC0, 0xD6); cadd(&t, 0xD8, 0xDE); cadd(&t, 0x391, 0x3A9); cadd(&t, 0x410, 0x42F);
    } else if (!strcmp(name, "Ll") || !strcmp(name, "Lowercase_Letter") || !strcmp(name, "Lowercase")) {
        cadd(&t, 'a', 'z'); cadd(&t, 0xDF, 0xF6); cadd(&t, 0xF8, 0xFF); cadd(&t, 0x3B1, 0x3C9); cadd(&t, 0x430, 0x44F);
    } else if (!strcmp(name, "N") || !strcmp(name, "Number") || !strcmp(name, "Nd") || !strcmp(name, "Decimal_Number")) {
        cadd(&t, '0', '9'); cadd(&t, 0x660, 0x669); cadd(&t, 0x966, 0x96F); cadd(&t, 0xFF10, 0xFF19);
    } else if (!strcmp(name, "P") || !strcmp(name, "Punctuation")) {
        cadd(&t, '!', '#'); cadd(&t, '%', '*'); cadd(&t, ',', '/'); cadd(&t, ':', ';'); cadd(&t, '?', '@');
        cadd(&t, '[', ']'); cadd(&t, '_', '_'); cadd(&t, '{', '{'); cadd(&t, '}', '}'); cadd(&t, 0xA1, 0xA1);
        cadd(&t, 0xA7, 0xA7); cadd(&t, 0xAB, 0xAB); cadd(&t, 0xB6, 0xB7); cadd(&t, 0xBB, 0xBB); cadd(&t, 0xBF, 0xBF);
        cadd(&t, 0x2010, 0x2027); cadd(&t, 0x3001, 0x3003);
    } else if (!strcmp(name, "S") || !strcmp(name, "Symbol")) {
        cadd(&t, '$', '$'); cadd(&t, '+', '+'); cadd(&t, '<', '>'); cadd(&t, '^', '^'); cadd(&t, '`', '`');
        cadd(&t, '|', '|'); cadd(&t, '~', '~'); cadd(&t, 0xA2, 0xA6); cadd(&t, 0x20A0, 0x20C0); cadd(&t, 0x2190, 0x23FF);
    } else if (!strcmp(name, "Z") || !strcmp(name, "Separator") || !strcmp(name, "White_Space") || !strcmp(name, "space")) {
        cadd_list(&t, space_r, sizeof space_r / sizeof space_r[0] / 2, false);
    } else if (!strcmp(name, "ASCII")) {
        cadd(&t, 0, 0x7F);
    } else if (!strcmp(name, "Any")) {
        cadd(&t, 0, 0x10FFFF);
    } else if (!strcmp(name, "Emoji") || !strcmp(name, "Emoji_Presentation") || !strcmp(name, "Extended_Pictographic")) {
        cadd(&t, 0x231A, 0x231B); cadd(&t, 0x2600, 0x27BF); cadd(&t, 0x2B50, 0x2B55); cadd(&t, 0x1F000, 0x1FAFF);
    } else if (!strcmp(name, "Script=Latin") || !strcmp(name, "sc=Latin") || !strcmp(name, "Script=Latn")) {
        cadd(&t, 'A', 'Z'); cadd(&t, 'a', 'z'); cadd(&t, 0xC0, 0x24F); cadd(&t, 0x1E00, 0x1EFF);
    } else if (!strcmp(name, "Script=Greek") || !strcmp(name, "sc=Greek")) {
        cadd(&t, 0x370, 0x3FF); cadd(&t, 0x1F00, 0x1FFF);
    } else if (!strcmp(name, "Script=Cyrillic") || !strcmp(name, "sc=Cyrillic")) {
        cadd(&t, 0x400, 0x52F);
    } else if (!strcmp(name, "Script=Han") || !strcmp(name, "sc=Han")) {
        cadd(&t, 0x2E80, 0x2FDF); cadd(&t, 0x3400, 0x4DBF); cadd(&t, 0x4E00, 0x9FFF); cadd(&t, 0x20000, 0x2FA1F);
    } else if (!strcmp(name, "Script=Hiragana") || !strcmp(name, "sc=Hiragana")) {
        cadd(&t, 0x3041, 0x309F);
    } else if (!strcmp(name, "Script=Katakana") || !strcmp(name, "sc=Katakana")) {
        cadd(&t, 0x30A0, 0x30FF);
    } else {
        free(t.r);
        return false;
    }
    cnormalize(&t);
    cadd_list(c, t.r, t.n, neg);
    free(t.r);
    return true;
}

static int hexd(uint32_t c)
{
    if (c >= '0' && c <= '9')
        return (int)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return (int)(c - 'A' + 10);
    return -1;
}

static bool syntax_char(uint32_t c)
{
    return c && c < 128 && strchr("^$\\.*+?()[]{}|/", (int)c);
}

/* \u escapes: \uXXXX, \u{X} (unicode), surrogate pairs 😀 (unicode) */
static int64_t unicode_escape(struct rparse *P)
{
    /* at the 'u' */
    uint32_t save = P->i;
    P->i++;
    if (P->unicode && peek_u(P, 0) == '{') {
        P->i++;
        int64_t v = 0;
        int nd = 0;
        while (hexd(peek_u(P, 0)) >= 0) {
            v = v * 16 + hexd(P->s[P->i++]);
            nd++;
            if (v > 0x10FFFF)
                rfail(P, "invalid Unicode escape");
        }
        if (!nd || peek_u(P, 0) != '}')
            rfail(P, "invalid Unicode escape");
        P->i++;
        return v;
    }
    int64_t v = 0;
    for (int k = 0; k < 4; k++) {
        int h = hexd(peek_u(P, 0));
        if (h < 0) {
            if (P->unicode)
                rfail(P, "invalid Unicode escape");
            P->i = save + 1;                    /* Annex B: \u is "u" */
            return 'u';
        }
        v = v * 16 + h;
        P->i++;
    }
    if (P->unicode && v >= 0xD800 && v <= 0xDBFF && peek_u(P, 0) == '\\' && peek_u(P, 1) == 'u') {
        uint32_t s2 = P->i;
        P->i += 2;
        int64_t w = 0;
        bool ok = true;
        for (int k = 0; k < 4; k++) {
            int h = hexd(peek_u(P, 0));
            if (h < 0) {
                ok = false;
                break;
            }
            w = w * 16 + h;
            P->i++;
        }
        if (ok && w >= 0xDC00 && w <= 0xDFFF)
            return 0x10000 + ((v - 0xD800) << 10) + (w - 0xDC00);
        P->i = s2;
    }
    return v;
}

/* A class escape after '\' (P->i at the letter) into c; returns false
 * if it is not a class (then *ch is the character). */
static bool class_escape(struct rparse *P, struct cbuild *c, uint32_t *ch, bool in_class)
{
    uint32_t e = P->s[P->i];
    switch (e) {
    case 'd': case 'D':
        P->i++;
        cadd_list(c, digit_r, 1, e == 'D');
        return true;
    case 'w': case 'W':
        P->i++;
        if (P->unicode && (P->flags & RF_I))
            cadd_list(c, word_iu_r, 6, e == 'W');
        else
            cadd_list(c, word_r, 4, e == 'W');
        return true;
    case 's': case 'S':
        P->i++;
        cadd_list(c, space_r, sizeof space_r / sizeof space_r[0] / 2, e == 'S');
        return true;
    case 'p': case 'P':
        if (P->unicode) {
            P->i++;
            if (peek_u(P, 0) != '{')
                rfail(P, "invalid property name");
            P->i++;
            char name[64];
            int k = 0;
            while (!at_end(P) && P->s[P->i] != '}' && k < 63)
                name[k++] = (char)P->s[P->i++];
            name[k] = 0;
            if (peek_u(P, 0) != '}')
                rfail(P, "invalid property name");
            P->i++;
            if (!property_class(P->unicode ? c : c, name, e == 'P'))
                rfail(P, "invalid property name");
            return true;
        }
        break;
    }
    P->i++;
    switch (e) {
    case 'f': *ch = '\f'; return false;
    case 'n': *ch = '\n'; return false;
    case 'r': *ch = '\r'; return false;
    case 't': *ch = '\t'; return false;
    case 'v': *ch = '\v'; return false;
    case 'b':
        if (in_class) {
            *ch = '\b';
            return false;
        }
        break;
    case '-':
        if (in_class && P->unicode) {
            *ch = '-';
            return false;
        }
        break;
    case 'c': {
        uint32_t l = peek_u(P, 0);
        if ((l >= 'a' && l <= 'z') || (l >= 'A' && l <= 'Z')) {
            P->i++;
            *ch = l % 32;
            return false;
        }
        if (in_class && !P->unicode && ((l >= '0' && l <= '9') || l == '_')) {
            P->i++;
            *ch = l % 32;
            return false;
        }
        if (P->unicode)
            rfail(P, "invalid escape");
        P->i--;                                 /* Annex B: "\c" is a backslash */
        *ch = '\\';
        return false;
    }
    case '0':
        if (!(peek_u(P, 0) >= '0' && peek_u(P, 0) <= '9')) {
            *ch = 0;
            return false;
        }
        if (P->unicode)
            rfail(P, "invalid decimal escape");
        /* fall through: a legacy octal escape */
    case '1': case '2': case '3': case '4': case '5': case '6': case '7':
        if (P->unicode && in_class)
            rfail(P, "invalid class escape");
        {
            uint32_t v = e - '0';
            if (peek_u(P, 0) >= '0' && peek_u(P, 0) <= '7') {
                v = v * 8 + (P->s[P->i++] - '0');
                if (e <= '3' && peek_u(P, 0) >= '0' && peek_u(P, 0) <= '7')
                    v = v * 8 + (P->s[P->i++] - '0');
            }
            *ch = v;
            return false;
        }
    case 'x': {
        int h1 = hexd(peek_u(P, 0)), h2 = hexd(peek_u(P, 1));
        if (h1 >= 0 && h2 >= 0) {
            P->i += 2;
            *ch = (uint32_t)(h1 * 16 + h2);
            return false;
        }
        if (P->unicode)
            rfail(P, "invalid escape");
        *ch = 'x';
        return false;
    }
    case 'u': {
        P->i--;
        int64_t v = unicode_escape(P);
        *ch = (uint32_t)v;
        return false;
    }
    }
    if (P->unicode && !syntax_char(e) && e != '/')
        rfail(P, "invalid escape");
    if (e >= 0xD800 && e <= 0xDBFF && P->unicode) {
        P->i--;
        *ch = take_char(P);
        return false;
    }
    *ch = e;
    return false;
}

static int add_class(struct rparse *P, struct cbuild *c, bool neg)
{
    struct regex *re = P->re;
    cnormalize(c);
    re->classes = realloc(re->classes, (re->nclasses + 1) * sizeof *re->classes);
    if (!re->classes)
        abort();
    re->classes[re->nclasses].r = c->r;
    re->classes[re->nclasses].n = c->n;
    re->classes[re->nclasses].neg = neg;
    return (int)re->nclasses++;
}

static void parse_class(struct rparse *P, struct rbuf *b, bool backward)
{
    /* after '[' */
    bool neg = false;
    if (peek_u(P, 0) == '^') {
        neg = true;
        P->i++;
    }
    struct cbuild c = { 0 };
    for (;;) {
        if (at_end(P))
            rfail(P, "missing terminating ] for character class");
        if (P->s[P->i] == ']') {
            P->i++;
            break;
        }
        uint32_t lo;
        bool lo_class = false;
        if (P->s[P->i] == '\\') {
            P->i++;
            if (at_end(P))
                rfail(P, "\\ at end of pattern");
            lo_class = class_escape(P, &c, &lo, true);
        } else
            lo = take_char(P);
        if (peek_u(P, 0) == '-' && peek_u(P, 1) != ']' && peek_u(P, 1) != 0xFFFFFFFF) {
            P->i++;
            uint32_t hi;
            bool hi_class = false;
            struct cbuild tmp = { 0 };
            if (P->s[P->i] == '\\') {
                P->i++;
                if (at_end(P))
                    rfail(P, "\\ at end of pattern");
                hi_class = class_escape(P, &tmp, &hi, true);
            } else
                hi = take_char(P);
            if (lo_class || hi_class) {
                if (P->unicode)
                    rfail(P, "invalid character class");
                /* Annex B: a class in a range: the three as themselves */
                if (!lo_class)
                    cadd(&c, lo, lo);
                cadd(&c, '-', '-');
                if (hi_class)
                    for (uint32_t k = 0; k < tmp.n; k++)
                        cadd(&c, tmp.r[2 * k], tmp.r[2 * k + 1]);
                else
                    cadd(&c, hi, hi);
                free(tmp.r);
                continue;
            }
            free(tmp.r);
            if (lo > hi)
                rfail(P, "range out of order in character class");
            cadd(&c, lo, hi);
            continue;
        }
        if (!lo_class)
            cadd(&c, lo, lo);
    }
    if (P->flags & RF_I)
        case_close(P, &c);
    int idx = add_class(P, &c, neg);
    put(P, b, backward ? R_CLASS_B : R_CLASS);
    put(P, b, idx);
}

static void emit_char(struct rparse *P, struct rbuf *b, uint32_t c, bool backward)
{
    if (!P->unicode && c >= 0x10000) {
        /* (cannot happen: non-unicode patterns are code units) */
    }
    if (P->flags & RF_I) {
        put(P, b, backward ? R_CHAR_I_B : R_CHAR_I);
        put(P, b, (int32_t)canon(c, P->unicode));
    } else {
        put(P, b, backward ? R_CHAR_B : R_CHAR);
        put(P, b, (int32_t)c);
    }
}

static void parse_disjunction(struct rparse *P, struct rbuf *b, bool backward);

static uint32_t parse_int(struct rparse *P, bool *ok)
{
    uint64_t v = 0;
    *ok = false;
    while (peek_u(P, 0) >= '0' && peek_u(P, 0) <= '9') {
        v = v * 10 + (P->s[P->i++] - '0');
        if (v > 0x7FFFFFFF)
            v = 0x7FFFFFFF;
        *ok = true;
    }
    return (uint32_t)v;
}

/* {n}, {n,}, {n,m}: false (position unchanged) if not a quantifier */
static bool parse_braces(struct rparse *P, uint32_t *min, uint32_t *max)
{
    uint32_t save = P->i;
    if (peek_u(P, 0) != '{')
        return false;
    P->i++;
    bool ok;
    *min = parse_int(P, &ok);
    if (!ok) {
        P->i = save;
        return false;
    }
    *max = *min;
    if (peek_u(P, 0) == ',') {
        P->i++;
        bool ok2;
        uint32_t m = parse_int(P, &ok2);
        *max = ok2 ? m : 0xFFFFFFFF;
    }
    if (peek_u(P, 0) != '}') {
        P->i = save;
        return false;
    }
    P->i++;
    return true;
}

static uint16_t *group_name(struct rparse *P, uint32_t *len)
{
    /* after '<': up to '>' */
    struct sbuf b = { 0 };
    for (;;) {
        if (at_end(P))
            rfail(P, "invalid group name");
        uint32_t c = P->s[P->i];
        if (c == '>') {
            P->i++;
            break;
        }
        if (c == '\\') {
            P->i++;
            if (peek_u(P, 0) != 'u')
                rfail(P, "invalid group name");
            bool saved = P->unicode;
            P->unicode = true;
            int64_t v = unicode_escape(P);
            P->unicode = saved;
            sb_code_point(&b, (uint32_t)v);
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '$' || c == '_' || c >= 0x80 ||
              (b.n && c >= '0' && c <= '9')))
            rfail(P, "invalid group name");
        sb_unit(&b, c);
        P->i++;
    }
    if (!b.n)
        rfail(P, "invalid group name");
    *len = b.n;
    return b.s;
}

/* The code for one atom (into b); whether it may be quantified. */
static bool parse_atom(struct rparse *P, struct rbuf *b, bool backward, uint32_t *first_group, uint32_t *last_group)
{
    uint32_t c = P->s[P->i];
    *first_group = P->group;
    switch (c) {
    case '^':
        P->i++;
        put(P, b, (P->flags & RF_M) ? R_BOL_M : R_BOL);
        return false;
    case '$':
        P->i++;
        put(P, b, (P->flags & RF_M) ? R_EOL_M : R_EOL);
        return false;
    case '.':
        P->i++;
        put(P, b, (P->flags & RF_S) ? (backward ? R_ANY_ALL_B : R_ANY_ALL) : (backward ? R_ANY_B : R_ANY));
        *last_group = P->group;
        return true;
    case '[':
        P->i++;
        parse_class(P, b, backward);
        *last_group = P->group;
        return true;
    case '(': {
        P->i++;
        if (++P->depth > 200)
            rfail(P, "regular expression too deeply nested");
        if (peek_u(P, 0) == '?') {
            uint32_t k = peek_u(P, 1);
            if (k == ':') {
                P->i += 2;
                parse_disjunction(P, b, backward);
                if (peek_u(P, 0) != ')')
                    rfail(P, "missing )");
                P->i++;
                P->depth--;
                *last_group = P->group;
                return true;
            }
            if (k == '=' || k == '!' || (k == '<' && (peek_u(P, 2) == '=' || peek_u(P, 2) == '!'))) {
                bool behind = k == '<';
                bool negative = behind ? peek_u(P, 2) == '!' : k == '!';
                P->i += behind ? 3 : 2;
                struct rbuf sub = { 0 };
                parse_disjunction(P, &sub, behind);
                if (peek_u(P, 0) != ')')
                    rfail(P, "missing )");
                P->i++;
                put(P, &sub, R_LOOK_END);
                put(P, b, R_LOOK);
                put(P, b, (behind ? 2 : 0) | (negative ? 1 : 0));
                put(P, b, (int32_t)sub.n);
                append(P, b, &sub);
                free(sub.w);
                P->depth--;
                *last_group = P->group;
                /* Annex B: lookaheads may be quantified outside unicode mode */
                return !P->unicode && !behind;
            }
            if (k == '<') {
                P->i += 2;
                uint32_t len;
                uint16_t *name = group_name(P, &len);
                uint32_t g = P->group++;
                for (uint32_t i = 1; i < g; i++)
                    if (P->re->names[i] && P->re->name_len[i] == len && !memcmp(P->re->names[i], name, len * 2))
                        rfail(P, "duplicate group name");
                P->re->names[g] = name;
                P->re->name_len[g] = len;
                put(P, b, R_SAVE);
                put(P, b, (int32_t)(2 * g + (backward ? 1 : 0)));
                parse_disjunction(P, b, backward);
                put(P, b, R_SAVE);
                put(P, b, (int32_t)(2 * g + (backward ? 0 : 1)));
                if (peek_u(P, 0) != ')')
                    rfail(P, "missing )");
                P->i++;
                P->depth--;
                *last_group = P->group;
                return true;
            }
            rfail(P, "invalid group");
        }
        uint32_t g = P->group++;
        put(P, b, R_SAVE);
        put(P, b, (int32_t)(2 * g + (backward ? 1 : 0)));
        parse_disjunction(P, b, backward);
        put(P, b, R_SAVE);
        put(P, b, (int32_t)(2 * g + (backward ? 0 : 1)));
        if (peek_u(P, 0) != ')')
            rfail(P, "missing )");
        P->i++;
        P->depth--;
        *last_group = P->group;
        return true;
    }
    case ')':
        rfail(P, "unmatched parentheses");
    case '*': case '+': case '?':
        rfail(P, "nothing to repeat");
    case '{':
        if (P->unicode)
            rfail(P, "nothing to repeat");
        {
            uint32_t mn, mx;
            if (parse_braces(P, &mn, &mx))
                rfail(P, "nothing to repeat");
        }
        P->i++;
        emit_char(P, b, '{', backward);
        *last_group = P->group;
        return true;
    case '}': case ']':
        if (P->unicode)
            rfail(P, "lone quantifier brackets");
        P->i++;
        emit_char(P, b, c, backward);
        *last_group = P->group;
        return true;
    case '\\': {
        P->i++;
        if (at_end(P))
            rfail(P, "\\ at end of pattern");
        uint32_t e = P->s[P->i];
        if (e == 'b' || e == 'B') {
            P->i++;
            put(P, b, e == 'b' ? R_WORDB : R_NWORDB);
            return false;
        }
        if (e >= '1' && e <= '9') {
            uint32_t save = P->i;
            bool ok;
            uint32_t n = parse_int(P, &ok);
            if (n < P->total_groups) {
                put(P, b, backward ? R_BACKREF_B : R_BACKREF);
                put(P, b, (int32_t)n);
                *last_group = P->group;
                return true;
            }
            if (P->unicode)
                rfail(P, "invalid escape");
            P->i = save;                        /* Annex B: an octal escape or the digit */
            if (e >= '8') {
                P->i++;
                emit_char(P, b, e, backward);
                *last_group = P->group;
                return true;
            }
        }
        if (e == 'k' && (P->unicode || P->has_names)) {
            P->i++;
            if (peek_u(P, 0) != '<')
                rfail(P, "invalid named reference");
            P->i++;
            uint32_t len;
            uint16_t *name = group_name(P, &len);
            /* resolved after the whole pattern (the group may come later) */
            put(P, b, backward ? R_BACKREF_B : R_BACKREF);
            put(P, b, -1);
            /* keep the name in the next words: length, then units */
            put(P, b, (int32_t)len);
            for (uint32_t k = 0; k < len; k++)
                put(P, b, name[k]);
            free(name);
            *last_group = P->group;
            return true;
        }
        struct cbuild cl = { 0 };
        uint32_t ch;
        if (class_escape(P, &cl, &ch, false)) {
            if (P->flags & RF_I)
                case_close(P, &cl);
            int idx = add_class(P, &cl, false);
            put(P, b, backward ? R_CLASS_B : R_CLASS);
            put(P, b, idx);
        } else {
            free(cl.r);
            emit_char(P, b, ch, backward);
        }
        *last_group = P->group;
        return true;
    }
    }
    emit_char(P, b, take_char(P), backward);
    *last_group = P->group;
    return true;
}

/* Wraps the atom's code (a) in its quantifier into b. */
static void quantify(struct rparse *P, struct rbuf *b, struct rbuf *a, uint32_t min, uint32_t max, bool greedy,
                     uint32_t g0, uint32_t g1)
{
    if (max < min)
        rfail(P, "numbers out of order in {} quantifier");
    if (max == 0)
        return;
    if ((uint64_t)a->n * (min ? min : 1) > (1u << 20) || (max != 0xFFFFFFFF && (uint64_t)a->n * max > (1u << 20)))
        rfail(P, "regular expression too large");
    struct rbuf body = { 0 };
    if (g1 > g0) {
        put(P, &body, R_RESET);
        put(P, &body, (int32_t)(2 * g0));
        put(P, &body, (int32_t)(2 * g1 - 1));
    }
    append(P, &body, a);
    for (uint32_t k = 0; k < min; k++)
        append(P, b, &body);
    if (max == 0xFFFFFFFF) {
        /* L: SPLIT +body, +exit ; MARK r ; body ; CHECK r ; JMP L */
        int32_t r = (int32_t)P->re->nregs++;
        int32_t to_exit = 9 + (int32_t)body.n;  /* SPLIT 3, MARK 2, body, CHECK 2, JMP 2 */
        uint32_t L = b->n;
        put(P, b, R_SPLIT);
        put(P, b, greedy ? 3 : to_exit);
        put(P, b, greedy ? to_exit : 3);
        put(P, b, R_MARK);
        put(P, b, r);
        append(P, b, &body);
        put(P, b, R_CHECK);
        put(P, b, r);
        put(P, b, R_JMP);
        put(P, b, (int32_t)L - (int32_t)b->n + 1);
    } else {
        /* (max - min) optional ones, each inside the one before */
        uint32_t opt = max - min;
        uint32_t *fixups = malloc((opt + 1) * sizeof *fixups);
        if (!fixups)
            abort();
        for (uint32_t k = 0; k < opt; k++) {
            int32_t r = (int32_t)P->re->nregs++;
            uint32_t at = b->n;
            put(P, b, R_SPLIT);
            put(P, b, 0);
            put(P, b, 0);
            fixups[k] = at;
            put(P, b, R_MARK);
            put(P, b, r);
            append(P, b, &body);
            put(P, b, R_CHECK);
            put(P, b, r);
        }
        uint32_t end = b->n;
        for (uint32_t k = 0; k < opt; k++) {
            uint32_t at = fixups[k];
            int32_t to_body = 3, to_exit = (int32_t)(end - at);
            b->w[at + 1] = greedy ? to_body : to_exit;
            b->w[at + 2] = greedy ? to_exit : to_body;
        }
        free(fixups);
    }
    free(body.w);
}

static void parse_alternative(struct rparse *P, struct rbuf *b, bool backward)
{
    /* terms collected, then put in order (or reversed, matching backwards) */
    struct rbuf *terms = 0;
    uint32_t nt = 0, ct = 0;
    while (!at_end(P) && P->s[P->i] != '|' && P->s[P->i] != ')') {
        struct rbuf atom = { 0 }, term = { 0 };
        uint32_t g0, g1;
        bool quantifiable = parse_atom(P, &atom, backward, &g0, &g1);
        uint32_t min = 1, max = 1;
        bool q = false;
        uint32_t c = peek_u(P, 0);
        if (c == '*' || c == '+' || c == '?') {
            P->i++;
            min = c == '+' ? 1 : 0;
            max = c == '?' ? 1 : 0xFFFFFFFF;
            q = true;
        } else if (c == '{') {
            uint32_t mn, mx;
            if (parse_braces(P, &mn, &mx)) {
                min = mn;
                max = mx;
                q = true;
            } else if (P->unicode)
                rfail(P, "incomplete quantifier");
        }
        if (q) {
            if (!quantifiable)
                rfail(P, "nothing to repeat");
            bool greedy = true;
            if (peek_u(P, 0) == '?') {
                P->i++;
                greedy = false;
            }
            quantify(P, &term, &atom, min, max, greedy, g0, g1);
            free(atom.w);
        } else
            term = atom;
        if (nt == ct) {
            ct = ct ? ct * 2 : 8;
            terms = realloc(terms, ct * sizeof *terms);
            if (!terms)
                abort();
        }
        terms[nt++] = term;
    }
    for (uint32_t k = 0; k < nt; k++) {
        struct rbuf *t = &terms[backward ? nt - 1 - k : k];
        append(P, b, t);
        free(t->w);
    }
    free(terms);
}

static void parse_disjunction(struct rparse *P, struct rbuf *b, bool backward)
{
    struct rbuf first = { 0 };
    parse_alternative(P, &first, backward);
    if (peek_u(P, 0) != '|') {
        append(P, b, &first);
        free(first.w);
        return;
    }
    P->i++;
    struct rbuf rest = { 0 };
    parse_disjunction(P, &rest, backward);
    /* SPLIT +first, +rest ; first ; JMP end ; rest */
    put(P, b, R_SPLIT);
    put(P, b, 3);
    put(P, b, 3 + (int32_t)first.n + 2);
    append(P, b, &first);
    put(P, b, R_JMP);
    put(P, b, (int32_t)rest.n + 2);
    append(P, b, &rest);
    free(first.w);
    free(rest.w);
}

static int op_words(int32_t op, const int32_t *w)
{
    switch (op) {
    case R_CHAR: case R_CHAR_I: case R_CLASS: case R_SAVE: case R_MARK: case R_CHECK: case R_JMP:
    case R_CHAR_B: case R_CHAR_I_B: case R_CLASS_B:
        return 2;
    case R_BACKREF: case R_BACKREF_B:
        return w[1] == -1 ? 3 + w[2] : 2;
    case R_SPLIT: case R_RESET: case R_LOOK:
        return 3;
    }
    return 1;
}

/* Counts the capturing groups and finds whether names are used. */
static void prescan(struct rparse *P)
{
    uint32_t groups = 1;
    bool in_class = false;
    for (uint32_t i = 0; i < P->n; i++) {
        uint16_t c = P->s[i];
        if (c == '\\') {
            i++;
            continue;
        }
        if (in_class) {
            if (c == ']')
                in_class = false;
            continue;
        }
        if (c == '[')
            in_class = true;
        else if (c == '(') {
            if (i + 1 < P->n && P->s[i + 1] == '?') {
                if (i + 2 < P->n && P->s[i + 2] == '<' && i + 3 < P->n && P->s[i + 3] != '=' && P->s[i + 3] != '!') {
                    groups++;
                    P->has_names = true;
                }
            } else
                groups++;
        }
    }
    P->total_groups = groups;
}

struct regex *regex_compile(struct js *J, const uint16_t *s, uint32_t n, uint32_t flags, const char **err)
{
    jmp_buf fail;
    struct rparse P = { .J = J, .s = s, .n = n, .flags = flags, .unicode = (flags & (RF_U | RF_V)) != 0 };
    P.fail = &fail;
    struct regex *re = calloc(1, sizeof *re);
    struct rbuf b = { 0 };
    if (!re)
        abort();
    re->flags = flags;
    P.re = re;
    prescan(&P);
    re->names = calloc(P.total_groups, sizeof *re->names);
    re->name_len = calloc(P.total_groups, sizeof *re->name_len);
    P.group = 1;
    if (setjmp(fail)) {
        free(b.w);
        *err = P.error;
        re->ngroups = P.total_groups;
        regex_free(re);
        return 0;
    }
    put(&P, &b, R_SAVE);
    put(&P, &b, 0);
    parse_disjunction(&P, &b, false);
    if (!at_end(&P))
        rfail(&P, "unmatched parentheses");
    put(&P, &b, R_SAVE);
    put(&P, &b, 1);
    put(&P, &b, R_MATCH);
    re->ngroups = P.group;
    /* named back references: their group numbers */
    for (uint32_t pc = 0, words; pc < b.n; pc += words) {
        words = (uint32_t)op_words(b.w[pc], b.w + pc);
        if ((b.w[pc] == R_BACKREF || b.w[pc] == R_BACKREF_B) && b.w[pc + 1] == -1) {
            uint32_t len = (uint32_t)b.w[pc + 2];
            int found = -1;
            for (uint32_t g = 1; g < re->ngroups; g++)
                if (re->names[g] && re->name_len[g] == len) {
                    bool same = true;
                    for (uint32_t k = 0; k < len; k++)
                        if (re->names[g][k] != (uint16_t)b.w[pc + 3 + k])
                            same = false;
                    if (same)
                        found = (int)g;
                }
            if (found < 0)
                rfail(&P, "invalid named capture referenced");
            b.w[pc + 1] = found;                /* then a jump over the name */
            b.w[pc + 2] = R_JMP;
            b.w[pc + 3] = (int32_t)len + 1;
        }
    }
    re->code = b.w;
    re->len = b.n;
    *err = 0;
    return re;
}

void regex_free(struct regex *re)
{
    if (!re)
        return;
    free(re->code);
    for (uint32_t i = 0; i < re->nclasses; i++)
        free(re->classes[i].r);
    free(re->classes);
    if (re->names)
        for (uint32_t i = 0; i < re->ngroups; i++)
            free(re->names[i]);
    free(re->names);
    free(re->name_len);
    free(re);
}

uint32_t regex_ngroups(struct regex *re) { return re->ngroups; }
uint32_t regex_flags(struct regex *re) { return re->flags; }
const uint16_t *regex_group_name(struct regex *re, uint32_t g, uint32_t *len)
{
    *len = re->name_len[g];
    return re->names[g];
}

/* --- matching ----------------------------------------------------------- */

enum { BT_POS, BT_SLOT, BT_REG };

struct bt {
    int32_t kind;
    int32_t a;                                  /* pc, slot or register */
    int32_t b;                                  /* pos or the old value */
};

struct rexec {
    struct regex *re;
    const uint16_t *s;
    int32_t n;
    int32_t *cap;
    int32_t *regs;
    struct bt *bt;
    uint32_t nbt, cbt;
    uint64_t steps;
    bool unicode, icase;
    bool aborted;
};

static void push(struct rexec *X, int32_t kind, int32_t a, int32_t b)
{
    if (X->nbt == X->cbt) {
        uint32_t cap = X->cbt ? X->cbt * 2 : 256;
        if (cap > (1u << 26)) {
            X->aborted = true;
            return;
        }
        X->bt = realloc(X->bt, cap * sizeof *X->bt);
        if (!X->bt)
            abort();
        X->cbt = cap;
    }
    X->bt[X->nbt].kind = kind;
    X->bt[X->nbt].a = a;
    X->bt[X->nbt].b = b;
    X->nbt++;
}

static bool is_word(struct rexec *X, uint32_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           (X->unicode && X->icase && (c == 0x17F || c == 0x212A));
}

static bool is_lt(uint32_t c)
{
    return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029;
}

static uint32_t char_at(struct rexec *X, int32_t pos, int32_t *next)
{
    uint32_t c = X->s[pos];
    if (X->unicode && c >= 0xD800 && c <= 0xDBFF && pos + 1 < X->n && X->s[pos + 1] >= 0xDC00 &&
        X->s[pos + 1] <= 0xDFFF) {
        *next = pos + 2;
        return 0x10000 + ((c - 0xD800) << 10) + (X->s[pos + 1] - 0xDC00);
    }
    *next = pos + 1;
    return c;
}

static uint32_t char_before(struct rexec *X, int32_t pos, int32_t *prev)
{
    uint32_t c = X->s[pos - 1];
    if (X->unicode && c >= 0xDC00 && c <= 0xDFFF && pos >= 2 && X->s[pos - 2] >= 0xD800 && X->s[pos - 2] <= 0xDBFF) {
        *prev = pos - 2;
        return 0x10000 + ((X->s[pos - 2] - 0xD800) << 10) + (c - 0xDC00);
    }
    *prev = pos - 1;
    return c;
}

static bool in_class(struct rclass *c, uint32_t ch)
{
    uint32_t lo = 0, hi = c->n;
    bool found = false;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (ch < c->r[2 * mid])
            hi = mid;
        else if (ch > c->r[2 * mid + 1])
            lo = mid + 1;
        else {
            found = true;
            break;
        }
    }
    return found != c->neg;
}

/* what a class looks for: with /i the canonical form (case_close) */
static uint32_t class_char(struct rexec *X, uint32_t ch)
{
    return X->icase ? canon(ch, X->unicode) : ch;
}

static int32_t run(struct rexec *X, int32_t pc, int32_t pos);

static int32_t run(struct rexec *X, int32_t pc, int32_t pos)
{
    const int32_t *code = X->re->code;
    uint32_t base = X->nbt;
    int32_t n = X->n;
    for (;;) {
        if (++X->steps > 200000000u) {
            X->aborted = true;
            return -1;
        }
        int32_t next;
        uint32_t c;
        switch (code[pc]) {
        case R_CHAR:
            if (pos < n && (c = char_at(X, pos, &next)) == (uint32_t)code[pc + 1]) {
                pos = next;
                pc += 2;
                continue;
            }
            goto fail;
        case R_CHAR_I:
            if (pos < n && canon(char_at(X, pos, &next), X->unicode) == (uint32_t)code[pc + 1]) {
                pos = next;
                pc += 2;
                continue;
            }
            goto fail;
        case R_CHAR_B:
            if (pos > 0 && char_before(X, pos, &next) == (uint32_t)code[pc + 1]) {
                pos = next;
                pc += 2;
                continue;
            }
            goto fail;
        case R_CHAR_I_B:
            if (pos > 0 && canon(char_before(X, pos, &next), X->unicode) == (uint32_t)code[pc + 1]) {
                pos = next;
                pc += 2;
                continue;
            }
            goto fail;
        case R_ANY:
            if (pos < n && !is_lt(char_at(X, pos, &next))) {
                pos = next;
                pc++;
                continue;
            }
            goto fail;
        case R_ANY_ALL:
            if (pos < n) {
                char_at(X, pos, &next);
                pos = next;
                pc++;
                continue;
            }
            goto fail;
        case R_ANY_B:
            if (pos > 0 && !is_lt(char_before(X, pos, &next))) {
                pos = next;
                pc++;
                continue;
            }
            goto fail;
        case R_ANY_ALL_B:
            if (pos > 0) {
                char_before(X, pos, &next);
                pos = next;
                pc++;
                continue;
            }
            goto fail;
        case R_CLASS:
            if (pos < n && in_class(&X->re->classes[code[pc + 1]], class_char(X, char_at(X, pos, &next)))) {
                pos = next;
                pc += 2;
                continue;
            }
            goto fail;
        case R_CLASS_B:
            if (pos > 0 && in_class(&X->re->classes[code[pc + 1]], class_char(X, char_before(X, pos, &next)))) {
                pos = next;
                pc += 2;
                continue;
            }
            goto fail;
        case R_BOL:
            if (pos == 0) {
                pc++;
                continue;
            }
            goto fail;
        case R_EOL:
            if (pos == n) {
                pc++;
                continue;
            }
            goto fail;
        case R_BOL_M:
            if (pos == 0 || is_lt(X->s[pos - 1])) {
                pc++;
                continue;
            }
            goto fail;
        case R_EOL_M:
            if (pos == n || is_lt(X->s[pos])) {
                pc++;
                continue;
            }
            goto fail;
        case R_WORDB: case R_NWORDB: {
            bool a = pos > 0 && is_word(X, X->s[pos - 1]), b = pos < n && is_word(X, X->s[pos]);
            if ((a != b) == (code[pc] == R_WORDB)) {
                pc++;
                continue;
            }
            goto fail;
        }
        case R_SPLIT:
            push(X, BT_POS, pc + code[pc + 2], pos);
            if (X->aborted)
                return -1;
            pc += code[pc + 1];
            continue;
        case R_JMP:
            pc += code[pc + 1];
            continue;
        case R_SAVE: {
            int32_t slot = code[pc + 1];
            push(X, BT_SLOT, slot, X->cap[slot]);
            X->cap[slot] = pos;
            pc += 2;
            continue;
        }
        case R_RESET:
            for (int32_t s = code[pc + 1]; s <= code[pc + 2]; s++) {
                if (X->cap[s] != -1) {
                    push(X, BT_SLOT, s, X->cap[s]);
                    X->cap[s] = -1;
                }
            }
            pc += 3;
            continue;
        case R_MARK: {
            int32_t r = code[pc + 1];
            push(X, BT_REG, r, X->regs[r]);
            X->regs[r] = pos;
            pc += 2;
            continue;
        }
        case R_CHECK:
            if (X->regs[code[pc + 1]] == pos)
                goto fail;
            pc += 2;
            continue;
        case R_BACKREF: case R_BACKREF_B: {
            int32_t g = code[pc + 1];
            int words = op_words(code[pc], code + pc);
            int32_t s0 = X->cap[2 * g], s1 = X->cap[2 * g + 1];
            if (s0 < 0 || s1 < 0) {
                pc += words;
                continue;
            }
            int32_t len = s1 - s0;
            bool back = code[pc] == R_BACKREF_B;
            if (X->icase && X->unicode) {
                /* by code points: a pair compares as one character */
                int32_t i = back ? s1 : s0, j = pos, ni, nj;
                while (back ? i > s0 : i < s1) {
                    if (back ? j <= 0 : j >= n)
                        goto fail;
                    uint32_t a = back ? char_before(X, i, &ni) : char_at(X, i, &ni);
                    uint32_t b = back ? char_before(X, j, &nj) : char_at(X, j, &nj);
                    if (canon(a, true) != canon(b, true))
                        goto fail;
                    i = ni, j = nj;
                }
                pos = j;
                pc += words;
                continue;
            }
            int32_t from = back ? pos - len : pos;
            if (from < 0 || from + len > n)
                goto fail;
            bool ok = true;
            for (int32_t k = 0; k < len && ok; k++) {
                uint32_t a = X->s[s0 + k], b = X->s[from + k];
                ok = X->icase ? canon(a, X->unicode) == canon(b, X->unicode) : a == b;
            }
            if (!ok)
                goto fail;
            pos = back ? from : from + len;
            pc += words;
            continue;
        }
        case R_LOOK: {
            int kind = code[pc + 1];
            int32_t len = code[pc + 2];
            int32_t sub = pc + 3;
            uint32_t ng = X->re->ngroups * 2;
            int32_t *saved = malloc(ng * sizeof *saved);
            if (!saved)
                abort();
            memcpy(saved, X->cap, ng * sizeof *saved);
            int32_t r = run(X, sub, pos);
            if (X->aborted) {
                free(saved);
                return -1;
            }
            bool negative = kind & 1;
            if ((r >= 0) == negative) {
                memcpy(X->cap, saved, ng * sizeof *saved);
                free(saved);
                goto fail;
            }
            if (negative)
                memcpy(X->cap, saved, ng * sizeof *saved);
            else
                for (uint32_t s = 0; s < ng; s++)
                    if (saved[s] != X->cap[s])
                        push(X, BT_SLOT, (int32_t)s, saved[s]);
            free(saved);
            pc = sub + len;
            continue;
        }
        case R_LOOK_END:
        case R_MATCH: {
            /* drop this run's backtracking points; keep its captures */
            X->nbt = base;
            return pos;
        }
        }
    fail:
        for (;;) {
            if (X->nbt == base)
                return -1;
            struct bt *t = &X->bt[--X->nbt];
            if (t->kind == BT_SLOT)
                X->cap[t->a] = t->b;
            else if (t->kind == BT_REG)
                X->regs[t->a] = t->b;
            else {
                pc = t->a;
                pos = t->b;
                break;
            }
        }
    }
}

/* Matches at `start` only (the caller scans). 1 matched (caps filled),
 * 0 not, -1 too much backtracking. */
int regex_match(struct regex *re, const uint16_t *s, uint32_t n, uint32_t start, int32_t *caps)
{
    struct rexec X = { .re = re, .s = s, .n = (int32_t)n, .cap = caps,
                       .unicode = (re->flags & (RF_U | RF_V)) != 0, .icase = (re->flags & RF_I) != 0 };
    for (uint32_t i = 0; i < re->ngroups * 2; i++)
        caps[i] = -1;
    int32_t regs_small[64];
    X.regs = re->nregs <= 64 ? regs_small : malloc(re->nregs * sizeof *X.regs);
    if (!X.regs)
        abort();
    for (uint32_t i = 0; i < re->nregs; i++)
        X.regs[i] = -1;
    int32_t r = run(&X, 0, (int32_t)start);
    free(X.bt);
    if (X.regs != regs_small)
        free(X.regs);
    if (X.aborted)
        return -1;
    return r >= 0;
}

/* Scans from `start` (sticky: only there). 1 found (caps), 0 none, -1 aborted. */
int regex_exec(struct regex *re, const uint16_t *s, uint32_t n, uint32_t start, bool sticky, int32_t *caps)
{
    /* a pattern starting with a plain character: skip to it */
    int32_t first = -1;
    if (re->len > 3 && re->code[0] == R_SAVE && re->code[2] == R_CHAR && re->code[3] < 0x10000)
        first = re->code[3];
    for (uint32_t i = start; i <= n; i++) {
        if (first >= 0 && !sticky) {
            while (i < n && s[i] != first)
                i++;
            if (i >= n)
                return 0;
        }
        int r = regex_match(re, s, n, i, caps);
        if (r != 0)
            return r;
        if (sticky)
            return 0;
        if ((re->flags & (RF_U | RF_V)) && i + 1 < n && s[i] >= 0xD800 && s[i] <= 0xDBFF && s[i + 1] >= 0xDC00 &&
            s[i + 1] <= 0xDFFF)
            i++;
    }
    return 0;
}

uint32_t regex_parse_flags(const uint16_t *f, uint32_t n, bool *ok)
{
    uint32_t flags = 0;
    *ok = true;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t bit;
        switch (f[i]) {
        case 'g': bit = RF_G; break;
        case 'i': bit = RF_I; break;
        case 'm': bit = RF_M; break;
        case 's': bit = RF_S; break;
        case 'u': bit = RF_U; break;
        case 'y': bit = RF_Y; break;
        case 'd': bit = RF_D; break;
        case 'v': bit = RF_V; break;
        default: *ok = false; return 0;
        }
        if (flags & bit) {
            *ok = false;
            return 0;
        }
        flags |= bit;
    }
    if ((flags & RF_U) && (flags & RF_V))
        *ok = false;
    return flags;
}

const char *regex_check(struct js *J, struct jsstring *src, struct jsstring *flags)
{
    bool ok;
    struct jsstring *ff = str_flat(J, flags);
    uint32_t fl = regex_parse_flags(ff->chars, str_len(ff), &ok);
    if (!ok)
        return "invalid flags";
    struct jsstring *sf = str_flat(J, src);
    const char *err;
    struct regex *re = regex_compile(J, sf->chars, str_len(sf), fl, &err);
    if (!re)
        return err;
    regex_free(re);
    return 0;
}

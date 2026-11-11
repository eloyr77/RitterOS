/*
 * Strings and atoms (27o). A string is UTF-16 code units, as the language
 * counts them; text comes in and goes out as UTF-8 (a lone surrogate goes
 * out as U+FFFD). A concatenation of 32 units or more is a rope (left and
 * right) until its characters are wanted: then it is copied once, without
 * recursion (a loop of `s += x` makes a rope as deep as the loop is long),
 * and the rope keeps the flat copy.
 *
 * Atoms are the strings used as property names, one per text, in an open
 * hash table that the collector rebuilds after marking (atoms nothing holds
 * any more are dropped).
 */
#include "jsint.h"

struct jsstring *str_alloc(struct js *J, uint32_t len)
{
    struct jsstring *s = gc_alloc(J, CELL_STRING, sizeof(struct jsstring) + (size_t)len * 2);
    if (!s) {
        fprintf(stderr, "js: out of memory\n");
        abort();
    }
    s->h.extra = len;
    return s;
}

struct jsstring *str_flat(struct js *J, struct jsstring *s)
{
    if (!(s->h.flags & STR_ROPE))
        return s;
    if (s->h.flags & STR_FLATTENED)
        return s->flat;
    uint32_t n = str_len(s);
    struct jsstring *f = str_alloc(J, n);
    struct jsstring **stack = malloc(64 * sizeof *stack);
    uint32_t depth = 0, cap = 64, pos = 0;
    if (!stack)
        abort();
    stack[depth++] = s;
    while (depth) {
        struct jsstring *x = stack[--depth];
        if ((x->h.flags & STR_ROPE) && !(x->h.flags & STR_FLATTENED)) {
            if (depth + 2 > cap) {
                cap *= 2;
                stack = realloc(stack, cap * sizeof *stack);
                if (!stack)
                    abort();
            }
            stack[depth++] = x->right;
            stack[depth++] = x->left;
            continue;
        }
        struct jsstring *src = (x->h.flags & STR_ROPE) ? x->flat : x;
        memcpy(f->chars + pos, src->chars, (size_t)str_len(src) * 2);
        pos += str_len(src);
    }
    free(stack);
    s->flat = f;
    s->left = s->right = 0;
    s->h.flags |= STR_FLATTENED;
    return f;
}

const uint16_t *str_chars(struct js *J, struct jsstring *s)
{
    return str_flat(J, s)->chars;
}

struct jsstring *str_from_u16(struct js *J, const uint16_t *u, uint32_t n)
{
    if (n == 1 && u[0] < 256)
        return str_from_char(J, u[0]);
    if (n == 0)
        return J->atom[A_empty];
    struct jsstring *s = str_alloc(J, n);
    memcpy(s->chars, u, (size_t)n * 2);
    return s;
}

struct jsstring *str_from_char(struct js *J, uint32_t unit)
{
    if (unit < 256) {
        if (!J->char_strings[unit]) {
            struct jsstring *s = str_alloc(J, 1);
            s->chars[0] = (uint16_t)unit;
            J->char_strings[unit] = s;
        }
        return J->char_strings[unit];
    }
    struct jsstring *s = str_alloc(J, 1);
    s->chars[0] = (uint16_t)unit;
    return s;
}

/* Decodes one UTF-8 character at s[*i] (n bytes); invalid bytes give U+FFFD. */
static uint32_t utf8_decode(const unsigned char *s, size_t n, size_t *i)
{
    unsigned char c = s[*i];
    if (c < 0x80) {
        (*i)++;
        return c;
    }
    int need;
    uint32_t cp, min;
    if (c >= 0xC2 && c <= 0xDF)
        need = 1, cp = c & 0x1F, min = 0x80;
    else if (c >= 0xE0 && c <= 0xEF)
        need = 2, cp = c & 0x0F, min = 0x800;
    else if (c >= 0xF0 && c <= 0xF4)
        need = 3, cp = c & 0x07, min = 0x10000;
    else {
        (*i)++;
        return 0xFFFD;
    }
    size_t j = *i + 1;
    for (int k = 0; k < need; k++, j++) {
        if (j >= n || (s[j] & 0xC0) != 0x80) {
            *i = j;
            return 0xFFFD;
        }
        cp = cp << 6 | (s[j] & 0x3F);
    }
    *i = j;
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0xFFFD;
    return cp;
}

struct jsstring *str_from_utf8(struct js *J, const char *text, size_t n)
{
    const unsigned char *s = (const unsigned char *)text;
    size_t units = 0;
    bool ascii = true;
    for (size_t i = 0; i < n;) {
        if (s[i] < 0x80) {
            units++;
            i++;
            continue;
        }
        ascii = false;
        uint32_t cp = utf8_decode(s, n, &i);
        units += cp >= 0x10000 ? 2 : 1;
    }
    if (units > STR_MAX_LEN)
        units = STR_MAX_LEN;
    if (units == 0)
        return J->atom[A_empty];
    if (units == 1 && ascii)
        return str_from_char(J, s[0]);
    struct jsstring *r = str_alloc(J, (uint32_t)units);
    uint32_t k = 0;
    for (size_t i = 0; i < n && k < units;) {
        uint32_t cp = s[i] < 0x80 ? s[i++] : utf8_decode(s, n, &i);
        if (cp >= 0x10000) {
            if (k + 2 > units)
                break;
            cp -= 0x10000;
            r->chars[k++] = (uint16_t)(0xD800 + (cp >> 10));
            r->chars[k++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else
            r->chars[k++] = (uint16_t)cp;
    }
    return r;
}

struct jsstring *str_from_ascii(struct js *J, const char *s)
{
    return str_from_utf8(J, s, strlen(s));
}

char *str_to_utf8(struct js *J, struct jsstring *str, size_t *lenp)
{
    struct jsstring *f = str_flat(J, str);
    uint32_t n = str_len(f);
    const uint16_t *u = f->chars;
    size_t bytes = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = u[i];
        if (c < 0x80)
            bytes += 1;
        else if (c < 0x800)
            bytes += 2;
        else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF)
            bytes += 4, i++;
        else
            bytes += 3;
    }
    char *out = malloc(bytes + 1);
    if (!out)
        abort();
    size_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = u[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF)
            c = 0xFFFD;
        if (c < 0x80)
            out[k++] = (char)c;
        else if (c < 0x800) {
            out[k++] = (char)(0xC0 | c >> 6);
            out[k++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out[k++] = (char)(0xE0 | c >> 12);
            out[k++] = (char)(0x80 | (c >> 6 & 0x3F));
            out[k++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[k++] = (char)(0xF0 | c >> 18);
            out[k++] = (char)(0x80 | (c >> 12 & 0x3F));
            out[k++] = (char)(0x80 | (c >> 6 & 0x3F));
            out[k++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[k] = 0;
    if (lenp)
        *lenp = k;
    return out;
}

struct jsstring *str_concat(struct js *J, struct jsstring *a, struct jsstring *b)
{
    uint32_t na = str_len(a), nb = str_len(b);
    if (na == 0)
        return b;
    if (nb == 0)
        return a;
    if ((uint64_t)na + nb > STR_MAX_LEN) {
        throw_range(J, "Out of memory");
        return 0;
    }
    if (na + nb < 32) {
        struct jsstring *fa = str_flat(J, a), *fb = str_flat(J, b);
        struct jsstring *s = str_alloc(J, na + nb);
        memcpy(s->chars, fa->chars, (size_t)na * 2);
        memcpy(s->chars + na, fb->chars, (size_t)nb * 2);
        return s;
    }
    struct jsstring *s = gc_alloc(J, CELL_STRING, sizeof(struct jsstring));
    if (!s)
        abort();
    s->h.flags = STR_ROPE;
    s->h.extra = na + nb;
    s->left = a;
    s->right = b;
    return s;
}

struct jsstring *str_sub(struct js *J, struct jsstring *s, uint32_t from, uint32_t to)
{
    if (from >= to)
        return J->atom[A_empty];
    if (from == 0 && to == str_len(s))
        return s;
    struct jsstring *f = str_flat(J, s);
    return str_from_u16(J, f->chars + from, to - from);
}

bool str_equal(struct js *J, struct jsstring *a, struct jsstring *b)
{
    if (a == b)
        return true;
    if (str_len(a) != str_len(b))
        return false;
    if ((a->h.flags & STR_ATOM) && (b->h.flags & STR_ATOM))
        return false;
    if ((a->h.flags & STR_HASHED) && (b->h.flags & STR_HASHED) && a->hash != b->hash)
        return false;
    struct jsstring *fa = str_flat(J, a), *fb = str_flat(J, b);
    return memcmp(fa->chars, fb->chars, (size_t)str_len(a) * 2) == 0;
}

int str_compare(struct js *J, struct jsstring *a, struct jsstring *b)
{
    if (a == b)
        return 0;
    struct jsstring *fa = str_flat(J, a), *fb = str_flat(J, b);
    uint32_t na = str_len(fa), nb = str_len(fb), n = na < nb ? na : nb;
    for (uint32_t i = 0; i < n; i++)
        if (fa->chars[i] != fb->chars[i])
            return fa->chars[i] < fb->chars[i] ? -1 : 1;
    return na < nb ? -1 : na > nb ? 1 : 0;
}

static uint32_t hash_units(const uint16_t *u, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++)
        h = (h ^ u[i]) * 16777619u;
    return h ? h : 1;
}

uint32_t str_hash(struct js *J, struct jsstring *s)
{
    if (s->h.flags & STR_HASHED)
        return s->hash;
    struct jsstring *f = str_flat(J, s);
    uint32_t h = hash_units(f->chars, str_len(f));
    s->hash = h;
    s->h.flags |= STR_HASHED;
    return h;
}

uint32_t str_char_at(struct js *J, struct jsstring *s, uint32_t i)
{
    return str_flat(J, s)->chars[i];
}

int32_t str_index_of(struct js *J, struct jsstring *s, struct jsstring *what, uint32_t from)
{
    struct jsstring *fs = str_flat(J, s), *fw = str_flat(J, what);
    uint32_t n = str_len(fs), m = str_len(fw);
    if (m == 0)
        return from <= n ? (int32_t)from : (int32_t)n;
    if (m > n)
        return -1;
    for (uint32_t i = from; i + m <= n; i++)
        if (fs->chars[i] == fw->chars[0] && memcmp(fs->chars + i, fw->chars, (size_t)m * 2) == 0)
            return (int32_t)i;
    return -1;
}

bool str_is_ascii_cstr(struct js *J, struct jsstring *s, const char *c)
{
    size_t n = strlen(c);
    if (str_len(s) != n)
        return false;
    const uint16_t *u = str_chars(J, s);
    for (size_t i = 0; i < n; i++)
        if (u[i] != (unsigned char)c[i])
            return false;
    return true;
}

/* --- atoms ------------------------------------------------------------- */

bool parse_index(const uint16_t *s, uint32_t n, uint32_t *index)
{
    if (n == 0 || n > 10)
        return false;
    if (s[0] == '0') {
        if (n != 1)
            return false;
        *index = 0;
        return true;
    }
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10 + (s[i] - '0');
    }
    if (v > 4294967294u)
        return false;
    *index = (uint32_t)v;
    return true;
}

static bool atoms_grow(struct js *J)
{
    uint32_t cap = J->atoms_cap ? J->atoms_cap * 2 : 1024;
    struct jsstring **t = calloc(cap, sizeof *t);
    if (!t)
        return false;
    for (uint32_t i = 0; i < J->atoms_cap; i++) {
        struct jsstring *a = J->atoms[i];
        if (!a)
            continue;
        uint32_t k = a->hash & (cap - 1);
        while (t[k])
            k = (k + 1) & (cap - 1);
        t[k] = a;
    }
    free(J->atoms);
    J->atoms = t;
    J->atoms_cap = cap;
    return true;
}

static struct jsstring *atom_lookup(struct js *J, const uint16_t *u, uint32_t n, uint32_t h)
{
    if (!J->atoms_cap)
        return 0;
    for (uint32_t k = h & (J->atoms_cap - 1);; k = (k + 1) & (J->atoms_cap - 1)) {
        struct jsstring *a = J->atoms[k];
        if (!a)
            return 0;
        if (a->hash == h && str_len(a) == n && memcmp(a->chars, u, (size_t)n * 2) == 0)
            return a;
    }
}

static void atom_insert(struct js *J, struct jsstring *s)
{
    if ((J->natoms + 1) * 2 > J->atoms_cap && !atoms_grow(J))
        abort();
    uint32_t k = s->hash & (J->atoms_cap - 1);
    while (J->atoms[k])
        k = (k + 1) & (J->atoms_cap - 1);
    J->atoms[k] = s;
    J->natoms++;
    s->h.flags |= STR_ATOM;
    uint32_t index;
    if (parse_index(s->chars, str_len(s), &index)) {
        s->h.flags |= STR_INDEX;
        s->index = index;
    }
}

struct jsstring *atom(struct js *J, struct jsstring *s)
{
    if (s->h.flags & STR_ATOM)
        return s;
    struct jsstring *f = str_flat(J, s);
    if (f->h.flags & STR_ATOM)
        return f;
    uint32_t h = str_hash(J, f);
    struct jsstring *a = atom_lookup(J, f->chars, str_len(f), h);
    if (a)
        return a;
    atom_insert(J, f);
    return f;
}

struct jsstring *atom_u16(struct js *J, const uint16_t *u, uint32_t n)
{
    uint32_t h = hash_units(u, n);
    struct jsstring *a = atom_lookup(J, u, n, h);
    if (a)
        return a;
    struct jsstring *s = n == 0 ? 0 : (n == 1 && u[0] < 256) ? str_from_char(J, u[0]) : 0;
    if (s && (s->h.flags & STR_ATOM))
        return s;
    if (!s) {
        s = str_alloc(J, n);
        memcpy(s->chars, u, (size_t)n * 2);
    }
    s->hash = h;
    s->h.flags |= STR_HASHED;
    atom_insert(J, s);
    return s;
}

struct jsstring *atom_cstr(struct js *J, const char *ascii)
{
    uint16_t buf[64];
    size_t n = strlen(ascii);
    if (n > 64) {
        struct jsstring *s = str_from_ascii(J, ascii);
        return atom(J, s);
    }
    for (size_t i = 0; i < n; i++)
        buf[i] = (unsigned char)ascii[i];
    return atom_u16(J, buf, (uint32_t)n);
}

struct jsstring *atom_index(struct js *J, uint32_t index)
{
    uint16_t buf[12];
    char tmp[12];
    int n = snprintf(tmp, sizeof tmp, "%u", index);
    for (int i = 0; i < n; i++)
        buf[i] = (uint16_t)tmp[i];
    return atom_u16(J, buf, (uint32_t)n);
}

void atoms_sweep(struct js *J)
{
    uint32_t cap = J->atoms_cap;
    if (!cap)
        return;
    struct jsstring **old = J->atoms;
    struct jsstring **t = calloc(cap, sizeof *t);
    if (!t)
        abort();
    uint32_t live = 0;
    for (uint32_t i = 0; i < cap; i++) {
        struct jsstring *a = old[i];
        if (!a)
            continue;
        if (!a->h.mark) {
            a->h.flags &= ~(STR_ATOM | STR_INDEX);   /* (swept next) */
            continue;
        }
        uint32_t k = a->hash & (cap - 1);
        while (t[k])
            k = (k + 1) & (cap - 1);
        t[k] = a;
        live++;
    }
    free(old);
    J->atoms = t;
    J->natoms = live;
}

/* --- building strings ---------------------------------------------------- */

static bool sb_room(struct sbuf *b, uint32_t more)
{
    if (b->failed)
        return false;
    if ((uint64_t)b->n + more > STR_MAX_LEN) {
        b->failed = true;
        return false;
    }
    if (b->n + more <= b->cap)
        return true;
    uint32_t cap = b->cap ? b->cap : 16;
    while (cap < b->n + more)
        cap = cap > STR_MAX_LEN / 2 ? STR_MAX_LEN : cap * 2;
    uint16_t *s = realloc(b->s, (size_t)cap * 2);
    if (!s) {
        b->failed = true;
        return false;
    }
    b->s = s;
    b->cap = cap;
    return true;
}

void sb_unit(struct sbuf *b, uint32_t unit)
{
    if (sb_room(b, 1))
        b->s[b->n++] = (uint16_t)unit;
}

void sb_code_point(struct sbuf *b, uint32_t cp)
{
    if (cp >= 0x10000) {
        cp -= 0x10000;
        sb_unit(b, 0xD800 + (cp >> 10));
        sb_unit(b, 0xDC00 + (cp & 0x3FF));
    } else
        sb_unit(b, cp);
}

void sb_ascii(struct sbuf *b, const char *s)
{
    size_t n = strlen(s);
    if (!sb_room(b, (uint32_t)n))
        return;
    for (size_t i = 0; i < n; i++)
        b->s[b->n++] = (unsigned char)s[i];
}

void sb_units(struct sbuf *b, const uint16_t *s, uint32_t n)
{
    if (n && sb_room(b, n)) {
        memcpy(b->s + b->n, s, (size_t)n * 2);
        b->n += n;
    }
}

void sb_str(struct js *J, struct sbuf *b, struct jsstring *s)
{
    struct jsstring *f = str_flat(J, s);
    sb_units(b, f->chars, str_len(f));
}

struct jsstring *sb_finish(struct js *J, struct sbuf *b)
{
    if (b->failed) {
        sb_free(b);
        throw_range(J, "Out of memory");
        return 0;
    }
    struct jsstring *s = str_from_u16(J, b->s, b->n);
    sb_free(b);
    return s;
}

void sb_free(struct sbuf *b)
{
    free(b->s);
    b->s = 0;
    b->n = b->cap = 0;
}

/* --- characters ---------------------------------------------------------- */

bool js_is_line_terminator(uint32_t c)
{
    return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029;
}

bool js_is_space(uint32_t c)
{
    switch (c) {
    case 9: case 10: case 11: case 12: case 13: case 32: case 0xA0: case 0x1680:
    case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
        return true;
    }
    return c >= 0x2000 && c <= 0x200A;
}

/* Case mapping (27s-2): the Unicode Character Database's tables
 * (casetab.h, made by scripts/casetab.py). */
struct case_run {
    uint32_t at;        /* first | (step - 1) << 21 | (count - 1) << 22 */
    int32_t delta;
};

struct case_special {
    uint32_t c, upper[3], lower[3];
};

#include "casetab.h"

static uint32_t run_map(const struct case_run *r, size_t n, uint32_t c)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        uint32_t first = r[mid].at & 0x1FFFFF, step = (r[mid].at >> 21 & 1) + 1, count = (r[mid].at >> 22) + 1;
        if (c < first)
            hi = mid;
        else if (c > first + (count - 1) * step)
            lo = mid + 1;
        else
            return (c - first) % step ? c : (uint32_t)((int32_t)c + r[mid].delta);
    }
    return c;
}

static bool in_ranges(const uint32_t (*r)[2], size_t n, uint32_t c)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c < r[mid][0])
            hi = mid;
        else if (c > r[mid][1])
            lo = mid + 1;
        else
            return true;
    }
    return false;
}

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

uint32_t js_case_upper(uint32_t c)
{
    return c < 'a' ? c : c <= 'z' ? c - 32 : c < 0xB5 ? c : run_map(case_upper, COUNT(case_upper), c);
}

uint32_t js_case_lower(uint32_t c)
{
    return c < 'A' ? c : c <= 'Z' ? c + 32 : c < 0xC0 ? c : run_map(case_lower, COUNT(case_lower), c);
}

uint32_t js_case_fold(uint32_t c)
{
    return c < 'A' ? c : c <= 'Z' ? c + 32 : c < 0xB5 ? c : run_map(case_fold, COUNT(case_fold), c);
}

/* the full mapping of c (one to three code points into out): the
 * unconditional ones of SpecialCasing.txt, else the simple one */
int js_case_full(uint32_t c, bool upper, uint32_t out[3])
{
    if (c >= 0xDF) {
        size_t lo = 0, hi = COUNT(case_special);
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (c < case_special[mid].c)
                hi = mid;
            else if (c > case_special[mid].c)
                lo = mid + 1;
            else {
                const uint32_t *m = upper ? case_special[mid].upper : case_special[mid].lower;
                int n = 0;
                while (n < 3 && m[n])
                    out[n] = m[n], n++;
                return n;
            }
        }
    }
    out[0] = upper ? js_case_upper(c) : js_case_lower(c);
    return 1;
}

bool js_case_cased(uint32_t c)
{
    return in_ranges(case_cased, COUNT(case_cased), c);
}

bool js_case_ignorable(uint32_t c)
{
    return in_ranges(case_ignorable, COUNT(case_ignorable), c);
}

/* the i-th range of code points some mapping changes (false past the last):
 * everything else is its own upper, lower and folded case */
bool js_case_changing(size_t i, uint32_t *lo, uint32_t *hi)
{
    if (i >= COUNT(case_changing))
        return false;
    *lo = case_changing[i][0];
    *hi = case_changing[i][1];
    return true;
}

/*
 * Floating point to text and back, exactly (step 22q): printf's %f %e %g %a
 * and strtod/strtof. A double is m * 2^e exactly, so its decimal digits are
 * those of the integer m * 2^e (e >= 0) or m * 5^-e (e < 0, the point -e
 * places from the right): big integers give every digit, and rounding to
 * the precision asked for is then to nearest with ties to even on the exact
 * value, as C (and macOS's and glibc's printf) do. Text to a double is the
 * same in reverse: the decimal digits as a big integer D, D * 10^E or D *
 * 2^s / 10^-E, the top 64 bits and whether anything below them was not zero,
 * then one rounding to the 53 (or 24) bits of the type, subnormals and
 * overflow included. Our own code; no tables, no floating-point arithmetic
 * in the conversions themselves. Everything is on the caller's stack (some
 * 6 KiB at most), so threads may print at once.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "fpconv.h"

/* --- Big integers: base 2^32 limbs, the lowest first --- */

#define LIMBS 160                   /* 5120 bits: more than any value here needs */

struct big {
    int n;                          /* limbs in use (no zero limb on top) */
    uint32_t d[LIMBS];
};

static void big_set(struct big *a, uint64_t v)
{
    a->n = 0;
    while (v) {
        a->d[a->n++] = (uint32_t)v;
        v >>= 32;
    }
}

static void big_mul_add(struct big *a, uint32_t m, uint32_t add)
{
    uint64_t carry = add;
    for (int i = 0; i < a->n; i++) {
        uint64_t t = (uint64_t)a->d[i] * m + carry;
        a->d[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry && a->n < LIMBS)
        a->d[a->n++] = (uint32_t)carry;
}

/* a *= 5^k or 10^k, in steps that fit a limb. */
static void big_mul_pow(struct big *a, uint32_t base, int k)
{
    uint32_t step = base == 5 ? 1220703125u : 1000000000u;   /* 5^13, 10^9 */
    int per = base == 5 ? 13 : 9;
    for (; k >= per; k -= per)
        big_mul_add(a, step, 0);
    uint32_t rest = 1;
    for (; k > 0; k--)
        rest *= base;
    if (rest > 1)
        big_mul_add(a, rest, 0);
}

static void big_shl(struct big *a, int bits)
{
    int limbs = bits / 32, b = bits % 32;
    if (a->n == 0)
        return;
    if (b) {
        uint32_t carry = 0;
        for (int i = 0; i < a->n; i++) {
            uint32_t v = a->d[i];
            a->d[i] = v << b | carry;
            carry = v >> (32 - b);
        }
        if (carry && a->n < LIMBS)
            a->d[a->n++] = carry;
    }
    if (limbs) {
        int n = a->n + limbs > LIMBS ? LIMBS - limbs : a->n;
        memmove(a->d + limbs, a->d, (size_t)n * 4);
        memset(a->d, 0, (size_t)limbs * 4);
        a->n = n + limbs;
    }
}

static int big_bits(const struct big *a)
{
    return a->n ? (a->n - 1) * 32 + 32 - __builtin_clz(a->d[a->n - 1]) : 0;
}

static bool big_bit(const struct big *a, int i)
{
    return i / 32 < a->n && (a->d[i / 32] >> (i % 32) & 1);
}

/* Is any bit below bit i set? */
static bool big_any_below(const struct big *a, int i)
{
    for (int k = 0; k < i / 32 && k < a->n; k++)
        if (a->d[k])
            return true;
    return i % 32 && i / 32 < a->n && (a->d[i / 32] & ((1u << (i % 32)) - 1));
}

/* a - (b << shift) >= 0: compare, and subtract. */
static int big_cmp_shifted(const struct big *a, const struct big *b, int shift)
{
    int top = big_bits(b) + shift;
    int at = big_bits(a);
    if (at != top)
        return at > top ? 1 : -1;
    for (int i = top - 1; i >= 0; i--) {
        bool x = big_bit(a, i), y = i >= shift && big_bit(b, i - shift);
        if (x != y)
            return x ? 1 : -1;
    }
    return 0;
}

static void big_sub_shifted(struct big *a, const struct big *b, int shift)
{
    struct big s = *b;
    big_shl(&s, shift);
    int64_t borrow = 0;
    for (int i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->d[i] - (i < s.n ? s.d[i] : 0) - borrow;
        borrow = t < 0;
        a->d[i] = (uint32_t)(t + (borrow ? (int64_t)1 << 32 : 0));
    }
    while (a->n && a->d[a->n - 1] == 0)
        a->n--;
}

/* a /= m, the remainder returned. */
static uint32_t big_divmod_small(struct big *a, uint32_t m)
{
    uint64_t r = 0;
    for (int i = a->n - 1; i >= 0; i--) {
        uint64_t t = r << 32 | a->d[i];
        a->d[i] = (uint32_t)(t / m);
        r = t % m;
    }
    while (a->n && a->d[a->n - 1] == 0)
        a->n--;
    return (uint32_t)r;
}

/* The top 64 bits of a (its top bit at bit 63) and the exponent of its
 * lowest kept bit; *sticky when a bit below them is set. */
static uint64_t big_top64(const struct big *a, int *low, bool *sticky)
{
    int bits = big_bits(a);
    int from = bits > 64 ? bits - 64 : 0;
    uint64_t v = 0;
    for (int i = bits - 1; i >= from; i--)
        v = v << 1 | big_bit(a, i);
    v <<= 64 - (bits - from);
    *low = from - (64 - (bits - from));
    *sticky = big_any_below(a, from);
    return v;
}

/* --- A double's exact decimal digits --- */

/* The digits of v (finite, > 0) into buf, no leading zeros, and the place
 * of the decimal point: value = 0.DIGITS * 10^point. The count is returned. */
static int exact_digits(double v, char *buf, int size, int *point)
{
    uint64_t bits;
    memcpy(&bits, &v, 8);
    int e = (int)(bits >> 52 & 0x7FF);
    uint64_t m = bits & 0xFFFFFFFFFFFFFULL;
    if (e)
        m |= 1ULL << 52;
    else
        e = 1;
    e -= 1075;                                  /* v = m * 2^e */
    struct big a;
    big_set(&a, m);
    int scale = 0;                              /* v = a / 10^scale */
    if (e >= 0) {
        big_shl(&a, e);
    } else {
        big_mul_pow(&a, 5, -e);
        scale = -e;
    }
    char rev[1200];
    int n = 0;
    while (a.n) {                               /* nine digits at a time, the lowest first */
        uint32_t chunk = big_divmod_small(&a, 1000000000u);
        for (int k = 0; k < 9; k++, chunk /= 10)
            rev[n++] = (char)('0' + chunk % 10);
    }
    while (n > 1 && rev[n - 1] == '0')
        n--;
    int count = 0;
    for (int i = n - 1; i >= 0 && count < size; i--)
        buf[count++] = rev[i];
    *point = n - scale;
    return count;
}

/* Round digits[0..n) to keep digits (keep may be <= 0 or >= n): to nearest,
 * ties to even. The rounded digits go to out (zeros after the given ones)
 * and their count is returned: keep, or keep + 1 when a carry runs out of
 * the first digit (999 -> 1000; the point then moves one up). */
static int round_digits(const char *digits, int n, int keep, char *out, int *point)
{
    bool up = false;
    if (keep < 0)
        return 0;                               /* below a tenth of the last place */
    if (keep < n) {
        char d = digits[keep];
        if (d > '5') {
            up = true;
        } else if (d == '5') {
            bool more = false;
            for (int i = keep + 1; i < n; i++)
                more |= digits[i] != '0';
            up = more || (keep > 0 && (digits[keep - 1] - '0') % 2);
        }
    }
    for (int i = 0; i < keep; i++)
        out[i] = i < n ? digits[i] : '0';
    if (!up)
        return keep;
    for (int i = keep - 1; i >= 0; i--) {
        if (out[i] != '9') {
            out[i]++;
            return keep;
        }
        out[i] = '0';
    }
    out[0] = '1';                               /* all nines: 1 and keep zeros */
    for (int i = 1; i <= keep; i++)
        out[i] = '0';
    (*point)++;
    return keep + 1;
}

/* --- printf's conversions --- */

static int put_exponent(char *out, int e, int min_digits)
{
    int n = 0;
    out[n++] = e < 0 ? '-' : '+';
    if (e < 0)
        e = -e;
    char d[8];
    int k = 0;
    do {
        d[k++] = (char)('0' + e % 10);
        e /= 10;
    } while (e);
    while (k < min_digits)
        d[k++] = '0';
    while (k)
        out[n++] = d[--k];
    return n;
}

/* %f of |v| (digits[0..n), point) with precision p: the rounded digits
 * r[0..count) have the point after r[point - 1]. */
static int format_f(char *out, const char *digits, int n, int point, int p, bool alt)
{
    char r[1400];
    int count = round_digits(digits, n, point + p, r, &point);
    int len = 0;
    if (point <= 0)
        out[len++] = '0';
    for (int i = 0; i < point; i++)
        out[len++] = i < count ? r[i] : '0';
    if (p > 0 || alt)
        out[len++] = '.';
    for (int i = 0; i < p; i++) {
        int k = point + i;                      /* (negative: a zero after the point) */
        out[len++] = k >= 0 && k < count ? r[k] : '0';
    }
    return len;
}

/* %e of |v| with precision p (p + 1 significant digits). */
static int format_e(char *out, const char *digits, int n, int point, int p, bool alt, bool upper)
{
    char r[1400];
    round_digits(digits, n, p + 1, r, &point);
    int len = 0;
    out[len++] = r[0];
    if (p > 0 || alt)
        out[len++] = '.';
    for (int i = 1; i <= p; i++)
        out[len++] = r[i];
    out[len++] = upper ? 'E' : 'e';
    return len + put_exponent(out + len, point - 1, 2);
}

/* %a of |v| with precision p (-1: as many hex digits as needed). */
static int format_a(char *out, double v, int p, bool alt, bool upper)
{
    uint64_t bits;
    memcpy(&bits, &v, 8);
    int e = (int)(bits >> 52 & 0x7FF);
    uint64_t m = bits & 0xFFFFFFFFFFFFFULL;
    const char *hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int len = 0;
    out[len++] = '0';
    out[len++] = upper ? 'X' : 'x';
    int lead = 1;
    if (e == 0 && m == 0) {
        lead = 0;
        e = 0;
    } else if (e == 0) {                        /* subnormal: normalized, as macOS does */
        int shift = __builtin_clzll(m) - 11;
        m = (m << shift) & 0xFFFFFFFFFFFFFULL;
        e = 1 - shift - 1023;
    } else {
        e -= 1023;
    }
    if (p >= 0 && p < 13) {                     /* round the 52 fraction bits to 4p */
        int drop = 52 - 4 * p;
        uint64_t keep = m >> drop, rest = m & ((1ULL << drop) - 1), half = 1ULL << (drop - 1);
        if (rest > half || (rest == half && (keep & 1)))
            keep++;
        if (keep >> (4 * p)) {                  /* a carry into the leading digit */
            lead++;
            keep &= (1ULL << (4 * p)) - 1;
        }
        m = keep << drop;
    }
    out[len++] = (char)('0' + lead);
    int digits = p >= 0 ? p : 13;
    if (p < 0)
        while (digits > 0 && ((m >> (52 - 4 * digits)) & 0xF) == 0)
            digits--;
    if (digits > 0 || alt)
        out[len++] = '.';
    for (int i = 0; i < digits; i++)
        out[len++] = i < 13 ? hex[(m >> (48 - 4 * i)) & 0xF] : '0';
    out[len++] = upper ? 'P' : 'p';
    return len + put_exponent(out + len, e, 1);
}

int fp_format(char *out, double v, char conv, int precision, bool alt)
{
    bool upper = conv == 'E' || conv == 'F' || conv == 'G' || conv == 'A';
    char c = (char)(conv | 0x20);
    uint64_t bits;
    memcpy(&bits, &v, 8);
    if ((bits >> 52 & 0x7FF) == 0x7FF) {        /* inf, nan (without a sign, as macOS) */
        const char *t = (bits << 12) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        memcpy(out, t, 3);
        return 3;
    }
    v = v < 0 ? -v : v;
    if (c == 'a')
        return format_a(out, v, precision, alt, upper);
    int p = precision < 0 ? 6 : precision;
    if (p > 1100)
        p = 1100;                               /* (the buffers' size; all exact by then) */
    char digits[1200];
    int n = 1, point = 1;
    if (v == 0)
        digits[0] = '0';
    else
        n = exact_digits(v, digits, sizeof(digits), &point);
    if (c == 'f')
        return format_f(out, digits, n, point, p, alt);
    if (c == 'e')
        return format_e(out, digits, n, v == 0 ? 1 : point, p, alt, upper);
    /* %g: %e's exponent X at precision P decides; trailing zeros go unless #. */
    int P = p == 0 ? 1 : p;
    int x = 0;
    if (v != 0) {
        char r[1400];
        int pt = point;
        round_digits(digits, n, P, r, &pt);
        x = pt - 1;
    }
    int len = P > x && x >= -4 ? format_f(out, digits, n, point, P - 1 - x, alt)
                               : format_e(out, digits, n, v == 0 ? 1 : point, P - 1, alt, upper);
    if (!alt) {
        char *exp = 0;
        for (int i = 0; i < len; i++)
            if (out[i] == 'e' || out[i] == 'E')
                exp = out + i;
        int end = exp ? (int)(exp - out) : len, dot = -1;
        for (int i = 0; i < end; i++)
            if (out[i] == '.')
                dot = i;
        if (dot >= 0) {
            int cut = end;
            while (cut > dot + 1 && out[cut - 1] == '0')
                cut--;
            if (cut == dot + 1)
                cut = dot;
            memmove(out + cut, out + end, (size_t)(len - end));
            len -= end - cut;
        }
    }
    return len;
}

/* --- Text to binary --- */

/* q * 2^low (q's top bit set, sticky: more below) rounded to a type of
 * mant bits (53 or 24) with exponents emin..emax: its bits. Sets *range on
 * overflow (infinity) and on a result that is 0 although the value is not. */
static uint64_t round_to(uint64_t q, int low, bool sticky, int mant, int emin, int emax,
                         bool *range)
{
    int bias = emax;
    int t = 63 + low;                           /* the exponent of q's top bit */
    if (t > emax) {
        *range = true;
        return (uint64_t)(emax + bias + 1) << (mant - 1);       /* infinity */
    }
    int drop = 64 - mant + (t < emin ? emin - t : 0);
    uint64_t kept;
    bool round, rest;
    if (drop > 64) {
        kept = 0;
        round = false;
        rest = true;
    } else if (drop == 64) {
        kept = 0;
        round = q >> 63;
        rest = (q << 1) || sticky;
    } else {
        kept = q >> drop;
        round = q >> (drop - 1) & 1;
        rest = (q & ((1ULL << (drop - 1)) - 1)) || sticky;
    }
    if (round && (rest || (kept & 1)))
        kept++;
    uint64_t bits;
    if (t < emin)                               /* subnormal (a carry makes it normal) */
        bits = kept;
    else
        bits = ((uint64_t)(t + bias) << (mant - 1)) + kept - (1ULL << (mant - 1));
    if (bits >> (mant - 1) >= (uint64_t)(emax + bias + 1))
        *range = true;                          /* rounded up into infinity */
    if (bits == 0)
        *range = true;                          /* the value was not 0 */
    return bits;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static bool starts_word(const char *s, const char *word)
{
    for (; *word; s++, word++)
        if (lower((unsigned char)*s) != *word)
            return false;
    return true;
}

static int hex_value(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : (c = lower(c)) >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

#define KEEP_DIGITS 800                 /* enough to decide any rounding */

/* The text at s as a binary number of mant bits: its bits (no sign) and
 * where it ended (s itself if no number). */
static uint64_t text_to_bits(const char *s, const char **end, int mant, int emin, int emax,
                             bool *negative, bool *range)
{
    const char *p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r'))
        p++;
    *negative = *p == '-';
    if (*p == '-' || *p == '+')
        p++;
    *range = false;
    uint64_t inf = (uint64_t)(2 * emax + 1) << (mant - 1);
    if (starts_word(p, "inf")) {
        *end = p + (starts_word(p, "infinity") ? 8 : 3);
        return inf;
    }
    if (starts_word(p, "nan")) {
        p += 3;
        if (*p == '(') {                        /* nan(chars): taken whole if it closes */
            const char *q = p + 1;
            while ((*q >= '0' && *q <= '9') || (lower(*q) >= 'a' && lower(*q) <= 'z') || *q == '_')
                q++;
            if (*q == ')')
                p = q + 1;
        }
        *end = p;
        return inf | 1ULL << (mant - 2);        /* a quiet NaN */
    }
    if (p[0] == '0' && lower(p[1]) == 'x' &&
        (hex_value((unsigned char)p[2]) >= 0 || (p[2] == '.' && hex_value((unsigned char)p[3]) >= 0))) {
        p += 2;
        uint64_t q = 0;
        int exp2 = 0, used = 0;
        bool sticky = false, any = false, dot = false;
        for (;; p++) {
            if (*p == '.' && !dot) {
                dot = true;
                continue;
            }
            int h = hex_value((unsigned char)*p);
            if (h < 0)
                break;
            any = true;
            if (used < 16) {
                if (q || h)
                    used++;
                q = q << 4 | (uint64_t)h;
                exp2 -= dot ? 4 : 0;
            } else {
                sticky |= h != 0;
                exp2 += dot ? 0 : 4;
            }
        }
        if (lower(*p) == 'p') {
            const char *e = p + 1;
            bool eneg = *e == '-';
            if (*e == '-' || *e == '+')
                e++;
            if (*e >= '0' && *e <= '9') {
                long x = 0;
                for (; *e >= '0' && *e <= '9'; e++)
                    x = x < 100000 ? x * 10 + (*e - '0') : x;
                exp2 += (int)(eneg ? -x : x);
                p = e;
            }
        }
        *end = any ? p : s;
        if (!q)
            return 0;
        int shift = __builtin_clzll(q);
        return round_to(q << shift, exp2 - shift, sticky, mant, emin, emax, range);
    }
    /* Decimal: the significant digits (KEEP_DIGITS, then a sticky 1). */
    char digits[KEEP_DIGITS + 1];
    int n = 0, exp10 = 0;
    bool any = false, dot = false, dropped = false;
    for (;; p++) {
        if (*p == '.' && !dot) {
            dot = true;
            continue;
        }
        if (*p < '0' || *p > '9')
            break;
        any = true;
        if (n == 0 && *p == '0') {
            if (dot)
                exp10--;                        /* leading zeros after the point */
            continue;
        }
        if (n < KEEP_DIGITS) {
            digits[n++] = *p;
            if (dot)
                exp10--;
        } else {
            dropped |= *p != '0';
            if (!dot)
                exp10++;
        }
    }
    if (!any) {
        *end = s;
        return 0;
    }
    if (lower(*p) == 'e') {
        const char *e = p + 1;
        bool eneg = *e == '-';
        if (*e == '-' || *e == '+')
            e++;
        if (*e >= '0' && *e <= '9') {
            long x = 0;
            for (; *e >= '0' && *e <= '9'; e++)
                x = x < 100000 ? x * 10 + (*e - '0') : x;
            exp10 += (int)(eneg ? -x : x);
            p = e;
        }
    }
    *end = p;
    if (n == 0)
        return 0;
    if (dropped)
        digits[n++] = '1';                      /* (below every place that matters) */
    exp10 -= dropped;
    if (n + exp10 > 330) {                      /* far above the largest double */
        *range = true;
        return inf;
    }
    if (n + exp10 < -345) {                     /* far below the smallest */
        *range = true;
        return 0;
    }
    struct big d, t;
    big_set(&d, 0);
    for (int i = 0; i < n; i++) {
        if (d.n == 0)
            big_set(&d, (uint64_t)(digits[i] - '0'));
        else
            big_mul_add(&d, 10, (uint32_t)(digits[i] - '0'));
    }
    int low;
    bool sticky;
    uint64_t q;
    if (exp10 >= 0) {
        big_mul_pow(&d, 10, exp10);
        q = big_top64(&d, &low, &sticky);
    } else {
        big_set(&t, 1);
        big_mul_pow(&t, 10, -exp10);
        int s2 = big_bits(&t) - big_bits(&d) + 66;
        if (s2 < 0)
            s2 = 0;
        big_shl(&d, s2);
        /* q = d / t by shifts and subtractions (some 66 quotient bits). */
        struct big quot;
        big_set(&quot, 0);
        int top = big_bits(&d) - big_bits(&t);
        quot.n = top / 32 + 1;
        memset(quot.d, 0, (size_t)quot.n * 4);
        for (int i = top; i >= 0; i--)
            if (big_cmp_shifted(&d, &t, i) >= 0) {
                big_sub_shifted(&d, &t, i);
                quot.d[i / 32] |= 1u << (i % 32);
            }
        while (quot.n && quot.d[quot.n - 1] == 0)
            quot.n--;
        q = big_top64(&quot, &low, &sticky);
        sticky |= d.n != 0;                     /* a remainder */
        low -= s2;
    }
    return round_to(q, low, sticky, mant, emin, emax, range);
}

double fp_strtod(const char *s, char **end)
{
    const char *e;
    bool negative, range;
    uint64_t bits = text_to_bits(s, &e, 53, -1022, 1023, &negative, &range);
    if (end)
        *end = (char *)e;
    if (e == s)
        return 0;
    if (range)
        errno = ERANGE;
    bits |= (uint64_t)negative << 63;
    double v;
    memcpy(&v, &bits, 8);
    return v;
}

float fp_strtof(const char *s, char **end)
{
    const char *e;
    bool negative, range;
    uint64_t bits = text_to_bits(s, &e, 24, -126, 127, &negative, &range);
    if (end)
        *end = (char *)e;
    if (e == s)
        return 0;
    if (range)
        errno = ERANGE;
    uint32_t b = (uint32_t)bits | (uint32_t)negative << 31;
    float v;
    memcpy(&v, &b, 4);
    return v;
}

double fp_long_double(long double x)
{
#if defined(__aarch64__)
    /* ARM's long double is IEEE binary128: the compiler's conversion would
     * call __trunctfdf2, which is built on this function (longdouble.c),
     * so it is rounded here: the top 64 of the 113 significand bits, the
     * rest as sticky. */
    uint64_t w[2];
    memcpy(w, &x, 16);
    uint64_t hi = w[1], lo = w[0];
    uint64_t sign = hi >> 63 << 63, bits;
    int e = (int)(hi >> 48 & 0x7FFF);
    uint64_t frac_hi = hi & 0xFFFFFFFFFFFFULL;
    bool range = false;
    if (e == 0x7FFF) {                          /* inf, nan */
        bits = 0x7FFULL << 52 | ((frac_hi | lo) ? 1ULL << 51 | frac_hi << 4 : 0);
    } else if (e == 0) {                        /* 0, or far below the smallest double */
        bits = 0;
    } else {
        uint64_t q = (1ULL << 48 | frac_hi) << 15 | lo >> 49;
        bool sticky = (lo & ((1ULL << 49) - 1)) != 0;
        bits = round_to(q, e - 16383 - 63, sticky, 53, -1022, 1023, &range);
    }
    bits |= sign;
    double d;
    memcpy(&d, &bits, 8);
    return d;
#else
    return (double)x;                           /* (x86-64: an x87 store, no helper) */
#endif
}

long double fp_to_long_double(double d)
{
#if defined(__aarch64__)
    /* ARM's long double is IEEE binary128; the compiler's conversion would
     * call __extenddftf2, which is built on this function (longdouble.c),
     * so the bits are built here: the same sign, the exponent rebiased, the
     * fraction moved up. */
    uint64_t b;
    memcpy(&b, &d, sizeof(b));
    uint64_t sign = b >> 63, e = b >> 52 & 0x7FF, f = b & 0xFFFFFFFFFFFFFULL;
    uint64_t hi, lo = f << 60;
    if (e == 0x7FF) {                               /* infinity, NaN */
        hi = sign << 63 | 0x7FFFULL << 48 | f >> 4;
    } else if (e == 0 && f == 0) {
        hi = sign << 63;
        lo = 0;
    } else {
        if (e == 0) {                               /* subnormal: normalize */
            int shift = __builtin_clzll(f) - 11;
            f = (f << shift) & 0xFFFFFFFFFFFFFULL;
            e = 1 - (uint64_t)shift;
            lo = f << 60;
        }
        hi = sign << 63 | (e - 1023 + 16383) << 48 | f >> 4;
    }
    uint64_t q[2] = { lo, hi };                     /* (little-endian: low word first) */
    long double r;
    memcpy(&r, q, sizeof(r));
    return r;
#else
    return d;                                   /* (x86-64: an x87 load, no helper) */
#endif
}

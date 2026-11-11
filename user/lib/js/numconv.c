/*
 * Numbers to text and back (27o). Number::toString wants the shortest
 * digits that read back as the same double: the C library's %e rounds
 * correctly, so the digits are the first precision whose text strtod reads
 * back exactly (integers below 2^53 directly). Text to numbers follows
 * StringToNumber: decimal through strtod once the syntax is checked
 * (strtod also takes "inf", "nan" and hex floats, which the language does
 * not), 0x/0o/0b by their bits with one correct rounding.
 */
#include <math.h>
#include "jsint.h"

int shortest_digits(double d, char *digits, int *point)
{
    char buf[40];
    for (int p = 1; p <= 17; p++) {
        snprintf(buf, sizeof buf, "%.*e", p - 1, d);
        if (p == 17 || strtod(buf, 0) == d)
            break;
    }
    int k = 0;
    const char *s = buf;
    for (; *s && *s != 'e'; s++)
        if (*s >= '0' && *s <= '9')
            digits[k++] = *s;
    int e = atoi(s + 1);
    while (k > 1 && digits[k - 1] == '0')
        k--;
    digits[k] = 0;
    *point = e + 1;
    return k;
}

int number_to_ascii(double d, char *out)
{
    if (d != d)
        return snprintf(out, 32, "NaN");
    if (d == 0)
        return snprintf(out, 32, "0");
    if (isinf(d))
        return snprintf(out, 32, "%s", d < 0 ? "-Infinity" : "Infinity");
    char *o = out;
    if (d < 0) {
        *o++ = '-';
        d = -d;
    }
    if (d < 9007199254740992.0 && d == floor(d))
        return (int)(o - out) + snprintf(o, 24, "%llu", (unsigned long long)d);
    char digits[24];
    int n;
    int k = shortest_digits(d, digits, &n);
    if (k <= n && n <= 21) {
        memcpy(o, digits, (size_t)k);
        o += k;
        for (int i = k; i < n; i++)
            *o++ = '0';
    } else if (0 < n && n <= 21) {
        memcpy(o, digits, (size_t)n);
        o += n;
        *o++ = '.';
        memcpy(o, digits + n, (size_t)(k - n));
        o += k - n;
    } else if (-6 < n && n <= 0) {
        *o++ = '0';
        *o++ = '.';
        for (int i = 0; i < -n; i++)
            *o++ = '0';
        memcpy(o, digits, (size_t)k);
        o += k;
    } else {
        int e = n - 1;
        *o++ = digits[0];
        if (k > 1) {
            *o++ = '.';
            memcpy(o, digits + 1, (size_t)(k - 1));
            o += k - 1;
        }
        o += snprintf(o, 8, "e%c%d", e < 0 ? '-' : '+', e < 0 ? -e : e);
    }
    *o = 0;
    return (int)(o - out);
}

/* Big numbers for radix conversions: 37 limbs of 32 bits, the top limb
 * the integer part of a fraction scaled by 2^BIG_K. */
#define BIG_LIMBS 37
#define BIG_K (32 * (BIG_LIMBS - 1))

struct big {
    uint32_t w[BIG_LIMBS];
};

static void big_power(struct big *b, int bit)
{
    memset(b, 0, sizeof *b);
    if (bit >= 0 && bit < 32 * BIG_LIMBS)
        b->w[bit / 32] = (uint32_t)1 << (bit % 32);
}

/* m (0.5 <= m < 1) * 2^53 * 2^shift: shift may be negative (an integer
 * whose low bits are not there: then exact as a right shift) */
static void big_from_mantissa(struct big *b, double m, int shift)
{
    memset(b, 0, sizeof *b);
    uint64_t M = (uint64_t)ldexp(m, 53);
    if (shift < 0) {
        M = shift <= -64 ? 0 : M >> -shift;
        shift = 0;
    }
    for (int i = 0; i < 64; i++)
        if (M >> i & 1) {
            int bit = i + shift;
            if (bit < 32 * BIG_LIMBS)
                b->w[bit / 32] |= (uint32_t)1 << (bit % 32);
        }
}

static void big_mul(struct big *b, uint32_t k)
{
    uint64_t carry = 0;
    for (int i = 0; i < BIG_LIMBS; i++) {
        uint64_t v = (uint64_t)b->w[i] * k + carry;
        b->w[i] = (uint32_t)v;
        carry = v >> 32;
    }
}

static void big_add(struct big *a, const struct big *b)
{
    uint64_t carry = 0;
    for (int i = 0; i < BIG_LIMBS; i++) {
        uint64_t v = (uint64_t)a->w[i] + b->w[i] + carry;
        a->w[i] = (uint32_t)v;
        carry = v >> 32;
    }
}

static int big_cmp(const struct big *a, const struct big *b)
{
    for (int i = BIG_LIMBS; i-- > 0;)
        if (a->w[i] != b->w[i])
            return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}

static bool big_is_zero(const struct big *b)
{
    for (int i = 0; i < BIG_LIMBS; i++)
        if (b->w[i])
            return false;
    return true;
}

static uint32_t big_div(struct big *b, uint32_t k)
{
    uint64_t r = 0;
    for (int i = BIG_LIMBS; i-- > 0;) {
        uint64_t v = r << 32 | b->w[i];
        b->w[i] = (uint32_t)(v / k);
        r = v % k;
    }
    return (uint32_t)r;
}

struct jsstring *number_to_string(struct js *J, double d, int radix)
{
    char buf[2300];
    if (radix == 10 || d != d || isinf(d)) {
        number_to_ascii(d, buf);
        return str_from_ascii(J, buf);
    }
    if (d == 0)
        return str_from_ascii(J, "0");
    static const char digit[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    bool neg = d < 0;
    if (neg)
        d = -d;
    double ip = floor(d), fp = d - ip;
    /* The fraction's digits exactly (as JavaScriptCore and V8): F = fp * 2^K
     * as a big number, multiplied by the radix digit by digit, stopping once
     * what is left is below half the gap to the next double (delta), the
     * last digit rounded to even. */
    char frac[1100];
    int nf = 0;
    if (fp > 0) {
        struct big F, D, half, one;
        int e;
        double m = frexp(fp, &e);
        big_from_mantissa(&F, m, e - 53 + BIG_K);
        int u;
        frexp(nextafter(d, INFINITY) - d, &u);  /* ulp = 2^(u-1) */
        big_power(&D, u - 2 + BIG_K);           /* delta = ulp / 2 */
        big_power(&half, BIG_K - 1);
        big_power(&one, BIG_K);
        if (big_cmp(&F, &D) >= 0) {
            do {
                big_mul(&F, (uint32_t)radix);
                big_mul(&D, (uint32_t)radix);
                int dg = (int)F.w[BIG_LIMBS - 1];
                F.w[BIG_LIMBS - 1] = 0;
                frac[nf++] = digit[dg];
                int c = big_cmp(&F, &half);
                if (c > 0 || (c == 0 && (dg & 1))) {
                    struct big sum = F;
                    big_add(&sum, &D);
                    if (big_cmp(&sum, &one) > 0) {
                        for (;;) {      /* round up, carrying back */
                            if (nf == 0) {
                                ip += 1;
                                break;
                            }
                            char ch = frac[--nf];
                            int v = ch > '9' ? ch - 'a' + 10 : ch - '0';
                            if (v + 1 < radix) {
                                frac[nf++] = digit[v + 1];
                                break;
                            }
                        }
                        break;
                    }
                }
            } while (big_cmp(&F, &D) >= 0 && nf < 1090);
        }
    }
    /* the integer part exactly, by dividing a big number */
    char tmp[1100];
    int n = 0;
    if (ip == 0)
        tmp[n++] = '0';
    else {
        struct big I;
        int e;
        double m = frexp(ip, &e);
        big_from_mantissa(&I, m, e - 53);
        while (!big_is_zero(&I) && n < 1090)
            tmp[n++] = digit[big_div(&I, (uint32_t)radix)];
    }
    int k = 0;
    if (neg)
        buf[k++] = '-';
    while (n > 0)
        buf[k++] = tmp[--n];
    if (nf > 0) {
        buf[k++] = '.';
        memcpy(buf + k, frac, (size_t)nf);
        k += nf;
    }
    buf[k] = 0;
    return str_from_ascii(J, buf);
}

static int digit_value(uint32_t c)
{
    if (c >= '0' && c <= '9')
        return (int)(c - '0');
    if (c >= 'a' && c <= 'z')
        return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'Z')
        return (int)(c - 'A' + 10);
    return 99;
}

/* Digits of a power-of-two radix to the nearest double. */
static double pow2_radix(const uint16_t *s, uint32_t n, int bits)
{
    uint64_t m = 0;
    int exp = 0;
    bool sticky = false;
    for (uint32_t i = 0; i < n; i++) {
        int d = digit_value(s[i]);
        for (int b = bits - 1; b >= 0; b--) {
            int bit = (d >> b) & 1;
            if (m < ((uint64_t)1 << 62))
                m = m * 2 + (uint64_t)bit;
            else {
                exp++;
                sticky |= bit;
            }
        }
    }
    if (m == 0)
        return 0;
    int len = 64 - __builtin_clzll(m);
    if (len > 53) {
        int shift = len - 53;
        uint64_t rem = m & (((uint64_t)1 << shift) - 1), half = (uint64_t)1 << (shift - 1);
        m >>= shift;
        exp += shift;
        if (rem > half || (rem == half && (sticky || (m & 1))))
            m++;
    }
    return ldexp((double)m, exp);
}

double string_to_number(const uint16_t *s, uint32_t n)
{
    uint32_t a = 0, b = n;
    while (a < b && js_is_space(s[a]))
        a++;
    while (b > a && js_is_space(s[b - 1]))
        b--;
    if (a == b)
        return 0;
    s += a;
    n = b - a;
    if (n > 2 && s[0] == '0') {
        int bits = 0, radix = 0;
        if (s[1] == 'x' || s[1] == 'X')
            bits = 4, radix = 16;
        else if (s[1] == 'o' || s[1] == 'O')
            bits = 3, radix = 8;
        else if (s[1] == 'b' || s[1] == 'B')
            bits = 1, radix = 2;
        if (radix) {
            for (uint32_t i = 2; i < n; i++)
                if (digit_value(s[i]) >= radix)
                    return NAN;
            return pow2_radix(s + 2, n - 2, bits);
        }
    }
    uint32_t used;
    double d = parse_float_prefix(s, n, &used);
    if (used != n)
        return NAN;
    return d;
}

double parse_float_prefix(const uint16_t *s, uint32_t n, uint32_t *used)
{
    uint32_t i = 0;
    bool neg = false;
    if (i < n && (s[i] == '+' || s[i] == '-'))
        neg = s[i++] == '-';
    static const char inf[] = "Infinity";
    if (n - i >= 8) {
        uint32_t k = 0;
        while (k < 8 && s[i + k] == (uint16_t)inf[k])
            k++;
        if (k == 8) {
            *used = i + 8;
            return neg ? -INFINITY : INFINITY;
        }
    }
    uint32_t start = i, digits = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9')
        i++, digits++;
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && s[i] >= '0' && s[i] <= '9')
            i++, digits++;
    }
    if (digits == 0) {
        *used = 0;
        return NAN;
    }
    uint32_t end = i;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        uint32_t j = i + 1;
        if (j < n && (s[j] == '+' || s[j] == '-'))
            j++;
        if (j < n && s[j] >= '0' && s[j] <= '9') {
            while (j < n && s[j] >= '0' && s[j] <= '9')
                j++;
            end = j;
        }
    }
    *used = end;
    uint32_t len = end - start;
    char small[128];
    char *buf = len < sizeof small ? small : malloc(len + 1);
    if (!buf)
        return NAN;
    for (uint32_t k = 0; k < len; k++)
        buf[k] = (char)s[start + k];
    buf[len] = 0;
    double d = strtod(buf, 0);
    if (buf != small)
        free(buf);
    return neg ? -d : d;
}

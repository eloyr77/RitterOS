/*
 * The constants of the programs' math library (22s; written in C in 63,
 * scripts/mathconst.py until then): double-double values (hi + lo, hi the
 * nearest double, lo the nearest double to the rest) of pi/2, ln 2, ln 10,
 * 1/ln 2, 1/ln 10, 1/n! and 1/(2n+1), and the bits of 2/pi after the point
 * (for reducing large arguments of sin, cos and tan); since 22z also
 * 2/sqrt(pi) (erf), ln sqrt(2 pi), Euler's gamma, zeta(k)/k (lgamma near 1
 * and 2) and Stirling's B_2k / (2k (2k - 1)) (lgamma, tgamma).
 *
 * mathconst OUT.h
 *
 * Everything is computed here, in binary fixed point with 1600 bits after
 * the point and 448 before it (our own arithmetic, no library): pi by
 * Machin's formula, ln by atanh's series, square roots by Newton's method;
 * the Bernoulli numbers by their recurrence; gamma and zeta by
 * Euler-Maclaurin summation with them, as the Python script did with its
 * 420-digit decimals. Doubles are rounded correctly (half to even) and
 * written as Python's float.hex() writes them.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAC_LIMBS 50                           /* 1600 bits after the point */
#define INT_LIMBS 14                            /* 448 bits before it */
#define LIMBS (FRAC_LIMBS + INT_LIMBS)
#define FRAC_BITS (32 * FRAC_LIMBS)

typedef struct {
    int neg;
    uint32_t d[LIMBS];                          /* little-endian; the point after d[FRAC_LIMBS - 1] */
} num;

static num from_int(int64_t v)
{
    num r = { 0 };
    uint64_t m = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    r.neg = v < 0;
    r.d[FRAC_LIMBS] = (uint32_t)m;
    r.d[FRAC_LIMBS + 1] = (uint32_t)(m >> 32);
    return r;
}

static int is_zero(const num *a)
{
    for (int i = 0; i < LIMBS; i++)
        if (a->d[i])
            return 0;
    return 1;
}

static int cmp_mag(const num *a, const num *b)
{
    for (int i = LIMBS - 1; i >= 0; i--)
        if (a->d[i] != b->d[i])
            return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

static num add_mag(const num *a, const num *b)
{
    num r = { 0 };
    uint64_t c = 0;
    for (int i = 0; i < LIMBS; i++) {
        c += (uint64_t)a->d[i] + b->d[i];
        r.d[i] = (uint32_t)c;
        c >>= 32;
    }
    if (c) {
        fprintf(stderr, "mathconst: overflow\n");
        exit(1);
    }
    return r;
}

static num sub_mag(const num *a, const num *b)    /* |a| >= |b| */
{
    num r = { 0 };
    int64_t c = 0;
    for (int i = 0; i < LIMBS; i++) {
        c += (int64_t)a->d[i] - b->d[i];
        r.d[i] = (uint32_t)c;
        c >>= 32;
    }
    return r;
}

static num add(num a, num b)
{
    num r;
    if (a.neg == b.neg) {
        r = add_mag(&a, &b);
        r.neg = a.neg;
    } else if (cmp_mag(&a, &b) >= 0) {
        r = sub_mag(&a, &b);
        r.neg = a.neg;
    } else {
        r = sub_mag(&b, &a);
        r.neg = b.neg;
    }
    if (is_zero(&r))
        r.neg = 0;
    return r;
}

static num neg(num a)
{
    if (!is_zero(&a))
        a.neg = !a.neg;
    return a;
}

static num sub(num a, num b)
{
    return add(a, neg(b));
}

static num mul(num a, num b)
{
    static uint32_t p[2 * LIMBS];
    memset(p, 0, sizeof(p));
    for (int i = 0; i < LIMBS; i++) {
        if (!a.d[i])
            continue;
        uint64_t c = 0;
        for (int j = 0; j < LIMBS; j++) {
            c += (uint64_t)a.d[i] * b.d[j] + p[i + j];
            p[i + j] = (uint32_t)c;
            c >>= 32;
        }
        for (int k = i + LIMBS; c; k++) {
            c += p[k];
            p[k] = (uint32_t)c;
            c >>= 32;
        }
    }
    num r = { 0 };
    for (int i = 0; i < LIMBS; i++)
        r.d[i] = p[i + FRAC_LIMBS];
    for (int i = LIMBS + FRAC_LIMBS; i < 2 * LIMBS; i++)
        if (p[i]) {
            fprintf(stderr, "mathconst: overflow\n");
            exit(1);
        }
    r.neg = !is_zero(&r) && a.neg != b.neg;
    return r;
}

static num mul_small(num a, uint32_t m)
{
    uint64_t c = 0;
    for (int i = 0; i < LIMBS; i++) {
        c += (uint64_t)a.d[i] * m;
        a.d[i] = (uint32_t)c;
        c >>= 32;
    }
    if (c) {
        fprintf(stderr, "mathconst: overflow\n");
        exit(1);
    }
    return a;
}

static num div_small(num a, uint32_t m)
{
    uint64_t r = 0;
    for (int i = LIMBS - 1; i >= 0; i--) {
        r = r << 32 | a.d[i];
        a.d[i] = (uint32_t)(r / m);
        r %= m;
    }
    if (is_zero(&a))
        a.neg = 0;
    return a;
}

/* a / b: (|a| << FRAC_BITS) / |b| by long division */
static num divide(num a, num b)
{
    static uint32_t n[LIMBS + FRAC_LIMBS], rem[LIMBS + 1], q[LIMBS + FRAC_LIMBS];
    memset(n, 0, sizeof(n));
    memset(rem, 0, sizeof(rem));
    memset(q, 0, sizeof(q));
    for (int i = 0; i < LIMBS; i++)
        n[i + FRAC_LIMBS] = a.d[i];
    int top = LIMBS + FRAC_LIMBS;
    while (top > 0 && !n[top - 1])
        top--;
    for (int bit = 32 * top - 1; bit >= 0; bit--) {
        uint32_t carry = n[bit / 32] >> (bit % 32) & 1;
        for (int i = 0; i <= LIMBS; i++) {
            uint32_t out = rem[i] >> 31;
            rem[i] = rem[i] << 1 | carry;
            carry = out;
        }
        /* rem >= b? */
        int ge = 1;
        if (!rem[LIMBS])
            for (int i = LIMBS - 1; i >= 0; i--)
                if (rem[i] != b.d[i]) {
                    ge = rem[i] > b.d[i];
                    break;
                }
        if (ge) {
            int64_t c = 0;
            for (int i = 0; i <= LIMBS; i++) {
                c += (int64_t)rem[i] - (i < LIMBS ? b.d[i] : 0);
                rem[i] = (uint32_t)c;
                c >>= 32;
            }
            q[bit / 32] |= 1u << (bit % 32);
        }
    }
    num r = { 0 };
    for (int i = 0; i < LIMBS; i++)
        r.d[i] = q[i];
    r.neg = !is_zero(&r) && a.neg != b.neg;
    return r;
}

/* the highest set bit's place, counted from the lowest fraction bit; -1: zero */
static int top_bit(const num *a)
{
    for (int i = LIMBS - 1; i >= 0; i--)
        if (a->d[i])
            for (int b = 31; b >= 0; b--)
                if (a->d[i] >> b & 1)
                    return 32 * i + b;
    return -1;
}

static int bit_at(const num *a, int k)
{
    return k >= 0 && k < 32 * LIMBS ? (int)(a->d[k / 32] >> (k % 32) & 1) : 0;
}

/* the nearest double (half to even) */
static double to_double(num a)
{
    int t = top_bit(&a);
    if (t < 0)
        return 0.0;
    uint64_t m = 0;
    for (int k = t; k > t - 53; k--)
        m = m << 1 | (uint64_t)bit_at(&a, k);
    int low = t - 53;                           /* the first bit left out */
    int half = bit_at(&a, low), rest = 0;
    for (int k = low - 1; k >= 0 && !rest; k--)
        rest = bit_at(&a, k);
    if (half && (rest || (m & 1)))
        m++;
    double d = ldexp((double)m, t - 52 - FRAC_BITS);
    return a.neg ? -d : d;
}

/* a double, exactly */
static num from_double(double x)
{
    num r = { 0 };
    if (x == 0)
        return r;
    int e;
    double f = frexp(fabs(x), &e);
    uint64_t m = (uint64_t)ldexp(f, 53);        /* x = m * 2^(e - 53) */
    int shift = e - 53 + FRAC_BITS;
    for (int k = 0; k < 53; k++)
        if (m >> k & 1) {
            int at = k + shift;
            if (at >= 0 && at < 32 * LIMBS)
                r.d[at / 32] |= 1u << (at % 32);
        }
    r.neg = x < 0;
    return r;
}

/* atanh(z) = z + z^3/3 + z^5/5 + ..., |z| <= 1/3 */
static num atanh_series(num z)
{
    num z2 = mul(z, z), power = z, total = z;
    for (uint32_t k = 3;; k += 2) {
        power = mul(power, z2);
        num t = div_small(power, k);
        if (is_zero(&t))
            break;
        total = add(total, t);
    }
    return total;
}

static num ln2_value;

/* ln x for x > 0: x = 2^e m with m in [1, 2), ln m = 2 atanh((m - 1)/(m + 1)) */
static num ln(num x)
{
    int e = top_bit(&x) - FRAC_BITS;
    /* m = x / 2^e by moving the bits */
    num s = { 0 };
    for (int k = 0; k < 32 * LIMBS; k++) {
        int from = k + e;
        if (from >= 0 && from < 32 * LIMBS && bit_at(&x, from))
            s.d[k / 32] |= 1u << (k % 32);
    }
    num m = s, one = from_int(1);
    num z = divide(sub(m, one), add(m, one));
    num r = mul_small(atanh_series(z), 2);
    num el = mul_small(ln2_value, (uint32_t)(e < 0 ? -e : e));
    return e < 0 ? sub(r, el) : add(r, el);
}

static num sqrt_num(num x)
{
    num y = from_double(sqrt(to_double(x)));
    for (int i = 0; i < 12; i++) {
        num next = div_small(add(y, divide(x, y)), 2);
        if (cmp_mag(&next, &y) == 0)
            break;
        y = next;
    }
    return y;
}

/* atan(1/n) by its series */
static num atan_inv(uint32_t n)
{
    num x = div_small(from_int(1), n), power = x, total = x;
    for (uint32_t k = 3;; k += 2) {
        power = div_small(power, n * n);
        num t = div_small(power, k);
        if (is_zero(&t))
            break;
        total = ((k / 2) % 2) ? sub(total, t) : add(total, t);
    }
    return total;
}

/* (Python's float.hex) */
static const char *hexf(double x, char *buf)
{
    uint64_t u;
    memcpy(&u, &x, 8);
    const char *sign = u >> 63 ? "-" : "";
    int exp = (int)(u >> 52 & 0x7FF);
    uint64_t frac = u & 0xFFFFFFFFFFFFFull;
    if (!exp && !frac)
        sprintf(buf, "%s0x0.0p+0", sign);
    else if (!exp)
        sprintf(buf, "%s0x0.%013llxp-1022", sign, (unsigned long long)frac);
    else
        sprintf(buf, "%s0x1.%013llxp%+d", sign, (unsigned long long)frac, exp - 1023);
    return buf;
}

static FILE *out;

static void dd_text(num d, char *text)
{
    double hi = to_double(d);
    double lo = to_double(sub(d, from_double(hi)));
    char a[64], b[64];
    sprintf(text, "{ %s, %s }", hexf(hi, a), hexf(lo, b));
}

static void put(const char *name, num d)
{
    char t[160];
    dd_text(d, t);
    fprintf(out, "static const dd %s = %s;\n", name, t);
}

static num B[81];                               /* B_0 .. B_80, B_1 = -1/2 */

static void bernoulli(void)
{
    B[0] = from_int(1);
    for (int m = 1; m <= 80; m++) {
        num total = { 0 }, c = from_int(1);     /* c = binomial(m + 1, j) */
        for (int j = 0; j < m; j++) {
            total = add(total, mul(c, B[j]));
            c = div_small(mul_small(c, (uint32_t)(m + 1 - j)), (uint32_t)(j + 1));
        }
        B[m] = neg(div_small(total, (uint32_t)(m + 1)));
    }
}

/* zeta(s) for an integer s >= 2: the first 99 terms, then the tail's
 * Euler-Maclaurin sum from 100 (B_2k / (2k)! s (s+1) ... (s+2k-2) 100^(-s-2k+1)) */
static num zeta(int s)
{
    num total = { 0 };
    for (uint32_t k = 1; k < 100; k++) {
        num t = from_int(1);
        for (int i = 0; i < s; i++)
            t = div_small(t, k);
        total = add(total, t);
    }
    num t = from_int(1);                        /* 100^(1-s)/(s-1) */
    for (int i = 0; i < s - 1; i++)
        t = div_small(t, 100);
    total = add(total, div_small(t, (uint32_t)(s - 1)));
    t = from_int(1);                            /* 100^-s/2 */
    for (int i = 0; i < s; i++)
        t = div_small(t, 100);
    total = add(total, div_small(t, 2));
    for (int k = 1; k < 40; k++) {
        num term = B[2 * k];
        for (uint32_t f = 1; f <= (uint32_t)(2 * k); f++)
            term = div_small(term, f);
        /* the rising product s (s+1) ... (s+2k-2), each factor with a /100 */
        for (int f = 0; f < 2 * k - 1; f++)
            term = div_small(mul_small(term, (uint32_t)(s + f)), 100);
        for (int i = 0; i < s; i++)
            term = div_small(term, 100);
        total = add(total, term);
    }
    return total;
}

/* Euler's gamma: H_100 - ln 100 - 1/200 + sum B_2k / (2k 100^2k) */
static num euler_gamma(void)
{
    num h = { 0 };
    for (uint32_t k = 1; k <= 100; k++)
        h = add(h, div_small(from_int(1), k));
    num g = sub(sub(h, ln(from_int(100))), div_small(from_int(1), 200));
    for (int k = 1; k < 40; k++) {
        num t = div_small(B[2 * k], (uint32_t)(2 * k));
        for (int i = 0; i < 2 * k; i++)
            t = div_small(t, 100);
        g = add(g, t);
    }
    return g;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: mathconst OUT.h\n");
        return 2;
    }
    num pi = sub(mul_small(atan_inv(5), 16), mul_small(atan_inv(239), 4));     /* Machin */
    ln2_value = mul_small(atanh_series(div_small(from_int(1), 3)), 2);        /* ln 2 = 2 atanh(1/3) */
    num ln10 = ln(from_int(10));
    bernoulli();
    out = fopen(argv[1], "w");
    if (!out) {
        fprintf(stderr, "mathconst: cannot write %s\n", argv[1]);
        return 1;
    }
    fprintf(out, "/* Generated by scripts/mathconst.c: do not edit. */\n\n");
    put("PI_2", div_small(pi, 2));
    put("PI_4", div_small(pi, 4));
    put("PI", pi);
    put("LN2", ln2_value);
    put("LN10", ln10);
    num one = from_int(1);
    put("INV_LN2", divide(one, ln2_value));
    put("INV_LN10", divide(one, ln10));
    char t[160];
    fprintf(out, "static const dd INV_FACT[41] = {\n");
    num fact = from_int(1);
    for (int n = 0; n <= 40; n++) {
        if (n)
            fact = mul_small(fact, (uint32_t)n);
        dd_text(divide(one, fact), t);
        fprintf(out, "    %s,   /* 1/%d! */\n", t, n);
    }
    fprintf(out, "};\n");
    fprintf(out, "static const dd INV_ODD[41] = {\n");
    for (int n = 0; n <= 40; n++) {
        dd_text(div_small(one, (uint32_t)(2 * n + 1)), t);
        fprintf(out, "    %s,   /* 1/%d */\n", t, 2 * n + 1);
    }
    fprintf(out, "};\n");
    put("TWO_OVER_SQRT_PI", divide(from_int(2), sqrt_num(pi)));
    put("LN_SQRT_2PI", div_small(ln(mul_small(pi, 2)), 2));
    put("EULER_GAMMA", euler_gamma());
    fprintf(out, "static const dd ZETA_OVER_K[65] = {\n");
    for (int k = 0; k <= 64; k++) {
        num v = k >= 2 ? div_small(zeta(k), (uint32_t)k) : from_int(0);
        dd_text(v, t);
        fprintf(out, "    %s,   /* zeta(%d)/%d */\n", t, k, k);
    }
    fprintf(out, "};\n");
    fprintf(out, "static const dd STIRLING[31] = {\n");
    for (int k = 0; k <= 30; k++) {
        num v = k ? div_small(B[2 * k], (uint32_t)(2 * k * (2 * k - 1))) : from_int(0);
        dd_text(v, t);
        fprintf(out, "    %s,   /* B_%d/(%d*%d) */\n", t, 2 * k, 2 * k, 2 * k - 1);
    }
    fprintf(out, "};\n");
    /* 2/pi's bits after the point: word k holds bits 64k+1 .. 64k+64, the
     * first of them in the word's top bit */
    num two_pi = divide(from_int(2), pi);
    fprintf(out, "static const uint64_t TWO_OVER_PI[22] = {\n");
    for (int w = 0; w < 22; w++) {
        uint64_t word = 0;
        for (int b = 0; b < 64; b++)
            word = word << 1 | (uint64_t)bit_at(&two_pi, FRAC_BITS - 1 - (64 * w + b));
        fprintf(out, "%s0x%016llXULL,%s", w % 3 == 0 ? "    " : " ", (unsigned long long)word,
                w % 3 == 2 || w == 21 ? "\n" : "");
    }
    fprintf(out, "};\n");
    return fclose(out) != 0;
}

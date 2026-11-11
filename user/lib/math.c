/*
 * math.h (22m: absolute values, square roots, rounding; 22s: the rest). Our
 * own code. The square root is the processor's instruction (-fno-math-errno
 * makes __builtin_sqrt sqrtsd or fsqrt). Everything else is computed in
 * double-double arithmetic: a value hi + lo with |lo| <= ulp(hi)/2, about
 * 106 bits, using the exact sum and product of two doubles (Knuth's TwoSum,
 * Dekker's split; this file is built with -ffp-contract=off, so a*b+c is
 * never fused into one rounding). Each function reduces its argument
 * exactly (e^x by ln 2, sin and cos by pi/2 with 2/pi's bits for large
 * arguments, logarithms by powers of two), sums a series in double-double
 * and rounds once at the end: the result is within an ulp of the exact
 * value, almost always the nearest double. The constants (pi/2, ln 2, 1/n!,
 * 1/(2n+1), 2/pi's bits) come from scripts/mathconst.c. The special cases
 * (zeros, infinities, NaN, domain errors) are C's Annex F's. Since 22z
 * also the rest of C99's math.h: rint and the others that round in the
 * current direction (fenv.h), the exact operations (remainder, fma by
 * integers), erf, erfc, lgamma and tgamma (double-double series, their
 * constants from mathconst.c too); the long double forms and nexttoward
 * (a long double argument) are mathl.c's.
 */
#include <fenv.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    double hi, lo;
} dd;

#include "libm_tables.h"

/* --- Bits --- */

static uint64_t bits_of(double x)
{
    uint64_t b;
    memcpy(&b, &x, 8);
    return b;
}

static double from_bits(uint64_t b)
{
    double x;
    memcpy(&x, &b, 8);
    return x;
}

double fabs(double x) { return __builtin_fabs(x); }
float fabsf(float x) { return __builtin_fabsf(x); }
double sqrt(double x) { return __builtin_sqrt(x); }
float sqrtf(float x) { return __builtin_sqrtf(x); }

double copysign(double x, double y)
{
    return from_bits((bits_of(x) & ~(1ULL << 63)) | (bits_of(y) & 1ULL << 63));
}

/* x * 2^n, rounded once (subnormal results through a 2^53 step, so that
 * only the last multiplication rounds). */
double scalbn(double x, int n)
{
    if (n > 1023) {
        x *= 0x1p1023;
        n -= 1023;
        if (n > 1023) {
            x *= 0x1p1023;
            n -= 1023;
            if (n > 1023)
                n = 1023;
        }
    } else if (n < -1022) {
        x *= 0x1p-1022 * 0x1p53;
        n += 1022 - 53;
        if (n < -1022) {
            x *= 0x1p-1022 * 0x1p53;
            n += 1022 - 53;
            if (n < -1022)
                n = -1022;
        }
    }
    return x * from_bits((uint64_t)(0x3FF + n) << 52);
}

double ldexp(double x, int n)
{
    return scalbn(x, n);
}

double frexp(double x, int *e)
{
    uint64_t b = bits_of(x);
    int exp = (int)(b >> 52 & 0x7FF);
    if (exp == 0) {
        if (x == 0) {
            *e = 0;
            return x;
        }
        x *= 0x1p64;                            /* subnormal: made normal first */
        b = bits_of(x);
        exp = (int)(b >> 52 & 0x7FF) - 64;
    } else if (exp == 0x7FF) {
        *e = 0;
        return x;
    }
    *e = exp - 1022;
    return from_bits((b & ~(0x7FFULL << 52)) | 0x3FEULL << 52);
}

/* --- Rounding (22m) --- */

static int whole(double x)
{
    return !(fabs(x) < 4503599627370496.0);    /* 2^52 and above, inf and nan */
}

double trunc(double x)
{
    if (whole(x))
        return x;
    return copysign((double)(int64_t)x, x);     /* (-0.5 is -0) */
}

double floor(double x)
{
    double t = trunc(x);
    return t > x ? t - 1 : t;
}

double ceil(double x)
{
    double t = trunc(x);
    return t < x ? copysign(t + 1, x) : t;      /* (-0.5 is -0) */
}

double round(double x)
{
    if (whole(x))
        return x;
    double t = trunc(x);
    double rest = x - t;                        /* exact */
    if (rest >= 0.5)
        return t + 1;
    if (rest <= -0.5)
        return t - 1;
    return t;
}

double modf(double x, double *whole_part)
{
    if (isinf(x)) {
        *whole_part = x;
        return copysign(0.0, x);
    }
    double t = trunc(x);
    *whole_part = t;
    return copysign(x - t, x);                  /* (exact; NaN stays NaN) */
}

/* The exact remainder of x / y with x's sign (22s: by the mantissas, so
 * also when x / y is far beyond 2^53). Both mantissas start at bit 52
 * (subnormals moved up, their exponents below 1), then one bit of the
 * quotient at a time. */
static uint64_t mantissa(uint64_t b, int *e)
{
    *e = (int)(b >> 52 & 0x7FF);
    uint64_t m = b & 0xFFFFFFFFFFFFFULL;
    if (*e) {
        m |= 1ULL << 52;
    } else {
        *e = 1;
        while (!(m >> 52)) {
            m <<= 1;
            (*e)--;
        }
    }
    return m;
}

double fmod(double x, double y)
{
    if (y == 0 || isnan(x) || isnan(y) || isinf(x))
        return NAN;
    if (isinf(y) || fabs(x) < fabs(y))
        return x;
    uint64_t bx = bits_of(x);
    int ex, ey;
    uint64_t mx = mantissa(bx, &ex), my = mantissa(bits_of(y), &ey);
    for (; ex > ey; ex--) {
        if (mx >= my)
            mx -= my;
        mx <<= 1;
    }
    if (mx >= my)
        mx -= my;
    if (mx == 0)
        return copysign(0.0, x);
    while (!(mx >> 52)) {                       /* normalize */
        mx <<= 1;
        ex--;
    }
    uint64_t r;
    if (ex >= 1)
        r = (uint64_t)ex << 52 | (mx & 0xFFFFFFFFFFFFFULL);
    else
        r = mx >> (1 - ex);                     /* subnormal (exact: below y's) */
    return from_bits(r | (bx & 1ULL << 63));
}

double fmin(double x, double y)
{
    if (isnan(x))
        return y;
    if (isnan(y))
        return x;
    if (x == y)
        return signbit(x) ? x : y;              /* (-0 before +0) */
    return x < y ? x : y;
}

double fmax(double x, double y)
{
    if (isnan(x))
        return y;
    if (isnan(y))
        return x;
    if (x == y)
        return signbit(x) ? y : x;
    return x > y ? x : y;
}

/* --- Double-double arithmetic --- */

static dd two_sum(double a, double b)
{
    double s = a + b, bb = s - a;
    return (dd){ s, (a - (s - bb)) + (b - bb) };
}

static dd fast_two_sum(double a, double b)      /* |a| >= |b| */
{
    double s = a + b;
    return (dd){ s, b - (s - a) };
}

static void split(double a, double *hi, double *lo)
{
    double c = 134217729.0 * a;                 /* 2^27 + 1 */
    *hi = c - (c - a);
    *lo = a - *hi;
}

static dd two_prod(double a, double b)
{
    double p = a * b, ah, al, bh, bl;
    split(a, &ah, &al);
    split(b, &bh, &bl);
    return (dd){ p, ((ah * bh - p) + ah * bl + al * bh) + al * bl };
}

static dd dd_d(double a)
{
    return (dd){ a, 0 };
}

static dd dd_add(dd a, dd b)
{
    dd s = two_sum(a.hi, b.hi), t = two_sum(a.lo, b.lo);
    s.lo += t.hi;
    s = fast_two_sum(s.hi, s.lo);
    s.lo += t.lo;
    return fast_two_sum(s.hi, s.lo);
}

static dd dd_neg(dd a)
{
    return (dd){ -a.hi, -a.lo };
}

static dd dd_sub(dd a, dd b)
{
    return dd_add(a, dd_neg(b));
}

static dd dd_mul(dd a, dd b)
{
    dd p = two_prod(a.hi, b.hi);
    p.lo += a.hi * b.lo + a.lo * b.hi;
    return fast_two_sum(p.hi, p.lo);
}

static dd dd_mul_d(dd a, double b)
{
    dd p = two_prod(a.hi, b);
    p.lo += a.lo * b;
    return fast_two_sum(p.hi, p.lo);
}

static dd dd_div(dd a, dd b)
{
    double q1 = a.hi / b.hi;
    dd r = dd_sub(a, dd_mul_d(b, q1));
    double q2 = r.hi / b.hi;
    r = dd_sub(r, dd_mul_d(b, q2));
    double q3 = r.hi / b.hi;
    return dd_add(fast_two_sum(q1, q2), dd_d(q3));
}

static dd dd_sqrt(dd a)
{
    if (a.hi <= 0)
        return dd_d(0);
    double x = sqrt(a.hi);
    dd e = dd_sub(a, two_prod(x, x));
    return fast_two_sum(x, e.hi / (2 * x));
}

static dd dd_scale(dd a, int n)                 /* times 2^n, exact (no under/overflow) */
{
    if (n > 1000 || n < -1000) {                /* (2^n itself may not be a normal double) */
        a = dd_scale(a, n / 2);
        n -= n / 2;
    }
    double f = from_bits((uint64_t)(0x3FF + n) << 52);
    return (dd){ a.hi * f, a.lo * f };
}

static double dd_round(dd a)
{
    return a.hi + a.lo;
}

/* --- exp and log --- */

/* e^r for |r| <= ln(2)/2, by its series (24 terms: the next is below 2^-110). */
static dd exp_small(dd r)
{
    dd s = INV_FACT[24];
    for (int n = 23; n >= 0; n--)
        s = dd_add(dd_mul(s, r), INV_FACT[n]);
    return s;
}

/* e^x = m * 2^k with m in [0.7, 1.42] (x a double-double, |x| <= 746). */
static dd exp_parts(dd x, int *k)
{
    double n = floor(x.hi * INV_LN2.hi + 0.5);
    dd r = dd_sub(x, dd_mul_d(LN2, n));
    *k = (int)n;
    return exp_small(r);
}

/* m * 2^k as a double, rounded once. Small results (22z: until then
 * rounded twice below 2^-1021, at most an ulp off): rounded at 53 bits
 * while still scaled up by 2^300 (m's low part would become subnormal and
 * lose bits otherwise), then scaled back, which is exact for a normal
 * result; a subnormal one is counted in units of the smallest subnormal,
 * 2^-1074, and rounded there to an integer (ties to even), which times
 * 2^-1074 is exact. */
static double scale_result(dd m, int k)
{
    if (k >= 1024)                              /* m < 1 may bring it back below the top */
        return dd_round(dd_scale(m, k - 1)) * 2;
    if (k > -900)
        return dd_round(dd_scale(m, k));
    double r = dd_round(dd_scale(m, k + 300));
    if (fabs(r) >= 0x1p-722)
        return r * 0x1p-300;
    dd u = dd_scale(m, k + 1074);
    double base = floor(u.hi);
    dd frac = two_sum(u.hi - base, u.lo);       /* (u.hi - base: exact) */
    if (frac.hi < 0) {
        frac = dd_add(frac, dd_d(1));
        base -= 1;
    }
    int up = frac.hi > 0.5 || (frac.hi == 0.5 && (frac.lo > 0 || (frac.lo == 0 && fmod(base, 2) != 0)));
    return (base + up) * 0x1p-1074;
}

double exp(double x)
{
    if (isnan(x))
        return x;
    if (x > 709.782712893384)
        return HUGE_VAL;
    if (x < -745.1332191019412)
        return 0;
    if (fabs(x) < 0x1p-54)
        return 1 + x;
    int k;
    dd m = exp_parts(dd_d(x), &k);
    return scale_result(m, k);
}

double exp2(double x)
{
    if (isnan(x))
        return x;
    if (x >= 1024)
        return HUGE_VAL;
    if (x < -1075)
        return 0;
    double n = floor(x + 0.5);
    dd r = dd_mul_d(LN2, x - n);                /* (x - n: exact) */
    return scale_result(exp_small(r), (int)n);
}

/* e^x - 1 for |x| <= ln(2)/2, from the series' second term on. */
static dd expm1_small(dd x)
{
    dd s = INV_FACT[25];
    for (int n = 24; n >= 1; n--)
        s = dd_add(dd_mul(s, x), INV_FACT[n]);
    return dd_mul(s, x);
}

double expm1(double x)
{
    if (isnan(x))
        return x;
    if (x > 709.782712893384)
        return HUGE_VAL;
    if (x < -40)
        return -1;                              /* (e^-40 is far below an ulp of 1) */
    if (fabs(x) < 0x1p-60)
        return x;
    if (fabs(x) <= 0.34657359027997264)
        return dd_round(expm1_small(dd_d(x)));
    int k;
    dd m = exp_parts(dd_d(x), &k);
    if (k > 1000)
        return scale_result(m, k);              /* (the 1 no longer shows) */
    return dd_round(dd_sub(dd_scale(m, k), dd_d(1)));
}

/* ln(x) for x > 0 (a double-double, finite): x = m * 2^k with m near 1,
 * ln m = 2 atanh(s), s = (m - 1)/(m + 1), |s| <= 0.172 (22 terms). */
static dd log_dd(dd x)
{
    int k = 0;
    uint64_t b = bits_of(x.hi);
    if ((b >> 52 & 0x7FF) == 0) {               /* subnormal */
        x = dd_scale(x, 64);
        b = bits_of(x.hi);
        k = -64;
    }
    k += (int)(b >> 52 & 0x7FF) - 1023;
    dd m = dd_scale(x, -((int)(b >> 52 & 0x7FF) - 1023));
    if (m.hi > 1.4142135623730951) {
        m = dd_scale(m, -1);
        k++;
    }
    dd s = dd_div(dd_sub(m, dd_d(1)), dd_add(m, dd_d(1)));
    dd s2 = dd_mul(s, s), t = INV_ODD[22];
    for (int n = 21; n >= 0; n--)
        t = dd_add(dd_mul(t, s2), INV_ODD[n]);
    dd l = dd_scale(dd_mul(t, s), 1);
    return dd_add(dd_mul_d(LN2, (double)k), l);
}

static bool log_special(double x, double *r)
{
    if (isnan(x) || x < 0) {
        *r = isnan(x) ? x : NAN;
        return true;
    }
    if (x == 0) {
        *r = -HUGE_VAL;
        return true;
    }
    if (isinf(x)) {
        *r = x;
        return true;
    }
    return false;
}

double log(double x)
{
    double r;
    if (log_special(x, &r))
        return r;
    return dd_round(log_dd(dd_d(x)));
}

double log2(double x)
{
    double r;
    if (log_special(x, &r))
        return r;
    return dd_round(dd_mul(log_dd(dd_d(x)), INV_LN2));
}

double log10(double x)
{
    double r;
    if (log_special(x, &r))
        return r;
    return dd_round(dd_mul(log_dd(dd_d(x)), INV_LN10));
}

/* ln(1 + t) for a double-double t > -1, also for t near 0. */
static dd log1p_dd(dd t)
{
    if (fabs(t.hi) < 0.25) {                    /* 2 atanh(t / (2 + t)) */
        dd s = dd_div(t, dd_add(dd_d(2), t));
        dd s2 = dd_mul(s, s), u = INV_ODD[22];
        for (int n = 21; n >= 0; n--)
            u = dd_add(dd_mul(u, s2), INV_ODD[n]);
        return dd_scale(dd_mul(u, s), 1);
    }
    return log_dd(dd_add(dd_d(1), t));
}

double log1p(double x)
{
    if (isnan(x) || x < -1)
        return isnan(x) ? x : NAN;
    if (x == -1)
        return -HUGE_VAL;
    if (isinf(x))
        return x;
    if (fabs(x) < 0x1p-60)
        return x;
    return dd_round(log1p_dd(dd_d(x)));
}

/* --- pow --- */

/* Is y an integer, and an odd one? */
static int integer_kind(double y)
{
    if (!isfinite(y) || trunc(y) != y)
        return 0;                               /* not an integer */
    if (fabs(y) >= 0x1p53)
        return 2;                               /* even */
    return (int64_t)y % 2 ? 1 : 2;              /* 1 odd, 2 even */
}

double pow(double x, double y)
{
    if (y == 0 || x == 1)
        return 1;
    if (isnan(x) || isnan(y))
        return x + y;
    int kind = integer_kind(y);
    if (x == 0) {
        bool odd = kind == 1;
        if (y < 0)
            return odd ? copysign(HUGE_VAL, x) : HUGE_VAL;
        return odd ? x : 0;
    }
    if (isinf(y)) {
        if (x == -1)
            return 1;
        bool big = fabs(x) > 1;
        return (y > 0) == big ? HUGE_VAL : 0;
    }
    if (isinf(x)) {
        bool odd = kind == 1;
        if (x > 0)
            return y > 0 ? HUGE_VAL : 0;
        if (y > 0)
            return odd ? -HUGE_VAL : HUGE_VAL;
        return odd ? -0.0 : 0;
    }
    bool negate = false;
    if (x < 0) {
        if (kind == 0)
            return NAN;                         /* a negative base, a fraction: no real power */
        negate = kind == 1;
        x = -x;
    }
    dd l = log_dd(dd_d(x));
    double rough = l.hi * y;                    /* (y huge: the split in dd_mul_d would overflow) */
    dd t = fabs(rough) > 1e6 || fabs(y) > 0x1p900 ? dd_d(rough) : dd_mul_d(l, y);
    double r;
    if (t.hi > 709.8)
        r = HUGE_VAL;
    else if (t.hi < -745.2)
        r = 0;
    else {
        int k;
        dd m = exp_parts(t, &k);
        r = scale_result(m, k);
    }
    return negate ? -r : r;
}

double cbrt(double x)
{
    if (x == 0 || !isfinite(x))
        return x;
    double a = fabs(x);
    int e;
    double m = frexp(a, &e);                    /* a = m * 2^e */
    int r = e % 3;
    if (r < 0)
        r += 3;
    m = ldexp(m, r);
    e -= r;                                     /* a = m * 2^e, e a multiple of 3, m in [0.5, 4) */
    double y = 0.6 + 0.4 * m;                   /* a first guess for m's root */
    for (int i = 0; i < 6; i++)
        y = y - (y * y * y - m) / (3 * y * y);  /* Newton */
    dd yy = dd_d(y), cube = dd_mul(dd_mul(yy, yy), yy);
    dd fix = dd_div(dd_sub(dd_d(m), cube), dd_mul_d(dd_mul(yy, yy), 3));
    double root = dd_round(dd_add(yy, fix));
    return copysign(ldexp(root, e / 3), x);
}

double hypot(double x, double y)
{
    if (isinf(x) || isinf(y))
        return HUGE_VAL;
    if (isnan(x) || isnan(y))
        return x + y;
    x = fabs(x);
    y = fabs(y);
    if (x < y) {
        double t = x;
        x = y;
        y = t;
    }
    if (y == 0)
        return x;
    int e, ey;
    frexp(x, &e);
    frexp(y, &ey);
    if (e - ey > 60)
        return x;                               /* (y^2 far below x^2's last bit) */
    double xs = ldexp(x, -e), ys = ldexp(y, -e);    /* both at most 1: no overflow */
    dd sum = dd_add(two_prod(xs, xs), two_prod(ys, ys));
    return ldexp(dd_round(dd_sqrt(sum)), e);
}

/* --- sin, cos, tan --- */

/* 64 bits of 2/pi from bit j on (bit 1 is the first after the point; bits
 * before it, j <= 0, are zero). */
static uint64_t two_over_pi_bits(int j)
{
    int at = j - 1;                             /* bit index from the table's start */
    if (at < -63)
        return 0;
    if (at < 0)
        return TWO_OVER_PI[0] >> -at;
    int w = at / 64, s = at % 64;
    uint64_t v = TWO_OVER_PI[w] << s;
    if (s && w + 1 < 22)
        v |= TWO_OVER_PI[w + 1] >> (64 - s);
    return v;
}

/* x (finite, >= 0) as q * pi/2 + r with |r| <= pi/4 (r a double-double):
 * x = m * 2^e; x * 2/pi from a window of 2/pi's bits around 2^-e
 * (Payne-Hanek): its integer part mod 4 is q, the 190 bits after the point
 * the fraction, which times pi/2 is r. */
static int reduce(double x, dd *r)
{
    if (x <= PI_4.hi) {
        *r = dd_d(x);
        return 0;
    }
    uint64_t b = bits_of(x);
    int e = (int)(b >> 52 & 0x7FF) - 1075;
    uint64_t m = (b & 0xFFFFFFFFFFFFFULL) | 1ULL << 52;
    int j0 = e - 1;
    uint64_t w0 = two_over_pi_bits(j0), w1 = two_over_pi_bits(j0 + 64),
             w2 = two_over_pi_bits(j0 + 128);
    /* P = m * W (W = w0:w1:w2, 192 bits), as four words p3:p2:p1:p0. */
    unsigned __int128 t2 = (unsigned __int128)m * w2, t1 = (unsigned __int128)m * w1,
                      t0 = (unsigned __int128)m * w0;
    uint64_t p0 = (uint64_t)t2;
    unsigned __int128 c = (t2 >> 64) + (uint64_t)t1;
    uint64_t p1 = (uint64_t)c;
    c = (c >> 64) + (t1 >> 64) + (uint64_t)t0;
    uint64_t p2 = (uint64_t)c;
    /* x * 2/pi = P * 2^-190 (mod 4): q is P's bits 191-190. */
    int q = (int)(p2 >> 62);
    uint64_t f2 = p2 & 0x3FFFFFFFFFFFFFFFULL;   /* the fraction: f2:p1:p0, 190 bits */
    bool negative = false;
    if (f2 >> 61) {                             /* 0.5 or more: -(1 - f), one more quarter */
        q++;
        negative = true;
        f2 = ~f2 & 0x3FFFFFFFFFFFFFFFULL;
        p1 = ~p1;
        p0 = ~p0;                               /* (one's complement: off by 2^-190, nothing) */
    }
    /* The fraction F = (f2:p1:p0) * 2^-190 as a double-double: its top 106
     * bits, A * 2^(-53-lz) + B * 2^(-106-lz) once its leading 1 is moved to
     * the top (lz < 64: a double is never closer to a multiple of pi/2 than
     * about 2^-62 of it). */
    unsigned __int128 high = (unsigned __int128)f2 << 64 | p1;     /* 126 bits */
    if (high == 0) {
        *r = dd_d(0);
        return q & 3;
    }
    int lz = (high >> 64 ? __builtin_clzll((uint64_t)(high >> 64))
                         : 64 + __builtin_clzll((uint64_t)high)) - 2;
    if (lz > 63)
        lz = 63;
    unsigned __int128 shifted = high << lz | (lz ? (unsigned __int128)(p0 >> (64 - lz)) : 0);
    double A = (double)(uint64_t)(shifted >> 73);                   /* bits 125-73: 53 */
    double B = (double)(uint64_t)((shifted >> 20) & 0x1FFFFFFFFFFFFFULL);   /* 72-20 */
    dd f = fast_two_sum(ldexp(A, -53 - lz), ldexp(B, -106 - lz));
    if (negative)
        f = dd_neg(f);
    *r = dd_mul(f, PI_2);
    return q & 3;
}

/* sin and cos of |r| <= pi/4 by their series (14 terms each). */
static dd cos_small(dd r)
{
    dd r2 = dd_mul(r, r), s = INV_FACT[28];
    for (int n = 26; n >= 0; n -= 2) {
        s = dd_mul(s, r2);
        s = dd_sub(INV_FACT[n], s);
    }
    return s;
}

static dd sin_series(dd r)
{
    dd r2 = dd_mul(r, r), s = INV_FACT[29];
    for (int n = 27; n >= 1; n -= 2)
        s = dd_sub(INV_FACT[n], dd_mul(s, r2));
    return dd_mul(s, r);
}

double sin(double x)
{
    if (!isfinite(x))
        return x - x;                           /* NaN */
    if (fabs(x) < 0x1p-27)
        return x;                               /* (x^3/6 is below half an ulp of x) */
    dd r;
    int q = reduce(fabs(x), &r);
    dd v = q & 1 ? cos_small(r) : sin_series(r);
    if (q & 2)
        v = dd_neg(v);
    double s = dd_round(v);
    return x < 0 ? -s : s;
}

double cos(double x)
{
    if (!isfinite(x))
        return x - x;
    if (fabs(x) < 0x1p-27)
        return 1;
    dd r;
    int q = reduce(fabs(x), &r);
    dd v = q & 1 ? sin_series(r) : cos_small(r);
    if (q == 1 || q == 2)
        v = dd_neg(v);
    return dd_round(v);
}

double tan(double x)
{
    if (!isfinite(x))
        return x - x;
    if (fabs(x) < 0x1p-27)
        return x;
    dd r;
    int q = reduce(fabs(x), &r);
    dd s = sin_series(r), c = cos_small(r);
    double t = dd_round(q & 1 ? dd_neg(dd_div(c, s)) : dd_div(s, c));
    return x < 0 ? -t : t;
}

/* --- Inverse trigonometry --- */

/* atan of a double-double t >= 0: 1/t above 1, then two halvings
 * (atan t = 2 atan(t / (1 + sqrt(1 + t^2)))) to |t| <= tan(pi/16), then the
 * series (24 terms). */
static dd atan_dd(dd t)
{
    bool inverted = t.hi > 1;
    if (inverted)
        t = dd_div(dd_d(1), t);
    for (int i = 0; i < 2; i++)
        t = dd_div(t, dd_add(dd_d(1), dd_sqrt(dd_add(dd_d(1), dd_mul(t, t)))));
    dd t2 = dd_mul(t, t), s = INV_ODD[24];      /* t - t^3/3 + t^5/5 - ... */
    for (int n = 23; n >= 0; n--)
        s = dd_sub(INV_ODD[n], dd_mul(s, t2));
    dd a = dd_scale(dd_mul(s, t), 2);
    return inverted ? dd_sub(PI_2, a) : a;
}

double atan(double x)
{
    if (isnan(x))
        return x;
    if (isinf(x))
        return copysign(PI_2.hi, x);
    if (fabs(x) < 0x1p-27)
        return x;
    if (fabs(x) > 0x1p60)
        return copysign(dd_round(dd_sub(PI_2, dd_d(1 / fabs(x)))), x);
    return copysign(dd_round(atan_dd(dd_d(fabs(x)))), x);
}

double atan2(double y, double x)
{
    if (isnan(x) || isnan(y))
        return x + y;
    if (y == 0) {
        if (signbit(x))
            return copysign(PI.hi, y);          /* (+-pi for x -0 or below) */
        return y;                               /* +-0 */
    }
    if (x == 0)
        return copysign(PI_2.hi, y);
    if (isinf(x)) {
        if (isinf(y))
            return copysign(x > 0 ? PI_4.hi : dd_round(dd_mul_d(PI_4, 3)), y);
        return x > 0 ? copysign(0.0, y) : copysign(PI.hi, y);
    }
    if (isinf(y))
        return copysign(PI_2.hi, y);
    int ex, ey;
    frexp(x, &ex);
    frexp(y, &ey);
    dd a;
    if (ey - ex > 60)
        a = dd_sub(PI_2, dd_d(fabs(x) / fabs(y)));
    else if (ex - ey > 60)
        a = dd_d(fabs(y) / fabs(x));            /* (atan t is t this small: x > 0 handles it) */
    else {
        int s = ex > ey ? ex : ey;
        a = atan_dd(dd_div(dd_d(ldexp(fabs(y), -s)), dd_d(ldexp(fabs(x), -s))));
    }
    if (x < 0)
        a = dd_sub(PI, a);
    double r = dd_round(a);
    return copysign(r, y);
}

double asin(double x)
{
    if (isnan(x) || fabs(x) > 1)
        return isnan(x) ? x : NAN;
    double a = fabs(x);
    if (a < 0x1p-27)
        return x;
    dd r;
    if (a <= 0.5) {                             /* atan(a / sqrt(1 - a^2)) */
        dd c = dd_sqrt(dd_sub(dd_d(1), two_prod(a, a)));
        r = atan_dd(dd_div(dd_d(a), c));
    } else {                                    /* pi/2 - 2 asin(sqrt((1 - a)/2)) */
        dd z = dd_sqrt(dd_d((1 - a) / 2));      /* (1 - a: exact) */
        dd c = dd_sqrt(dd_sub(dd_d(1), dd_mul(z, z)));
        dd half = atan_dd(dd_div(z, c));
        r = dd_sub(PI_2, dd_scale(half, 1));
    }
    return copysign(dd_round(r), x);
}

double acos(double x)
{
    if (isnan(x) || fabs(x) > 1)
        return isnan(x) ? x : NAN;
    if (x == 1)
        return 0;
    if (x == -1)
        return PI.hi;
    /* 2 atan(sqrt((1 - x)/(1 + x))) */
    dd q = dd_div(two_sum(1, -x), two_sum(1, x));
    return dd_round(dd_scale(atan_dd(dd_sqrt(q)), 1));
}

/* --- Hyperbolic functions --- */

/* sinh and cosh of |x| <= 0.35 by their series. */
static dd sinh_small(dd x)
{
    dd x2 = dd_mul(x, x), s = INV_FACT[23];
    for (int n = 21; n >= 1; n -= 2)
        s = dd_add(INV_FACT[n], dd_mul(s, x2));
    return dd_mul(s, x);
}

static dd cosh_small(dd x)
{
    dd x2 = dd_mul(x, x), s = INV_FACT[22];
    for (int n = 20; n >= 0; n -= 2)
        s = dd_add(INV_FACT[n], dd_mul(s, x2));
    return s;
}

double sinh(double x)
{
    if (!isfinite(x) || x == 0)
        return x;
    double a = fabs(x);
    if (a < 0x1p-27)
        return x;
    double r;
    if (a <= 0.35) {
        r = dd_round(sinh_small(dd_d(a)));
    } else if (a > 40) {                        /* e^a / 2 (e^-a no longer shows) */
        if (a > 710.4758600739439)
            return copysign(HUGE_VAL, x);
        int k;
        dd m = exp_parts(dd_d(a), &k);
        r = scale_result(m, k - 1);
    } else {
        int k;
        dd e = exp_parts(dd_d(a), &k);
        e = dd_scale(e, k);
        r = dd_round(dd_scale(dd_sub(e, dd_div(dd_d(1), e)), -1));
    }
    return copysign(r, x);
}

double cosh(double x)
{
    if (isnan(x))
        return x;
    double a = fabs(x);
    if (isinf(a))
        return a;
    if (a < 0x1p-27)
        return 1;
    if (a <= 0.35)
        return dd_round(cosh_small(dd_d(a)));
    int k;
    if (a > 40) {
        if (a > 710.4758600739439)
            return HUGE_VAL;
        dd m = exp_parts(dd_d(a), &k);
        return scale_result(m, k - 1);
    }
    dd e = dd_scale(exp_parts(dd_d(a), &k), k);
    return dd_round(dd_scale(dd_add(e, dd_div(dd_d(1), e)), -1));
}

double tanh(double x)
{
    if (isnan(x) || x == 0)
        return x;
    double a = fabs(x);
    if (a >= 20)
        return copysign(1, x);                  /* (1 - 2e^-40 rounds to 1) */
    if (a < 0x1p-27)
        return x;
    dd t;
    if (a <= 0.35) {
        t = dd_div(sinh_small(dd_d(a)), cosh_small(dd_d(a)));
    } else {                                    /* expm1(2a) / (expm1(2a) + 2) */
        int k;
        dd m = exp_parts(dd_d(2 * a), &k);
        dd em = dd_sub(dd_scale(m, k), dd_d(1));
        t = dd_div(em, dd_add(em, dd_d(2)));
    }
    return copysign(dd_round(t), x);
}

double asinh(double x)
{
    if (!isfinite(x) || x == 0)
        return x;
    double a = fabs(x);
    if (a < 0x1p-27)
        return x;
    dd r;
    if (a > 0x1p28) {                           /* ln(2a) */
        r = dd_add(log_dd(dd_d(a)), LN2);
    } else {                                    /* log1p(a + a^2 / (1 + sqrt(1 + a^2))) */
        dd a2 = two_prod(a, a);
        dd t = dd_add(dd_d(a), dd_div(a2, dd_add(dd_d(1), dd_sqrt(dd_add(dd_d(1), a2)))));
        r = log1p_dd(t);
    }
    return copysign(dd_round(r), x);
}

double acosh(double x)
{
    if (isnan(x) || x < 1)
        return isnan(x) ? x : NAN;
    if (isinf(x))
        return x;
    if (x == 1)
        return 0;
    if (x > 0x1p28)
        return dd_round(dd_add(log_dd(dd_d(x)), LN2));
    /* log1p((x - 1) + sqrt((x - 1)(x + 1))) */
    dd xm = two_sum(x, -1), xp = two_sum(x, 1);
    return dd_round(log1p_dd(dd_add(xm, dd_sqrt(dd_mul(xm, xp)))));
}

double atanh(double x)
{
    if (isnan(x) || fabs(x) > 1)
        return isnan(x) ? x : NAN;
    double a = fabs(x);
    if (a == 1)
        return copysign(HUGE_VAL, x);
    if (a < 0x1p-27)
        return x;
    /* log1p(2a / (1 - a)) / 2 */
    dd t = dd_div(dd_d(2 * a), two_sum(1, -a));
    return copysign(dd_round(dd_scale(log1p_dd(t), -1)), x);
}

/* --- float forms: the double ones, rounded to float --- */

float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }
float tanf(float x) { return (float)tan(x); }
float asinf(float x) { return (float)asin(x); }
float acosf(float x) { return (float)acos(x); }
float atanf(float x) { return (float)atan(x); }
float atan2f(float y, float x) { return (float)atan2(y, x); }
float sinhf(float x) { return (float)sinh(x); }
float coshf(float x) { return (float)cosh(x); }
float tanhf(float x) { return (float)tanh(x); }
float asinhf(float x) { return (float)asinh(x); }
float acoshf(float x) { return (float)acosh(x); }
float atanhf(float x) { return (float)atanh(x); }
float expf(float x) { return (float)exp(x); }
float exp2f(float x) { return (float)exp2(x); }
float expm1f(float x) { return (float)expm1(x); }
float logf(float x) { return (float)log(x); }
float log2f(float x) { return (float)log2(x); }
float log10f(float x) { return (float)log10(x); }
float log1pf(float x) { return (float)log1p(x); }
float powf(float x, float y) { return (float)pow(x, y); }
float cbrtf(float x) { return (float)cbrt(x); }
float hypotf(float x, float y) { return (float)hypot(x, y); }
float floorf(float x) { return (float)floor(x); }
float ceilf(float x) { return (float)ceil(x); }
float truncf(float x) { return (float)trunc(x); }
float roundf(float x) { return (float)round(x); }
float fmodf(float x, float y) { return (float)fmod(x, y); }
float copysignf(float x, float y) { return (float)copysign(x, y); }
float fminf(float x, float y) { return (float)fmin(x, y); }
float fmaxf(float x, float y) { return (float)fmax(x, y); }
float scalbnf(float x, int n) { return (float)scalbn(x, n); }
float ldexpf(float x, int n) { return (float)scalbn(x, n); }

float frexpf(float x, int *e)
{
    return (float)frexp(x, e);
}

float modff(float x, float *whole_part)
{
    double w;
    double f = modf(x, &w);
    *whole_part = (float)w;
    return (float)f;
}

/* --- (22z) Rounding in the current direction (fenv.h), exact operations --- */

/* x rounded to an integer by the processor's direction: adding and taking
 * away 2^52 leaves no fraction bits, the addition rounding as fesetround
 * says (and raising inexact when x had a fraction). */
double rint(double x)
{
    if (whole(x))
        return x;
    volatile double big = copysign(4503599627370496.0, x);
    double r = (x + big) - big;
    return copysign(r, x);                      /* (-0.3 is -0) */
}

double nearbyint(double x)
{
    int had = fetestexcept(FE_INEXACT);
    double r = rint(x);
    if (!had)
        feclearexcept(FE_INEXACT);              /* (nearbyint never raises it) */
    return r;
}

/* r (an integer or not finite) as a long: out of range or NaN is invalid,
 * LONG_MIN as most systems give. */
static long to_long(double r)
{
    if (!(r >= -0x1p63 && r < 0x1p63)) {
        feraiseexcept(FE_INVALID);
        return LONG_MIN;
    }
    return (long)r;
}

long lrint(double x) { return to_long(rint(x)); }
long long llrint(double x) { return to_long(rint(x)); }
long lround(double x) { return to_long(round(x)); }
long long llround(double x) { return to_long(round(x)); }

/* x - n y with n the nearest integer to x / y (ties: even), exactly, and
 * n's low 31 bits with x / y's sign. The division as fmod's, one bit of
 * the quotient at a time. */
double remquo(double x, double y, int *quo)
{
    *quo = 0;
    if (y == 0 || isnan(x) || isnan(y) || isinf(x))
        return NAN;
    double ay = fabs(y), r;
    uint64_t q = 0;
    if (isinf(y) || fabs(x) < ay) {
        r = fabs(x);
    } else {
        int ex, ey;
        uint64_t mx = mantissa(bits_of(x), &ex), my = mantissa(bits_of(y), &ey);
        for (; ex > ey; ex--) {
            q = q << 1 | (mx >= my);
            if (mx >= my)
                mx -= my;
            mx <<= 1;
        }
        q = q << 1 | (mx >= my);
        if (mx >= my)
            mx -= my;
        if (mx == 0) {
            r = 0;
        } else {
            while (!(mx >> 52)) {
                mx <<= 1;
                ex--;
            }
            r = from_bits(ex >= 1 ? (uint64_t)ex << 52 | (mx & 0xFFFFFFFFFFFFFULL)
                                  : mx >> (1 - ex));
        }
    }
    double rest = ay - r;                       /* (exact whenever r >= ay / 2) */
    if (r > rest || (r == rest && (q & 1))) {
        r = -rest;
        q++;
    }
    int n = (int)(q & 0x7FFFFFFF);
    *quo = signbit(x) != signbit(y) ? -n : n;
    return signbit(x) ? -r : (r == 0 ? 0.0 : r);
}

double remainder(double x, double y)
{
    int q;
    return remquo(x, y, &q);
}

double fdim(double x, double y)
{
    if (isnan(x) || isnan(y))
        return x + y;
    return x > y ? x - y : 0.0;
}

double nextafter(double x, double y)
{
    if (isnan(x) || isnan(y))
        return x + y;
    if (x == y)
        return y;
    if (x == 0)
        return copysign(0x1p-1074, y);
    uint64_t b = bits_of(x);
    b += (x < y) == (x > 0) ? 1 : -1;           /* (away from 0 or toward it) */
    return from_bits(b);
}

int ilogb(double x)
{
    if (x == 0 || isnan(x)) {
        feraiseexcept(FE_INVALID);
        return x == 0 ? FP_ILOGB0 : FP_ILOGBNAN;
    }
    if (isinf(x)) {
        feraiseexcept(FE_INVALID);
        return INT_MAX;
    }
    int e;
    frexp(x, &e);
    return e - 1;
}

double logb(double x)
{
    if (x == 0) {
        feraiseexcept(FE_DIVBYZERO);
        return -HUGE_VAL;
    }
    if (!isfinite(x))
        return x * x;                           /* (+inf, NaN) */
    int e;
    frexp(x, &e);
    return e - 1;
}

double scalbln(double x, long n)
{
    return scalbn(x, n > 100000 ? 100000 : n < -100000 ? -100000 : (int)n);
}

double nan(const char *tag)
{
    (void)tag;
    return NAN;
}

/* fma: x * y + z rounded once, by integers. The product (up to 106 bits)
 * and z go into 128 bits, the larger one's top at bit 125; bits of the
 * other shifted out below are kept as a sticky bit (they are then at
 * least 21 bits below the top, so no cancellation reaches them). The sum
 * is rounded to `bits` bits (53, or 24 for fmaf) in the current direction,
 * flags raised as the processor would; emin is the lowest bit's smallest
 * exponent (subnormals), emax the top bit's largest. */
static uint64_t round_bits(int negative, unsigned __int128 v, int e0, int sticky, int bits,
                           int emin, int emax, int *exponent)
{
    int top = 127;
    while (!(v >> top & 1))
        top--;
    int lsb = top + e0 - (bits - 1);            /* the result's lowest bit's exponent */
    if (lsb < emin)
        lsb = emin;
    int cut = lsb - e0;
    uint64_t t;
    int guard = 0;
    if (cut <= 0) {
        t = (uint64_t)(v << -cut);
    } else if (cut >= 128) {
        /* (far below the smallest subnormal: shifting by 128 or more is
         * undefined in C, and clang's and our cc's code gave two different
         * meaningless values; found by building this file with our cc) */
        t = 0;
        guard = cut == 128 ? (int)(v >> 127) : 0;
        if (cut == 128 ? (v << 1) != 0 : v != 0)
            sticky = 1;
    } else {
        t = (uint64_t)(v >> cut);
        guard = (int)(v >> (cut - 1) & 1);
        if (cut > 1 && (v & (((unsigned __int128)1 << (cut - 1)) - 1)))
            sticky = 1;
    }
    int inexact = guard || sticky, mode = fegetround(), up = 0;
    if (mode == FE_TONEAREST)
        up = guard && (sticky || (t & 1));
    else if (mode == FE_UPWARD)
        up = inexact && !negative;
    else if (mode == FE_DOWNWARD)
        up = inexact && negative;
    t += (uint64_t)up;
    if (t >> bits) {
        t >>= 1;
        lsb++;
    }
    if (inexact)
        feraiseexcept(FE_INEXACT | (t >> (bits - 1) || lsb > emin ? 0 : FE_UNDERFLOW));
    if (lsb + bits - 1 > emax) {                /* overflow: infinity or the largest */
        feraiseexcept(FE_OVERFLOW | FE_INEXACT);
        int to_inf = mode == FE_TONEAREST || (mode == FE_UPWARD && !negative) ||
                     (mode == FE_DOWNWARD && negative);
        *exponent = to_inf ? INT_MAX : emax - (bits - 1);
        return to_inf ? 0 : (1ULL << bits) - 1;
    }
    *exponent = lsb;
    return t;
}

/* The exact x * y + z of finite nonzero x, y and finite z into round_bits. */
static uint64_t fma_core(double x, double y, double z, int bits, int emin, int emax,
                         int *negative, int *exponent, int *zero)
{
    int ex, ey, ez;
    uint64_t mx = mantissa(bits_of(x), &ex), my = mantissa(bits_of(y), &ey);
    unsigned __int128 p = (unsigned __int128)mx * my;
    int ep = ex + ey - 2150;                    /* (each lowest bit is 2^(e - 1075)) */
    int pneg = signbit(x) != signbit(y), zneg = signbit(z);
    int ptop = ep + (p >> 105 ? 105 : 104);
    if (z != 0) {
        uint64_t mz = mantissa(bits_of(z), &ez);
        ez -= 1075;
        int ztop = ez + 52;
        int za = ztop > ptop;                   /* z is the larger */
        int e0 = (za ? ztop : ptop) - 125, sticky = 0;
        /* place the larger exactly, the other shifted (right: sticky) */
        unsigned __int128 big = za ? ((unsigned __int128)mz << (ez - e0)) : (p << (ep - e0));
        int other_lsb = za ? ep : ez;
        unsigned __int128 other = za ? p : (unsigned __int128)mz;
        int shift = other_lsb - e0;
        if (shift >= 0) {
            other <<= shift;
        } else if (shift > -128) {
            sticky = (other & (((unsigned __int128)1 << -shift) - 1)) != 0;
            other >>= -shift;
        } else {
            sticky = other != 0;
            other = 0;
        }
        int bneg = za ? zneg : pneg, oneg = za ? pneg : zneg;
        unsigned __int128 sum;
        if (bneg == oneg) {
            sum = big + other;
        } else if (big >= other) {
            sum = big - other - (unsigned __int128)sticky;
        } else {                                /* (no sticky here: see above) */
            sum = other - big;
            bneg = oneg;
        }
        *negative = bneg;
        if (sum == 0 && !sticky) {
            *zero = 1;
            return 0;
        }
        *zero = 0;
        return round_bits(bneg, sum, e0, sticky, bits, emin, emax, exponent);
    }
    *negative = pneg;                           /* (z = 0: the product alone, rounded once) */
    *zero = 0;
    return round_bits(pneg, p, ep, 0, bits, emin, emax, exponent);
}

static double make_double(int negative, uint64_t t, int lsb)
{
    double r;
    if (lsb == INT_MAX)
        r = HUGE_VAL;
    else if (t >> 52)
        r = from_bits((uint64_t)(lsb + 1075) << 52 | (t & 0xFFFFFFFFFFFFFULL));
    else
        r = from_bits(t);                       /* (subnormal: lsb is -1074) */
    return negative ? -r : r;
}

double fma(double x, double y, double z)
{
    if (!isfinite(x) || !isfinite(y))
        return x * y + z;                       /* (infinities and NaNs as C says) */
    if (!isfinite(z))
        return z + (x * 0 * y);                 /* (z, or NaN for a NaN) */
    if (x == 0 || y == 0)
        return x * y + z;                       /* (exact: signs of zero as C's) */
    int negative, lsb, zero;
    uint64_t t = fma_core(x, y, z, 53, -1074, 1023, &negative, &lsb, &zero);
    if (zero)
        return fegetround() == FE_DOWNWARD ? -0.0 : 0.0;
    return make_double(negative, t, lsb);
}

float fmaf(float x, float y, float z)
{
    if (!isfinite(x) || !isfinite(y) || !isfinite(z) || x == 0 || y == 0)
        return (float)((double)x * y + z);      /* (exact product: one rounding) */
    int negative, lsb, zero;
    uint64_t t = fma_core(x, y, z, 24, -149, 127, &negative, &lsb, &zero);
    if (zero)
        return fegetround() == FE_DOWNWARD ? -0.0f : 0.0f;
    float r;
    if (lsb == INT_MAX) {
        r = HUGE_VALF;
    } else {
        uint32_t fb = t >> 23 ? (uint32_t)(lsb + 150) << 23 | (uint32_t)(t & 0x7FFFFF) : (uint32_t)t;
        memcpy(&r, &fb, 4);
    }
    return negative ? -r : r;
}

/* --- (22z) erf, erfc, lgamma, tgamma (double-double, as the rest) --- */

/* e^y for a double-double y, rounded once. */
static double exp_of(dd y)
{
    if (y.hi > 709.8)
        return HUGE_VAL;
    if (y.hi < -745.2)
        return 0;
    int k;
    dd m = exp_parts(y, &k);
    return scale_result(m, k);
}

/* erf(x) for 2^-500 <= x <= 6: (2/sqrt(pi)) e^-x^2 times the sum of
 * 2^n x^(2n+1) / (1 3 5 ... (2n+1)), all of whose terms are positive (no
 * cancellation, unlike the Taylor series). */
static dd erf_dd(double x)
{
    dd x2 = two_prod(x, x), step = dd_scale(x2, 1), t = dd_d(x), s = t;
    for (int n = 1; n < 400; n++) {
        t = dd_div(dd_mul(t, step), dd_d(2 * n + 1));
        s = dd_add(s, t);
        if (t.hi < s.hi * 0x1p-110)
            break;
    }
    int k;
    dd e = exp_parts(dd_neg(x2), &k);
    return dd_scale(dd_mul(dd_mul(TWO_OVER_SQRT_PI, e), s), k);
}

double erf(double x)
{
    if (isnan(x))
        return x;
    double a = fabs(x);
    if (a > 6)
        return copysign(1.0, x);                /* (erfc(6) is below half an ulp of 1) */
    if (a < 0x1p-500)                           /* 2x/sqrt(pi): x^3 terms below the last bit */
        return x == 0 ? x : copysign(dd_round(dd_scale(dd_mul_d(TWO_OVER_SQRT_PI, a * 0x1p600), -600)), x);
    return copysign(dd_round(erf_dd(a)), x);
}

/* erfc(x) for x >= 4: e^-x^2 / sqrt(pi) / (x + (1/2)/(x + 1/(x + (3/2)/(x + ...)))),
 * the continued fraction evaluated forward (Lentz) until it settles. */
static double erfc_far(double x)
{
    dd b = dd_d(x), f = b, c = b, d = dd_d(0);
    for (int n = 1; n < 5000; n++) {
        dd a = dd_d(n * 0.5);
        d = dd_add(b, dd_mul(a, d));
        d = dd_div(dd_d(1), d);
        c = dd_add(b, dd_div(a, c));
        dd delta = dd_mul(c, d);
        f = dd_mul(f, delta);
        if (fabs(delta.hi - 1) + fabs(delta.lo) < 0x1p-108)
            break;
    }
    dd x2 = two_prod(x, x);
    int k;
    dd e = exp_parts(dd_neg(x2), &k);
    dd v = dd_div(dd_mul(dd_scale(TWO_OVER_SQRT_PI, -1), e), f);
    return scale_result(v, k);
}

double erfc(double x)
{
    if (isnan(x))
        return x;
    if (x < -6)
        return 2;                               /* (2 - erfc(6) rounds to 2) */
    if (x > 27.3)
        return 0;                               /* (below the smallest subnormal) */
    if (x >= 4)
        return erfc_far(x);
    if (fabs(x) < 0x1p-500)
        return 1;
    dd e = erf_dd(fabs(x));
    return dd_round(x < 0 ? dd_add(dd_d(1), e) : dd_sub(dd_d(1), e));
}

/* sin(pi x) of a double x, its argument reduced exactly (no pi in it). */
static dd sin_pi(double x)
{
    double r = x - 2 * round(x / 2);            /* in [-1, 1], exact */
    double a = fabs(r);
    if (a > 0.5)
        a = 1 - a;                              /* sin(pi (1 - a)) = sin(pi a) */
    dd s = a <= 0.25 ? sin_series(dd_mul_d(PI, a)) : cos_small(dd_mul_d(PI, 0.5 - a));
    return r < 0 ? dd_neg(s) : s;
}

/* ln Gamma(1 + z) for |z| <= 1/4: -gamma z + sum (-1)^k zeta(k)/k z^k. */
static dd lgamma_near_1(dd z)
{
    dd s = ZETA_OVER_K[64];
    for (int k = 63; k >= 2; k--) {
        s = dd_mul(s, z);
        s = dd_sub(ZETA_OVER_K[k], s);          /* (the signs alternate) */
    }
    s = dd_mul(s, dd_mul(z, z));
    return dd_sub(s, dd_mul(EULER_GAMMA, z));
}

/* ln Gamma(x) for x > 0 (a double-double, x.hi below 2^1000). Near 1 and
 * 2, where it passes through 0, the series above; elsewhere Stirling's
 * series at y = x + n >= 20 (its terms below 2^-110 by the 20th), minus
 * the logarithm of x (x + 1) ... (x + n - 1). */
static dd lgamma_pos(dd x)
{
    if (x.hi >= 0.75 && x.hi <= 1.25)
        return lgamma_near_1(dd_sub(x, dd_d(1)));
    if (x.hi >= 1.75 && x.hi <= 2.25) {
        dd z = dd_sub(x, dd_d(2));
        return dd_add(lgamma_near_1(z), log1p_dd(z));
    }
    dd y = x, prod = dd_d(1);
    while (y.hi < 20) {
        prod = dd_mul(prod, y);
        y = dd_add(y, dd_d(1));
    }
    dd ln_y = log_dd(y), inv = dd_div(dd_d(1), y), inv2 = dd_mul(inv, inv);
    dd s = dd_add(dd_sub(dd_mul(dd_sub(y, dd_d(0.5)), ln_y), y), LN_SQRT_2PI);
    dd term = inv;
    for (int k = 1; k <= 30; k++) {
        dd add = dd_mul(STIRLING[k], term);
        s = dd_add(s, add);
        if (fabs(add.hi) < fabs(s.hi) * 0x1p-110)
            break;
        term = dd_mul(term, inv2);
    }
    return prod.hi == 1 && prod.lo == 0 ? s : dd_sub(s, log_dd(prod));
}

/* ln |Gamma(x)| for a negative x that is not an integer, by reflection:
 * ln pi - ln |sin(pi x)| - ln Gamma(1 - x). */
static dd lgamma_neg(double x, int *sign)
{
    dd s = sin_pi(x);
    *sign = s.hi < 0 ? -1 : 1;
    if (s.hi < 0)
        s = dd_neg(s);
    dd one_minus = two_sum(1, -x);              /* (exact) */
    return dd_sub(dd_sub(log_dd(PI), log_dd(s)), lgamma_pos(one_minus));
}

double lgamma(double x)
{
    if (isnan(x))
        return x;
    if (isinf(x))
        return HUGE_VAL;
    if (x <= 0 && trunc(x) == x) {
        feraiseexcept(FE_DIVBYZERO);
        return HUGE_VAL;                        /* (a pole) */
    }
    if (x > 2.5e305)
        return HUGE_VAL;
    if (x > 0)
        return dd_round(lgamma_pos(dd_d(x)));
    int sign;
    return dd_round(lgamma_neg(x, &sign));
}

double tgamma(double x)
{
    if (isnan(x))
        return x;
    if (x == 0) {
        feraiseexcept(FE_DIVBYZERO);
        return copysign(HUGE_VAL, x);
    }
    if (isinf(x))
        return x > 0 ? x : NAN;
    if (x < 0 && trunc(x) == x) {
        feraiseexcept(FE_INVALID);
        return NAN;                             /* (a pole of either sign) */
    }
    if (x > 171.7)
        return HUGE_VAL;
    if (x > 0)
        return exp_of(lgamma_pos(dd_d(x)));
    int sign;
    double v = exp_of(lgamma_neg(x, &sign));
    return sign < 0 ? -v : v;
}

/* (22z) float forms: the double ones, rounded (exact for the integer and
 * exact operations; fmaf has its own rounding above). */
float rintf(float x) { return (float)rint(x); }
float nearbyintf(float x) { return (float)nearbyint(x); }
long lrintf(float x) { return lrint(x); }
long long llrintf(float x) { return llrint(x); }
long lroundf(float x) { return lround(x); }
long long llroundf(float x) { return llround(x); }
float remainderf(float x, float y) { return (float)remainder(x, y); }
float remquof(float x, float y, int *quo) { return (float)remquo(x, y, quo); }
float fdimf(float x, float y) { return isnan(x) || isnan(y) ? x + y : x > y ? x - y : 0.0f; }
int ilogbf(float x) { return ilogb(x); }
float logbf(float x) { return (float)logb(x); }
float scalblnf(float x, long n) { return (float)scalbln(x, n); }
float nanf(const char *tag) { (void)tag; return NAN; }
float erff(float x) { return (float)erf(x); }
float erfcf(float x) { return (float)erfc(x); }
float lgammaf(float x) { return (float)lgamma(x); }
float tgammaf(float x) { return (float)tgamma(x); }

float nextafterf(float x, float y)
{
    if (isnan(x) || isnan(y))
        return x + y;
    if (x == y)
        return y;
    if (x == 0)
        return copysignf(0x1p-149f, y);
    uint32_t b;
    memcpy(&b, &x, 4);
    b += (x < y) == (x > 0) ? 1 : -1;
    memcpy(&x, &b, 4);
    return x;
}

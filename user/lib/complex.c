/*
 * complex.h (step 22x): complex numbers from math.h's functions, in double.
 * No complex arithmetic is written here (the compiler would call this
 * file's own helpers): parts in, parts out. The inverse functions are
 * Kahan's ("Branch Cuts for Complex Elementary Functions", 1987), which
 * need only csqrt, atan2, asinh and log1p and keep their precision near
 * the cuts; clog's real part near |z| = 1 comes from exact squares
 * (Dekker's products: built with -ffp-contract=off). The * and / helpers
 * are C's Annex G algorithms, as LLVM's compiler-rt has them. float forms
 * round the double results; long double forms widen them (ARM's binary128
 * through fpconv, without the compiler's conversion helpers).
 */
#include <complex.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "fpconv.h"

typedef double _Complex dcomplex;

#define RE(z) (__real__ (z))
#define IM(z) (__imag__ (z))
#define MAKE(x, y) __builtin_complex((double)(x), (double)(y))

/* logb (not in math.h): the exponent of x as a double. */
static double exponent_of(double x)
{
    if (x == 0)
        return -INFINITY;
    if (!isfinite(x))
        return fabs(x);
    int e;
    frexp(x, &e);
    return e - 1;
}

/* --- parts, size, angle --- */

double creal(dcomplex z) { return RE(z); }
double cimag(dcomplex z) { return IM(z); }
double cabs(dcomplex z) { return hypot(RE(z), IM(z)); }
double carg(dcomplex z) { return atan2(IM(z), RE(z)); }
dcomplex conj(dcomplex z) { return MAKE(RE(z), -IM(z)); }

dcomplex cproj(dcomplex z)
{
    if (isinf(RE(z)) || isinf(IM(z)))
        return MAKE(INFINITY, copysign(0.0, IM(z)));
    return z;
}

/* --- exponential, logarithm, powers, roots --- */

dcomplex cexp(dcomplex z)
{
    double x = RE(z), y = IM(z);
    if (y == 0)
        return MAKE(exp(x), y);                 /* (a real z: imag. part kept, also -0) */
    if (isinf(x) && x < 0 && !isfinite(y))
        return MAKE(0.0, 0.0);
    if (isinf(x) && x > 0 && !isfinite(y))
        return MAKE(x, NAN);
    if (!isfinite(y))
        return MAKE(NAN, NAN);
    double c = cos(y), s = sin(y);
    if (x > 709) {                              /* exp(x) overflows, exp(x) cos y may not */
        double half = exp(x / 2);
        return MAKE(half * c * half, half * s * half);
    }
    double e = exp(x);
    return MAKE(e * c, e * s);
}

/* Dekker's split and exact product: a * b == *hi + *lo. */
static void exact_product(double a, double b, double *hi, double *lo)
{
    const double split = 134217729.0;           /* 2^27 + 1 */
    double ta = split * a, a1 = ta - (ta - a), a2 = a - a1;
    double tb = split * b, b1 = tb - (tb - b), b2 = b - b1;
    *hi = a * b;
    *lo = ((a1 * b1 - *hi) + a1 * b2 + a2 * b1) + a2 * b2;
}

/* a + b == *sum + *err exactly (Knuth's two-sum). */
static void exact_sum(double a, double b, double *sum, double *err)
{
    double s = a + b, bb = s - a;
    *sum = s;
    *err = (a - (s - bb)) + (b - bb);
}

/* log|z|: near |z| = 1 log1p of x^2 + y^2 - 1, summed exactly enough. */
static double log_abs(double x, double y)
{
    double a = fabs(x), b = fabs(y);
    if (a < b) {
        double t = a;
        a = b, b = t;
    }
    if (a > 0x1p500 || b < 0x1p-500 || !isfinite(a))
        return log(hypot(a, b));
    double aa, al, bb, bl;
    exact_product(a, a, &aa, &al);
    exact_product(b, b, &bb, &bl);
    double sum, err;
    exact_sum(aa, bb, &sum, &err);
    if (sum < 0.5 || sum > 2)
        return 0.5 * log(sum) + (err + al + bl) / (2 * sum);
    double s1, e1, s2, e2;
    exact_sum(aa, -1, &s1, &e1);
    exact_sum(s1, bb, &s2, &e2);
    return 0.5 * log1p(s2 + (e1 + e2 + al + bl));
}

dcomplex clog(dcomplex z)
{
    double x = RE(z), y = IM(z);
    if (isinf(x) || isinf(y))
        return MAKE(INFINITY, atan2(y, x));     /* (NaN too: atan2 gives NaN) */
    if (isnan(x) || isnan(y))
        return MAKE(NAN, NAN);
    if (x == 0 && y == 0)
        return MAKE(-INFINITY, atan2(y, x));   /* (-0 + 0i: pi) */
    return MAKE(log_abs(x, y), atan2(y, x));
}

dcomplex cpow(dcomplex z, dcomplex w)
{
    double r = cabs(z), t = carg(z), x = RE(w), y = IM(w);
    if (r == 0)
        return MAKE(0.0, 0.0);
    double size = pow(r, x), angle = x * t;
    if (y != 0) {
        size *= exp(-y * t);
        angle += y * log_abs(RE(z), IM(z));
    }
    return MAKE(size * cos(angle), size * sin(angle));
}

dcomplex csqrt(dcomplex z)
{
    double x = RE(z), y = IM(z);
    if (isinf(y))
        return MAKE(INFINITY, y);
    if (isnan(x))
        return MAKE(NAN, NAN);
    if (isinf(x)) {
        if (x > 0)
            return MAKE(x, isnan(y) ? y : copysign(0.0, y));
        return MAKE(isnan(y) ? y : 0.0, copysign(INFINITY, y));
    }
    if (isnan(y))
        return MAKE(NAN, NAN);
    if (x == 0 && y == 0)
        return MAKE(0.0, y);
    double scale = 1;                           /* (no overflow in |x| + |z|, no underflow) */
    if (fabs(x) > 0x1p1020 || fabs(y) > 0x1p1020)
        x *= 0.25, y *= 0.25, scale = 2;
    else if (fabs(x) < 0x1p-1000 && fabs(y) < 0x1p-1000)
        x *= 0x1p108, y *= 0x1p108, scale = 0x1p-54;
    double t = sqrt((fabs(x) + hypot(x, y)) * 0.5);
    if (x >= 0)
        return MAKE(t * scale, y / (2 * t) * scale);
    return MAKE(fabs(y) / (2 * t) * scale, copysign(t, y) * scale);
}

/* --- trigonometric and hyperbolic --- */

/* cosh x * a and sinh x * b where cosh x overflows but the products may
 * not: e^|x| / 2 in two halves. */
static dcomplex large_cosh_sinh(double x, double a, double b)
{
    double h = exp(fabs(x) / 2);
    return MAKE(a * 0.5 * h * h, copysign(1.0, x) * b * 0.5 * h * h);
}

dcomplex csinh(dcomplex z)
{
    double x = RE(z), y = IM(z);
    if (y == 0)
        return MAKE(sinh(x), y);
    if (fabs(x) > 709 && isfinite(y)) {
        dcomplex r = large_cosh_sinh(x, sin(y), cos(y));
        return MAKE(IM(r), RE(r));
    }
    return MAKE(sinh(x) * cos(y), cosh(x) * sin(y));
}

dcomplex ccosh(dcomplex z)
{
    double x = RE(z), y = IM(z);
    if (y == 0)
        return MAKE(cosh(x), copysign(0.0, x) * y);
    if (fabs(x) > 709 && isfinite(y))
        return large_cosh_sinh(x, cos(y), sin(y));
    return MAKE(cosh(x) * cos(y), sinh(x) * sin(y));
}

dcomplex ctanh(dcomplex z)
{
    double x = RE(z), y = IM(z);
    if (fabs(x) > 22) {                         /* tanh x is +-1 to double's precision */
        double e = exp(-2 * fabs(x));
        return MAKE(copysign(1.0, x), isfinite(y) ? 4 * sin(y) * cos(y) * e : copysign(0.0, y));
    }
    if (y == 0)
        return MAKE(tanh(x), y);
    double t = tan(y), beta = 1 + t * t, s = sinh(x), rho = sqrt(1 + s * s);
    double d = 1 + beta * s * s;
    return MAKE(beta * rho * s / d, t / d);
}

/* sin z = -i sinh(iz) and the like: iz = (-y, x), -i(a + ib) = (b, -a). */
dcomplex csin(dcomplex z)
{
    dcomplex h = csinh(MAKE(-IM(z), RE(z)));
    return MAKE(IM(h), -RE(h));
}

dcomplex ccos(dcomplex z)
{
    return ccosh(MAKE(-IM(z), RE(z)));
}

dcomplex ctan(dcomplex z)
{
    dcomplex h = ctanh(MAKE(-IM(z), RE(z)));
    return MAKE(IM(h), -RE(h));
}

/* --- inverses (Kahan) --- */

dcomplex casin(dcomplex z)
{
    double x = RE(z), y = IM(z);
    dcomplex s1 = csqrt(MAKE(1 - x, -y)), s2 = csqrt(MAKE(1 + x, y));
    return MAKE(atan2(x, RE(s1) * RE(s2) - IM(s1) * IM(s2)),
                asinh(RE(s1) * IM(s2) - IM(s1) * RE(s2)));
}

dcomplex cacos(dcomplex z)
{
    double x = RE(z), y = IM(z);
    dcomplex s1 = csqrt(MAKE(1 - x, -y)), s2 = csqrt(MAKE(1 + x, y));
    return MAKE(2 * atan2(RE(s1), RE(s2)), asinh(RE(s2) * IM(s1) - IM(s2) * RE(s1)));
}

dcomplex cacosh(dcomplex z)
{
    double x = RE(z), y = IM(z);
    dcomplex s1 = csqrt(MAKE(x - 1, y)), s2 = csqrt(MAKE(x + 1, y));
    return MAKE(asinh(RE(s1) * RE(s2) + IM(s1) * IM(s2)), 2 * atan2(IM(s1), RE(s2)));
}

dcomplex catanh(dcomplex z)
{
    double x = RE(z), y = IM(z);
    double h = hypot(x, y);
    if (h > 0x1p500)                            /* 1/z, to double's precision */
        return MAKE(x / h / h, copysign(M_PI_2, y));
    double one_minus = 1 - fabs(x);
    double re = log1p(4 * fabs(x) / (one_minus * one_minus + y * y)) / 4;
    double im = atan2(2 * y, (1 - x) * (1 + x) - y * y) / 2;
    return MAKE(copysign(re, x), im);
}

dcomplex casinh(dcomplex z)
{
    dcomplex a = casin(MAKE(-IM(z), RE(z)));    /* asinh z = -i asin(iz) */
    return MAKE(IM(a), -RE(a));
}

dcomplex catan(dcomplex z)
{
    dcomplex a = catanh(MAKE(-IM(z), RE(z)));   /* atan z = -i atanh(iz) */
    return MAKE(IM(a), -RE(a));
}

/* --- the compiler's * and / (Annex G, as compiler-rt) --- */

static double unit_or_zero(double v)
{
    return copysign(isinf(v) ? 1.0 : 0.0, v);
}

static double nan_to_zero(double v)
{
    return isnan(v) ? copysign(0.0, v) : v;
}

dcomplex __muldc3(double a, double b, double c, double d)
{
    double ac = a * c, bd = b * d, ad = a * d, bc = b * c;
    double re = ac - bd, im = ad + bc;
    if (isnan(re) && isnan(im)) {
        int again = 0;
        if (isinf(a) || isinf(b)) {
            a = unit_or_zero(a), b = unit_or_zero(b);
            c = nan_to_zero(c), d = nan_to_zero(d);
            again = 1;
        }
        if (isinf(c) || isinf(d)) {
            c = unit_or_zero(c), d = unit_or_zero(d);
            a = nan_to_zero(a), b = nan_to_zero(b);
            again = 1;
        }
        if (!again && (isinf(ac) || isinf(bd) || isinf(ad) || isinf(bc))) {
            a = nan_to_zero(a), b = nan_to_zero(b), c = nan_to_zero(c), d = nan_to_zero(d);
            again = 1;
        }
        if (again) {
            re = INFINITY * (a * c - b * d);
            im = INFINITY * (a * d + b * c);
        }
    }
    return MAKE(re, im);
}

dcomplex __divdc3(double a, double b, double c, double d)
{
    int scale = 0;
    double size = exponent_of(fmax(fabs(c), fabs(d)));
    if (isfinite(size)) {
        scale = (int)size;
        c = scalbn(c, -scale);
        d = scalbn(d, -scale);
    }
    double denom = c * c + d * d;
    double re = scalbn((a * c + b * d) / denom, -scale);
    double im = scalbn((b * c - a * d) / denom, -scale);
    if (isnan(re) && isnan(im)) {
        if (denom == 0 && (!isnan(a) || !isnan(b))) {
            re = copysign(INFINITY, c) * a;
            im = copysign(INFINITY, c) * b;
        } else if ((isinf(a) || isinf(b)) && isfinite(c) && isfinite(d)) {
            a = unit_or_zero(a), b = unit_or_zero(b);
            re = INFINITY * (a * c + b * d);
            im = INFINITY * (b * c - a * d);
        } else if (isinf(size) && size > 0 && isfinite(a) && isfinite(b)) {
            c = unit_or_zero(c), d = unit_or_zero(d);
            re = 0.0 * (a * c + b * d);
            im = 0.0 * (b * c - a * d);
        }
    }
    return MAKE(re, im);
}

/* --- float: the double results rounded --- */

typedef float _Complex fcomplex;
#define FROM_F(z) MAKE(RE(z), IM(z))
#define TO_F(z) __builtin_complex((float)RE(z), (float)IM(z))

float crealf(fcomplex z) { return RE(z); }
float cimagf(fcomplex z) { return IM(z); }
float cabsf(fcomplex z) { return (float)cabs(FROM_F(z)); }
float cargf(fcomplex z) { return (float)carg(FROM_F(z)); }
fcomplex conjf(fcomplex z) { return __builtin_complex(RE(z), -IM(z)); }
fcomplex cprojf(fcomplex z) { dcomplex r = cproj(FROM_F(z)); return TO_F(r); }
fcomplex cpowf(fcomplex z, fcomplex w) { dcomplex r = cpow(FROM_F(z), FROM_F(w)); return TO_F(r); }

#define FLOAT_FORM(name)                                                    \
fcomplex name##f(fcomplex z)                                                \
{                                                                           \
    dcomplex r = name(FROM_F(z));                                           \
    return TO_F(r);                                                         \
}

FLOAT_FORM(cexp)
FLOAT_FORM(clog)
FLOAT_FORM(csqrt)
FLOAT_FORM(csin)
FLOAT_FORM(ccos)
FLOAT_FORM(ctan)
FLOAT_FORM(casin)
FLOAT_FORM(cacos)
FLOAT_FORM(catan)
FLOAT_FORM(csinh)
FLOAT_FORM(ccosh)
FLOAT_FORM(ctanh)
FLOAT_FORM(casinh)
FLOAT_FORM(cacosh)
FLOAT_FORM(catanh)

fcomplex __mulsc3(float a, float b, float c, float d)
{
    dcomplex r = __muldc3(a, b, c, d);
    return TO_F(r);
}

fcomplex __divsc3(float a, float b, float c, float d)
{
    dcomplex r = __divdc3(a, b, c, d);
    return TO_F(r);
}

/* --- long double: the double results widened --- */

typedef long double _Complex lcomplex;
#define FROM_L(z) MAKE(fp_long_double(RE(z)), fp_long_double(IM(z)))
#define TO_L(z) __builtin_complex(fp_to_long_double(RE(z)), fp_to_long_double(IM(z)))

long double creall(lcomplex z) { return RE(z); }
long double cimagl(lcomplex z) { return IM(z); }
long double cabsl(lcomplex z) { return fp_to_long_double(cabs(FROM_L(z))); }
long double cargl(lcomplex z) { return fp_to_long_double(carg(FROM_L(z))); }

lcomplex conjl(lcomplex z)
{
    long double im = IM(z);
    unsigned char bytes[sizeof(long double)];   /* (the sign bit flipped: no arithmetic) */
    memcpy(bytes, &im, sizeof(im));
#if defined(__x86_64__)
    bytes[9] ^= 0x80;                           /* x87: sign in byte 9 */
#else
    bytes[sizeof(bytes) - 1] ^= 0x80;           /* binary128: sign in the last byte */
#endif
    memcpy(&im, bytes, sizeof(im));
    return __builtin_complex(RE(z), im);
}

lcomplex cprojl(lcomplex z) { dcomplex r = cproj(FROM_L(z)); return TO_L(r); }
lcomplex cpowl(lcomplex z, lcomplex w) { dcomplex r = cpow(FROM_L(z), FROM_L(w)); return TO_L(r); }

#define LONG_FORM(name)                                                     \
lcomplex name##l(lcomplex z)                                                \
{                                                                           \
    dcomplex r = name(FROM_L(z));                                           \
    return TO_L(r);                                                         \
}

LONG_FORM(cexp)
LONG_FORM(clog)
LONG_FORM(csqrt)
LONG_FORM(csin)
LONG_FORM(ccos)
LONG_FORM(ctan)
LONG_FORM(casin)
LONG_FORM(cacos)
LONG_FORM(catan)
LONG_FORM(csinh)
LONG_FORM(ccosh)
LONG_FORM(ctanh)
LONG_FORM(casinh)
LONG_FORM(cacosh)
LONG_FORM(catanh)

#define LONG_HELPER(name, of)                                               \
lcomplex name(long double a, long double b, long double c, long double d)   \
{                                                                           \
    dcomplex r = of(fp_long_double(a), fp_long_double(b), fp_long_double(c), \
                    fp_long_double(d));                                     \
    return TO_L(r);                                                         \
}

LONG_HELPER(__mulxc3, __muldc3)
LONG_HELPER(__divxc3, __divdc3)
LONG_HELPER(__multc3, __muldc3)
LONG_HELPER(__divtc3, __divdc3)

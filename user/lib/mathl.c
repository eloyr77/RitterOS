/*
 * math.h's long double forms (step 22z): the double functions on the
 * argument rounded to double, the result widened, as everywhere in the
 * library (long double has double's precision here). The conversions are
 * fpconv.c's, never the compiler's (on ARM those would be the binary128
 * helpers, built on the same functions). fabsl and copysignl only move the
 * sign bit, so they keep every bit of an x87 value too. nexttoward and
 * nexttowardf are here for their long double argument.
 */
#include <math.h>
#include <string.h>
#include "fpconv.h"

#define D(x) fp_long_double(x)
#define LD(d) fp_to_long_double(d)

#if defined(__x86_64__)
#define SIGN_BYTE 9                             /* x87: the sign in byte 9 */
#else
#define SIGN_BYTE (sizeof(long double) - 1)     /* binary128: in the last byte */
#endif

static long double with_sign(long double x, int negative)
{
    unsigned char b[sizeof(long double)];
    memcpy(b, &x, sizeof(b));
    b[SIGN_BYTE] = (unsigned char)((b[SIGN_BYTE] & 0x7F) | (negative ? 0x80 : 0));
    memcpy(&x, b, sizeof(b));
    return x;
}

static int sign_of(long double x)
{
    unsigned char b[sizeof(long double)];
    memcpy(b, &x, sizeof(b));
    return b[SIGN_BYTE] >> 7;
}

long double fabsl(long double x) { return with_sign(x, 0); }
long double copysignl(long double x, long double y) { return with_sign(x, sign_of(y)); }

#define ONE(name)                                                           \
long double name##l(long double x) { return LD(name(D(x))); }
#define TWO(name)                                                           \
long double name##l(long double x, long double y) { return LD(name(D(x), D(y))); }

ONE(sqrt) ONE(floor) ONE(ceil) ONE(trunc) ONE(round) ONE(rint) ONE(nearbyint)
ONE(sin) ONE(cos) ONE(tan) ONE(asin) ONE(acos) ONE(atan)
ONE(sinh) ONE(cosh) ONE(tanh) ONE(asinh) ONE(acosh) ONE(atanh)
ONE(exp) ONE(exp2) ONE(expm1) ONE(log) ONE(log2) ONE(log10) ONE(log1p) ONE(cbrt)
ONE(logb) ONE(erf) ONE(erfc) ONE(lgamma) ONE(tgamma)
TWO(fmod) TWO(atan2) TWO(pow) TWO(hypot) TWO(fmin) TWO(fmax) TWO(remainder) TWO(fdim)
TWO(nextafter)

long double fmal(long double x, long double y, long double z) { return LD(fma(D(x), D(y), D(z))); }
long double frexpl(long double x, int *e) { return LD(frexp(D(x), e)); }
long double ldexpl(long double x, int n) { return LD(ldexp(D(x), n)); }
long double scalbnl(long double x, int n) { return LD(scalbn(D(x), n)); }
long double scalblnl(long double x, long n) { return LD(scalbln(D(x), n)); }
long double remquol(long double x, long double y, int *quo) { return LD(remquo(D(x), D(y), quo)); }
long double nexttowardl(long double x, long double y) { return LD(nextafter(D(x), D(y))); }
long double nanl(const char *tag) { return LD(nan(tag)); }
int ilogbl(long double x) { return ilogb(D(x)); }
long lrintl(long double x) { return lrint(D(x)); }
long long llrintl(long double x) { return llrint(D(x)); }
long lroundl(long double x) { return lround(D(x)); }
long long llroundl(long double x) { return llround(D(x)); }

double nexttoward(double x, long double y)
{
    return nextafter(x, D(y));                  /* (y at double's precision) */
}

float nexttowardf(float x, long double y)
{
    double t = D(y);
    if (isnan(x) || isnan(t))
        return (float)(x + t);
    if (x == t)
        return (float)t;
    return nextafterf(x, x < t ? HUGE_VALF : -HUGE_VALF);
}

long double modfl(long double x, long double *whole)
{
    double w;
    double f = modf(D(x), &w);
    *whole = LD(w);
    return LD(f);
}

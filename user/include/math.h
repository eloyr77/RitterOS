/*
 * math.h: C99's mathematics (22m: absolute values, square roots, rounding;
 * 22s: the rest). Our own code, in double-double arithmetic (about 32
 * significant digits inside): results are within one unit in the last place
 * of the exact value, nearly always the nearest double to it. Arguments of
 * sin, cos and tan are reduced exactly, also very large ones (sin(1e300)).
 * The float forms (sinf, ...) are the double ones rounded to float. errno
 * is never set (math_errhandling is 0): domain errors give NaN, overflows
 * HUGE_VAL, as C's Annex F says.
 */
#ifndef RITTEROS_MATH_H
#define RITTEROS_MATH_H

#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_PI_4      0.78539816339744830962
#define M_1_PI      0.31830988618379067154
#define M_2_PI      0.63661977236758134308
#define M_2_SQRTPI  1.12837916709551257390
#define M_E         2.7182818284590452354
#define M_LOG2E     1.4426950408889634074
#define M_LOG10E    0.43429448190325182765
#define M_LN2       0.69314718055994530942
#define M_LN10      2.30258509299404568402
#define M_SQRT2     1.41421356237309504880
#define M_SQRT1_2   0.70710678118654752440
#define HUGE_VAL    __builtin_huge_val()
#define HUGE_VALF   __builtin_huge_valf()
#define HUGE_VALL   __builtin_huge_vall()
#define INFINITY    __builtin_inff()
#define NAN         __builtin_nanf("")
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define isnormal(x) __builtin_isnormal(x)
#define signbit(x)  __builtin_signbit(x)
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4
#define fpclassify(x) \
    __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define math_errhandling 0
/* (22z) */
#define FP_ILOGB0   (-2147483647 - 1)
#define FP_ILOGBNAN (-2147483647 - 1)
#define isgreater(x, y)      __builtin_isgreater(x, y)
#define isgreaterequal(x, y) __builtin_isgreaterequal(x, y)
#define isless(x, y)         __builtin_isless(x, y)
#define islessequal(x, y)    __builtin_islessequal(x, y)
#define islessgreater(x, y)  __builtin_islessgreater(x, y)
#define isunordered(x, y)    __builtin_isunordered(x, y)
typedef float float_t;
typedef double double_t;

double fabs(double x);
float fabsf(float x);
double sqrt(double x);
float sqrtf(float x);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);                 /* halfway cases away from zero */
double fmod(double x, double y);
float floorf(float x);
float ceilf(float x);
float truncf(float x);
float roundf(float x);
float fmodf(float x, float y);

/* (22s) Trigonometry, in radians. */
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);       /* the angle of (x, y), -pi to pi */
float sinf(float x);
float cosf(float x);
float tanf(float x);
float asinf(float x);
float acosf(float x);
float atanf(float x);
float atan2f(float y, float x);

/* (22s) Hyperbolic functions. */
double sinh(double x);
double cosh(double x);
double tanh(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);
float sinhf(float x);
float coshf(float x);
float tanhf(float x);
float asinhf(float x);
float acoshf(float x);
float atanhf(float x);

/* (22s) Exponentials, logarithms and powers. expm1 and log1p keep their
 * precision near 0 (e^x - 1, ln(1 + x)). */
double exp(double x);
double exp2(double x);
double expm1(double x);
double log(double x);
double log2(double x);
double log10(double x);
double log1p(double x);
double pow(double x, double y);
double cbrt(double x);
double hypot(double x, double y);       /* sqrt(x^2 + y^2) without overflow */
float expf(float x);
float exp2f(float x);
float expm1f(float x);
float logf(float x);
float log2f(float x);
float log10f(float x);
float log1pf(float x);
float powf(float x, float y);
float cbrtf(float x);
float hypotf(float x, float y);

/* (22s) The parts of a number: x = m * 2^e (frexp: m in [0.5, 1)), x *
 * 2^n (ldexp, scalbn), the whole and fraction parts (modf); copysign,
 * fmin and fmax (a NaN loses to a number). */
double frexp(double x, int *e);
double ldexp(double x, int n);
double scalbn(double x, int n);
double modf(double x, double *whole);
double copysign(double x, double y);
double fmin(double x, double y);
double fmax(double x, double y);
float frexpf(float x, int *e);
float ldexpf(float x, int n);
float scalbnf(float x, int n);
float modff(float x, float *whole);
float copysignf(float x, float y);
float fminf(float x, float y);
float fmaxf(float x, float y);

/* (22z) Rounding in the current direction (fenv.h's fesetround): rint
 * (raises inexact for a fraction), nearbyint (never), lrint and llrint
 * (out of range: invalid, LONG_MIN); lround and llround round halfway
 * cases away from zero. */
double rint(double x);
double nearbyint(double x);
long lrint(double x);
long long llrint(double x);
long lround(double x);
long long llround(double x);
float rintf(float x);
float nearbyintf(float x);
long lrintf(float x);
long long llrintf(float x);
long lroundf(float x);
long long llroundf(float x);

/* (22z) Exact operations: remainder (x - n y, n the nearest integer to
 * x / y, ties even) and remquo (also n's low 31 bits, with x / y's sign),
 * fdim (x - y or 0), fma (x * y + z rounded once), nextafter and
 * nexttoward (the next double toward y), ilogb and logb (the exponent;
 * ilogb(0) is FP_ILOGB0), scalbln (scalbn with a long), nan (a quiet NaN;
 * the text is not used). */
double remainder(double x, double y);
double remquo(double x, double y, int *quo);
double fdim(double x, double y);
double fma(double x, double y, double z);
double nextafter(double x, double y);
double nexttoward(double x, long double y);
int ilogb(double x);
double logb(double x);
double scalbln(double x, long n);
double nan(const char *tag);
float remainderf(float x, float y);
float remquof(float x, float y, int *quo);
float fdimf(float x, float y);
float fmaf(float x, float y, float z);
float nextafterf(float x, float y);
float nexttowardf(float x, long double y);
int ilogbf(float x);
float logbf(float x);
float scalblnf(float x, long n);
float nanf(const char *tag);

/* (22z) The error function, its complement 1 - erf (precise also where
 * it is tiny), the gamma function and the logarithm of its size (no
 * signgam: lgamma's sign is tgamma's). */
double erf(double x);
double erfc(double x);
double tgamma(double x);
double lgamma(double x);
float erff(float x);
float erfcf(float x);
float tgammaf(float x);
float lgammaf(float x);

/* (22z) The long double forms of all of the above: the double functions,
 * long double having double's precision here (fabsl and copysignl keep
 * every bit). */
long double fabsl(long double x);
long double sqrtl(long double x);
long double floorl(long double x);
long double ceill(long double x);
long double truncl(long double x);
long double roundl(long double x);
long double rintl(long double x);
long double nearbyintl(long double x);
long lrintl(long double x);
long long llrintl(long double x);
long lroundl(long double x);
long long llroundl(long double x);
long double fmodl(long double x, long double y);
long double remainderl(long double x, long double y);
long double remquol(long double x, long double y, int *quo);
long double fdiml(long double x, long double y);
long double fmal(long double x, long double y, long double z);
long double fminl(long double x, long double y);
long double fmaxl(long double x, long double y);
long double copysignl(long double x, long double y);
long double nextafterl(long double x, long double y);
long double nexttowardl(long double x, long double y);
long double nanl(const char *tag);
long double frexpl(long double x, int *e);
long double ldexpl(long double x, int n);
long double scalbnl(long double x, int n);
long double scalblnl(long double x, long n);
long double modfl(long double x, long double *whole);
int ilogbl(long double x);
long double logbl(long double x);
long double sinl(long double x);
long double cosl(long double x);
long double tanl(long double x);
long double asinl(long double x);
long double acosl(long double x);
long double atanl(long double x);
long double atan2l(long double y, long double x);
long double sinhl(long double x);
long double coshl(long double x);
long double tanhl(long double x);
long double asinhl(long double x);
long double acoshl(long double x);
long double atanhl(long double x);
long double expl(long double x);
long double exp2l(long double x);
long double expm1l(long double x);
long double logl(long double x);
long double log2l(long double x);
long double log10l(long double x);
long double log1pl(long double x);
long double powl(long double x, long double y);
long double cbrtl(long double x);
long double hypotl(long double x, long double y);
long double erfl(long double x);
long double erfcl(long double x);
long double tgammal(long double x);
long double lgammal(long double x);

/* (22x) The compiler's helpers for long double on AArch64, where it is
 * IEEE binary128 in software: arithmetic, comparisons and conversions, at
 * double's precision (as the rest of long double here); called by compiled
 * code, not by programs. In the library on both architectures (x86-64's
 * long double is the x87's own and never calls them). */
long double __addtf3(long double a, long double b);
long double __subtf3(long double a, long double b);
long double __multf3(long double a, long double b);
long double __divtf3(long double a, long double b);
long double __negtf2(long double a);
long double __powitf2(long double a, int b);
int __eqtf2(long double a, long double b);
int __netf2(long double a, long double b);
int __lttf2(long double a, long double b);
int __letf2(long double a, long double b);
int __gttf2(long double a, long double b);
int __getf2(long double a, long double b);
int __cmptf2(long double a, long double b);
int __unordtf2(long double a, long double b);
long double __extendsftf2(float a);
long double __extenddftf2(double a);
float __trunctfsf2(long double a);
double __trunctfdf2(long double a);
int __fixtfsi(long double a);
long __fixtfdi(long double a);
unsigned int __fixunstfsi(long double a);
unsigned long __fixunstfdi(long double a);
long double __floatsitf(int a);
long double __floatditf(long a);
long double __floatunsitf(unsigned int a);
long double __floatunditf(unsigned long a);

#endif

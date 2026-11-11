/*
 * complex.h (step 22x): C99's complex numbers. The functions are our own,
 * computed in double from math.h's (Kahan's formulas for the inverse
 * functions, which keep their branch cuts and precision near them); the
 * float forms are the double ones rounded, the long double ones the double
 * ones widened (long double at double's precision, as strtold). Infinities
 * and NaNs as C's Annex G for cabs, carg, cproj, cexp, clog and csqrt.
 * Branch cuts: along the negative real axis for clog, csqrt and cpow, the
 * real axis outside [-1, 1] for casin, cacos and catanh, and so on, as C.
 */
#ifndef RITTEROS_COMPLEX_H
#define RITTEROS_COMPLEX_H

#define complex _Complex
#define _Complex_I (__extension__ 1.0iF)
#define I _Complex_I
#define CMPLX(x, y) __builtin_complex((double)(x), (double)(y))
#define CMPLXF(x, y) __builtin_complex((float)(x), (float)(y))
#define CMPLXL(x, y) __builtin_complex((long double)(x), (long double)(y))

double creal(double _Complex z);
double cimag(double _Complex z);
double cabs(double _Complex z);                 /* |z|, as hypot */
double carg(double _Complex z);                 /* the angle, -pi to pi */
double _Complex conj(double _Complex z);
double _Complex cproj(double _Complex z);       /* infinities onto the Riemann sphere's one */
double _Complex cexp(double _Complex z);
double _Complex clog(double _Complex z);
double _Complex cpow(double _Complex z, double _Complex w);
double _Complex csqrt(double _Complex z);
double _Complex csin(double _Complex z);
double _Complex ccos(double _Complex z);
double _Complex ctan(double _Complex z);
double _Complex casin(double _Complex z);
double _Complex cacos(double _Complex z);
double _Complex catan(double _Complex z);
double _Complex csinh(double _Complex z);
double _Complex ccosh(double _Complex z);
double _Complex ctanh(double _Complex z);
double _Complex casinh(double _Complex z);
double _Complex cacosh(double _Complex z);
double _Complex catanh(double _Complex z);

float crealf(float _Complex z);
float cimagf(float _Complex z);
float cabsf(float _Complex z);
float cargf(float _Complex z);
float _Complex conjf(float _Complex z);
float _Complex cprojf(float _Complex z);
float _Complex cexpf(float _Complex z);
float _Complex clogf(float _Complex z);
float _Complex cpowf(float _Complex z, float _Complex w);
float _Complex csqrtf(float _Complex z);
float _Complex csinf(float _Complex z);
float _Complex ccosf(float _Complex z);
float _Complex ctanf(float _Complex z);
float _Complex casinf(float _Complex z);
float _Complex cacosf(float _Complex z);
float _Complex catanf(float _Complex z);
float _Complex csinhf(float _Complex z);
float _Complex ccoshf(float _Complex z);
float _Complex ctanhf(float _Complex z);
float _Complex casinhf(float _Complex z);
float _Complex cacoshf(float _Complex z);
float _Complex catanhf(float _Complex z);

long double creall(long double _Complex z);
long double cimagl(long double _Complex z);
long double cabsl(long double _Complex z);
long double cargl(long double _Complex z);
long double _Complex conjl(long double _Complex z);
long double _Complex cprojl(long double _Complex z);
long double _Complex cexpl(long double _Complex z);
long double _Complex clogl(long double _Complex z);
long double _Complex cpowl(long double _Complex z, long double _Complex w);
long double _Complex csqrtl(long double _Complex z);
long double _Complex csinl(long double _Complex z);
long double _Complex ccosl(long double _Complex z);
long double _Complex ctanl(long double _Complex z);
long double _Complex casinl(long double _Complex z);
long double _Complex cacosl(long double _Complex z);
long double _Complex catanl(long double _Complex z);
long double _Complex csinhl(long double _Complex z);
long double _Complex ccoshl(long double _Complex z);
long double _Complex ctanhl(long double _Complex z);
long double _Complex casinhl(long double _Complex z);
long double _Complex cacoshl(long double _Complex z);
long double _Complex catanhl(long double _Complex z);

/* The compiler's helpers for * and / of complex numbers (C's Annex G:
 * an infinite operand gives an infinite result, NaN parts or not); called
 * by compiled code, not by programs. The long double pairs are x86-64's
 * (xc3) and AArch64's (tc3) names; both are in the library. */
double _Complex __muldc3(double a, double b, double c, double d);
double _Complex __divdc3(double a, double b, double c, double d);
float _Complex __mulsc3(float a, float b, float c, float d);
float _Complex __divsc3(float a, float b, float c, float d);
long double _Complex __mulxc3(long double a, long double b, long double c, long double d);
long double _Complex __divxc3(long double a, long double b, long double c, long double d);
long double _Complex __multc3(long double a, long double b, long double c, long double d);
long double _Complex __divtc3(long double a, long double b, long double c, long double d);

#endif

/*
 * long double on AArch64 (step 22x): the compiler's helpers for IEEE
 * binary128, which it calls for every long double operation. Each works at
 * double's precision (the library's long double: strtold, printf's %Lf,
 * complex.h's l forms): its arguments rounded to double (fpconv.c, from the
 * bits), the operation in double, the result widened. No long double
 * arithmetic is written here (it would call these). On x86-64 the same
 * functions exist for one interface on both, but nothing calls them.
 */
#include <math.h>
#include "fpconv.h"

#define D(x) fp_long_double(x)
#define LD(d) fp_to_long_double(d)

long double __addtf3(long double a, long double b) { return LD(D(a) + D(b)); }
long double __subtf3(long double a, long double b) { return LD(D(a) - D(b)); }
long double __multf3(long double a, long double b) { return LD(D(a) * D(b)); }
long double __divtf3(long double a, long double b) { return LD(D(a) / D(b)); }
long double __negtf2(long double a) { return LD(-D(a)); }

long double __powitf2(long double a, int b)
{
    double x = D(a), r = 1;
    unsigned n = b < 0 ? 0u - (unsigned)b : (unsigned)b;
    for (; n; n >>= 1, x *= x)
        if (n & 1)
            r *= x;
    return LD(b < 0 ? 1 / r : r);
}

/* Comparisons as compiler-rt's: 0 equal, -1 less, 1 greater; a NaN gives
 * what makes the asked comparison false (1 for eq/ne/lt/le, -1 for gt/ge). */
static int compare(long double a, long double b, int unordered)
{
    double x = D(a), y = D(b);
    if (isnan(x) || isnan(y))
        return unordered;
    return x < y ? -1 : x > y;
}

int __eqtf2(long double a, long double b) { return compare(a, b, 1); }
int __netf2(long double a, long double b) { return compare(a, b, 1); }
int __lttf2(long double a, long double b) { return compare(a, b, 1); }
int __letf2(long double a, long double b) { return compare(a, b, 1); }
int __cmptf2(long double a, long double b) { return compare(a, b, 1); }
int __gttf2(long double a, long double b) { return compare(a, b, -1); }
int __getf2(long double a, long double b) { return compare(a, b, -1); }
int __unordtf2(long double a, long double b) { return isnan(D(a)) || isnan(D(b)); }

long double __extendsftf2(float a) { return LD(a); }
long double __extenddftf2(double a) { return LD(a); }
float __trunctfsf2(long double a) { return (float)D(a); }
double __trunctfdf2(long double a) { return D(a); }
int __fixtfsi(long double a) { return (int)D(a); }
long __fixtfdi(long double a) { return (long)D(a); }
unsigned int __fixunstfsi(long double a) { return (unsigned int)D(a); }
unsigned long __fixunstfdi(long double a) { return (unsigned long)D(a); }
long double __floatsitf(int a) { return LD(a); }
long double __floatditf(long a) { return LD((double)a); }
long double __floatunsitf(unsigned int a) { return LD(a); }
long double __floatunditf(unsigned long a) { return LD((double)a); }

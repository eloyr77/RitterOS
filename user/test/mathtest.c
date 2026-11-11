/*
 * Test: math.h's functions (22s), every bit. Prints each function's result
 * (%a) for fixed arguments (the special ones of C's Annex F among them) and
 * for pseudo-random ones over its range. The test builds this program on
 * the Mac too, against our own user/lib/math.c (the functions renamed, so
 * the Mac's libm stays out): both must print the same, so the library
 * computes on RitterOS exactly what was checked on the Mac against an
 * exact reference (Python's decimal; see CLAUDE.md, 22s). "mathtest exact"
 * prints arguments with results, which the test checks itself against the
 * exact values (scripts/mathref.py).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define T "mathtest: "

static uint64_t state = 0x2545F4914F6CDD1DULL;

static double unit(void)
{
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)(state >> 11) * 0x1p-53;
}

static double between(double lo, double hi)
{
    return lo + (hi - lo) * unit();
}

static double scaled(int e1, int e2)
{
    double m = 1 + unit();
    int e = e1 + (int)(unit() * (e2 - e1 + 1));
    return ldexp(m, e) * (unit() < 0.5 ? -1 : 1);
}

static const double special[] = { 0.0, -0.0, 1.0, -1.0, 2.0, 0.5, -0.5, 3.0, 10.0, 100.0,
                                  1e-300, 0x1p-1074, 0x1p-1022, 1e308, 710.0, -746.0, 0.25,
                                  1.0 / 0.0, -1.0 / 0.0, 0.0 / 0.0 };
#define SPECIALS (int)(sizeof(special) / sizeof(special[0]))

static void one(const char *name, double (*f)(double), double lo, double hi, int e1, int e2)
{
    printf(T "%s", name);
    for (int i = 0; i < SPECIALS; i++)
        printf(" %a", f(special[i]));
    printf("\n" T "%s", name);
    for (int i = 0; i < 24; i++)
        printf(" %a", f(between(lo, hi)));
    printf("\n" T "%s", name);
    for (int i = 0; i < 24; i++)
        printf(" %a", f(scaled(e1, e2)));
    printf("\n");
}

static void two(const char *name, double (*f)(double, double), double lo, double hi)
{
    printf(T "%s", name);
    for (int i = 0; i < SPECIALS; i += 2)
        for (int j = 1; j < SPECIALS; j += 3)
            printf(" %a", f(special[i], special[j]));
    printf("\n" T "%s", name);
    for (int i = 0; i < 24; i++) {
        double x = between(lo, hi), y = between(lo, hi);
        printf(" %a", f(x, y));
    }
    printf("\n");
}

static double fmod_(double x, double y) { return fmod(x, y); }
static double ldexp_(double x) { return ldexp(x, 37); }
static double frexp_(double x)
{
    int e;
    double m = frexp(x, &e);
    return m + e;
}
static double modf_(double x)
{
    double w;
    double f = modf(x, &w);
    return f + w * 3;
}
static double sinf_(double x) { return sinf((float)x); }
static double powf_(double x, double y) { return powf((float)x, (float)y); }
static double expf_(double x) { return expf((float)x); }
static double logf_(double x) { return logf((float)x); }
static double atanf_(double x) { return atanf((float)x); }

/* mathtest exact: arguments and results ("mathtest: exact NAME X Y R"),
 * which scripts/mathref.py checks against the exact values. */
static void exact1(const char *name, double (*f)(double), double lo, double hi, int e1, int e2)
{
    for (int i = 0; i < 16; i++) {
        double x = i % 2 ? between(lo, hi) : scaled(e1, e2);
        if (lo >= 0 && x < 0)
            x = -x;
        printf(T "exact %s %a 0x0p+0 %a\n", name, x, f(x));
    }
}

static void exact2(const char *name, double (*f)(double, double), double lo, double hi)
{
    for (int i = 0; i < 16; i++) {
        double x = between(lo, hi), y = between(lo, hi);
        printf(T "exact %s %a %a %a\n", name, x, y, f(x, y));
    }
}

static double atan2_yx(double x, double y) { return atan2(y, x); }  /* (mathref: x, y order) */
static double fma1(double x, double y) { return fma(x, y, x); }
static double fma2(double x, double y) { return fma(x, y, -(x * y)); }  /* (the product's error) */

static void exact_all(void)
{
    exact1("sin", sin, -10, 10, -30, 1023);
    exact1("cos", cos, -10, 10, -30, 1023);
    exact1("tan", tan, -10, 10, -30, 1023);
    exact1("asin", asin, -1, 1, -60, -1);
    exact1("acos", acos, -1, 1, -60, -1);
    exact1("atan", atan, -10, 10, -1074, 1023);
    exact1("sinh", sinh, -30, 30, -60, 9);
    exact1("cosh", cosh, -30, 30, -60, 9);
    exact1("tanh", tanh, -30, 30, -60, 9);
    exact1("asinh", asinh, -10, 10, -1074, 1023);
    exact1("acosh", acosh, 1, 10, 0, 1023);
    exact1("atanh", atanh, -1, 1, -60, -1);
    exact1("exp", exp, -745, 709, -60, 9);
    exact1("exp2", exp2, -1074, 1023, -60, 9);
    exact1("expm1", expm1, -30, 30, -60, 9);
    exact1("log", log, 0, 10, -1074, 1023);
    exact1("log2", log2, 0, 10, -1074, 1023);
    exact1("log10", log10, 0, 10, -1074, 1023);
    exact1("log1p", log1p, 0, 10, -80, 1000);
    exact1("cbrt", cbrt, -10, 10, -1074, 1023);
    exact2("pow", pow, 0, 20);
    exact2("hypot", hypot, -1e3, 1e3);
    exact2("atan2", atan2_yx, -10, 10);
    exact1("erf", erf, -6, 6, -60, 2);
    exact1("erfc", erfc, -6, 27, -60, 4);
    exact1("lgamma", lgamma, -30, 40, -60, 900);
    exact1("tgamma", tgamma, -40, 171, -60, 7);
    exact2("fma1", fma1, -1e10, 1e10);
    exact2("fma2", fma2, -1e10, 1e10);
    exact2("remainder", remainder, -1e6, 1e6);
    exact2("nextafter", nextafter, -10, 10);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "exact")) {
        exact_all();
        printf(T "done\n");
        return 0;
    }
    one("sin", sin, -10, 10, -30, 1023);
    one("cos", cos, -10, 10, -30, 1023);
    one("tan", tan, -10, 10, -30, 1023);
    one("asin", asin, -1, 1, -60, -1);
    one("acos", acos, -1, 1, -60, -1);
    one("atan", atan, -10, 10, -1074, 1023);
    one("sinh", sinh, -30, 30, -60, 9);
    one("cosh", cosh, -30, 30, -60, 9);
    one("tanh", tanh, -30, 30, -60, 9);
    one("asinh", asinh, -10, 10, -1074, 1023);
    one("acosh", acosh, 1, 10, 0, 1023);
    one("atanh", atanh, -1, 1, -60, -1);
    one("exp", exp, -745, 710, -60, 9);
    one("exp2", exp2, -1075, 1024, -60, 10);
    one("expm1", expm1, -30, 30, -60, 9);
    one("log", log, 0, 10, -1074, 1023);
    one("log2", log2, 0, 10, -1074, 1023);
    one("log10", log10, 0, 10, -1074, 1023);
    one("log1p", log1p, -1, 10, -80, 1000);
    one("cbrt", cbrt, -10, 10, -1074, 1023);
    one("ldexp", ldexp_, -10, 10, -1074, 1023);
    one("frexp", frexp_, -10, 10, -1074, 1023);
    one("modf", modf_, -10, 10, -1074, 60);
    one("sinf", sinf_, -10, 10, -30, 60);
    one("expf", expf_, -100, 80, -30, 6);
    one("logf", logf_, 0, 100, -140, 120);
    one("atanf", atanf_, -10, 10, -140, 120);
    one("rint", rint, -10, 10, -60, 60);
    one("erf", erf, -6, 6, -60, 2);
    one("erfc", erfc, -6, 27, -60, 4);
    one("lgamma", lgamma, -30, 40, -60, 900);
    one("tgamma", tgamma, -40, 171, -60, 7);
    one("logb", logb, -10, 10, -1074, 1023);
    two("atan2", atan2, -10, 10);
    two("pow", pow, 0, 20);
    two("hypot", hypot, -1e3, 1e3);
    two("fmod", fmod_, -1e6, 1e6);
    two("powf", powf_, 0, 10);
    two("remainder", remainder, -1e6, 1e6);
    two("fdim", fdim, -10, 10);
    two("nextafter", nextafter, -10, 10);
    two("fma1", fma1, -1e10, 1e10);
    two("fma2", fma2, -1e10, 1e10);
    printf(T "done\n");
    return 0;
}

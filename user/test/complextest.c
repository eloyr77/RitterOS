/*
 * Test: complex.h (22x). "complextest exact" prints arguments and results
 * ("complextest: exact NAME X Y U V RE IM", hex floats; U V only for cpow),
 * which the test checks against exact values (scripts/complexref.py:
 * Python's decimal); without it, the special values of C's Annex G, the
 * float and long double forms, and the compiler's * and / helpers, each
 * checked here.
 */
#include <complex.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define T "complextest: "

static uint64_t state = 0x9E3779B97F4A7C15ULL;

static double unit(void)
{
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)(state >> 11) * 0x1p-53;
}

/* A value of either sign: often near 1, sometimes tiny or huge. */
static double pick(int e1, int e2)
{
    double r = unit(), v;
    if (r < 0.4)
        v = unit() * 4 - 2;
    else
        v = ldexp(1 + unit(), e1 + (int)(unit() * (e2 - e1 + 1)));
    return unit() < 0.5 ? -v : v;
}

typedef double complex (*one_fn)(double complex);

static void exact1(const char *name, one_fn f, int e1, int e2)
{
    for (int i = 0; i < 400; i++) {
        double x = pick(e1, e2), y = pick(e1, e2);
        if (i % 25 == 0)
            y = 0.0;                            /* (on the real axis, also its cuts) */
        double complex r = f(CMPLX(x, y));
        printf(T "exact %s %a %a 0x0p+0 0x0p+0 %a %a\n", name, x, y, creal(r), cimag(r));
    }
}

static void exact_all(void)
{
    exact1("cexp", cexp, -30, 9);
    exact1("clog", clog, -1000, 1020);
    exact1("csqrt", csqrt, -1000, 1020);
    exact1("csin", csin, -30, 9);
    exact1("ccos", ccos, -30, 9);
    exact1("ctan", ctan, -30, 9);
    exact1("csinh", csinh, -30, 9);
    exact1("ccosh", ccosh, -30, 9);
    exact1("ctanh", ctanh, -30, 9);
    exact1("casin", casin, -60, 60);
    exact1("cacos", cacos, -60, 60);
    exact1("catan", catan, -60, 60);
    exact1("casinh", casinh, -60, 60);
    exact1("cacosh", cacosh, -60, 60);
    exact1("catanh", catanh, -60, 60);
    for (int i = 0; i < 400; i++) {
        double x = pick(-10, 10), y = pick(-10, 10), u = pick(-3, 2), v = pick(-3, 2);
        if (i % 20 == 0)
            v = 0.0;
        double complex r = cpow(CMPLX(x, y), CMPLX(u, v));
        printf(T "exact cpow %a %a %a %a %a %a\n", x, y, u, v, creal(r), cimag(r));
    }
}

static int failures;

static void same(const char *what, double complex got, double re, double im)
{
    int ok = (isnan(re) ? isnan(creal(got)) : creal(got) == re && signbit(creal(got)) == signbit(re)) &&
             (isnan(im) ? isnan(cimag(got)) : cimag(got) == im && signbit(cimag(got)) == signbit(im));
    if (!ok) {
        printf(T "FAIL %s: (%a, %a), not (%a, %a)\n", what, creal(got), cimag(got), re, im);
        failures++;
    }
}

#if defined(__x86_64__)
#define LONG_BYTES 10                           /* (x87: the rest is padding) */
#else
#define LONG_BYTES sizeof(long double)
#endif

static void same_l(const char *what, long double complex got, long double re, long double im)
{
    long double gre = creall(got), gim = cimagl(got);
    if (memcmp(&gre, &re, LONG_BYTES) || memcmp(&gim, &im, LONG_BYTES)) {
        printf(T "FAIL %s\n", what);
        failures++;
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "exact")) {
        exact_all();
        printf(T "done\n");
        return 0;
    }
    const double inf = INFINITY, nan = NAN;
    /* Annex G */
    same("cabs(inf, nan)", cabs(CMPLX(inf, nan)), inf, 0);
    same("cproj(inf, -2)", cproj(CMPLX(inf, -2)), inf, -0.0);
    same("cproj(nan, -inf)", cproj(CMPLX(nan, -inf)), inf, -0.0);
    same("cexp(0, -0)", cexp(CMPLX(0, -0.0)), 1, -0.0);
    same("cexp(inf, 0)", cexp(CMPLX(inf, 0)), inf, 0);
    same("cexp(-inf, 1)", cexp(CMPLX(-inf, 1)), 0 * cos(1.0), 0 * sin(1.0));
    same("cexp(-inf, inf)", cexp(CMPLX(-inf, inf)), 0, 0);
    same("cexp(nan, 0)", cexp(CMPLX(nan, 0)), nan, 0);
    same("cexp(1, inf)", cexp(CMPLX(1, inf)), nan, nan);
    same("clog(-0, 0)", clog(CMPLX(-0.0, 0)), -inf, M_PI);
    same("clog(0, -0)", clog(CMPLX(0, -0.0)), -inf, -0.0);
    same("clog(-inf, 1)", clog(CMPLX(-inf, 1)), inf, M_PI);
    same("clog(nan, inf)", clog(CMPLX(nan, inf)), inf, nan);
    same("clog(-1, -0)", clog(CMPLX(-1, -0.0)), 0, -M_PI);
    same("csqrt(-4, 0)", csqrt(CMPLX(-4, 0)), 0, 2);
    same("csqrt(-4, -0)", csqrt(CMPLX(-4, -0.0)), 0, -2);
    same("csqrt(0, -0)", csqrt(CMPLX(0, -0.0)), 0, -0.0);
    same("csqrt(nan, inf)", csqrt(CMPLX(nan, inf)), inf, inf);
    same("csqrt(-inf, 3)", csqrt(CMPLX(-inf, 3)), 0, inf);
    same("csqrt(inf, nan)", csqrt(CMPLX(inf, nan)), inf, nan);
    /* exact results */
    same("csqrt(3, 4)", csqrt(CMPLX(3, 4)), 2, 1);
    same("casin(0, 0)", casin(CMPLX(0, 0)), 0, 0);
    same("catanh(-0, 0)", catanh(CMPLX(-0.0, 0)), -0.0, 0);
    same("conj(1, 0)", conj(CMPLX(1, 0)), 1, -0.0);

    /* the operators, through __muldc3/__divdc3 */
    volatile double one = 1, zero = 0;
    double complex a = CMPLX(one, 2), b = CMPLX(3, -one);
    same("(1+2i)(3-i)", a * b, 5, 5);
    same("(1+2i)/(3-i)", a / b, 0.1, 0.7);
    same("(inf+nan i)(1+i): an infinity", cabs(CMPLX(inf, nan) * CMPLX(one, one)), inf, 0);
    same("1/(0+0i): an infinity", cabs(CMPLX(one, 0) / CMPLX(zero, zero)), inf, 0);
    same("1/(inf+inf i): 0", CMPLX(one, one) / CMPLX(inf, inf), 0, 0);
    double complex big = CMPLX(1e300, 1e300) / CMPLX(1e300 * one, 1e300);
    if (!(fabs(creal(big) - 1) < 1e-15 && fabs(cimag(big)) < 1e-15)) {
        printf(T "FAIL 1e300(1+i)/1e300(1+i): (%a, %a)\n", creal(big), cimag(big));
        failures++;
    }

    /* float and long double forms: the double results rounded and widened */
    float complex fz = CMPLXF(0.5f, -1.25f);
    double complex dz = CMPLX(0.5, -1.25);
    float complex fr = ccosf(fz);
    double complex dr = ccos(dz);
    same("ccosf", CMPLX(crealf(fr), cimagf(fr)), (float)creal(dr), (float)cimag(dr));
    float complex fq = fz / CMPLXF(2, 1);
    same("float /", CMPLX(crealf(fq), cimagf(fq)), (float)-0.05, (float)-0.6);
    /* (long double: compared by its bits with constants; on ARM converting
     * it at run time needs the compiler's helpers, which are not there) */
    long double complex lz = CMPLXL(0.5, -1.25);
    same_l("long double *", lz * CMPLXL(2, 1), 2.25L, -2.0L);
    same_l("long double /", CMPLXL(2.25, -2) / CMPLXL(2, 1), 0.5L, -1.25L);
    same_l("conjl", conjl(lz), 0.5L, 1.25L);
    same_l("csqrtl(3, 4)", csqrtl(CMPLXL(3, 4)), 2.0L, 1.0L);
    same_l("cexpl(0, -0)", cexpl(CMPLXL(0, -0.0)), 1.0L, -0.0L);
    /* at run time (volatile): on ARM through the binary128 helpers */
    volatile long double p = 1.5L, q = 2.25L, r = -0.5L;
    long double complex lv = CMPLXL(p, r) * CMPLXL(q, p);      /* (1.5 - 0.5i)(2.25 + 1.5i) */
    same_l("long double complex * at run time", lv, 4.125L, 1.125L);
    same_l("long double complex / at run time", lv / CMPLXL(q, p), 1.5L, -0.5L);
    long double sum = p + q, product = p * q, quotient = q / p, difference = p - q;
    same_l("long double + * / - at run time", CMPLXL(sum, product), 3.75L, 3.375L);
    same_l("long double / - at run time", CMPLXL(quotient, difference), 1.5L, -0.75L);
    volatile int seven = 7;
    long double from_int = seven;
    int back = (int)(p * 4), compared = (p < q) + 2 * (q > p) + 4 * (p == p) + 8 * (p != q) +
                                       16 * (r <= p) + 32 * !(p >= q);
    double as_double = (double)q;
    same_l("int to long double", CMPLXL(from_int, 0), 7.0L, 0.0L);
    if (back != 6 || compared != 63 || as_double != 2.25) {
        printf(T "FAIL long double conversions and comparisons: %d %d %a\n", back, compared, as_double);
        failures++;
    }

    printf(T "%s\n", failures ? "FAILED" : "all ok");
    return failures != 0;
}

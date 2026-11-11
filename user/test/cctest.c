/*
 * cctest (60): C for the compiler's test. The build makes it with clang
 * (/system/<arch>/test/cctest); the test compiles this source again with
 * RitterOS's own cc, as and ld on RitterOS, runs both, and wants the same
 * lines: integer and floating arithmetic and conversions, long double,
 * structures passed and returned (each ABI class), variadic functions,
 * bit-fields, initializers, VLAs, function pointers and the GNU extras the
 * system's sources use; since 61b also what the backends fold into their
 * instructions (offsets beyond a page, indexes, immediates, compares),
 * since 61c what ir.c folds and the registers kept across calls; since 62a
 * __int128 and _Complex; since 62b per-thread variables; since 62c inline
 * assembly; since 65e-52 _Atomic objects and <stdatomic.h>, also from
 * threads running at once (a lost update changes the totals); since 39a
 * floats and doubles in registers, since 39b inlined calls with structure
 * values and small structures kept as values, since 39c common
 * subexpressions and loops' invariants and induction variables, since
 * 39d loops testing at their bottom, since 39e unrolled loops, since 39f
 * loops on 4 elements at a time and int16 products 8 at a time.
 */
#include <complex.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); fails++; } } while (0)

static volatile int vzero = 0, vone = 1, vminus = -1;
static volatile unsigned vu = 0xFFFFFFF0u;
static volatile long vl = -1234567890123L;
static volatile unsigned char vuc = 200;
static volatile signed char vsc = -100;
static volatile short vs = -30000;
static volatile unsigned short vus = 60000;

static void integers(void)
{
    int a = 7, b = -3;
    printf("int: %d %d %d %d %d\n", a + b, a - b, a * b, a / b, a % b);
    printf("shifts: %d %d %u %d\n", -16 >> 2, 1 << 30, vu >> 4, (int)(vu << 3));
    printf("unsigned: %u %u %u\n", vu / 7, vu % 7, vu * 3u);
    printf("long: %ld %ld %ld\n", vl / 1000, vl % 1000, vl * 3);
    printf("unsigned long: %lu %lu\n", (unsigned long)vl / 7, (unsigned long)vl % 7);
    printf("chars: %d %d %d %d\n", vuc, vsc, vuc + vsc, (char)(vuc + 100));
    printf("shorts: %d %d %d\n", vs, vus, vs * vus);
    printf("compare: %d %d %d %d\n", (unsigned)vminus < vu, vminus < (int)vu, (long)vminus < 0L, vuc > vsc);
    printf("bool: %d %d %d\n", (bool)2, (bool)0.5, (bool)vzero);
    int64_t big = INT64_MAX;
    printf("int64: %lld %lld\n", (long long)big, (long long)(big + vminus));
    uint32_t u32 = 0x80000000u;
    printf("u32: %u %d %llu\n", u32 >> 31, (int32_t)u32 >> 31, (unsigned long long)u32 << 1);
    printf("neg: %d %u %ld\n", -vone, -(unsigned)vone, -(long)vone);
    printf("bitops: %x %x %x %x\n", 0xF0F0 & 0x3C3C, 0xF0F0 | 0x3C3C, 0xF0F0 ^ 0x3C3C, ~0xF0F0);
    printf("logic: %d %d %d %d\n", vone && vzero, vone || vzero, !vone, !vzero);
    int c = 5;
    c += 3; c -= 1; c *= 4; c /= 3; c %= 5; c <<= 3; c >>= 1; c &= 0xFF; c |= 0x100; c ^= 0x11;
    printf("compound: %d\n", c);
    unsigned char uc = 250;
    uc += 10;
    printf("wrap: %d\n", uc);
    int i = 5;
    int j = i++;
    j += ++i;
    int k = i--;
    int m = --i;
    printf("incdec: %d %d %d %d\n", i, j, k, m);
    long double ld = 3;
    printf("ternary: %d %ld\n", vone ? 10 : 20, vzero ? 1L : (long)ld);
}

static void floats(void)
{
    volatile double d = 2.5, e = -0.75;
    volatile float f = 1.25f;
    printf("double: %g %g %g %g\n", d + e, d - e, d * e, d / e);
    printf("float: %g %g\n", (double)(f * 3), (double)(f / 3));
    printf("conv: %d %ld %u %lu\n", (int)d, (long)e, (unsigned)d, (unsigned long)(d * 1e18));
    printf("conv2: %g %g %g %g\n", (double)vl, (double)vu, (double)(unsigned long)vl, (double)vsc);
    printf("compare: %d %d %d %d\n", d > e, d < e, d == 2.5, f != 1.25f);
    volatile double nan = 0.0 / vzero;
    printf("nan: %d %d %d %d %d\n", nan == nan, nan != nan, nan < 1, nan >= 1, !nan);
    printf("neg: %g %g\n", -d, (double)-f);
    float g = (float)d;
    double h = f;
    printf("f2f: %g %g\n", (double)g, h);
    unsigned long big = 18446744073709551615ul;
    printf("u2f: %.0f %.0f\n", (double)big, (double)(big / 3));
    volatile double huge = 1.8e19;
    printf("f2u: %lu\n", (unsigned long)huge);
    long double ld = d;
    ld = ld * 3 + 1;
    printf("long double: %g %d %d\n", (double)ld, ld > 8, (int)(ld / 2));
    long double l2 = vl;
    printf("ld conv: %ld %g\n", (long)(l2 / 10), (double)(ld - l2));
    unsigned long uv = (unsigned long)(long double)big;
    printf("ld u64: %lu\n", uv);
}

/* (39b: the functions passing structures are noinline, so the calling
 * convention stays tested now that cc inlines such functions) */
struct point {
    int x, y;
};
struct mixed {
    char c;
    double d;
    short s;
};
struct big {
    long a[5];
};
struct two_doubles {
    double a, b;
};
struct small {
    char a, b, c;
};
struct float_int {
    float f;
    int i;
};

__attribute__((noinline)) static struct point make_point(int x, int y)
{
    struct point p = { x, y };
    return p;
}

__attribute__((noinline)) static struct big make_big(long base)
{
    struct big b;
    for (int i = 0; i < 5; i++)
        b.a[i] = base + i;
    return b;
}

__attribute__((noinline)) static long sum_big(struct big b, int k)
{
    long s = 0;
    for (int i = 0; i < 5; i++)
        s += b.a[i] * k;
    return s;
}

__attribute__((noinline)) static double dot(struct two_doubles t, struct point p)
{
    return t.a * p.x + t.b * p.y;
}

__attribute__((noinline)) static struct small small_of(struct small s)
{
    s.a++;
    s.c += 2;
    return s;
}

__attribute__((noinline)) static struct float_int fi(struct float_int x, double scale)
{
    x.f *= (float)scale;
    x.i *= 2;
    return x;
}

static void structs(void)
{
    struct point p = make_point(3, 4);
    printf("point: %d %d\n", p.x, p.y);
    struct big b = make_big(100);
    printf("big: %ld %ld\n", b.a[0], sum_big(b, 2));
    struct two_doubles t = { 1.5, -2 };
    printf("dot: %g\n", dot(t, p));
    struct small s = { 1, 2, 3 };
    s = small_of(s);
    printf("small: %d %d %d\n", s.a, s.b, s.c);
    struct float_int x = { 1.5f, 21 };
    x = fi(x, 4);
    printf("float_int: %g %d\n", (double)x.f, x.i);
    struct mixed m = { 'a', 3.25, 7 };
    printf("mixed: %c %g %d %zu\n", m.c, m.d, m.s, sizeof(m));
    struct point arr[3] = { [1] = { 5, 6 }, { .y = 9 } };
    printf("arr: %d %d %d %d %d %d\n", arr[0].x, arr[0].y, arr[1].x, arr[1].y, arr[2].x, arr[2].y);
    struct point *pp = &arr[1];
    pp->x += 10;
    (pp + 1)->x = -1;
    printf("ptr: %d %d %td\n", arr[1].x, arr[2].x, pp - arr);
    struct point q = p;
    q.x = 100;
    printf("copy: %d %d\n", p.x, q.x);
    union {
        int i;
        float f;
        unsigned char b[4];
    } u = { .f = 1.0f };
    printf("union: %x %d\n", u.i, u.b[3]);
}

struct bits {
    unsigned a : 3;
    signed b : 4;
    unsigned c : 9;
    int : 0;
    unsigned d : 30;
    _Bool e : 1;
};

static void bitfields(void)
{
    struct bits x = { 5, -3, 300, 1000000, 1 };
    printf("bits: %u %d %u %u %d %zu\n", x.a, x.b, x.c, x.d, x.e, sizeof(x));
    int nine = vone * 9, seven = vone * 7, fifteen = vone * 15;
    x.a = nine;                         /* (cut to 3 bits: 1) */
    x.b = seven;
    x.c += 300;
    x.b++;
    printf("bits2: %u %d %u\n", x.a, x.b, x.c);
    int v = (x.a = fifteen);
    printf("bits3: %d %u\n", v, x.a);
}

static int fib(int n)
{
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

static int classify(int c)
{
    switch (c) {
    case 0:
        return 100;
    case 1:
    case 2:
        return 200;
    case 'a' ... 'z':
        return 300;
    default:
        return -1;
    case 1000:
        return 400;
    }
}

static int loops(void)
{
    int total = 0;
    for (int i = 0; i < 10; i++) {
        if (i == 3)
            continue;
        if (i == 8)
            break;
        total += i;
    }
    int k = 0;
    while (k < 5)
        k += 2;
    do
        k--;
    while (k > 2);
    int g = 0;
again:
    g++;
    if (g < 4)
        goto again;
    return total * 100 + k * 10 + g;
}

static int sum_ints(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    int s = 0;
    for (int i = 0; i < n; i++)
        s += va_arg(ap, int);
    va_end(ap);
    return s;
}

static double sum_mixed(const char *kinds, ...)
{
    va_list ap, copy;
    va_start(ap, kinds);
    va_copy(copy, ap);
    double s = 0;
    for (const char *k = kinds; *k; k++) {
        if (*k == 'i')
            s += va_arg(ap, int);
        else if (*k == 'l')
            s += (double)va_arg(ap, long);
        else if (*k == 'd')
            s += va_arg(ap, double);
        else if (*k == 'L')
            s += (double)va_arg(ap, long double);
        else if (*k == 'p') {
            struct point p = va_arg(ap, struct point);
            s += p.x * 1000 + p.y;
        } else if (*k == 'b') {
            struct big b = va_arg(ap, struct big);
            s += (double)b.a[4];
        } else if (*k == 't') {
            struct two_doubles t = va_arg(ap, struct two_doubles);
            s += t.a - t.b;
        }
    }
    double first = va_arg(copy, int);
    va_end(copy);
    va_end(ap);
    return s + first / 1000;
}

static void varargs(void)
{
    printf("sum_ints: %d\n", sum_ints(10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
    struct point p = { 7, 8 };
    struct big b = make_big(40);
    struct two_doubles t = { 10, 4 };
    printf("sum_mixed: %g\n", sum_mixed("idlLpbt", 1, 2.5, 30L, (long double)0.25, p, b, t));
    printf("many doubles: %g\n", sum_mixed("dddddddddddi", 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0, 12));
    char buf[64];
    snprintf(buf, sizeof(buf), "%5.2f|%-4d|%s|%c|%lx", 3.14159, 42, "str", 'Z', 0xdeadbeefUL);
    printf("snprintf: %s\n", buf);
}

static int cmp_ints(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

static int apply(int (*f)(int), int x)
{
    return f(x);
}

static int twice(int x)
{
    return 2 * x;
}

static const char *names[] = { "zero", "one", "two" };
static int table[] = { 1, 2, 3, [10] = 11 };
static struct point origin_points[] = { { 1, 2 }, { 3, 4 } };
static int *table_end = &table[11];
static const char greeting[] = "hello";
static char writable[] = "world";
static long long_value = 5000000000L;
static double dvalue = -1.5e300;
static float fvalue = 0.1f;

static void pointers_and_data(void)
{
    int a[] = { 5, 3, 9, 1, 7 };
    qsort(a, 5, sizeof(int), cmp_ints);
    printf("qsort: %d %d %d %d %d\n", a[0], a[1], a[2], a[3], a[4]);
    printf("fnptr: %d %d\n", apply(twice, 21), apply(fib, 15));
    printf("names: %s %s %zu\n", names[2], names[0], sizeof(names) / sizeof(names[0]));
    printf("table: %d %d %zu %td\n", table[2], table[10], sizeof(table), table_end - table);
    printf("points: %d\n", origin_points[1].y);
    printf("strings: %s %s %zu %d\n", greeting, writable, sizeof(greeting), greeting[1]);
    writable[0] = 'W';
    printf("writable: %s\n", writable);
    printf("data: %ld %g %g\n", long_value, dvalue, (double)fvalue);
    char *s = malloc(32);
    strcpy(s, "dynamic");
    strcat(s, " memory");
    printf("malloc: %s %zu\n", s, strlen(s));
    free(s);
    int matrix[3][4];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            matrix[i][j] = i * 10 + j;
    int (*row)[4] = &matrix[1];
    printf("matrix: %d %d %zu\n", matrix[2][3], (*row)[2], sizeof(matrix));
    int n = vone + 4;
    int vla[n];
    for (int i = 0; i < n; i++)
        vla[i] = i * i;
    printf("vla: %d %zu\n", vla[4], sizeof(vla));
    int *cl = (int[]){ 4, 5, 6 };
    printf("compound literal: %d\n", cl[1] + ((struct point){ .y = 2 }).y);
    printf("fib: %d classify: %d %d %d %d %d\n", fib(20), classify(0), classify(2), classify('q'), classify(1000),
           classify(-5));
    printf("loops: %d\n", loops());
    int x = ({
        int t = 6;
        t * 7;
    });
    printf("stmt expr: %d\n", x);
    printf("sizeof: %zu %zu %zu %zu %zu\n", sizeof(long double), sizeof(struct mixed), _Alignof(double),
           sizeof(struct bits), sizeof(union { char c; long l; }));
}

/* (61b) what the backends fold into instructions: immediates, frame and
 * global addresses with offsets (beyond a page, too), indexed loads and
 * stores (negative indexes as well), compare-and-branch */
static int garr[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
static char gtext[16] = "abcdefghijklmno";
static long glong[600];
static struct {
    char buf[8192];
    int len;
    short tail[3000];
} gbig;
static int gpages[6000];

static unsigned hash_text(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static void addressing(void)
{
    int sum = 0;
    for (int n = 0; n < 8; n++)
        sum += garr[n] * n;
    for (int n = 0; n < 15; n++)
        gtext[n] ^= 0x20;
    printf("sum: %d %s\n", sum, gtext);
    for (int n = 0; n < 600; n++)
        glong[n] = n * 3;
    long t = 0;
    for (int n = 599; n >= 0; n -= 7)
        t += glong[n];
    printf("strided: %ld %ld %ld\n", t, glong[599], glong[512]);
    int *mid = garr + 4;
    int ni = vminus * 3;
    long nl = vminus * 2L;
    unsigned char bytes[8] = { 9, 8, 7, 6, 5, 4, 3, 2 };
    short hs[4] = { -1, -2, -3, -4 };
    double ds[4] = { 0.5, 1.5, 2.5, 3.5 };
    int bi = vone * 3;
    printf("indexes: %d %d %d %d %d %g\n", mid[ni], mid[nl], bytes[bi], hs[bi - 1], mid[-1], ds[bi]);
    mid[ni] = 40;
    hs[bi] = 7;
    ds[bi - 2] = -9.0;
    bytes[bi + 1] = 0;
    printf("stores: %d %d %g %d\n", garr[1], hs[3], ds[1], bytes[4]);
    gbig.len = 77;
    gbig.tail[2999] = -5;
    gbig.buf[4095] = 9;
    gbig.len += gbig.buf[4095];
    gpages[1] = 11;
    gpages[1030] = 22;
    gpages[5999] = 44;
    for (int n = 0; n < 6000; n += 1000)
        gpages[n] += n;
    printf("pages: %d %d %d %d %d %d\n", gbig.len, gbig.tail[2999], gpages[1], gpages[1030], gpages[5999],
           gpages[5000]);
    unsigned u = vu;
    long l = vl;
    printf("immediates: %x %x %x %x %x %lx %lx %ld %ld %d\n", u & 0xFF00FF00u, u | 0x0F, u ^ 0xFFFF, u >> 4, u << 3,
           l & 0x00FFFF0000000000L, l | 7, l * 8, l * -5, vminus * 1103515245);
    int x = vone * 100, k = 0;
    if (x < 4095) k |= 1;
    if (0 <= x) k |= 2;
    if (4000 > x) k |= 4;
    if ((unsigned)x <= 99u) k |= 8;
    if (100 == x) k |= 16;
    printf("compares: %d\n", k);
    double d = 1.5, e = 0.0;
    int z = 0;
    for (int n = 0; n < 4; n++) {
        if (d < e) z |= 1;
        if (d > e) z |= 2;
        if (d == e) z |= 4;
        if (d != e) z |= 8;
        e += 1.0;
    }
    printf("float compares: %d hash: %u\n", z, hash_text("key-12345\xe9"));
    int a = 5, b = a++;
    b += (a += 2, a * 3);
    printf("increments: %d %d\n", a, b);
}

static int counter;
static int bump(void)
{
    return ++counter;
}

/* (61c) what ir.c works out before the backends: constants folded at
 * their edges, additions merged, constants first, copies forwarded,
 * loads narrowed, the order of an assignment's sides; and values kept in
 * registers across calls, large copies, memset and atomics (x86's r8, r9,
 * rsi and rdi must not hold a value over them) */
static long keep_many(long a, long b, long c, long d, long e, long f)
{
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f;
}

static const char *pick_text(void)
{
    return vone ? "ab&c" : "xy";
}

/* a copy inside a loop from a value made before it (awk's gsub: the
 * pointer must start again at each pass) */
static int rescan(void)
{
    const char *text = pick_text();
    int n = 0, passes = 0;
    while (passes < 3) {
        for (const char *r = text; *r; r++)
            n += *r == '&' ? 10 : 1;
        passes++;
    }
    return n;
}

static void folding(void)
{
    printf("constants: %d %d %d %u %ld %lu %d %d\n", (unsigned char)300, (signed char)200, (short)70000,
           (unsigned)-1, (long)(int)0x80000000u, (unsigned long)(unsigned)-5, -7 / 2, -7 % 2);
    printf("constant shifts: %d %u %ld %d %d\n", -16 >> 2, 0x80000000u >> 31, 1L << 40, 1 << 30, (int)(5u - 7u > 3));
    printf("constant compares: %d %d %d %d\n", -1 < 1, -1 < 1u, (unsigned char)255 == 255, (long)-1 > 0xFFFFFFFFu);
    int *p = garr;
    int *q = (p + 1) + 2;
    long base = vone * 1000;
    printf("merged: %d %ld %ld %d\n", *q, (base + 8) + 4, base * 1 + 0, (int)((char *)(q + 3) - (char *)p));
    int x = vone * 7, s = 0;
    for (int i = 0; i < 10; i++)
        s += 2 * i + (0 <= i - 5) + (3 > i);
    int old = x++, older = x--;
    printf("left constants: %d %d %d %d\n", s, 3 * x, old, older);
    unsigned char ub[4] = { 200, 100, 255, 1 };
    signed char sb[4] = { -1, -128, 127, 5 };
    unsigned t = 0;
    for (int i = 0; i < 4; i++)
        t = t * 131 + (unsigned char)sb[i] + (signed char)ub[i];
    printf("narrowed: %u %d %d\n", t, (unsigned char)*sb, (signed char)*ub);
    int a[6] = { 1, 2, 3, 4, 5, 6 };
    for (int j = 4; j >= 0; j--)
        a[j + 1] = a[j];
    a[0] = a[2] + a[3];
    printf("assigned: %d %d %d %d %d %d\n", a[0], a[1], a[2], a[3], a[4], a[5]);
    long v1 = vone * 11, v2 = vone * 22, v3 = vone * 33, v4 = vone * 44, v5 = vone * 55, v6 = vone * 66;
    long v7 = vone * 77, v8 = vone * 88, v9 = vone * 99;
    char big1[300], big2[300];
    memset(big1, 'x', sizeof(big1));
    long r1 = v1 + v2 + v3;
    memcpy(big2, big1, sizeof(big2));
    long r2 = v4 + v5 + v6 + big2[299];
    __atomic_fetch_add(&counter, 1, __ATOMIC_SEQ_CST);
    long r3 = keep_many(v1, v2, v3, v4, v5, v6) + v7 + v8 + v9;
    struct { long w[40]; } s1 = { { 1, 2, 3 } }, s2;
    s2 = s1;
    printf("rescan: %d\n", rescan());
    printf("kept: %ld %ld %ld %ld %ld %ld\n", r1, r2, r3, v7 + v8 + v9 + s2.w[2], v1 * v9, v5 - v4);
}

/* (62a) __int128: carries, products, shifts across 64 bits, signed and
 * unsigned compares, conversions, constants beyond 64 bits, values passed
 * and returned (AAPCS64's even register pair), division through the
 * library's helpers; and negative int constants in 64-bit initializers
 * (written as 0x00000000fffffffb until 62a) */
typedef unsigned __int128 u128;
typedef __int128 s128;
static volatile long vneg5 = -5;
static __int128 g128neg = -7, g128pos = 1234567890123;
static unsigned __int128 g128big = (unsigned __int128)1 << 100, g128mask = ~(unsigned __int128)0 >> 3;
static long g128long[3] = { -1, -2, 3 };
static int64_t g128id = -1;

static void show128(const char *name, u128 v)
{
    printf("%s %016llx%016llx\n", name, (unsigned long long)(v >> 64), (unsigned long long)v);
}

__attribute__((noinline)) static u128 mulwide(uint64_t a, uint64_t b) { return (u128)a * b; }
__attribute__((noinline)) static s128 pass(int k, s128 x, long y, s128 z) { return x * k + z - y; }
struct holder { char c; u128 v; short s; };
__attribute__((noinline)) static struct holder make_holder(u128 v) { struct holder h = { 'h', v, 7 }; return h; }

static void int128s(void)
{
    uint64_t a = 0xFFFFFFFFFFFFFFFFull * (unsigned)vone, b = 0x123456789ABCDEFull * (unsigned)vone;
    show128("add", (u128)a + a);
    show128("sub", (u128)b - a);
    show128("mulwide", mulwide(a, b));
    show128("mul128", ((u128)a << 3 | 5) * (((u128)b << 64) | 9));
    u128 x = ((u128)0x8000000000000001ull << 64) | 0xF00000000000000Full;
    int counts[] = { 0, 1, 63, 64, 65, 127 };
    for (int i = 0; i < 6; i++) {
        int n = counts[i] * vone;
        char nm[16];
        snprintf(nm, sizeof(nm), "shl%d", n); show128(nm, x << n);
        snprintf(nm, sizeof(nm), "shr%d", n); show128(nm, x >> n);
        snprintf(nm, sizeof(nm), "sar%d", n); show128(nm, (u128)((s128)x >> n));
    }
    show128("cshl", x << 70);
    show128("csar", (u128)((s128)x >> 100));
    s128 m = vneg5, p = 3 * vone;
    printf("cmp %d %d %d %d %d %d\n", m < p, (u128)m < (u128)p, m <= p, m == (s128)vneg5, m != p, x > (u128)p);
    printf("narrow %d %ld %u %d\n", (int)x, (long)(x >> 1), (unsigned)(m * 7), (_Bool)(x >> 127));
    show128("neg", (u128)-m);
    show128("not", ~x);
    show128("bits", (x & ((u128)a << 4)) ^ (x | 12345));
    show128("pass", (u128)pass(3 * vone, m, 10, (s128)1 << 100));
    struct holder h = make_holder(x);
    show128("struct", h.v);
    printf("struct %c %d\n", h.c, h.s);
    u128 arr[4];
    for (int i = 0; i < 4; i++)
        arr[i] = (u128)i << (40 * i);
    u128 sum = 0;
    for (int i = 0; i < 4; i++)
        sum += arr[i];
    sum++;
    sum <<= 1;
    show128("arr", sum);
    show128("g128neg", (u128)g128neg);
    show128("g128pos", (u128)g128pos);
    show128("g128big", g128big);
    show128("g128mask", g128mask);
    printf("g128long %ld %ld %ld %lld\n", g128long[0], g128long[1], g128long[2], (long long)g128id);
    u128 big = ((u128)0x0123456789ABCDEFull << 64) | 0xFEDCBA9876543210ull;
    show128("udiv", big / (u128)(1000000007u * (unsigned)vone));
    show128("umod", big % 97u);
    show128("sdiv", (u128)((s128)(0 - big) / (s128)(-12345 * vone)));
    show128("smod", (u128)((s128)(0 - big) % 1000));
    printf("conv %a %a %a %f\n", (double)big, (double)(s128)(0 - big), (double)(float)big, (double)(m * 1000000));
    double dd = 1.5e30 * vone;
    show128("fix", (u128)(s128)dd);
    show128("fixneg", (u128)(s128)-dd);
    show128("fixu", (u128)(dd * 100));
    printf("sizes %zu %zu\n", sizeof(u128), _Alignof(s128));
}

/* (62a) _Complex: each kind passed and returned (SysV's classes, x86's
 * st0/st1 for long double, AAPCS64's HFAs), calls into the library, * and /
 * through its helpers, real operands part by part, __real__/__imag__ as
 * lvalues, conversions, compares, static initializers */
static volatile double cvd = 1.5;
static volatile int cvi = 3;
static double complex cgz = 1.0 + 2.0i, cgw = CMPLX(-3.5, 0.25);
static float complex cgf = 2.0f - 1.0if;
static long double complex cgl = 4.0L + 8.0iL;
static double complex cgarr[3] = { 1, 2.0i, 3 - 3.0i };

static void cshowd(const char *n, double complex z) { printf("%s %a %a\n", n, creal(z), cimag(z)); }
static void cshowf(const char *n, float complex z) { printf("%s %a %a\n", n, (double)crealf(z), (double)cimagf(z)); }
static void cshowl(const char *n, long double complex z) { printf("%s %a %a\n", n, (double)creall(z), (double)cimagl(z)); }

static double complex cmix(int a, double complex z, float b, float complex w, long double complex l, double c)
{
    return z * a + (double complex)w * b + (double complex)l - c;
}
static float complex cfret(float complex a, float complex b) { return a * b + 1.0f; }
static long double complex clret(long double complex a, int k) { return a * a + k; }
static long double complex clpass(long double complex a, long double complex b, double x) { return a / b + x; }

static void complexes(void)
{
    double complex z = cvd + cvi * I, w = cgw;
    cshowd("add", z + w);
    cshowd("sub", z - w);
    cshowd("mul", z * w);
    cshowd("div", z / w);
    cshowd("rmul", 2.5 * z);
    cshowd("rdiv", z / 4.0);
    cshowd("rsub", 1.0 - z);
    cshowd("neg", -z);
    cshowd("conj", conj(z));
    printf("cabs %a carg %a\n", cabs(z), carg(z));
    cshowd("cexp", cexp(z));
    cshowd("csqrt", csqrt(w));
    cshowf("cexpf", cexpf(cgf));
    cshowl("csqrtl", csqrtl(cgl));
    double inf = 1.0 / 0.0;
    double complex big = CMPLX(inf, 1.0), nan_ = CMPLX(NAN, NAN);
    cshowd("infmul", big * z);
    cshowd("infdiv", z / CMPLX(inf, inf));
    cshowd("realinf", inf * CMPLX(0.0, 1.0));
    printf("isnan %d\n", isnan(creal(nan_ * z)));
    double complex u = z;
    __real__ u = 7.0;
    __imag__ u += 0.5;
    cshowd("parts", u);
    printf("real %a imag %a imagof %a\n", __real__ z, __imag__ z, __imag__ cvd);
    printf("cmp %d %d %d %d\n", z == z, z != w, z == cvd + 3.0i, (double complex)cvd == cvd);
    printf("bool %d %d %d\n", !!z, !CMPLX(0.0, 0.0), CMPLX(0.0, -0.0) ? 1 : 0);
    int k = (int)w;
    float fr = (float)z;
    printf("conv %d %a\n", k, (double)fr);
    float complex fz = (float complex)z;
    cshowf("tofloat", fz);
    long double complex lz = z;
    cshowl("tolong", lz);
    cshowd("global", cgz + cgarr[1] + cgarr[2]);
    cshowf("cgf", cgf);
    cshowl("cgl", cgl);
    cshowd("cmix", cmix(cvi, z, 2.0f, cgf, cgl, cvd));
    cshowf("cfret", cfret(cgf, CMPLXF(0.5f, 0.25f)));
    cshowl("clret", clret(cgl, cvi));
    cshowl("clpass", clpass(cgl, CMPLXL(1.0L, -1.0L), cvd));
    double complex acc = 0;
    for (int i = 0; i < 5; i++) {
        acc += cgarr[i % 3] * i;
        acc *= 1.0 + 0.5i;
    }
    acc++;
    cshowd("loop", acc);
    printf("sizes %zu %zu %zu %zu\n", sizeof(float complex), sizeof(double complex), sizeof(long double complex),
           _Alignof(long double complex));
}

/* (62b) per-thread variables: initialized and zeroed, aligned, an extern
 * declaration of one defined later, addresses taken and passed */
extern __thread int tls_later;
static __thread int tls_count = 41;
static __thread char tls_text[24];
static __thread long tls_wide __attribute__((aligned(32))) = -7;
_Thread_local double tls_real = 2.5;

static int *tls_pointer(void)
{
    return &tls_count;
}

static void tls_add(long *p, long v)
{
    *p += v;
}

static void per_thread(void)
{
    tls_count++;
    *tls_pointer() += 10;
    strcpy(tls_text, "per thread");
    tls_add(&tls_wide, 100);
    tls_real *= 3;
    tls_later += 5;
    printf("tls: %d %s %ld %g %d %d\n", tls_count, tls_text, tls_wide, tls_real, tls_later,
           (int)((unsigned long)&tls_wide % 32));
}

__thread int tls_later = 37;

/* (62c) inline assembly: outputs, in-outs, pinned registers, memory and
 * immediate operands, size modifiers, named operands, a register variable,
 * values kept over an asm that overwrites registers, %= */
static volatile int cvone = 1;
struct apair { long a, b; };

static long asm_add3(long a, long b, long c)
{
    long r;
#if defined(__x86_64__)
    __asm__("leaq (%1,%2), %0\n\taddq %3, %0" : "=r"(r) : "r"(a), "r"(b), "r"(c));
#else
    __asm__("add %0, %1, %2\n\tadd %0, %0, %3" : "=r"(r) : "r"(a), "r"(b), "r"(c));
#endif
    return r;
}

static void inline_asm(void)
{
    long x = 40 * cvone, y = 2;
    printf("asm_add3 %ld\n", asm_add3(x, y, 100));
    int n = 7 * cvone;
#if defined(__x86_64__)
    __asm__ volatile("addl $5, %0" : "+r"(n));
    printf("inout %d\n", n);
    uint32_t lo, hi;
    __asm__("movl $0x12345678, %%eax\n\tmovl $0x9abcdef0, %%edx" : "=a"(lo), "=d"(hi) : : "cc");
    printf("pinned %x %x\n", lo, hi);
    uint16_t w = 0;
    __asm__("movw %w1, %0" : "=m"(w) : "r"(0xBEEF * cvone));
    printf("word %x\n", w);
    struct apair p = { 3, 4 };
    __asm__("addq %[k], %[m]" : [m] "+m"(p.b) : [k] "i"(1000));
    printf("named %ld %ld\n", p.a, p.b);
    long keep1 = cvone * 11, keep2 = cvone * 22, keep3 = cvone * 33;
    __asm__ volatile("movq $1, %%rbx\n\tmovq $2, %%r12\n\tmovq $3, %%rsi\n\tmovq $4, %%r8" : : : "rbx", "r12", "rsi", "r8", "memory");
    printf("kept %ld %ld %ld\n", keep1, keep2, keep3);
    unsigned char b8 = 0x7F;
    __asm__("addb $1, %0" : "+r"(b8));
    printf("byte %d\n", b8);
#else
    __asm__ volatile("add %w0, %w0, #5" : "+r"(n));
    printf("inout %d\n", n);
    register long x0 __asm__("x0") = 21 * cvone;
    __asm__("add %0, %0, %0" : "+r"(x0));
    printf("regvar %ld\n", x0);
    struct apair p = { 3, 4 };
    long t;
    __asm__("ldr %0, %1" : "=r"(t) : "m"(p.b));
    __asm__("str %1, %0" : "=m"(p.a) : "r"(t * 100));
    printf("mem %ld %ld\n", p.a, p.b);
    long keep1 = cvone * 11, keep2 = cvone * 22, keep3 = cvone * 33;
    __asm__ volatile("mov x20, #1\n\tmov x21, #2\n\tmov x9, #3\n\tmov x1, #4" : : : "x20", "x21", "x9", "x1", "memory");
    printf("kept %ld %ld %ld\n", keep1, keep2, keep3);
    uint64_t v;
    __asm__("mov %x0, #%1" : "=r"(v) : "i"(77));
    printf("imm %lu\n", (unsigned long)v);
#endif
    int u1, u2;
    __asm__("Lasm%=: nop" : );
    __asm__("Lasm%=: nop" : );
    (void)u1; (void)u2;
    printf("asm done\n");
}

/* (63c) calls cc inlines (small static functions): parameters changed in
 * the callee, returns from loops and branches, a callee's address-taken
 * local, nested and repeated calls, narrow results, a callee that is also
 * called through a pointer, one with a static local, recursion (never
 * inlined) and a noinline function */
static int in_add(int a, int b) { return a + b; }
static int in_clamp(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}
static int in_first_even(const int *p, int n)
{
    for (int i = 0; i < n; i++)
        if (p[i] % 2 == 0)
            return p[i];
    return -1;
}
static int in_bump(int x) { x += 10; return x * 2; }
static int in_through(int v)
{
    int box = v;
    int *q = &box;
    *q += 1;
    return box;
}
static int in_twice(int v) { return in_add(v, v) + in_add(1, 2); }
static unsigned char in_narrow(int v) { return (unsigned char)(v + 250); }
static short in_short(int v) { return (short)(v * 1000); }
static int in_counter(void)
{
    static int calls;
    return ++calls;
}
static int in_fact(int n) { return n <= 1 ? 1 : n * in_fact(n - 1); }
static void in_store(int *p, int v)
{
    if (!p)
        return;
    *p = v;
}
static int in_kind(int c)
{
    switch (c) {
    case 'a': return 1;
    case 'b': return 2;
    default: return 0;
    }
}
__attribute__((noinline)) static int in_never(int v) { return v - 1; }
static double in_half(double d) { return d / 2; }
/* (39b) inlined with structures, unions, __int128 and _Complex values:
 * a callee changing its copy, returning a parameter, nested calls, the
 * same object twice, a member of the result, a callee called once */
struct in_cf { float re, im; };
union in_u { int i; float f; };
static struct in_cf in_cmul(struct in_cf a, struct in_cf b)
{
    struct in_cf r = { a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re };
    return r;
}
static struct in_cf in_scale(struct in_cf a, float k)
{
    a.re *= k;
    a.im *= k;
    return a;
}
static struct point in_swap(struct point p)
{
    int t = p.x;
    p.x = p.y;
    p.y = t;
    return p;
}
static struct big in_big_once(struct big b, long add)
{
    for (int i = 0; i < 5; i++)
        b.a[i] += add;
    return b;
}
static union in_u in_union(union in_u u) { u.i ^= 0x80000000; return u; }
static unsigned __int128 in_wide(unsigned __int128 x, int k) { return (x << k) | 1; }
static double complex in_csq(double complex z) { return z * z + 1; }
static struct small in_small(struct small s, int k) { s.b += (char)k; return s; }

static void inlining(void)
{
    int arr[5] = { 3, 7, 8, 9, 10 };
    int x = 5;
    printf("inline add %d clamp %d %d %d\n", in_add(x, 4), in_clamp(-3, 0, 9), in_clamp(12, 0, 9), in_clamp(4, 0, 9));
    printf("inline first even %d %d\n", in_first_even(arr, 5), in_first_even(arr, 2));
    printf("inline bump %d x %d\n", in_bump(x), x);
    printf("inline through %d twice %d\n", in_through(41), in_twice(in_add(2, 3)));
    printf("inline narrow %u short %d\n", (unsigned)in_narrow(10), in_short(40));
    int s = 0;
    for (int i = 0; i < 4; i++)
        s += in_add(i, in_clamp(i * 3, 2, 6));
    printf("inline loop %d counter %d %d\n", s, in_counter(), in_counter());
    int (*fp)(int, int) = in_add;
    printf("inline pointer %d fact %d never %d\n", fp(20, 22), in_fact(6), in_never(8));
    int cell = 0;
    in_store(&cell, 77);
    in_store(0, 1);
    printf("inline store %d kind %d%d%d half %a\n", cell, in_kind('a'), in_kind('b'), in_kind('z'), in_half(3.0));
    struct in_cf ph = { 1, 0 }, step = { 0.6f, 0.8f }, acc = { 0, 0 };
    for (int i = 0; i < 6; i++) {
        struct in_cf y = in_cmul(ph, in_scale(step, 2));
        acc.re += y.re;
        acc.im += y.im;
        ph = in_cmul(ph, step);
    }
    struct in_cf sq = in_cmul(step, step);
    printf("inline struct %a %a %a %a %a %a %a\n", (double)ph.re, (double)ph.im, (double)acc.re, (double)acc.im,
           (double)sq.re, (double)sq.im, (double)in_cmul(in_cmul(step, ph), step).im);
    struct point pt = { 1, 2 }, sw = in_swap(pt);
    struct big bg = { { 1, 2, 3, 4, 5 } }, bg2 = in_big_once(bg, 10);
    printf("inline copies %d %d %d %d %ld %ld %ld %d\n", pt.x, pt.y, sw.x, sw.y, bg.a[4], bg2.a[0], bg2.a[4],
           in_swap(in_swap(pt)).y);
    union in_u u = { .f = 2.5f }, un = in_union(u);
    unsigned __int128 w = in_wide(((unsigned __int128)vone << 70) + 3, 5);
    double complex z = in_csq(1.5 + 0.5 * I);
    struct small sm = { 1, 2, 3 }, sm2 = in_small(sm, 40);
    printf("inline others %a %a %016llx%016llx %a %a %d %d %d %d\n", (double)u.f, (double)un.f,
           (unsigned long long)(w >> 64), (unsigned long long)w, creal(z), cimag(z), sm.b, sm2.a, sm2.b, sm2.c);
}

/* (65e-52) _Atomic: every operator on one object, <stdatomic.h>'s
 * functions, and four threads updating shared objects at once */
#define ATOMIC_ROUNDS 100000
#define ATOMIC_WORKERS 4
static _Atomic int a_count;
static atomic_long a_sum;
static _Atomic double a_real;
static _Atomic unsigned a_bits;
static char a_buf[4];
static _Atomic(char *) a_cp = a_buf;
static atomic_uint a_fetch;
static _Atomic unsigned short a_short;

static void *atomic_worker(void *arg)
{
    int k = (int)(long)arg;
    for (int i = 0; i < ATOMIC_ROUNDS; i++) {
        a_count++;
        a_sum += 3;
        a_real += 1.0;
        a_bits |= 1u << k;
        a_cp++;
        --a_cp;
        atomic_fetch_add(&a_fetch, 2);
        a_short -= 1;
    }
    return 0;
}

struct with_atomic {
    int x;
    _Atomic long y;
};

static void atomics(void)
{
    _Atomic int a = 5;
    int r1 = (a += 3), r2 = (a *= 2), r3 = (a -= 1), r4 = (a <<= 2), r5 = (a /= 3);
    int r6 = (a %= 7), r7 = (a |= 8), r8 = (a &= 13), r9 = (a ^= 3), r10 = (a >>= 1);
    printf("atomic ops: %d %d %d %d %d %d %d %d %d %d\n", r1, r2, r3, r4, r5, r6, r7, r8, r9, r10);
    int p1 = a++, p2 = ++a, p3 = a--, p4 = --a;
    printf("atomic steps: %d %d %d %d %d\n", p1, p2, p3, p4, a);
    _Atomic unsigned char uc = 250;
    _Atomic signed char sc = 100;
    _Atomic _Bool b = 0;
    uc += 10;
    sc += 100;
    int b1 = b++, b2 = b, b3 = (b = 0);         /* (clang's b += 5 stores 6 into the */
    printf("atomic small: %d %d %d %d %d\n", uc, sc, b1, b2, b3);    /* _Bool: no op= of one) */
    _Atomic double d = 1.5;
    _Atomic float f = 0.5f;
    d += 2.25;
    d *= 2;
    double d1 = d++, d2 = --d;
    f /= 4;
    f -= 1;
    printf("atomic floats: %g %g %g %g\n", d1, d2, (double)d, (double)f);
    _Atomic long l = -7;
    l = l * 3;
    l = l + a;
    printf("atomic assign: %ld\n", (long)l);
    char buf[8];
    _Atomic(char *) p = buf;
    ++p;                                        /* (clang refuses += and -= of an */
    ++p;                                        /* atomic pointer, C11 6.5.16.2p1 */
    ++p;                                        /* allows them; cc takes them) */
    char *pp = p++;
    p--;
    --p;
    printf("atomic pointer: %d %d\n", (int)(p - buf), (int)(pp - buf));
    struct with_atomic s = { 1, 2 }, *ps = &s;
    s.y += 5;
    ps->y++;
    _Atomic int *ip = &a;
    (*ip)++;
    ip[0] += 2;
    printf("atomic lvalues: %ld %d %d %d\n", (long)s.y, s.x, a, (int)sizeof(atomic_llong));

    atomic_int m;
    atomic_init(&m, 4);
    int e = 4, e2 = 4;
    bool ok1 = atomic_compare_exchange_strong(&m, &e, 9);
    bool ok2 = atomic_compare_exchange_strong(&m, &e2, 1);
    printf("atomic compare: %d %d %d %d\n", ok1, ok2, e, e2);
    int f1 = atomic_fetch_add(&m, 5), f2 = atomic_fetch_sub(&m, 2), f3 = atomic_fetch_or(&m, 64);
    int f4 = atomic_fetch_and(&m, 0x4F), f5 = atomic_fetch_xor(&m, 1), f6 = atomic_exchange(&m, 100);
    int f7 = atomic_load(&m);
    atomic_store(&m, 7);
    int f8 = atomic_load_explicit(&m, memory_order_acquire);
    atomic_store_explicit(&m, 8, memory_order_release);
    printf("atomic functions: %d %d %d %d %d %d %d %d %d\n", f1, f2, f3, f4, f5, f6, f7, f8,
           atomic_fetch_add_explicit(&m, 1, memory_order_relaxed));
    int arr[4];
    _Atomic(int *) ap = arr;
    int *old = atomic_fetch_add(&ap, 2);
    long at2 = ap - arr;
    atomic_fetch_sub(&ap, 1);
    printf("atomic fetch pointer: %d %ld %d\n", (int)(old - arr), at2, (int)(ap - arr));
    atomic_flag fl = ATOMIC_FLAG_INIT;
    bool t1 = atomic_flag_test_and_set(&fl), t2 = atomic_flag_test_and_set(&fl);
    atomic_flag_clear(&fl);
    bool t3 = atomic_flag_test_and_set_explicit(&fl, memory_order_acq_rel);
    atomic_thread_fence(memory_order_seq_cst);
    atomic_signal_fence(memory_order_seq_cst);
    printf("atomic flag: %d %d %d %d\n", t1, t2, t3, atomic_is_lock_free(&m) ? 1 : 0);

    pthread_t workers[ATOMIC_WORKERS];
    a_short = 60000;
    for (long k = 0; k < ATOMIC_WORKERS; k++)
        CHECK(pthread_create(&workers[k], 0, atomic_worker, (void *)k) == 0);
    for (int k = 0; k < ATOMIC_WORKERS; k++)
        pthread_join(workers[k], 0);
    printf("atomic threads: %d %ld %g %x %d %u %u\n", a_count, (long)a_sum, (double)a_real, a_bits,
           (int)(a_cp - a_buf), a_fetch, a_short);
}

static void evaluation(void)
{
    int r = bump() + bump() * 10;
    printf("order: %d\n", r);
    int arr[4] = { 0 };
    int idx = 0;
    arr[idx++] = 5;                     /* (arr[idx++] = idx would be undefined) */
    printf("arr: %d %d\n", arr[0], idx);
    printf("comma: %d\n", (bump(), bump(), counter));
    printf("short circuit: %d %d\n", vzero && bump(), counter);
}

/* (39a) floats and doubles in registers (x86-64's xmm8-xmm15, shared with
 * the vectors): more alive at once than there are registers, kept across
 * calls (where no xmm register survives), loop accumulators, an operation
 * whose result takes its right operand's register (t = c - t), negation,
 * fabs and sqrt of register values, compares with NaN, conversions to and
 * from integers and long double, more than eight float arguments, and
 * integer constants made floats at compile time (float s = 0) */
struct fpair { float re, im; };
static volatile float vf3 = 3.0f, vfh = 0.5f;
static volatile double vd7 = 7.0;

static double sum10(float a, double b, float c, double d, float e, double f, float g, double h, float i, double j)
{
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j;
}

static float fdot(const float *a, const float *b, int n)
{
    float s = 0;
    for (int i = 0; i < n; i++)
        s += a[i] * b[i];
    return s;
}

static void ffir(const struct fpair *x, struct fpair *y, const float *h, int taps, int n)
{
    for (int k = taps - 1; k < n; k++) {
        float re = 0, im = 0;
        for (int i = 0; i < taps; i++) {
            re += h[i] * x[k - i].re;
            im += h[i] * x[k - i].im;
        }
        y[k].re = re;
        y[k].im = im;
    }
}

/* (no calls inside: the values stay in registers) */
static float pressure(float a, float b, double c)
{
    float f1 = a + b, f2 = a - b, f3 = a * b, f4 = a / b, f5 = f1 * f2, f6 = f3 - f4, f7 = f5 / f1, f8 = f6 * f6;
    double d1 = c * a, d2 = c - b, d3 = d1 / d2, d4 = d3 + f8;
    float g1 = f1 * f8 - f2, g2 = f3 / f7 + f4, g3 = f5 - f6 * f2;
    return (float)(f1 + f2 * 2 + f3 * 3 + f4 * 4 + f5 * 5 + f6 * 6 + f7 * 7 + f8 * 8 + d1 * 9 + d2 * 10 + d3 * 11 +
                   d4 * 12 + g1 * g2 - g3);
}

static float right_operand(float a, float b, float f1)
{
    float t = a;
    for (int i = 0; i < 5; i++) {
        t = b - t;
        t = 2 / t;
        t = f1 + t;
    }
    return t;
}

/* (40b) a loaded value used once, right away, read from memory by its
 * user: its address register free for the result (ptr, ip and dp end
 * there), a commutative one swapped, compares, float division */
__attribute__((noinline)) static long mem_operands(long k, const long *ptr, const int *ip, const double *dp,
                                                   const unsigned *up)
{
    long t = k - *ptr;
    int u = (int)k ^ *ip;
    double w = (double)k / *dp;
    long m = *(ptr + 1) * k;
    int lt = k < ptr[2], ul = (unsigned)k < *up;
    return t * 1000 + u + (long)(w * 8) + m * 7 + lt * 3 + ul + k;
}

/* (each pointer read once and done: its register the result's) */
__attribute__((noinline)) static long mem_subtract(long k, const long *p, const long *q, const int *r, const int *w)
{
    long t1 = k - *p, t2 = k - *q;
    int i3 = (int)k - *r, i4 = (int)k - *w;
    return t1 * 1000 + t2 * 100 + i3 * 10 + i4 + k;
}

/* (40c) loads read again: the same place twice is one load, unless
 * something between may write it (a store through another pointer to the
 * same place, a call, a structure's copy, an atomic, memset); a char and
 * an int of the same address; a loop writing what it reads */
static int ld_global = 5;
__attribute__((noinline)) static void ld_bump(void) { ld_global += 100; }

__attribute__((noinline)) static long reloads(int *p, int *q, struct point *sp, const struct point *sq, int n)
{
    int a = *p;
    *q = a + 7;
    int b = *p;                     /* (q is p) */
    int c = ld_global;
    ld_bump();
    int d = ld_global;
    int e = sp->x;
    *sp = *sq;
    int f = sp->x;
    int g = *p;
    __atomic_fetch_add(p, 3, __ATOMIC_SEQ_CST);
    int h = *p;
    unsigned char lo = *(unsigned char *)p;
    int whole = *p;
    int arr[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    for (int i = 1; i < n; i++)
        arr[i] = arr[i - 1] * 2 + arr[i];
    return (long)a * 1000000 + b * 10000 + c * 100 + d + e * 7 + f * 11 + g * 13 + h * 17 + lo * 19 + whole + arr[n - 1];
}

/* (40d) more floats alive than xmm8-xmm15 hold, all a call's arguments in
 * another order: xmm2-xmm7 are the arguments' registers there */
__attribute__((noinline)) static double sum_ten(float a, float b, float c, float d, float e, float f, float g, float h,
                                                float i, float j)
{
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j;
}

__attribute__((noinline)) static double ten_args(float x)
{
    float v1 = x + 1, v2 = x * 2, v3 = x - 3, v4 = x / 4, v5 = x * x, v6 = v1 * v2, v7 = v3 - v4, v8 = v5 + v6;
    float v9 = v7 * v8, v10 = v9 - v1;
    return sum_ten(v10, v9, v8, v7, v6, v5, v4, v3, v2, v1);   /* (one call: none lives over another) */
}

__attribute__((noinline)) static double ten_args2(float x)
{
    float v1 = x + 1, v2 = x * 2, v3 = x - 3, v4 = x / 4, v5 = x * x, v6 = v1 * v2, v7 = v3 - v4, v8 = v5 + v6;
    float v9 = v7 * v8, v10 = v9 - v1;
    return sum_ten(v1, v3, v5, v7, v9, v2, v4, v6, v8, v10);
}

/* (40e) unrolled copies with fixed offsets and the additions moved out of
 * addresses: i used as a value too, structure arrays, shorts, doubles, a
 * negative offset, the counter's last value, b[n - 1 - i] packed, and an
 * unsigned addition that wraps (sext(u + 1) is not sext(u) + 1 there) */
struct off_s { int a; short b; int c; };
__attribute__((noinline)) static long offsets(const int *x, const struct off_s *st, const short *sh, const double *dv,
                                              const short *hb, unsigned u, int n)
{
    long s1 = 0, s2 = 0, s3 = 0;
    double s4 = 0;
    int i;
    for (i = 1; i < n; i++)
        s1 += (long)x[i] * (i + 3) - x[i - 1];
    for (int j = 0; j < n; j++)
        s2 += st[j].a * 3 + st[j].b - st[j].c;
    for (int j = 0; j < n; j++)
        s3 += sh[j + 2] * sh[j];
    for (int j = 0; j < n; j++)
        s4 += dv[j] * (double)j;
    int m = 0;
    for (int j = 0; j < n; j++)
        m += hb[j] * hb[n - 1 - j];
    long wrap = (long)(int)(u + 1u);
    /* (a local structure's array of 4-byte rows, a byte of each set:
     * libimage's PNG palette, wrong once: an added constant over an added
     * constant, the inner one computed into its base's register) */
    struct { int head[6]; unsigned char pal[40][4]; } pk;
    memset(&pk, 0, sizeof(pk));
    for (int j = 0; j < 40; j++)
        pk.pal[j][3] = 255;
    for (int j = 0; j < n; j++)
        pk.pal[j][1] = (unsigned char)(j * 7);
    long pal = 0;
    for (int j = 0; j < 40; j++)
        pal = pal * 3 + pk.pal[j][0] + pk.pal[j][1] * 5 + pk.pal[j][3] * 2;
    return s1 * 7 + s2 * 5 + s3 * 3 + (long)s4 + m + i * 1000 + wrap + pal;
}

/* (40a-2) the same with ints: t's register is the result's, k lives on */
static long right_operand_i(long k, long t)
{
    for (int i = 0; i < 5; i++) {
        t = k - t;
        t = k * t + (k ^ t);
        t = (k | 3) - t;
    }
    return t * 1000 + k;
}

static double right_operand_d(double a, double c)
{
    double u = c;
    for (int i = 0; i < 4; i++)
        u = a - u * u / 64;
    return u;
}

/* negation, fabs, sqrt, compares with NaN and conversions of register
 * values, without a call in between (r: the results, as doubles) */
static void register_ops(float t, double u, float a, double c, float f4, double d1, double d2, float f5, double *r)
{
    float neg = -t;
    double negd = -u;
    r[0] = neg;
    r[1] = negd;
    r[2] = __builtin_fabsf(neg);
    r[3] = __builtin_fabs(negd);
    r[4] = __builtin_sqrtf(a);
    r[5] = __builtin_sqrt(c);
    float fnan = (float)(0.0 / vzero);
    double dnan = 0.0 / vzero;
    r[6] = (fnan == fnan) + 2 * (fnan != fnan) + 4 * (fnan < a) + 8 * (a <= fnan) + 16 * (dnan == dnan) +
           32 * (dnan > c) + 64 * (c >= dnan) + 128 * (fnan != a) + 256 * (a < f4) + 512 * (d1 >= d2);
    r[7] = (int)(f4 * -1.5f);
    r[8] = (unsigned)(d1 * 1e8);
    r[9] = (double)(long)(-d2 * 1e15);
    unsigned long ud = (unsigned long)(d1 * 1e17);
    r[10] = (double)ud;
    long double ld = f5;
    r[11] = (float)(ld / 3);
    r[12] = (double)(ld * c);
    r[13] = (float)ud;
    r[14] = (double)(unsigned long)0x8000000000000801ul * vone;
    r[15] = (float)(unsigned)(int)(f5 * 1e3f) + (double)(short)(int)d2;
}

static void float_registers(void)
{
    float a = vf3, b = vfh;
    double c = vd7;
    float f1 = a + b, f2 = a - b, f3 = a * b, f4 = a / b, f5 = f1 * f2, f6 = f3 - f4, f7 = f5 / f1, f8 = f6 * f6;
    double d1 = c * a, d2 = c - b, d3 = d1 / d2, d4 = d3 + f8;
    printf("many: %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.17g %.17g %.17g %.17g %.9g\n", f1, f2, f3, f4, f5, f6, f7,
           f8, d1, d2, d3, d4, pressure(a, b, c));
    float kept = a * 4 + b;
    double kept2 = c / 3;
    int n = bump();
    double after = kept + kept2 + sum10(f1, d1, f2, d2, f3, d3, f4, d4, f5, c) + (n - counter);
    printf("across calls: %.9g %.17g %.17g\n", kept, kept2, after);
    float t = right_operand(a, b, f1);
    double u = right_operand_d(a, c);
    printf("right operand: %.9g %.17g %ld\n", t, u, right_operand_i(7 * vone, 2 * vone));
    printf("ten arguments: %.17g %.17g\n", ten_args(1.5f * (float)vone), ten_args2(2.5f * (float)vone));
    {
        int ox[24];
        struct off_s ost[24];
        short osh[26], ohb[24];
        double odv[24];
        for (int q = 0; q < 24; q++) {
            ox[q] = q * 37 - 400;
            ost[q] = (struct off_s){ q * 5, (short)(q - 12), q * q };
            ohb[q] = (short)(q * 911 - 9000);
            odv[q] = q * 0.25 - 1;
        }
        for (int q = 0; q < 26; q++)
            osh[q] = (short)(q * 300 - 3000);
        printf("offsets: %ld %ld\n", offsets(ox, ost, osh, odv, ohb, 0x7fffffffu * (unsigned)vone, 21 * vone),
               offsets(ox, ost, osh, odv, ohb, 5u * (unsigned)vone, 6 * vone));
    }
    long mp[3] = { 40, -6, 99 };
    int mi = 0x55;
    double md = 0.25;
    unsigned mu = 3000000000u;
    int mr = 4, mw = -9;
    int rp = 41;
    struct point rs = { 3, 4 }, rq = { 30, 40 };
    printf("reloads: %ld\n", reloads(&rp, &rp, &rs, &rq, 8 * vone));
    printf("memory operands: %ld %ld\n", mem_operands(11 * vone, mp, &mi, &md, &mu),
           mem_subtract(5 * vone, &mp[0], &mp[1], &mr, &mw));
    double r[16];
    register_ops(t, u, a, c, f4, d1, d2, f5, r);
    printf("signs: %.9g %.17g %.9g %.17g %.9g %.17g\n", r[0], r[1], r[2], r[3], r[4], r[5]);
    printf("nan and compares: %.0f\n", r[6]);
    printf("conversions: %.0f %.0f %.0f %.0f %.9g %.17g %.9g %.17g %.17g\n", r[7], r[8], r[9], r[10], r[11], r[12],
           r[13], r[14], r[15]);
    float zero = 0, big = (float)16777217, minus = -3;
    double exact = 9007199254740993.0, from_int = 4294967295u, from_long = -(1L << 53);
    float from_huge = (float)18446744073709551615ul, from_odd = (float)((1L << 40) + 1);
    printf("constants: %.9g %.9g %.9g %.17g %.17g %.17g %.9g %.9g\n", zero, big, minus, exact, from_int, from_long,
           from_huge, from_odd);
    float xa[16], xb[16];
    for (int i = 0; i < 16; i++) {
        xa[i] = (float)i * b + a;
        xb[i] = (float)(16 - i) / a;
    }
    struct fpair x[24], y[24] = { { 0, 0 } };
    float h[5] = { 0.25f, -0.5f, 1.0f, -0.5f, 0.25f };
    for (int i = 0; i < 24; i++) {
        x[i].re = (float)(i * i % 7) - a;
        x[i].im = (float)(i % 5) * b;
    }
    ffir(x, y, h, 5, 24);
    printf("loops: %.9g %.9g %.9g %.9g %.9g\n", fdot(xa, xb, 16), y[4].re, y[4].im, y[23].re, y[23].im);
}

/* (39b-2) small local structures split into values, a field each: a
 * union read as another member (stays in memory), narrow and signed
 * members, padding and members never named but copied, bit-fields, a
 * nested structure assigned, constant indexes into an array member, one
 * whose address is taken, copies to and from memory and between locals,
 * a structure in a loop, and the size limit (64 bytes) */
struct sp_inner { short a; unsigned char b; };
struct sp_outer { char c; struct sp_inner in; int i; double d; };
struct sp_bits { unsigned lo : 5; signed mid : 7; unsigned hi : 20; };
struct sp_arr { int v[4]; float f; };
struct sp_big { long w[8]; };
struct sp_huge { long w[9]; };
static struct sp_outer sp_global = { 'g', { -300, 250 }, 77, 0.25 };
static volatile int sp_k = 3;

static void sp_take(struct sp_inner *p) { p->a += 1000; }

static void split_structs(void)
{
    union { int i; float f; } u;
    u.f = 1.5f * vone;
    int bits = u.i;
    u.i ^= 0x80000000;
    struct sp_outer o = sp_global, o2;
    o.in.a = (short)(o.in.a * 3);
    o.in.b = (unsigned char)(o.in.b + 10);
    o2 = o;
    o2.c = (char)-5;
    struct sp_inner in2 = o2.in;
    in2.b = 200;
    o2.in = in2;
    sp_global = o2;
    struct sp_bits bt = { 17, -20, 999999 };
    bt.mid += 3;
    bt.lo ^= 31;
    struct sp_arr ar = { { 1, 2, 3, 4 }, 0.5f };
    ar.v[2] += ar.v[1] * 10;
    ar.f *= (float)ar.v[3];
    struct sp_inner taken = { 5, 6 };
    sp_take(&taken);
    struct sp_big bg = { { 1, 2, 3, 4, 5, 6, 7, 8 } }, bg2;
    struct sp_huge hg = { { 9, 8, 7, 6, 5, 4, 3, 2, 1 } }, hg2;
    bg.w[7] += bg.w[0] * sp_k;
    bg2 = bg;
    hg.w[8] -= hg.w[0];
    hg2 = hg;
    struct point acc = { 0, 0 };
    for (int i = 0; i < 5; i++) {
        struct point step = { i, -i * sp_k };
        acc.x += step.x;
        acc.y += step.y;
        if (i == 3)
            acc = (struct point){ acc.y, acc.x };
    }
    struct small sm = { (char)-1, (char)200, 3 }, sm2 = sm;
    sm2.c = (char)(sm.a + sm.b);
    printf("split: %x %a %d %d %d %d %d %d %d %d\n", bits, (double)u.f, o.c, o.in.a, o.in.b, o.i, sp_global.c,
           sp_global.in.a, sp_global.in.b, (int)(sp_global.d * 8));
    printf("split: %u %d %u %d %d %a %d %d %ld %ld %ld %ld %d %d %d %d %d\n", bt.lo, bt.mid, bt.hi, ar.v[2], ar.v[3],
           (double)ar.f, taken.a, taken.b, bg2.w[0], bg2.w[7], hg2.w[0], hg2.w[8], acc.x, acc.y, sm2.a, sm2.b, sm2.c);
}

/* (39c) the same computation twice in a block is made once: not when an
 * operand changed between, conversions that differ only in sign or width,
 * the same constant in different classes; chains of merged results, an
 * address used twice (a structure array's members), divisions, builtins */
struct sx_pt { int x, y; long z; };
static long loop_shapes(void);

static void subexpressions(void)
{
    int a = vone * 6, b = vone * 7, c = vminus * 3;
    int s1 = a * b + c, s2 = a * b + c;
    a++;
    int s3 = a * b + c;
    signed char sc = (signed char)(vminus * 56);
    int e1 = (int)(signed char)sc + 1, e2 = (int)(unsigned char)sc + 1, e3 = (int)(short)(a * 9000) + 1;
    long l1 = (long)a * 4, l2 = (long)(a * 4);
    int i1 = a * 4;
    long big = (long)vone << 40;
    long w1 = big * 4 + (big * 4) / 3, w2 = (big * 4) % 7;
    unsigned u = 0x12345678u * (unsigned)vone;
    int z1 = __builtin_clz(u) + __builtin_popcount(u), z2 = __builtin_clz(u) * __builtin_popcount(u);
    struct sx_pt pts[6];
    for (int i = 0; i < 6; i++)
        pts[i] = (struct sx_pt){ i * a, i - b, (long)i * c };
    long acc = 0;
    for (int k = 2; k < 6; k++)
        for (int i = 0; i < 3; i++) {
            acc += pts[k - i].x * 3 + pts[k - i].y;
            acc ^= pts[k - i].z + (k - i);
            pts[k - i].y += (int)acc & 7;
        }
    int q = b / (c + 10) + b % (c + 10);
    printf("subexpressions: %d %d %d %d %d %d %ld %ld %d %ld %ld %d %d %ld %d %d\n", s1, s2, s3, e1, e2, e3, l1, l2,
           i1, w1, w2, z1, z2, acc, pts[3].y, q);
    printf("loop shapes: %ld\n", loop_shapes());
}

/* (39c-2) loops: invariant operations moved before them, addresses of an
 * induction variable made pointers: k - i, i + v, v - i and i - v, steps of
 * 1, -1 and 3, long counters, a continue, nested loops, a branch using the
 * address, a loop run no time around a division by zero (never moved), an
 * unsigned counter that wraps, a loop entered by goto */
static volatile int lp_zero = 0, lp_n = 12;

static long loop_shapes(void)
{
    int arr[40];
    long sum = 0;
    for (int i = 0; i < 40; i++)
        arr[i] = i * i - 7 * i;
    int k = 30 * vone, v = 5 * vone, n = lp_n;
    for (int i = 0; i < n; i++)
        sum += arr[k - i] * 3 + arr[i + v] - arr[v + i] + arr[k - v - i];
    for (int i = n; i > 0; i--)
        sum = sum * 3 + arr[i + 20] + arr[k - i];
    for (int i = 1; i < 30; i += 3) {
        if (i == 13)
            continue;
        sum += arr[i - 1 + v] ^ arr[38 - i];
    }
    for (long i = 2; i < 20; i++)
        sum -= arr[i + v] + arr[39 - i];
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 9; i++) {
            int t = arr[j * 9 - i + 8];
            if (t & 1)
                sum += arr[k - i - j] * t;
            else
                arr[i + j + 1] += 1;
        }
    int none = lp_zero, q = 0;
    for (int i = 0; i < none; i++)
        q += 100 / none + arr[k - i];
    unsigned top = 3 * (unsigned)vone;
    for (unsigned i = 0; i < 6; i++)
        sum += arr[(top - i) & 31];
    int g = 0;
    if (vone)
        goto inside;
    for (g = 0; g < 10; g++) {
inside:
        sum += arr[k - g];
    }
    struct sx_pt pts[8];
    for (int i = 0; i < 8; i++)
        pts[i] = (struct sx_pt){ i * 2, -i, (long)i * k };
    for (int i = 0; i < 6; i++) {
        pts[7 - i].x += pts[6 - i].y;
        sum += pts[7 - i].x + pts[7 - i].z;
    }
    return sum + q * 1000;
}

/* (39d) loops testing at their bottom: a test with values of its own, one
 * changing a variable, a while's continue (back to the first test), || in
 * the test (not moved), a volatile read in the test (as many reads), no
 * pass at all, a break, nested loops */
static volatile int rt_reads, rt_limit = 9;
static int rt_next(void) { return ++rt_reads; }

static void rotations(void)
{
    int a[16];
    for (int i = 0; i < 16; i++)
        a[i] = i * 3 - 10;
    int i = 0, lim = 20 * vone, s = 0;
    while (a[i] * 2 < lim)
        s += a[i++];
    int j = 0, n = 7 * vone, cnt = 0;
    while (j++ < n)
        cnt += j;
    int k = 0, odd = 0;
    while (k < 12) {
        k++;
        if (k & 1)
            continue;
        odd += k;
    }
    int m = 0, either = 0;
    while (m < 5 || a[m] < 0)
        either += a[m++];
    int r = 0, reads_before = rt_reads;
    while (rt_reads < rt_limit)
        r += rt_next();
    int none = 0;
    for (int q = 5 * vone; q < 5; q++)
        none++;
    int br = 0;
    for (int q = 0; q < 100; q++) {
        if (a[q % 16] > 20)
            break;
        br += q;
    }
    long nest = 0;
    for (int x = 0; x < 4; x++)
        for (int y = x; y < 6; y++)
            nest = nest * 7 + a[x + y];
    printf("rotations: %d %d %d %d %d %d %d %d %d %d %ld\n", s, i, cnt, j, odd, either, r, rt_reads - reads_before, none,
           br, nest);
}

/* (39e) unrolled loops: every trip count up to 9 (each remainder), steps
 * of 2 and 3, <= tests, a counter ending at INT32_MAX (i + 3 would
 * overflow), float sums whose result depends on their order, a value
 * carried from pass to pass, k - i pointers in the body, nested loops,
 * the counter's last value */
static volatile int un_big = INT32_MAX;

static void unrolled(void)
{
    int a[64];
    float f[64];
    for (int i = 0; i < 64; i++) {
        a[i] = i * i % 37 - 18;
        f[i] = i % 3 ? 1e8f : (float)i;
    }
    long trips = 0;
    for (int n = 0; n <= 9; n++) {
        int s = 0, i;
        for (i = 0; i < n * vone; i++)
            s += a[i] * (i + 1);
        trips = trips * 31 + s + i;
    }
    int s2 = 0, s3 = 0, s4 = 0;
    for (int i = 1; i < 40 * vone; i += 2)
        s2 += a[i];
    for (int i = 0; i <= 31 * vone; i += 3)
        s3 ^= a[i] << (i & 7);
    for (int i = 5; i <= 5 * vone; i++)
        s4 += a[i];
    int last = 0, top = un_big;
    for (int i = top - 5; i < top; i++)
        last = i;
    float fs = 0;
    for (int i = 0; i < 64 * vone; i++)
        fs += f[i] * (i & 1 ? 1.0f : -1.0f);
    int prev = 0, carried = 0;
    for (int i = 0; i < 30 * vone; i++) {
        carried += prev * a[i];
        prev = a[i] + i;
    }
    int k = 50 * vone, back = 0;
    for (int i = 0; i < 21 * vone; i++)
        back += a[k - i] * a[i + 3];
    long nest = 0;
    for (int x = 0; x < 5; x++)
        for (int y = 0; y < x * 3 * vone; y++)
            nest += a[x * 7 + y] * (x + 1);
    printf("unrolled: %ld %d %d %d %d %a %d %d %ld\n", trips, s2, s3, s4, last, (double)fs, carried, back, nest);
}

/* (39f) loops on 4 elements at a time (x86-64's SSE2): saxpy for every
 * length to 11 (each rest), int sums (two in one loop), arrays that
 * overlap 1 and 3 elements apart (the plain loop), in place, 4 apart (packed
 * again), shifts, & | ^, float division, broadcast variables and constants;
 * and loops that stay plain: an int multiply, a float sum, an unsigned
 * counter */
static void pk_saxpy(float *y, const float *x, float a, int n)
{
    for (int i = 0; i < n; i++)
        y[i] = a * x[i] + y[i];
}

static void pk_mix(int *c, const int *a, const int *b, int k, int n)
{
    for (int i = 0; i < n; i++)
        c[i] = ((a[i] + b[i]) ^ (a[i] >> 3)) - ((b[i] << 2) | k) + ((unsigned)a[i] >> 29 & 5);
}

__attribute__((noinline)) static void pk_copy(int *dst, const int *src, int n)
{
    for (int i = 0; i < n; i++)
        dst[i] = src[i] + 2;
}

static void packed(void)
{
    float fx[16], fy[16];
    double total = 0;
    for (int n = 0; n <= 11; n++) {
        for (int i = 0; i < 16; i++)
            fx[i] = (float)i * 0.75f - 3, fy[i] = (float)(i * i) / 7;
        pk_saxpy(fy, fx, 1.5f * (float)vone, n * vone);
        for (int i = 0; i < 16; i++)
            total = total * 1.0001 + fy[i];
    }
    int a[40], b[40], c[40];
    for (int i = 0; i < 40; i++)
        a[i] = i * 977 - 15000, b[i] = (i % 9) * 1234567 - 4000000;
    pk_mix(c, a, b, 0x55 * vone, 37 * vone);
    long mix = 0;
    for (int i = 0; i < 40; i++)
        mix = mix * 3 + (i < 37 ? c[i] : 0);
    int s1 = 0, s2 = 0;
    for (int i = 0; i < 39 * vone; i++) {
        s1 += a[i];
        s2 += b[i] ^ 7;
    }
    int o[48];
    for (int i = 0; i < 48; i++)
        o[i] = i;
    pk_copy(o + 1, o, 20 * vone);           /* (1 apart: each element from the one just written) */
    pk_copy(o + 3, o + 1, 21 * vone);
    pk_copy(o + 2, o + 2, 9 * vone);        /* (in place) */
    pk_copy(o + 24, o + 20, 20 * vone);     /* (4 apart: packed) */
    pk_copy(o, o + 1, 30 * vone);           /* (read ahead) */
    long ov = 0;
    for (int i = 0; i < 48; i++)
        ov = ov * 5 + o[i];
    float fd[20], fq[20];
    for (int i = 0; i < 20; i++)
        fd[i] = (float)i + 0.5f;
    float k = 3.0f * (float)vone;
    for (int i = 0; i < 20 * vone; i++)
        fq[i] = (fd[i] - k) / (fd[i] + 1.0f) * 0.5f;
    int m[20];
    for (int i = 0; i < 20 * vone; i++)
        m[i] = a[i] * b[i];
    float fs = 0;
    for (int i = 0; i < 20 * vone; i++)
        fs += fd[i] * 1e-3f + fq[i];
    unsigned u[20];
    for (unsigned i = 0; i < 20u * (unsigned)vone; i++)
        u[i] = (unsigned)a[i] + 3;
    printf("packed: %.17g %ld %d %d %ld %a %a %d %a %u\n", total, mix, s1, s2, ov, (double)fq[18], (double)fq[3], m[17],
           (double)fs, u[19]);
}

/* (39f-2) sums of int16 products 8 at a time (pmaddwd): a[i] * b[i],
 * a[i] * b[k - i], a[k - i] * b[i], both going down, every length to 17,
 * -32768 * -32768 pairs (pmaddwd's wrap), and plain ones: unsigned shorts,
 * a sum read elsewhere in the loop */
static void madds(void)
{
    short a[40], b[40];
    unsigned short ua[40];
    for (int i = 0; i < 40; i++) {
        a[i] = (short)(i * 1777 - 30000);
        b[i] = (short)((i % 11) * 3001 - 15000);
        ua[i] = (unsigned short)(i * 1601 + 30000);
    }
    a[3] = a[4] = b[3] = b[4] = -32768;
    long all = 0;
    for (int n = 0; n <= 17; n++) {
        int s1 = 0, s2 = 0, s3 = 0, s4 = 0;
        int k = 20 * vone, j = 39 * vone, m = n * vone;     /* (bounds read once: a volatile one is no loop's limit) */
        for (int i = 0; i < m; i++)
            s1 += a[i] * b[i];
        for (int i = 0; i < m; i++)
            s2 += a[i] * b[k - i];
        for (int i = 0; i < m; i++)
            s3 += a[k - i] * b[i];
        for (int i = 0; i < m; i++)
            s4 += a[j - i] * b[k - i];
        all = all * 7 + s1 + 3L * s2 - 5L * s3 + 11L * s4;
    }
    int su = 0, seen = 0, m33 = 33 * vone;
    for (int i = 0; i < m33; i++)
        su += ua[i] * b[i];
    for (int i = 0; i < m33; i++) {
        seen += a[i] * b[i];
        b[i] = (short)(seen & 0x7fff);
    }
    printf("madds: %ld %d %d\n", all, su, seen);
}

#include "cctest_simd.h"                     /* (32d) intrinsics() */

int main(void)
{
    integers();
    floats();
    structs();
    bitfields();
    varargs();
    pointers_and_data();
    evaluation();
    addressing();
    folding();
    int128s();
    complexes();
    per_thread();
    inline_asm();
    inlining();
    atomics();
    float_registers();
    split_structs();
    subexpressions();
    rotations();
    unrolled();
    packed();
    madds();
    intrinsics();
    printf("fails: %d\n", fails);
    return fails;
}

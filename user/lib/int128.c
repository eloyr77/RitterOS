/*
 * The helpers compilers call for __int128 (62a): division and remainder,
 * and conversions to and from float and double, under compiler-rt's names.
 * Static-only (in libritteros.a, linked into the programs that call them),
 * so libritteros.so.1's interface is unchanged. None of them divides or
 * converts a 128-bit value itself: a compiler would call the helper again.
 */
#include <stdint.h>

typedef unsigned __int128 u128;
typedef __int128 s128;

/* a / b and a % b by shifting and subtracting (b != 0) */
static u128 udivmod(u128 a, u128 b, u128 *rem)
{
    if (b > a) {
        *rem = a;
        return 0;
    }
    int shift = 0;
    while (!(b >> 127) && (b << 1) <= a) {
        b <<= 1;
        shift++;
    }
    u128 q = 0;
    for (; shift >= 0; shift--) {
        q <<= 1;
        if (a >= b) {
            a -= b;
            q |= 1;
        }
        b >>= 1;
    }
    *rem = a;
    return q;
}

u128 __udivti3(u128 a, u128 b);
u128 __umodti3(u128 a, u128 b);
s128 __divti3(s128 a, s128 b);
s128 __modti3(s128 a, s128 b);

u128 __udivti3(u128 a, u128 b)
{
    u128 r;
    return udivmod(a, b, &r);
}

u128 __umodti3(u128 a, u128 b)
{
    u128 r;
    udivmod(a, b, &r);
    return r;
}

/* (signs as C's: the quotient truncated, the remainder takes a's sign) */
s128 __divti3(s128 a, s128 b)
{
    u128 r, ua = a < 0 ? 0 - (u128)a : (u128)a, ub = b < 0 ? 0 - (u128)b : (u128)b;
    u128 q = udivmod(ua, ub, &r);
    return (a < 0) != (b < 0) ? (s128)(0 - q) : (s128)q;
}

s128 __modti3(s128 a, s128 b)
{
    u128 r, ua = a < 0 ? 0 - (u128)a : (u128)a, ub = b < 0 ? 0 - (u128)b : (u128)b;
    udivmod(ua, ub, &r);
    return a < 0 ? (s128)(0 - r) : (s128)r;
}

/* v (nonzero) rounded to nearest, ties to even, with `bits` significant
 * bits: the significand and the power of two of its lowest bit */
static uint64_t round_to(u128 v, int bits, int *lsb)
{
    int top = 127;
    while (!(v >> top & 1))
        top--;
    int cut = top - (bits - 1);
    if (cut <= 0) {
        *lsb = 0;
        return (uint64_t)v;
    }
    uint64_t m = (uint64_t)(v >> cut);
    int half = (int)(v >> (cut - 1) & 1);
    int rest = cut > 1 && (v & (((u128)1 << (cut - 1)) - 1)) != 0;
    if (half && (rest || (m & 1)))
        m++;
    if (m >> bits) {
        m >>= 1;
        cut++;
    }
    *lsb = cut;
    return m;
}

static double make_double(int negative, u128 v)
{
    if (!v)
        return negative ? -0.0 : 0.0;
    int lsb;
    uint64_t m = round_to(v, 53, &lsb);
    int top = 63;
    while (!(m >> top & 1))
        top--;
    uint64_t bits = (uint64_t)(top + lsb + 1023) << 52 | (m << (52 - top) & 0xFFFFFFFFFFFFFull);
    bits |= (uint64_t)negative << 63;
    union { uint64_t u; double d; } x = { bits };
    return x.d;
}

static float make_float(int negative, u128 v)
{
    if (!v)
        return negative ? -0.0f : 0.0f;
    int lsb;
    uint64_t m = round_to(v, 24, &lsb);
    int top = 63;
    while (!(m >> top & 1))
        top--;
    int e = top + lsb;
    if (e > 127)                                /* (beyond float: infinity) */
        return negative ? -__builtin_inff() : __builtin_inff();
    uint32_t bits = (uint32_t)(e + 127) << 23 | (uint32_t)(m << (23 - top) & 0x7FFFFF);
    bits |= (uint32_t)negative << 31;
    union { uint32_t u; float f; } x = { bits };
    return x.f;
}

double __floattidf(s128 a);
double __floatuntidf(u128 a);
float __floattisf(s128 a);
float __floatuntisf(u128 a);

double __floattidf(s128 a) { return make_double(a < 0, a < 0 ? 0 - (u128)a : (u128)a); }
double __floatuntidf(u128 a) { return make_double(0, a); }
float __floattisf(s128 a) { return make_float(a < 0, a < 0 ? 0 - (u128)a : (u128)a); }
float __floatuntisf(u128 a) { return make_float(0, a); }

/* |d| truncated (d finite and in range, as C requires) */
static u128 magnitude(double d)
{
    union { double d; uint64_t u; } x = { d };
    int e = (int)(x.u >> 52 & 0x7FF) - 1075;
    if ((x.u >> 52 & 0x7FF) == 0)
        return 0;                               /* (zero or subnormal: below 1) */
    u128 m = (x.u & 0xFFFFFFFFFFFFFull) | 0x10000000000000ull;
    if (e >= 0)
        return e > 75 ? 0 : m << e;                 /* (53 bits from bit e: at most bit 127) */
    return e <= -53 ? 0 : m >> -e;
}

s128 __fixdfti(double a);
u128 __fixunsdfti(double a);
s128 __fixsfti(float a);
u128 __fixunssfti(float a);

s128 __fixdfti(double a) { return a < 0 ? (s128)(0 - magnitude(a)) : (s128)magnitude(a); }
u128 __fixunsdfti(double a) { return a <= 0 ? 0 : magnitude(a); }
s128 __fixsfti(float a) { return __fixdfti(a); }
u128 __fixunssfti(float a) { return __fixunsdfti(a); }

/*
 * (32d) The calling convention for vectors between clang and cc: this file
 * is compiled by clang (cc_shell_test max, on the Mac) and called from
 * vabi2.c, compiled by RitterOS's cc; vectors go in xmm/ymm registers
 * among integers and doubles, both ways (vabi_call calls cc's function).
 */
#include <string.h>

typedef long long v4 __attribute__((vector_size(32)));
typedef float f4 __attribute__((vector_size(16)));
typedef double d4 __attribute__((vector_size(32)));

v4 vabi_add(int k, v4 a, double x, f4 b, v4 c, long n)
{
    long long pa[4], pc[4], r[4];
    float pb[4];
    memcpy(pa, &a, 32), memcpy(pc, &c, 32), memcpy(pb, &b, 16);
    for (int i = 0; i < 4; i++)
        r[i] = pa[i] + pc[i] * k + n + (long long)(x * 2) + (long long)pb[i];
    v4 v;
    memcpy(&v, r, 32);
    return v;
}

f4 vabi_call(f4 (*fn)(f4, d4, int, f4), f4 a, d4 b, int k, f4 c)
{
    return fn(a, b, k, c);
}

/* (32d) vabi1.c's other half: compiled by RitterOS's cc. */
#include <stdio.h>
#include <string.h>

typedef long long v4 __attribute__((vector_size(32)));
typedef float f4 __attribute__((vector_size(16)));
typedef double d4 __attribute__((vector_size(32)));

v4 vabi_add(int k, v4 a, double x, f4 b, v4 c, long n);
f4 vabi_call(f4 (*fn)(f4, d4, int, f4), f4 a, d4 b, int k, f4 c);

static f4 back(f4 a, d4 b, int k, f4 c)
{
    float pa[4], pc[4], r[4];
    double pb[4];
    memcpy(pa, &a, 16), memcpy(pb, &b, 32), memcpy(pc, &c, 16);
    for (int i = 0; i < 4; i++)
        r[i] = pa[i] * k + (float)pb[i] - pc[i];
    f4 v;
    memcpy(&v, r, 16);
    return v;
}

int main(void)
{
    long long pa[4] = { 1, -2, 300000000000LL, 4 }, pc[4] = { 10, 20, -30, 40 }, r[4];
    float pb[4] = { 1.5f, -2.5f, 8.0f, 100.25f }, pf[4] = { 0.5f, 1.0f, -3.0f, 2.0f }, pg[4] = { 1, 2, 3, 4 }, q[4];
    double pd[4] = { 0.25, -0.5, 10.0, 7.0 };
    v4 a, c;
    f4 b, f, g;
    d4 d;
    memcpy(&a, pa, 32), memcpy(&c, pc, 32), memcpy(&b, pb, 16), memcpy(&f, pf, 16), memcpy(&g, pg, 16);
    memcpy(&d, pd, 32);
    v4 v = vabi_add(3, a, 2.75, b, c, -7);
    memcpy(r, &v, 32);
    int right = 0;
    for (int i = 0; i < 4; i++)
        right += r[i] == pa[i] + pc[i] * 3 - 7 + 5 + (long long)pb[i];
    printf("vabi: vectors to clang's function and back: %d of 4 right\n", right);
    f4 w = vabi_call(back, f, d, 5, g);
    memcpy(q, &w, 16);
    right = 0;
    for (int i = 0; i < 4; i++)
        right += q[i] == pf[i] * 5 + (float)pd[i] - pg[i];
    printf("vabi: clang's function calling cc's with vectors: %d of 4 right\n", right);
    return 0;
}

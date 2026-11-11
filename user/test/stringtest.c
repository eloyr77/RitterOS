/*
 * Test (32e): the C library's memory and string functions (on x86-64 their
 * AVX2 or SSE2 versions, chosen by the processor; 32g: NEON on AArch64)
 * against plain byte loops
 * here: every length to 130 and some longer, every alignment to 63 of each
 * pointer (the other at a few), the byte sought at the start, the middle,
 * the end, twice or nowhere, overlapping moves both ways, and strings that
 * end right before a page the program may not read (a read past the end
 * would stop it). Built with -fno-builtin: the compiler must not turn the
 * byte loops into the functions they check.
 *
 * stringtest           the checks, a line per function
 * stringtest speed     GB/s of each at 64 B, 4 KiB, 256 KiB and 64 MiB
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define AREA    8192
static unsigned char *bufa, *bufb, *bufc;
static int lengths[160], nlengths;
static uint32_t seed = 12345;

static unsigned rnd(void)
{
    seed = seed * 1103515245u + 12345u;
    return seed >> 8;
}

/* a byte that is not 0 nor c, both halves of the range */
static unsigned char other(int c)
{
    unsigned char b;
    do
        b = (unsigned char)rnd();
    while (b == 0 || b == (unsigned char)c);
    return b;
}

static size_t ref_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static char *ref_strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return 0;
    }
}

static char *ref_strrchr(const char *s, int c)
{
    const char *last = 0;
    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

static void *ref_memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (size_t i = 0; i < n; i++)
        if (p[i] == (unsigned char)c)
            return (void *)(p + i);
    return 0;
}

static void *ref_memrchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    while (n--)
        if (p[n] == (unsigned char)c)
            return (void *)(p + n);
    return 0;
}

static int sign(int x) { return (x > 0) - (x < 0); }

static int ref_memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return x[i] - y[i];
    return 0;
}

static int ref_strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x != y || !x)
            return x - y;
    }
    return 0;
}

static int ref_strcmp(const char *a, const char *b) { return ref_strncmp(a, b, (size_t)-1); }

struct tally {
    const char *name;
    long right, wrong;
};

static struct tally t_strlen = { "strlen", 0, 0 }, t_strnlen = { "strnlen", 0, 0 }, t_strchr = { "strchr", 0, 0 },
    t_strrchr = { "strrchr", 0, 0 }, t_memchr = { "memchr", 0, 0 }, t_memrchr = { "memrchr", 0, 0 }, t_memcmp = { "memcmp", 0, 0 },
    t_strcmp = { "strcmp", 0, 0 }, t_strncmp = { "strncmp", 0, 0 }, t_memcpy = { "memcpy", 0, 0 }, t_memmove = { "memmove", 0, 0 },
    t_memset = { "memset", 0, 0 }, t_strcpy = { "strcpy", 0, 0 }, t_stpcpy = { "stpcpy", 0, 0 };

static void count(struct tally *t, int ok, const char *what, int a, int len, int extra)
{
    if (ok) {
        t->right++;
        return;
    }
    if (t->wrong++ < 5)
        printf("stringtest: FAIL: %s: %s (alignment %d, length %d, %d)\n", t->name, what, a, len, extra);
}

/* a string of len bytes at s (not 0, not c), its NUL after */
static void make_string(unsigned char *s, int len, int c)
{
    for (int i = 0; i < len; i++)
        s[i] = other(c);
    s[len] = 0;
}

static void searches(unsigned char *s, int a, int len)
{
    int c = 1 + (int)(rnd() % 255);
    int places[5] = { -1, 0, len / 2, len - 1, -2 };        /* none, start, middle, end, twice */
    for (int k = 0; k < 5; k++) {
        make_string(s, len, c);
        if (places[k] >= 0 && places[k] < len)
            s[places[k]] = (unsigned char)c;
        if (places[k] == -2 && len >= 3)
            s[len / 3] = s[2 * len / 3] = (unsigned char)c;
        const char *cs = (const char *)s;
        count(&t_strlen, strlen(cs) == ref_strlen(cs), "length", a, len, k);
        count(&t_strnlen, strnlen(cs, (size_t)len / 2) == (size_t)len / 2 &&
                              strnlen(cs, (size_t)len + 5) == (size_t)len, "length up to n", a, len, k);
        count(&t_strchr, strchr(cs, c) == ref_strchr(cs, c) && strchr(cs, 0) == ref_strchr(cs, 0),
              "place", a, len, k);
        count(&t_strrchr, strrchr(cs, c) == ref_strrchr(cs, c) && strrchr(cs, 0) == ref_strrchr(cs, 0),
              "place", a, len, k);
        size_t n = (size_t)len;
        count(&t_memchr, memchr(s, c, n) == ref_memchr(s, c, n) && memchr(s, c, n / 2) == ref_memchr(s, c, n / 2)
                             && memchr(s, 0, n + 1) == ref_memchr(s, 0, n + 1), "place", a, len, k);
        count(&t_memrchr, memrchr(s, c, n) == ref_memrchr(s, c, n) &&
                              memrchr(s, c, n / 2) == ref_memrchr(s, c, n / 2), "place", a, len, k);
    }
}

static void comparisons(unsigned char *x, unsigned char *y, int a, int len)
{
    for (int k = 0; k < 4; k++) {
        make_string(x, len, 0);
        memcpy(y, x, (size_t)len + 1);
        int at = len ? (int)(rnd() % (unsigned)len) : 0;
        if (k == 1 && len)
            y[at] = (unsigned char)(x[at] ^ 0x80);           /* differ in the top bit */
        if (k == 2 && len)
            y[at] = 0;                                      /* y shorter */
        if (k == 3 && len)
            x[at] = (unsigned char)(x[at] == 255 ? 1 : x[at] + 1);
        const char *cx = (const char *)x, *cy = (const char *)y;
        size_t n = (size_t)len;
        count(&t_memcmp, sign(memcmp(x, y, n)) == sign(ref_memcmp(x, y, n)) &&
                             sign(memcmp(y, x, n / 2)) == sign(ref_memcmp(y, x, n / 2)), "sign", a, len, k);
        count(&t_strcmp, sign(strcmp(cx, cy)) == sign(ref_strcmp(cx, cy)) &&
                             sign(strcmp(cy, cx)) == sign(ref_strcmp(cy, cx)), "sign", a, len, k);
        count(&t_strncmp, sign(strncmp(cx, cy, n / 2 + 1)) == sign(ref_strncmp(cx, cy, n / 2 + 1)) &&
                              sign(strncmp(cx, cy, n + 9)) == sign(ref_strncmp(cx, cy, n + 9)) &&
                              strncmp(cx, cy, 0) == 0, "sign", a, len, k);
    }
}

/* copies: dst from src, checked byte by byte with the bytes around */
static void copies(unsigned char *dst, unsigned char *src, int a, int len)
{
    make_string(src, len, 0);
    memset(dst - 8, 0x5A, (size_t)len + 24);
    void *r = memcpy(dst, src, (size_t)len);
    int ok = r == dst && ref_memcmp(dst, src, (size_t)len) == 0 && dst[-1] == 0x5A && dst[len] == 0x5A;
    count(&t_memcpy, ok, "bytes", a, len, 0);
    memset(dst - 8, 0x5A, (size_t)len + 24);
    char *e = stpcpy((char *)dst, (const char *)src);
    ok = e == (char *)dst + len && ref_strcmp((char *)dst, (char *)src) == 0 && dst[len + 1] == 0x5A;
    count(&t_stpcpy, ok, "bytes", a, len, 0);
    memset(dst - 8, 0x5A, (size_t)len + 24);
    ok = strcpy((char *)dst, (const char *)src) == (char *)dst && ref_strcmp((char *)dst, (char *)src) == 0 &&
         dst[len + 1] == 0x5A && dst[-1] == 0x5A;
    count(&t_strcpy, ok, "bytes", a, len, 0);
    int v = (int)(rnd() & 0xFF);
    memset(dst - 8, 0x5A, (size_t)len + 24);
    ok = memset(dst, v | 0x100, (size_t)len) == dst && dst[-1] == 0x5A && dst[len] == 0x5A;
    for (int i = 0; i < len; i++)
        ok &= dst[i] == (unsigned char)v;
    count(&t_memset, ok, "bytes", a, len, v);
}

static void moves(int len)
{
    static unsigned char want[AREA];
    for (int shift = -70; shift <= 70; shift++)
        for (int a = 0; a < 3; a++) {
            unsigned char *base = bufc + 512 + a * 7;
            for (int i = 0; i < len + 160; i++)
                base[i - 80] = (unsigned char)rnd();
            unsigned char *src = base, *dst = base + shift;
            for (int i = 0; i < len + 160; i++)
                want[i] = base[i - 80];
            for (int i = 0; i < len; i++)                     /* what dst gets: src as it was */
                want[shift + 80 + i] = src[i];
            void *r = memmove(dst, src, (size_t)len);
            int ok = r == dst;
            for (int i = 0; i < len + 160; i++)
                ok &= base[i - 80] == want[i];
            count(&t_memmove, ok, "bytes", a, len, shift);
        }
}

/* strings ending at the last byte before a page that may not be read */
static void page_ends(void)
{
    unsigned char *pages = mmap(0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pages == MAP_FAILED || mprotect(pages + 2 * 4096, 4096, PROT_NONE) != 0) {
        printf("stringtest: FAIL: no guard page\n");
        return;
    }
    unsigned char *end = pages + 2 * 4096, *other_end = pages + 4096;
    for (int len = 0; len <= 100; len++) {
        unsigned char *s = end - len - 1, *u = other_end - len - 1 - (len % 7);
        make_string(s, len, 'q');
        memcpy(u, s, (size_t)len + 1);
        const char *cs = (const char *)s, *cu = (const char *)u;
        count(&t_strlen, strlen(cs) == (size_t)len, "at a page's end", 0, len, 0);
        count(&t_strnlen, strnlen(cs, 1000) == (size_t)len, "at a page's end", 0, len, 0);
        count(&t_strchr, strchr(cs, 'q') == 0, "at a page's end", 0, len, 0);
        count(&t_strrchr, strrchr(cs, 'q') == 0, "at a page's end", 0, len, 0);
        count(&t_memchr, memchr(s, 'q', (size_t)len + 1) == 0 && memchr(s, 0, 1000) == s + len,
              "at a page's end", 0, len, 0);
        count(&t_memrchr, memrchr(s, 'q', (size_t)len + 1) == 0, "at a page's end", 0, len, 0);
        count(&t_memcmp, memcmp(s, u, (size_t)len + 1) == 0, "at a page's end", 0, len, 0);
        count(&t_strcmp, strcmp(cs, cu) == 0 && strcmp(cu, cs) == 0, "at a page's end", 0, len, 0);
        count(&t_strncmp, strncmp(cs, cu, 1000) == 0, "at a page's end", 0, len, 0);
        memset(s, 0, (size_t)len + 1);
        int zero = 1;
        for (int i = 0; i <= len; i++)
            zero &= s[i] == 0;
        count(&t_memset, zero, "at a page's end", 0, len, 0);
        memcpy(end - len - 1, u, (size_t)len + 1);
        count(&t_memcpy, ref_strcmp(cs, cu) == 0, "at a page's end", 0, len, 0);
    }
    munmap(pages, 3 * 4096);
}

static void report(struct tally *t)
{
    printf("stringtest: %s: %ld cases right%s\n", t->name, t->right, t->wrong ? ", FAILED" : "");
}

static int checks(void)
{
    for (int n = 0; n <= 130; n++)
        lengths[nlengths++] = n;
    static const int longer[] = { 200, 255, 256, 257, 300, 511, 512, 513, 1000 };
    for (size_t i = 0; i < sizeof(longer) / sizeof(longer[0]); i++)
        lengths[nlengths++] = longer[i];
    static const int others[] = { 0, 1, 15, 16, 31, 32, 33, 63 };
    for (int a = 0; a < 64; a++)
        for (int k = 0; k < nlengths; k++) {
            int len = lengths[k];
            searches(bufa + 64 + a, a, len);
            int b = others[(a + k) % 8];
            comparisons(bufa + 64 + a, bufb + 64 + b, a, len);
            copies(bufb + 64 + b, bufa + 64 + a, a, len);
        }
    for (int k = 0; k < nlengths; k += 3)
        moves(lengths[k]);
    page_ends();
    struct tally *all[] = { &t_strlen, &t_strnlen, &t_strchr, &t_strrchr, &t_memchr, &t_memrchr, &t_memcmp,
                            &t_strcmp, &t_strncmp, &t_memcpy, &t_memmove, &t_memset, &t_strcpy, &t_stpcpy };
    int wrong = 0;
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        report(all[i]);
        wrong += all[i]->wrong != 0;
    }
    printf("stringtest: %s\n", wrong ? "FAILED" : "all right");
    return wrong ? 1 : 0;
}

/* --- speed --------------------------------------------------------------- */

static uint64_t now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static volatile size_t sink;

static double rate(int f, unsigned char *a, unsigned char *b, size_t n)
{
    size_t reps = n < (8u << 20) ? (8u << 20) / n : 1, bytes = 0;
    uint64_t start = now(), took;
    do {
        for (size_t r = 0; r < reps; r++)
            switch (f) {
            case 0: memcpy(a, b, n); break;
            case 1: memmove(a + 1, a, n - 1); break;
            case 2: memset(a, (int)(r & 0x7F), n); break;
            case 3: sink += (size_t)memcmp(a, b, n); break;
            case 4: sink += (size_t)memchr(b, 1, n); break;
            case 5: sink += (size_t)memrchr(b, 1, n); break;
            case 6: sink += strlen((char *)b); break;
            case 7: sink += strnlen((char *)b, n); break;
            case 8: sink += (size_t)strchr((char *)b, 1); break;
            case 9: sink += (size_t)strrchr((char *)b, 1); break;
            case 10: sink += (size_t)strcmp((char *)a, (char *)b); break;
            case 11: sink += (size_t)strncmp((char *)a, (char *)b, n); break;
            case 12: strcpy((char *)a, (char *)b); break;
            case 13: sink += (size_t)stpcpy((char *)a, (char *)b); break;
            }
        bytes += reps * n;
        took = now() - start;
    } while (took < 200000000u);
    return (double)bytes / (double)took;
}

static int speed(void)
{
    static const char *names[] = { "memcpy", "memmove", "memset", "memcmp", "memchr", "memrchr", "strlen",
                                   "strnlen", "strchr", "strrchr", "strcmp", "strncmp", "strcpy", "stpcpy" };
    size_t big = 64u << 20, sizes[] = { 64, 4096, 262144, 64u << 20 };
    unsigned char *a = malloc(big + 64), *b = malloc(big + 64);
    if (!a || !b)
        return 1;
    printf("stringtest: speed (GB/s):  %8s %8s %8s %8s\n", "64 B", "4 KiB", "256 KiB", "64 MiB");
    for (int f = 0; f < 14; f++) {
        printf("stringtest: speed: %-8s", names[f]);
        for (int k = 0; k < 4; k++) {
            size_t n = sizes[k];
            memset(a, 'x', n);
            memset(b, 'x', n);
            a[n - 1] = b[n - 1] = 0;                /* strings of n - 1; memchr finds nothing */
            printf(" %8.2f", rate(f, a, b, f >= 6 && f != 7 && f != 11 ? n - 1 : n));
        }
        printf("\n");
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "speed") == 0)
        return speed();
    bufa = malloc(AREA);
    bufb = malloc(AREA);
    bufc = malloc(AREA);
    if (!bufa || !bufb || !bufc)
        return 1;
    return checks();
}

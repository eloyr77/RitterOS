/*
 * More of the C library (22m, for programs brought from elsewhere -- a port
 * of Doom first): numbers from text (strtol and its kin, strtod, atof),
 * abs, system and abort; strdup, strncpy and the rest of string.h's
 * copies; ctype.h's classes; strings.h's comparisons without case; assert;
 * locale.h's one locale (22n).
 * As C and POSIX define them, for ASCII and the "C" locale.
 */
#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ritteros.h"
#include "fpconv.h"

/* --- ctype.h --- */

int isdigit(int c) { return c >= '0' && c <= '9'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int isalpha(int c) { return islower(c) || isupper(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isblank(int c) { return c == ' ' || c == '\t'; }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int isprint(int c) { return c >= 32 && c < 127; }
int isgraph(int c) { return c > 32 && c < 127; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int tolower(int c) { return isupper(c) ? c + 32 : c; }
int toupper(int c) { return islower(c) ? c - 32 : c; }

/* --- strings.h --- */

int strcasecmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x)
            return x - y;
    }
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n > 0; n--, a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x)
            return x - y;
    }
    return 0;
}

/* --- string.h's copies --- */

char *strndup(const char *s, size_t n)
{
    size_t len = 0;
    while (len < n && s[len])
        len++;
    char *t = malloc(len + 1);
    if (!t)
        return 0;
    memcpy(t, s, len);
    t[len] = 0;
    return t;
}

char *strdup(const char *s)
{
    return strndup(s, (size_t)-1);
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst + strlen(dst);
    size_t i = 0;
    for (; i < n && src[i]; i++)
        d[i] = src[i];
    d[i] = 0;
    return dst;
}

/* --- numbers from text --- */

static int digit_value(int c)
{
    return isdigit(c) ? c - '0' : islower(c) ? c - 'a' + 10 : isupper(c) ? c - 'A' + 10 : 99;
}

/* The magnitude of an integer in text, its sign, and where it ended;
 * *overflow when it is past `limit`. */
static unsigned long long scan_integer(const char *s, char **end, int base, bool *negative,
                                       unsigned long long limit, bool *overflow)
{
    const char *p = s;
    while (isspace((unsigned char)*p))
        p++;
    *negative = *p == '-';
    if (*p == '-' || *p == '+')
        p++;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        isxdigit((unsigned char)p[2]))
        p += 2, base = 16;
    else if (base == 0 && p[0] == '0')
        base = 8;
    else if (base == 0)
        base = 10;
    unsigned long long v = 0;
    const char *digits = p;
    *overflow = false;
    if (base >= 2 && base <= 36)
        for (; digit_value((unsigned char)*p) < base; p++) {
            unsigned d = (unsigned)digit_value((unsigned char)*p);
            if (v > (limit - d) / (unsigned)base)
                *overflow = true;
            else
                v = v * (unsigned)base + d;
        }
    if (end)
        *end = (char *)(p == digits ? s : p);   /* (no digits: nothing read) */
    return v;
}

long long strtoll(const char *s, char **end, int base)
{
    bool negative, overflow;
    unsigned long long v = scan_integer(s, end, base, &negative,
                                        (unsigned long long)INT64_MAX + 1, &overflow);
    if (overflow || (!negative && v > (unsigned long long)INT64_MAX)) {
        errno = ERANGE;
        return negative ? INT64_MIN : INT64_MAX;
    }
    return negative ? (long long)(0 - v) : (long long)v;
}

long strtol(const char *s, char **end, int base)
{
    return (long)strtoll(s, end, base);         /* (long is 64 bits) */
}

unsigned long long strtoull(const char *s, char **end, int base)
{
    bool negative, overflow;
    unsigned long long v = scan_integer(s, end, base, &negative, UINT64_MAX, &overflow);
    if (overflow) {
        errno = ERANGE;
        return UINT64_MAX;
    }
    return negative ? 0 - v : v;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    return (unsigned long)strtoull(s, end, base);
}

double strtod(const char *s, char **end)
{
    return fp_strtod(s, end);                   /* (22q: correctly rounded, fpconv.c) */
}

double atof(const char *s)
{
    return strtod(s, 0);
}

/* --- stdlib.h's rest --- */

int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }
long long llabs(long long v) { return v < 0 ? -v : v; }

#if defined(__x86_64__)
#define SHELL "/system/x86_64/bin/sh"
#else
#define SHELL "/system/aarch64/bin/sh"
#endif

int system(const char *command)
{
    if (!command)
        return 1;
    char *argv[] = { "sh", "-c", (char *)command, 0 };
    int fds[3] = { -1, -1, -1 };
    pid_t pid = ritteros_spawn(SHELL, argv, 0, fds);
    if (pid < 0)
        return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    return status;
}

void abort(void)
{
    /* (22r) As C's: SIGABRT; its handler may run, but a handler that returns
     * does not save the program: the default, and SIGABRT again. */
    raise(SIGABRT);
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
    _Exit(134);                         /* (not reached) */
}

void ritteros_assert_failed(const char *expression, const char *file, int line,
                            const char *function)
{
    dprintf(STDERR_FILENO, "assertion failed: %s (%s:%d, %s)\n", expression, file, line, function);
    abort();
}

/* --- locale.h (22n): only the "C" locale --- */

static char empty[] = "", dot[] = ".", c_name[] = "C";
static struct lconv c_lconv = {
    dot, empty, empty, empty, empty, empty, empty, empty, empty, empty,
    127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127,
};

char *setlocale(int category, const char *locale)
{
    if (category < LC_CTYPE || category > LC_ALL)
        return 0;
    if (!locale || !locale[0] || !strcmp(locale, "C") || !strcmp(locale, "POSIX"))
        return c_name;
    return 0;
}

struct lconv *localeconv(void)
{
    return &c_lconv;
}

/* --- 22p: more of stdlib.h --- */

long long atoll(const char *s)
{
    return strtoll(s, 0, 10);
}

float strtof(const char *s, char **end)
{
    return fp_strtof(s, end);                   /* (rounded once, to float) */
}

long double strtold(const char *s, char **end)
{
    return fp_to_long_double(strtod(s, end));
}

div_t div(int n, int d)
{
    return (div_t){ n / d, n % d };
}

ldiv_t ldiv(long n, long d)
{
    return (ldiv_t){ n / d, n % d };
}

lldiv_t lldiv(long long n, long long d)
{
    return (lldiv_t){ n / d, n % d };
}

static void swap_bytes(char *a, char *b, size_t size)
{
    while (size--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

/* Quicksort with the middle element of three as the pivot and insertion
 * sort for short runs; the shorter side is sorted by recursion, the longer
 * one by the loop, so the depth stays at most log2(count). */
void qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *))
{
    char *b = base;
    while (count > 8) {
        char *lo = b, *mid = b + count / 2 * size, *hi = b + (count - 1) * size;
        if (compare(mid, lo) < 0)
            swap_bytes(mid, lo, size);
        if (compare(hi, mid) < 0) {
            swap_bytes(hi, mid, size);
            if (compare(mid, lo) < 0)
                swap_bytes(mid, lo, size);
        }
        swap_bytes(mid, lo, size);                  /* the pivot to the front */
        size_t i = 1, j = count - 1;
        for (;;) {
            while (i <= j && compare(b + i * size, lo) < 0)
                i++;
            while (j >= i && compare(b + j * size, lo) > 0)
                j--;
            if (i >= j)
                break;
            swap_bytes(b + i * size, b + j * size, size);
            i++;
            j--;
        }
        swap_bytes(lo, b + j * size, size);         /* the pivot at its place j */
        size_t left = j, right = count - j - 1;
        if (left < right) {
            qsort(b, left, size, compare);
            b += (j + 1) * size;
            count = right;
        } else {
            qsort(b + (j + 1) * size, right, size, compare);
            count = left;
        }
    }
    for (size_t i = 1; i < count; i++)
        for (size_t k = i; k > 0 && compare(b + (k - 1) * size, b + k * size) > 0; k--)
            swap_bytes(b + (k - 1) * size, b + k * size, size);
}

void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *))
{
    const char *b = base;
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = compare(key, b + mid * size);
        if (c == 0)
            return (void *)(b + mid * size);
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return 0;
}

/* A 64-bit linear congruential generator (Knuth's MMIX constants); rand
 * gives its top 31 bits, which are the good ones. */
static uint64_t rand_state = 1;

void srand(unsigned int seed)
{
    rand_state = seed;
}

int rand(void)
{
    rand_state = rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)(rand_state >> 33);
}

#define EXIT_FUNCTIONS 32
static void (*at_exit[EXIT_FUNCTIONS])(void), (*at_quick[EXIT_FUNCTIONS])(void);
static int at_exit_count, at_quick_count;

int atexit(void (*fn)(void))
{
    if (at_exit_count == EXIT_FUNCTIONS)
        return -1;
    at_exit[at_exit_count++] = fn;
    return 0;
}

int at_quick_exit(void (*fn)(void))
{
    if (at_quick_count == EXIT_FUNCTIONS)
        return -1;
    at_quick[at_quick_count++] = fn;
    return 0;
}

/* exit's first steps (syscalls.c): the last function given runs first; one
 * that calls exit again does not see itself again. */
void ritteros_run_atexit(void)
{
    while (at_exit_count > 0)
        at_exit[--at_exit_count]();
}

void _Exit(int code)
{
    syscall6(SYS_EXIT, code, 0, 0, 0, 0, 0);
    for (;;)
        ;                                           /* (exit never returns) */
}

void _exit(int code)
{
    _Exit(code);
}

void quick_exit(int code)
{
    while (at_quick_count > 0)
        at_quick[--at_quick_count]();
    _Exit(code);
}

int mblen(const char *s, size_t n)
{
    return mbtowc(0, s, n);
}

int mbtowc(wchar_t *wc, const char *s, size_t n)
{
    if (!s)
        return 0;                                   /* (no shift states) */
    if (n == 0)
        return -1;
    if (wc)
        *wc = (wchar_t)(unsigned char)*s;
    return *s ? 1 : 0;
}

int wctomb(char *s, wchar_t wc)
{
    if (!s)
        return 0;
    if ((unsigned long)wc > 255) {
        errno = EILSEQ;
        return -1;
    }
    *s = (char)wc;
    return 1;
}

size_t mbstowcs(wchar_t *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; src[i] && (!dst || i < n); i++)
        if (dst)
            dst[i] = (wchar_t)(unsigned char)src[i];
    if (dst && i < n)
        dst[i] = 0;
    return i;
}

size_t wcstombs(char *dst, const wchar_t *src, size_t n)
{
    size_t i = 0;
    for (; src[i] && (!dst || i < n); i++) {
        if ((unsigned long)src[i] > 255) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        if (dst)
            dst[i] = (char)src[i];
    }
    if (dst && i < n)
        dst[i] = 0;
    return i;
}

/* --- 22p: more of string.h --- */

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    while (s[n] && strchr(accept, s[n]))
        n++;
    return n;
}

char *strpbrk(const char *s, const char *breaks)
{
    for (; *s; s++)
        if (strchr(breaks, *s))
            return (char *)s;
    return 0;
}

char *strtok(char *s, const char *delimiters)
{
    static char *rest;
    if (!s)
        s = rest;
    if (!s)
        return 0;
    s += strspn(s, delimiters);
    if (!*s) {
        rest = 0;
        return 0;
    }
    char *end = s + strcspn(s, delimiters);
    if (*end) {
        *end = 0;
        rest = end + 1;
    } else {
        rest = 0;
    }
    return s;
}

size_t strnlen(const char *s, size_t n)
{
    const char *end = memchr(s, 0, n);      /* (32e: memchr's vector versions) */
    return end ? (size_t)(end - s) : n;
}

void *memccpy(void *dst, const void *src, int c, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *p = src;
    for (size_t i = 0; i < n; i++) {
        d[i] = p[i];
        if (p[i] == (unsigned char)c)
            return d + i + 1;
    }
    return 0;
}

int strcoll(const char *a, const char *b)
{
    return strcmp(a, b);
}

size_t strxfrm(char *dst, const char *src, size_t n)
{
    size_t len = strlen(src);
    if (n) {
        size_t k = len < n - 1 ? len : n - 1;
        memcpy(dst, src, k);
        dst[k] = 0;
    }
    return len;
}

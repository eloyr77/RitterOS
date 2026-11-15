/*
 * (43a) string.h's and strings.h's functions beyond C's that ported code
 * expects: POSIX's (strtok_r, strerror_r, strsignal, stpncpy), the BSDs'
 * (strlcpy, strlcat, strsep, bcopy, index, ffs) and GNU's (strchrnul,
 * mempcpy, memmem, strverscmp, explicit_bzero), as their manuals describe
 * them; the texts of strsignal are glibc's.
 */
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

char *strtok_r(char *s, const char *delimiters, char **save)
{
    if (!s)
        s = *save;
    if (!s)
        return 0;
    s += strspn(s, delimiters);
    if (!*s) {
        *save = 0;
        return 0;
    }
    char *end = s + strcspn(s, delimiters);
    if (*end)
        *end++ = 0;
    else
        end = 0;
    *save = end;
    return s;
}

int strerror_r(int error, char *buf, size_t size)
{
    const char *text = strerror(error);
    size_t n = strlen(text);
    if (size == 0)
        return ERANGE;
    if (n >= size) {
        memcpy(buf, text, size - 1);
        buf[size - 1] = 0;
        return ERANGE;
    }
    memcpy(buf, text, n + 1);
    return 0;
}

char *strsignal(int signal)
{
    static const char *const names[] = {
        [1] = "Hangup", [2] = "Interrupt", [3] = "Quit", [4] = "Illegal instruction",
        [5] = "Trace/breakpoint trap", [6] = "Aborted", [7] = "Bus error",
        [8] = "Floating point exception", [9] = "Killed", [10] = "User defined signal 1",
        [11] = "Segmentation fault", [12] = "User defined signal 2", [13] = "Broken pipe",
        [14] = "Alarm clock", [15] = "Terminated", [16] = "Stack fault",
        [17] = "Child exited", [18] = "Continued", [19] = "Stopped (signal)", [20] = "Stopped",
        [21] = "Stopped (tty input)", [22] = "Stopped (tty output)", [23] = "Urgent I/O condition",
        [24] = "CPU time limit exceeded", [25] = "File size limit exceeded",
        [26] = "Virtual timer expired", [27] = "Profiling timer expired",
        [28] = "Window changed", [29] = "I/O possible", [30] = "Power failure",
        [31] = "Bad system call",
    };
    if (signal > 0 && signal < (int)(sizeof(names) / sizeof(names[0])))
        return (char *)names[signal];
    return (char *)"Unknown signal";
}

char *strcasestr(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);
    for (; *haystack; haystack++)
        if (strncasecmp(haystack, needle, n) == 0)
            return (char *)haystack;
    return n == 0 ? (char *)haystack : 0;
}

char *strchrnul(const char *s, int c)
{
    while (*s && *s != (char)c)
        s++;
    return (char *)s;
}

char *stpncpy(char *dst, const char *src, size_t n)
{
    size_t k = strnlen(src, n);
    memcpy(dst, src, k);
    memset(dst + k, 0, n - k);
    return dst + k;
}

void *mempcpy(void *dst, const void *src, size_t n)
{
    return (char *)memcpy(dst, src, n) + n;
}

void *memmem(const void *haystack, size_t n, const void *needle, size_t m)
{
    const unsigned char *h = haystack, *e = needle;
    if (m == 0)
        return (void *)h;
    while (n >= m) {
        const unsigned char *p = memchr(h, e[0], n - m + 1);
        if (!p)
            return 0;
        if (memcmp(p, e, m) == 0)
            return (void *)p;
        n -= (size_t)(p - h) + 1;
        h = p + 1;
    }
    return 0;
}

char *strsep(char **s, const char *delimiters)
{
    char *start = *s;
    if (!start)
        return 0;
    char *end = start + strcspn(start, delimiters);
    if (*end)
        *end++ = 0, *s = end;
    else
        *s = 0;
    return start;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);
    if (size) {
        size_t k = n < size - 1 ? n : size - 1;
        memcpy(dst, src, k);
        dst[k] = 0;
    }
    return n;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t have = strnlen(dst, size);
    if (have == size)
        return size + strlen(src);
    return have + strlcpy(dst + have, src, size - have);
}

void explicit_bzero(void *s, size_t n)
{
    memset(s, 0, n);
    __asm__ volatile("" : : "r"(s) : "memory");     /* (the stores may not be dropped) */
}

/* GNU's order: the strings compared byte by byte, but where both have a
 * run of digits the runs compare as numbers, and a run with leading zeros
 * as a fraction ("000" < "00" < "01" < "010" < "09" < "0" < "1" < "9" <
 * "10"). A walk through the equal part keeps what kind of run it is in:
 * none, an integer, a fraction, or zeros only so far; at the first
 * difference that kind and the two bytes' kinds (other, digit, '0') say
 * whether the bytes decide, the runs' lengths do, or the answer is known. */
enum { NONE = 0, INTEGER = 3, FRACTION = 6, ZEROS = 9 };
enum { BYTES = 2, LENGTHS = 3 };

static int digit_kind(int c)
{
    return (c == '0') + (isdigit(c) != 0);      /* 0 other, 1 digit 1-9, 2 '0' */
}

int strverscmp(const char *a, const char *b)
{
    static const unsigned char after[] = {
        NONE, INTEGER, ZEROS,                   /* from NONE by the byte's kind */
        NONE, INTEGER, INTEGER,                 /* INTEGER */
        NONE, FRACTION, FRACTION,               /* FRACTION */
        NONE, FRACTION, ZEROS,                  /* ZEROS */
    };
    static const signed char decide[] = {
        BYTES, BYTES, BYTES, BYTES, LENGTHS, BYTES, BYTES, BYTES, BYTES,        /* NONE */
        BYTES, -1, -1, 1, LENGTHS, LENGTHS, 1, LENGTHS, LENGTHS,               /* INTEGER */
        BYTES, BYTES, BYTES, BYTES, BYTES, BYTES, BYTES, BYTES, BYTES,          /* FRACTION */
        BYTES, 1, 1, -1, BYTES, BYTES, -1, BYTES, BYTES,                       /* ZEROS */
    };
    const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
    int c = *p++, d = *q++;
    int state = NONE + digit_kind(c);
    while (c == d) {
        if (!c)
            return 0;
        state = after[state];
        c = *p++, d = *q++;
        state += digit_kind(c);
    }
    int how = decide[state * 3 + digit_kind(d)];
    if (how == BYTES)
        return c - d;
    if (how != LENGTHS)
        return how;
    while (isdigit(*p++))
        if (!isdigit(*q++))
            return 1;
    return isdigit(*q) ? -1 : c - d;
}

int bcmp(const void *a, const void *b, size_t n)
{
    return memcmp(a, b, n) != 0;
}

void bcopy(const void *src, void *dst, size_t n)
{
    memmove(dst, src, n);
}

void bzero(void *s, size_t n)
{
    memset(s, 0, n);
}

char *index(const char *s, int c)
{
    return strchr(s, c);
}

char *rindex(const char *s, int c)
{
    return strrchr(s, c);
}

int ffsll(long long v)
{
    return v ? __builtin_ctzll((unsigned long long)v) + 1 : 0;
}

int ffs(int v)
{
    return ffsll((long long)(unsigned)v);
}

int ffsl(long v)
{
    return ffsll((long long)v);
}

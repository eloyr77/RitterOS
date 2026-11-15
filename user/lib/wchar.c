/*
 * wchar.h's strings, numbers, dates and conversions (step 22v). The "C"
 * locale has one byte per character: a byte's wide character is its value,
 * and a wide character above 255 has no byte. Numbers and dates go through
 * the byte functions (strtol, strtod, strftime) on a copy of the text.
 * (The stream and formatting calls are in file.c and stdio.c.)
 */
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>
#include "mbconv.h"

/* --- strings --- */

size_t wcslen(const wchar_t *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

wchar_t *wcscpy(wchar_t *dst, const wchar_t *src)
{
    wchar_t *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

wchar_t *wcscat(wchar_t *dst, const wchar_t *src)
{
    wcscpy(dst + wcslen(dst), src);
    return dst;
}

wchar_t *wcsncat(wchar_t *dst, const wchar_t *src, size_t n)
{
    wchar_t *d = dst + wcslen(dst);
    size_t i = 0;
    for (; i < n && src[i]; i++)
        d[i] = src[i];
    d[i] = 0;
    return dst;
}

int wcscmp(const wchar_t *a, const wchar_t *b)
{
    for (; *a && *a == *b; a++, b++)
        ;
    return *a < *b ? -1 : *a > *b;
}

int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (; n && *a && *a == *b; n--, a++, b++)
        ;
    return !n ? 0 : *a < *b ? -1 : *a > *b;
}

int wcscoll(const wchar_t *a, const wchar_t *b)
{
    return wcscmp(a, b);                            /* (the "C" locale's order) */
}

size_t wcsxfrm(wchar_t *dst, const wchar_t *src, size_t n)
{
    size_t len = wcslen(src);
    if (len < n)
        wcscpy(dst, src);
    return len;
}

wchar_t *wcschr(const wchar_t *s, wchar_t c)
{
    for (;; s++) {
        if (*s == c)
            return (wchar_t *)s;
        if (!*s)
            return 0;
    }
}

wchar_t *wcsrchr(const wchar_t *s, wchar_t c)
{
    const wchar_t *last = 0;
    for (;; s++) {
        if (*s == c)
            last = s;
        if (!*s)
            return (wchar_t *)last;
    }
}

size_t wcsspn(const wchar_t *s, const wchar_t *accept)
{
    size_t n = 0;
    while (s[n] && wcschr(accept, s[n]))
        n++;
    return n;
}

size_t wcscspn(const wchar_t *s, const wchar_t *reject)
{
    size_t n = 0;
    while (s[n] && !wcschr(reject, s[n]))
        n++;
    return n;
}

wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept)
{
    s += wcscspn(s, accept);
    return *s ? (wchar_t *)s : 0;
}

wchar_t *wcsstr(const wchar_t *haystack, const wchar_t *needle)
{
    size_t n = wcslen(needle);
    for (; *haystack; haystack++)
        if (!wcsncmp(haystack, needle, n))
            return (wchar_t *)haystack;
    return n ? 0 : (wchar_t *)haystack;
}

wchar_t *wcstok(wchar_t *s, const wchar_t *delimiters, wchar_t **place)
{
    if (!s)
        s = *place;
    if (!s)
        return 0;
    s += wcsspn(s, delimiters);
    if (!*s) {
        *place = 0;
        return 0;
    }
    wchar_t *end = s + wcscspn(s, delimiters);
    if (*end)
        *end++ = 0;
    else
        end = 0;
    *place = end;
    return s;
}

wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n)
{
    for (; n; n--, s++)
        if (*s == c)
            return (wchar_t *)s;
    return 0;
}

int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (; n; n--, a++, b++)
        if (*a != *b)
            return *a < *b ? -1 : 1;
    return 0;
}

wchar_t *wmemcpy(wchar_t *dst, const wchar_t *src, size_t n)
{
    return memcpy(dst, src, n * sizeof(wchar_t));
}

wchar_t *wmemmove(wchar_t *dst, const wchar_t *src, size_t n)
{
    return memmove(dst, src, n * sizeof(wchar_t));
}

wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n)
{
    for (size_t i = 0; i < n; i++)
        s[i] = c;
    return s;
}

/* --- numbers: the bytes' functions on a copy of the ASCII start --- */

/* s's start up to the first character that is not ASCII, as bytes: into
 * small, or a heap copy (to be freed) when it is longer. */
static char *ascii_copy(const wchar_t *s, char *small, size_t room)
{
    size_t n = 0;
    while (s[n] > 0 && s[n] < 128)
        n++;
    char *copy = n < room ? small : malloc(n + 1);
    if (!copy)
        return 0;
    for (size_t i = 0; i < n; i++)
        copy[i] = (char)s[i];
    copy[n] = 0;
    return copy;
}

#define NUMBER(type, name, call)                                            \
type name(const wchar_t *s, wchar_t **end, int base)                        \
{                                                                           \
    char small[128], *e;                                                    \
    char *copy = ascii_copy(s, small, sizeof(small));                       \
    if (!copy) {                                                            \
        errno = ENOMEM;                                                     \
        if (end)                                                            \
            *end = (wchar_t *)s;                                            \
        return 0;                                                           \
    }                                                                       \
    type v = call(copy, &e, base);                                          \
    if (end)                                                                \
        *end = (wchar_t *)s + (e - copy);                                   \
    if (copy != small)                                                      \
        free(copy);                                                         \
    return v;                                                               \
}

NUMBER(long, wcstol, strtol)
NUMBER(long long, wcstoll, strtoll)
NUMBER(unsigned long, wcstoul, strtoul)
NUMBER(unsigned long long, wcstoull, strtoull)

#define FLOATING(type, name, call)                                          \
type name(const wchar_t *s, wchar_t **end)                                  \
{                                                                           \
    char small[128], *e;                                                    \
    char *copy = ascii_copy(s, small, sizeof(small));                       \
    if (!copy) {                                                            \
        errno = ENOMEM;                                                     \
        if (end)                                                            \
            *end = (wchar_t *)s;                                            \
        return 0;                                                           \
    }                                                                       \
    type v = call(copy, &e);                                                \
    if (end)                                                                \
        *end = (wchar_t *)s + (e - copy);                                   \
    if (copy != small)                                                      \
        free(copy);                                                         \
    return v;                                                               \
}

FLOATING(double, wcstod, strtod)
FLOATING(float, wcstof, strtof)
FLOATING(long double, wcstold, strtold)

/* --- dates --- */

size_t wcsftime(wchar_t *s, size_t size, const wchar_t *format, const struct tm *tm)
{
    size_t n = wcslen(format);
    char *fmt = malloc(n + 1), *out = malloc(size ? size : 1);
    size_t done = 0;
    if (fmt && out) {
        bool bytes = true;
        for (size_t i = 0; i <= n; i++) {
            bytes &= (unsigned)format[i] <= 255;
            fmt[i] = (char)format[i];
        }
        done = bytes && size ? strftime(out, size, fmt, tm) : 0;
        for (size_t i = 0; done && i <= done; i++)
            s[i] = (wchar_t)(unsigned char)out[i];
    }
    free(fmt);
    free(out);
    return done;
}

/* --- bytes and wide characters --- */

wint_t btowc(int c)
{
    if (c == EOF || c < -128 || c > 255)
        return WEOF;
    unsigned char b = (unsigned char)c;
    if (b >= 0x80 && mb_utf8())
        return WEOF;                            /* (43a-2: no character is one such byte) */
    return (wint_t)b;
}

int wctob(wint_t wc)
{
    if (wc == WEOF || (unsigned)wc > (mb_utf8() ? 0x7Fu : 0xFFu))
        return EOF;
    return (int)wc;
}

int mbsinit(const mbstate_t *state)
{
    return !state || !state->need;
}

size_t mbrtowc(wchar_t *wc, const char *s, size_t n, mbstate_t *state)
{
    static mbstate_t own;
    return mb_decode(wc, s, n, state ? state : &own);
}

size_t mbrlen(const char *s, size_t n, mbstate_t *state)
{
    static mbstate_t own;
    return mb_decode(0, s, n, state ? state : &own);
}

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *state)
{
    return mb_encode(s, wc, state);
}

/* (43a-2) at most nms bytes of *src into at most n wide characters */
size_t mbsnrtowcs(wchar_t *dst, const char **src, size_t nms, size_t n, mbstate_t *state)
{
    static mbstate_t own;
    if (!state)
        state = &own;
    const char *s = *src;
    size_t count = 0;
    while (!dst || count < n) {
        wchar_t wc;
        size_t k = mb_decode(&wc, s, nms, state);
        if (k == (size_t)-1) {
            if (dst)
                *src = s;
            return (size_t)-1;
        }
        if (k == (size_t)-2) {                  /* (the bytes end inside a character) */
            if (dst)
                *src = s + nms;
            return count;
        }
        if (dst)
            dst[count] = wc;
        if (k == 0) {
            if (dst)
                *src = 0;
            return count;
        }
        s += k, nms -= k, count++;
    }
    *src = s;
    return count;
}

size_t mbsrtowcs(wchar_t *dst, const char **src, size_t n, mbstate_t *state)
{
    return mbsnrtowcs(dst, src, (size_t)-1, n, state);
}

/* (43a-2) at most nwc wide characters of *src into at most n bytes */
size_t wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t n, mbstate_t *state)
{
    const wchar_t *s = *src;
    size_t count = 0;
    char bytes[8];
    for (size_t i = 0; i < nwc; i++) {
        size_t k = mb_encode(bytes, s[i], state);
        if (k == (size_t)-1) {
            if (dst)
                *src = s + i;
            return (size_t)-1;
        }
        if (s[i] == 0) {
            if (dst) {
                if (count < n)
                    dst[count] = 0;
                *src = 0;
            }
            return count;
        }
        if (dst) {
            if (count + k > n) {
                *src = s + i;
                return count;
            }
            memcpy(dst + count, bytes, k);
        }
        count += k;
    }
    if (dst)
        *src = s + nwc;
    return count;
}

size_t wcsrtombs(char *dst, const wchar_t **src, size_t n, mbstate_t *state)
{
    return wcsnrtombs(dst, src, (size_t)-1, n, state);
}

/* (43a) */
wchar_t *wcsdup(const wchar_t *s)
{
    size_t n = wcslen(s) + 1;
    wchar_t *c = malloc(n * sizeof(wchar_t));
    if (!c) {
        errno = ENOMEM;
        return 0;
    }
    return wmemcpy(c, s, n);
}

size_t wcsnlen(const wchar_t *s, size_t n)
{
    size_t k = 0;
    while (k < n && s[k])
        k++;
    return k;
}

int wcsncasecmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        wint_t x = towlower((wint_t)a[i]), y = towlower((wint_t)b[i]);
        if (x != y)
            return x < y ? -1 : 1;
        if (!x)
            break;
    }
    return 0;
}

int wcscasecmp(const wchar_t *a, const wchar_t *b)
{
    return wcsncasecmp(a, b, (size_t)-1);
}

wchar_t *wcpcpy(wchar_t *dst, const wchar_t *src)
{
    size_t n = wcslen(src);
    wmemcpy(dst, src, n + 1);
    return dst + n;
}

wchar_t *wcpncpy(wchar_t *dst, const wchar_t *src, size_t n)
{
    size_t k = wcsnlen(src, n);
    wmemcpy(dst, src, k);
    wmemset(dst + k, 0, n - k);
    return dst + k;
}

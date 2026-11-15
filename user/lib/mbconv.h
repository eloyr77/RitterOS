/*
 * (43a-2) The library's own bridge between bytes and wide characters
 * (locale.c): every conversion goes through these, so the C locale's one
 * byte a character and C.UTF-8's UTF-8 are decided in one place.
 *
 * The references are weak: a static link that has no use for locales
 * (ld.so, which formats its messages with stdio.c) leaves locale.c out,
 * and with it the thread's locale, a per-thread variable that ld.so
 * cannot have; there the C locale's rule below applies.
 */
#ifndef RITTEROS_MBCONV_H
#define RITTEROS_MBCONV_H

#include <errno.h>
#include <stddef.h>
#include <wchar.h>

int __ritteros_utf8(void) __attribute__((weak));
size_t __ritteros_mbr(wchar_t *wc, const char *s, size_t n, mbstate_t *state) __attribute__((weak));
size_t __ritteros_wcr(char *s, wchar_t wc, mbstate_t *state) __attribute__((weak));

static inline int mb_utf8(void)
{
    return __ritteros_utf8 ? __ritteros_utf8() : 0;
}

static inline size_t mb_decode(wchar_t *wc, const char *s, size_t n, mbstate_t *state)
{
    if (__ritteros_mbr)
        return __ritteros_mbr(wc, s, n, state);
    if (!s)
        return 0;
    if (n == 0)
        return (size_t)-2;
    if (wc)
        *wc = (wchar_t)(unsigned char)*s;
    return *s ? 1 : 0;
}

static inline size_t mb_encode(char *s, wchar_t wc, mbstate_t *state)
{
    if (__ritteros_wcr)
        return __ritteros_wcr(s, wc, state);
    if (!s)
        return 1;
    if ((unsigned)wc > 255) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    *s = (char)wc;
    return 1;
}

#endif

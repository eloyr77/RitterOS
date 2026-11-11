#include "string.h"
#include <stdint.h>

/*
 * Copies and fills go 8 bytes at a time where both addresses allow it (a
 * byte at a time was the cost of every block copy and screen scroll); the
 * ends and misaligned pairs go byte by byte.
 */
#define WORD sizeof(uint64_t)

/* May alias anything: copies are made through it into any kind of object. */
typedef uint64_t __attribute__((may_alias)) word_t;

static int both_aligned(const void *a, const void *b)
{
    return (((uintptr_t)a | (uintptr_t)b) & (WORD - 1)) == 0;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n && ((uintptr_t)d & (WORD - 1))) {
        *d++ = *s++;
        n--;
    }
    if (both_aligned(d, s)) {
        for (; n >= WORD; n -= WORD, d += WORD, s += WORD)
            *(word_t *)d = *(const word_t *)s;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d <= s || d >= s + n)
        return memcpy(dst, src, n);        /* forwards is safe */
    /* Overlapping, destination above the source: copy backwards. */
    d += n;
    s += n;
    while (n && ((uintptr_t)d & (WORD - 1))) {
        *--d = *--s;
        n--;
    }
    if (both_aligned(d, s)) {
        for (; n >= WORD; n -= WORD) {
            d -= WORD;
            s -= WORD;
            *(word_t *)d = *(const word_t *)s;
        }
    }
    while (n--)
        *--d = *--s;
    return dst;
}

void *memset(void *dst, int value, size_t n)
{
    unsigned char *d = dst;
    while (n && ((uintptr_t)d & (WORD - 1))) {
        *d++ = (unsigned char)value;
        n--;
    }
    uint64_t word = 0x0101010101010101ULL * (unsigned char)value;
    for (; n >= WORD; n -= WORD, d += WORD)
        *(word_t *)d = word;
    while (n--)
        *d++ = (unsigned char)value;
    return dst;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (; n > 0; p++, n--)
        if (*p == (unsigned char)c)
            return (void *)p;
    return 0;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n--; x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n > 0; n--, a++, b++) {
        if (*a != *b || !*a)
            return (unsigned char)*a - (unsigned char)*b;
    }
    return 0;
}

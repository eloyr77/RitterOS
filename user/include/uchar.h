/*
 * uchar.h (step 22w): Unicode characters and UTF-8. Unlike wchar.h's calls
 * (the "C" locale: a byte each), these always read and write UTF-8, the
 * text of RitterOS: char32_t holds a code point, char16_t a UTF-16 unit
 * (characters above U+FFFF are two: a surrogate pair), char8_t (C23) a
 * UTF-8 byte. Invalid UTF-8 (overlong forms, surrogates, above U+10FFFF,
 * a byte out of place) is EILSEQ.
 *
 * mbrtoXX: the character at s (at most n bytes) into *c; the bytes it took,
 * 0 for the NUL, (size_t)-2 for a character not complete yet (the bytes
 * are kept in *state), (size_t)-1 for invalid UTF-8, (size_t)-3 for a unit
 * given from *state without taking a byte (the second half of a pair, the
 * rest of a char8_t's character). XXrtomb: c's UTF-8 into s; the bytes
 * written (0 while a pair's or a character's first part is kept in
 * *state), (size_t)-1 for a value that is not one. A null state uses one
 * of the library's own.
 */
#ifndef RITTEROS_UCHAR_H
#define RITTEROS_UCHAR_H

#include <stddef.h>

typedef unsigned char char8_t;
typedef __CHAR16_TYPE__ char16_t;
typedef __CHAR32_TYPE__ char32_t;

#ifndef RITTEROS_MBSTATE_T
#define RITTEROS_MBSTATE_T
typedef struct {
    unsigned int need, bits;
} mbstate_t;
#endif

size_t mbrtoc8(char8_t *c, const char *s, size_t n, mbstate_t *state);
size_t c8rtomb(char *s, char8_t c, mbstate_t *state);
size_t mbrtoc16(char16_t *c, const char *s, size_t n, mbstate_t *state);
size_t c16rtomb(char *s, char16_t c, mbstate_t *state);
size_t mbrtoc32(char32_t *c, const char *s, size_t n, mbstate_t *state);
size_t c32rtomb(char *s, char32_t c, mbstate_t *state);

#endif

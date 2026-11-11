/*
 * uchar.h (step 22w): UTF-8 always, whatever the locale. One decoder takes
 * bytes (whole or a few at a time) to a code point and keeps a character
 * not yet complete in the mbstate_t; the others build on it. The state:
 * `need` holds what is kept (KIND in bits 4-7), the bytes still to come
 * (bits 0-3) and the character's length (bits 8-11); `bits` the value so
 * far, a surrogate, or bytes still to hand out.
 */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <uchar.h>

#define KIND(s) ((s)->need >> 4 & 15)
#define DECODING 1                      /* a UTF-8 character begun */
#define LOW_NEXT 2                      /* mbrtoc16: the pair's low half to give */
#define HIGH_KEPT 3                     /* c16rtomb: a pair's high half seen */
#define BYTES_NEXT 4                    /* mbrtoc8: the character's other bytes to give */

#define INVALID ((size_t)-1)
#define INCOMPLETE ((size_t)-2)
#define FROM_STATE ((size_t)-3)

static size_t invalid(mbstate_t *st)
{
    memset(st, 0, sizeof(*st));
    errno = EILSEQ;
    return INVALID;
}

/* The next character at s into *cp: the bytes taken from s, INCOMPLETE
 * (all n taken, kept), INVALID, 0 for the NUL. */
static size_t decode(uint32_t *cp, const char *s, size_t n, mbstate_t *st)
{
    if (!s)
        s = "", n = 1;
    if (n == 0)
        return INCOMPLETE;
    unsigned left = 0, len = 0;
    uint32_t v = 0;
    if (KIND(st) == DECODING) {
        left = st->need & 15;
        len = st->need >> 8 & 15;
        v = st->bits;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned b = (unsigned char)s[i];
        if (!left) {                                    /* a character's first byte */
            if (b < 0x80) {
                *cp = b;
                memset(st, 0, sizeof(*st));
                return b ? 1 : 0;
            }
            if (b >= 0xC2 && b <= 0xDF)
                len = 2, v = b & 0x1F;
            else if (b >= 0xE0 && b <= 0xEF)
                len = 3, v = b & 0x0F;
            else if (b >= 0xF0 && b <= 0xF4)
                len = 4, v = b & 0x07;
            else
                return invalid(st);                     /* (C0, C1: overlong; F5-FF; 80-BF) */
            left = len - 1;
            continue;
        }
        if ((b & 0xC0) != 0x80)
            return invalid(st);
        if (left == len - 1 &&                          /* the second byte: overlong, */
            ((len == 3 && v == 0 && b < 0xA0) ||        /* surrogate, above U+10FFFF? */
             (len == 3 && v == 0xD && b > 0x9F) ||
             (len == 4 && v == 0 && b < 0x90) ||
             (len == 4 && v == 4 && b > 0x8F)))
            return invalid(st);
        v = v << 6 | (b & 0x3F);
        if (--left == 0) {
            *cp = v;
            memset(st, 0, sizeof(*st));
            return i + 1;
        }
    }
    st->need = DECODING << 4 | left | len << 8;
    st->bits = v;
    return INCOMPLETE;
}

/* cp's UTF-8 into out: its length, or 0 for no code point. */
static unsigned encode(char *out, uint32_t cp)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
        return 0;
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | cp >> 12);
        out[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | cp >> 18);
    out[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    out[2] = (char)(0x80 | (cp >> 6 & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static mbstate_t own_c8, own_c8r, own_c16, own_c16r, own_c32, own_c32r;

size_t mbrtoc32(char32_t *c, const char *s, size_t n, mbstate_t *state)
{
    mbstate_t *st = state ? state : &own_c32;
    uint32_t cp;
    size_t r = decode(&cp, s, n, st);
    if (r <= 4 && c && s)
        *c = cp;
    return r;
}

size_t c32rtomb(char *s, char32_t c, mbstate_t *state)
{
    mbstate_t *st = state ? state : &own_c32r;
    char scratch[4];
    if (!s)
        s = scratch, c = 0;
    memset(st, 0, sizeof(*st));
    unsigned len = encode(s, c);
    return len ? len : invalid(st);
}

size_t mbrtoc16(char16_t *c, const char *s, size_t n, mbstate_t *state)
{
    mbstate_t *st = state ? state : &own_c16;
    if (KIND(st) == LOW_NEXT) {
        if (c)
            *c = (char16_t)st->bits;
        memset(st, 0, sizeof(*st));
        return FROM_STATE;
    }
    uint32_t cp;
    size_t r = decode(&cp, s, n, st);
    if (r > 4 || !s)
        return r;
    if (cp >= 0x10000) {
        cp -= 0x10000;
        if (c)
            *c = (char16_t)(0xD800 | cp >> 10);
        st->need = LOW_NEXT << 4;
        st->bits = 0xDC00 | (cp & 0x3FF);
    } else if (c) {
        *c = (char16_t)cp;
    }
    return r;
}

size_t c16rtomb(char *s, char16_t c, mbstate_t *state)
{
    mbstate_t *st = state ? state : &own_c16r;
    char scratch[4];
    if (!s)
        s = scratch, c = 0;
    uint32_t cp = c;
    if (KIND(st) == HIGH_KEPT) {
        if (c < 0xDC00 || c > 0xDFFF)
            return invalid(st);                         /* (a high half not followed by a low) */
        cp = 0x10000 + ((st->bits - 0xD800) << 10) + (c - 0xDC00u);
    } else if (c >= 0xD800 && c <= 0xDBFF) {
        st->need = HIGH_KEPT << 4;
        st->bits = c;
        return 0;
    } else if (c >= 0xDC00 && c <= 0xDFFF) {
        return invalid(st);                             /* (a low half alone) */
    }
    memset(st, 0, sizeof(*st));
    return encode(s, cp);
}

size_t mbrtoc8(char8_t *c, const char *s, size_t n, mbstate_t *state)
{
    mbstate_t *st = state ? state : &own_c8;
    if (KIND(st) == BYTES_NEXT) {
        unsigned left = st->need & 15;
        if (c)
            *c = (char8_t)(st->bits & 0xFF);
        st->bits >>= 8;
        st->need = --left ? (BYTES_NEXT << 4 | left) : 0;
        return FROM_STATE;
    }
    uint32_t cp;
    size_t r = decode(&cp, s, n, st);
    if (r > 4 || !s)
        return r;
    char bytes[4];
    unsigned len = encode(bytes, cp);
    if (c)
        *c = (char8_t)bytes[0];
    if (len > 1) {                                      /* the rest, from the state */
        st->need = BYTES_NEXT << 4 | (len - 1);
        st->bits = 0;
        for (unsigned i = len - 1; i >= 1; i--)
            st->bits = st->bits << 8 | (unsigned char)bytes[i];
    }
    return r;
}

size_t c8rtomb(char *s, char8_t c, mbstate_t *state)
{
    mbstate_t *st = state ? state : &own_c8r;
    if (!s) {
        memset(st, 0, sizeof(*st));
        return 1;
    }
    char byte = (char)c;
    uint32_t cp;
    size_t r = decode(&cp, &byte, 1, st);
    if (r == INCOMPLETE)
        return 0;                                       /* (kept until the character is whole) */
    if (r == INVALID)
        return INVALID;
    return encode(s, cp);
}

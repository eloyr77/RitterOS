/*
 * (43a-2) iconv: text from one encoding to another through Unicode code
 * points, a character at a time. The encodings: UTF-8; UTF-16 and
 * UTF-32 (and UCS-2, UCS-4) little or big endian, or with a byte order
 * mark (read: the mark's order, big endian without one; written: a mark,
 * then the machine's order, little endian, as glibc does); WCHAR_T (the
 * machine's wchar_t, UCS-4 here); ASCII; ISO-8859-1 (Latin-1); and
 * Windows-1252. Names are compared without case, '-' and '_' ("utf8",
 * "UTF-8", "latin1", "CP1252"...). "//TRANSLIT" after the target writes
 * '?' for a character it does not have, "//IGNORE" leaves such characters
 * (and bad input) out; without them iconv stops there with EILSEQ.
 */
#include <errno.h>
#include <iconv.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum kind { UTF8, UTF16, UTF16LE, UTF16BE, UCS2, UCS2LE, UCS2BE, UTF32, UTF32LE, UTF32BE, ASCII,
            LATIN1, CP1252 };

struct ritteros_iconv {
    enum kind from, to;
    bool translit, ignore;
    bool little_in;                     /* UTF-16/UTF-32 read: the order its mark gave */
    bool mark_read, mark_written;
};

static const struct { const char *name; enum kind kind; } names[] = {
    { "UTF8", UTF8 }, { "UTF16", UTF16 }, { "UTF16LE", UTF16LE }, { "UTF16BE", UTF16BE },
    { "UCS2", UCS2 }, { "UCS2LE", UCS2LE }, { "UCS2BE", UCS2BE }, { "UCS2INTERNAL", UCS2LE },
    { "UTF32", UTF32 }, { "UTF32LE", UTF32LE }, { "UTF32BE", UTF32BE }, { "UCS4", UTF32BE },
    { "UCS4LE", UTF32LE }, { "UCS4BE", UTF32BE }, { "WCHART", UTF32LE }, { "UCS4INTERNAL", UTF32LE },
    { "ASCII", ASCII }, { "USASCII", ASCII }, { "ANSIX3.41968", ASCII }, { "646", ASCII },
    { "ISO88591", LATIN1 }, { "LATIN1", LATIN1 }, { "L1", LATIN1 }, { "ISO885911987", LATIN1 },
    { "CP819", LATIN1 }, { "WINDOWS1252", CP1252 }, { "CP1252", CP1252 },
};

/* name to its kind, and its "//" options; false for an unknown one */
static bool parse(const char *name, enum kind *kind, bool *translit, bool *ignore)
{
    char plain[32];
    size_t n = 0;
    const char *options = strstr(name, "//");
    size_t end = options ? (size_t)(options - name) : strlen(name);
    for (size_t i = 0; i < end; i++) {
        char c = name[i];
        if (c == '-' || c == '_')
            continue;
        if (n + 1 >= sizeof(plain))
            return false;
        plain[n++] = c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
    }
    plain[n] = 0;
    if (translit) {
        *translit = options && strstr(options, "TRANSLIT");
        *ignore = options && strstr(options, "IGNORE");
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(plain, names[i].name)) {
            *kind = names[i].kind;
            return true;
        }
    return false;
}

iconv_t iconv_open(const char *to, const char *from)
{
    struct ritteros_iconv *cd = calloc(1, sizeof(*cd));
    bool unused1, unused2;
    if (!cd) {
        errno = ENOMEM;
        return (iconv_t)-1;
    }
    if (!parse(to, &cd->to, &cd->translit, &cd->ignore) || !parse(from, &cd->from, &unused1, &unused2)) {
        free(cd);
        errno = EINVAL;
        return (iconv_t)-1;
    }
    return cd;
}

int iconv_close(iconv_t cd)
{
    free(cd);
    return 0;
}

static const uint16_t cp1252_high[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039,
    0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC,
    0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

static uint32_t get16(const unsigned char *p, bool little)
{
    return little ? (uint32_t)(p[0] | p[1] << 8) : (uint32_t)(p[0] << 8 | p[1]);
}

static uint32_t get32(const unsigned char *p, bool little)
{
    return little ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24
                  : (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* The next character of p (n bytes) into *cp: its bytes, 0 when n ends
 * inside it, -1 when no character has these bytes. A byte order mark at
 * the start of UTF-16 or UTF-32 is read (its bytes, *cp = -1: nothing). */
static long decode(struct ritteros_iconv *cd, const unsigned char *p, size_t n, int32_t *cp)
{
    switch (cd->from) {
    case UTF8: {
        if (p[0] < 0x80) {
            *cp = p[0];
            return 1;
        }
        int need = p[0] >= 0xC2 && p[0] <= 0xDF ? 1 : p[0] >= 0xE0 && p[0] <= 0xEF ? 2
                 : p[0] >= 0xF0 && p[0] <= 0xF4 ? 3 : -1;
        if (need < 0)
            return -1;
        uint32_t v = p[0] & (0x3F >> need);
        for (int i = 1; i <= need; i++) {
            if ((size_t)i >= n)
                return 0;
            if ((p[i] & 0xC0) != 0x80)
                return -1;
            v = v << 6 | (p[i] & 0x3F);
        }
        static const uint32_t least[4] = { 0, 0x80, 0x800, 0x10000 };
        if (v < least[need] || (v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF)
            return -1;
        *cp = (int32_t)v;
        return need + 1;
    }
    case UTF16: case UTF16LE: case UTF16BE: case UCS2: case UCS2LE: case UCS2BE: {
        if (n < 2)
            return 0;
        bool little = cd->from == UTF16LE || cd->from == UCS2LE ||
                      ((cd->from == UTF16 || cd->from == UCS2) && cd->little_in);
        uint32_t u = get16(p, little);
        if ((cd->from == UTF16 || cd->from == UCS2) && !cd->mark_read) {
            cd->mark_read = true;
            if (u == 0xFEFF || u == 0xFFFE) {
                cd->little_in = (u == 0xFFFE) != little;
                *cp = -1;
                return 2;
            }
        }
        if (u >= 0xDC00 && u <= 0xDFFF)
            return -1;
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (cd->from == UCS2 || cd->from == UCS2LE || cd->from == UCS2BE)
                return -1;
            if (n < 4)
                return 0;
            uint32_t low = get16(p + 2, little);
            if (low < 0xDC00 || low > 0xDFFF)
                return -1;
            *cp = (int32_t)(0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00));
            return 4;
        }
        *cp = (int32_t)u;
        return 2;
    }
    case UTF32: case UTF32LE: case UTF32BE: {
        if (n < 4)
            return 0;
        bool little = cd->from == UTF32LE || (cd->from == UTF32 && cd->little_in);
        uint32_t u = get32(p, little);
        if (cd->from == UTF32 && !cd->mark_read) {
            cd->mark_read = true;
            if (u == 0xFEFF || u == 0xFFFE0000) {
                cd->little_in = u == 0xFFFE0000;
                *cp = -1;
                return 4;
            }
        }
        if ((u >= 0xD800 && u <= 0xDFFF) || u > 0x10FFFF)
            return -1;
        *cp = (int32_t)u;
        return 4;
    }
    case ASCII:
        if (p[0] > 0x7F)
            return -1;
        *cp = p[0];
        return 1;
    case LATIN1:
        *cp = p[0];
        return 1;
    case CP1252:
        if (p[0] >= 0x80 && p[0] < 0xA0) {
            if (!cp1252_high[p[0] - 0x80])
                return -1;
            *cp = cp1252_high[p[0] - 0x80];
        } else {
            *cp = p[0];
        }
        return 1;
    }
    return -1;
}

static void put16(unsigned char *p, uint32_t v, bool little)
{
    p[little ? 0 : 1] = (unsigned char)v;
    p[little ? 1 : 0] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, uint32_t v, bool little)
{
    for (int i = 0; i < 4; i++)
        p[little ? i : 3 - i] = (unsigned char)(v >> (8 * i));
}

/* cp into out (room bytes): its bytes, 0 when room is short, -1 when the
 * encoding has no such character. */
static long encode(struct ritteros_iconv *cd, uint32_t cp, unsigned char *out, size_t room)
{
    size_t mark = 0;
    if ((cd->to == UTF16 || cd->to == UCS2 || cd->to == UTF32) && !cd->mark_written)
        mark = cd->to == UTF32 ? 4 : 2;
    unsigned char tmp[8];
    long n;
    switch (cd->to) {
    case UTF8:
        if (cp < 0x80)
            tmp[0] = (unsigned char)cp, n = 1;
        else if (cp < 0x800)
            tmp[0] = (unsigned char)(0xC0 | cp >> 6), tmp[1] = (unsigned char)(0x80 | (cp & 0x3F)), n = 2;
        else if (cp < 0x10000) {
            tmp[0] = (unsigned char)(0xE0 | cp >> 12), tmp[1] = (unsigned char)(0x80 | (cp >> 6 & 0x3F));
            tmp[2] = (unsigned char)(0x80 | (cp & 0x3F)), n = 3;
        } else {
            tmp[0] = (unsigned char)(0xF0 | cp >> 18), tmp[1] = (unsigned char)(0x80 | (cp >> 12 & 0x3F));
            tmp[2] = (unsigned char)(0x80 | (cp >> 6 & 0x3F)), tmp[3] = (unsigned char)(0x80 | (cp & 0x3F));
            n = 4;
        }
        break;
    case UTF16: case UTF16LE: case UTF16BE: case UCS2: case UCS2LE: case UCS2BE: {
        bool little = cd->to != UTF16BE && cd->to != UCS2BE;
        if (cp >= 0x10000) {
            if (cd->to == UCS2 || cd->to == UCS2LE || cd->to == UCS2BE)
                return -1;
            put16(tmp, 0xD800 + ((cp - 0x10000) >> 10), little);
            put16(tmp + 2, 0xDC00 + ((cp - 0x10000) & 0x3FF), little);
            n = 4;
        } else {
            put16(tmp, cp, little);
            n = 2;
        }
        break;
    }
    case UTF32: case UTF32LE: case UTF32BE:
        put32(tmp, cp, cd->to != UTF32BE);
        n = 4;
        break;
    case ASCII:
        if (cp > 0x7F)
            return -1;
        tmp[0] = (unsigned char)cp, n = 1;
        break;
    case LATIN1:
        if (cp > 0xFF)
            return -1;
        tmp[0] = (unsigned char)cp, n = 1;
        break;
    case CP1252:
        n = -1;
        if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF))
            tmp[0] = (unsigned char)cp, n = 1;
        else
            for (int i = 0; i < 32; i++)
                if (cp1252_high[i] == cp)
                    tmp[0] = (unsigned char)(0x80 + i), n = 1;
        if (n < 0)
            return -1;
        break;
    default:
        return -1;
    }
    if ((size_t)n + mark > room)
        return 0;
    if (mark) {
        if (mark == 4)
            put32(out, 0xFEFF, true);
        else
            put16(out, 0xFEFF, true);
        cd->mark_written = true;
    }
    memcpy(out + mark, tmp, (size_t)n);
    return n + (long)mark;
}

size_t iconv(iconv_t cd, char **in, size_t *in_left, char **out, size_t *out_left)
{
    if (cd == (iconv_t)-1 || !cd) {
        errno = EBADF;
        return (size_t)-1;
    }
    if (!in || !*in) {                          /* back to the initial state */
        cd->mark_read = false;
        cd->little_in = false;
        return 0;
    }
    size_t irreversible = 0;
    bool skipped = false;
    while (*in_left > 0) {
        int32_t cp;
        long used = decode(cd, (const unsigned char *)*in, *in_left, &cp);
        if (used == 0) {
            errno = EINVAL;                     /* (the input ends inside a character) */
            return (size_t)-1;
        }
        if (used < 0) {
            if (!cd->ignore) {
                errno = EILSEQ;
                return (size_t)-1;
            }
            *in += 1, *in_left -= 1;
            skipped = true;
            continue;
        }
        if (cp < 0) {                           /* (a byte order mark read) */
            *in += used, *in_left -= (size_t)used;
            continue;
        }
        if (!out || !*out) {
            errno = E2BIG;
            return (size_t)-1;
        }
        long wrote = encode(cd, (uint32_t)cp, (unsigned char *)*out, *out_left);
        if (wrote < 0) {
            if (cd->translit)
                wrote = encode(cd, '?', (unsigned char *)*out, *out_left), irreversible++;
            else if (cd->ignore) {
                *in += used, *in_left -= (size_t)used;
                skipped = true;
                continue;
            } else {
                errno = EILSEQ;
                return (size_t)-1;
            }
        }
        if (wrote == 0) {
            errno = E2BIG;
            return (size_t)-1;
        }
        *in += used, *in_left -= (size_t)used;
        *out += wrote, *out_left -= (size_t)wrote;
    }
    if (skipped) {
        errno = EILSEQ;                         /* (as glibc's //IGNORE says at the end) */
        return (size_t)-1;
    }
    return irreversible;
}

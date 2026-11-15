/*
 * wctype.h (step 22v): the classes of wide characters, ctype.h's for
 * ASCII. In the "C" locale nothing above 127 is in a class; (43a-2) in
 * C.UTF-8 the characters above are Unicode's (ctypetab.h, glibc's
 * C.UTF-8 classes), and so are towupper and towlower. Also wcwidth and
 * wcswidth (wchar.h): a character's columns on a terminal.
 */
#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "ctypetab.h"

#include "mbconv.h"

static const char *const class_names[] = {
    0, "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct",
    "space", "upper", "xdigit",
};
static int (*const classes[])(int) = {
    0, isalnum, isalpha, isblank, iscntrl, isdigit, isgraph, islower, isprint, ispunct,
    isspace, isupper, isxdigit,
};
#define CLASSES (sizeof(classes) / sizeof(classes[0]))
/* the table's bits for each class above ASCII (digit, xdigit: none) */
static const uint16_t class_bits[] = {
    0, CT_ALPHA, CT_ALPHA, CT_BLANK, CT_CNTRL, 0, CT_GRAPH, CT_LOWER, CT_PRINT, CT_PUNCT,
    CT_SPACE, CT_UPPER, 0,
};

static int ascii(wint_t wc)
{
    return wc >= 0 && wc < 128;
}

/* A character's bits from the table (0 past it, or outside UTF-8). */
static unsigned bits_of(wint_t wc)
{
    if (wc < 128 || wc > 0x10FFFF || !mb_utf8())
        return 0;
    size_t lo = 0, hi = sizeof(ct_ranges) / sizeof(ct_ranges[0]);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if ((uint32_t)wc < ct_ranges[mid].first)
            hi = mid;
        else if ((uint32_t)wc > ct_ranges[mid].last)
            lo = mid + 1;
        else
            return ct_ranges[mid].bits;
    }
    return 0;
}

int iswctype(wint_t wc, wctype_t type)
{
    if (!type || type >= CLASSES)
        return 0;
    if (ascii(wc))
        return classes[type]((int)wc) != 0;
    return (bits_of(wc) & class_bits[type]) != 0;
}

wctype_t wctype(const char *name)
{
    for (wctype_t i = 1; i < CLASSES; i++)
        if (!strcmp(name, class_names[i]))
            return i;
    return 0;
}

int iswalnum(wint_t wc) { return iswctype(wc, 1); }
int iswalpha(wint_t wc) { return iswctype(wc, 2); }
int iswblank(wint_t wc) { return iswctype(wc, 3); }
int iswcntrl(wint_t wc) { return iswctype(wc, 4); }
int iswdigit(wint_t wc) { return ascii(wc) && isdigit((int)wc); }
int iswgraph(wint_t wc) { return iswctype(wc, 6); }
int iswlower(wint_t wc) { return iswctype(wc, 7); }
int iswprint(wint_t wc) { return iswctype(wc, 8); }
int iswpunct(wint_t wc) { return iswctype(wc, 9); }
int iswspace(wint_t wc) { return iswctype(wc, 10); }
int iswupper(wint_t wc) { return iswctype(wc, 11); }
int iswxdigit(wint_t wc) { return ascii(wc) && isxdigit((int)wc); }

/* The case of wc by a table of runs (first..last, every step-th, + delta). */
#define CASE_LOOKUP(table, wc)                                                          \
    do {                                                                                \
        size_t lo = 0, hi = sizeof(table) / sizeof(table[0]);                           \
        while (lo < hi) {                                                               \
            size_t mid = (lo + hi) / 2;                                                 \
            if ((uint32_t)(wc) < table[mid].first)                                      \
                hi = mid;                                                               \
            else if ((uint32_t)(wc) > table[mid].last)                                  \
                lo = mid + 1;                                                           \
            else                                                                        \
                return ((uint32_t)(wc) - table[mid].first) % table[mid].step ? (wc)     \
                       : (wint_t)((int32_t)(wc) + table[mid].delta);                    \
        }                                                                               \
        return wc;                                                                      \
    } while (0)

wint_t towlower(wint_t wc)
{
    if (ascii(wc))
        return wc >= 'A' && wc <= 'Z' ? wc + 32 : wc;
    if (wc > 0x10FFFF || !mb_utf8())
        return wc;
    CASE_LOOKUP(ct_lower, wc);
}

wint_t towupper(wint_t wc)
{
    if (ascii(wc))
        return wc >= 'a' && wc <= 'z' ? wc - 32 : wc;
    if (wc > 0x10FFFF || !mb_utf8())
        return wc;
    CASE_LOOKUP(ct_upper, wc);
}

wctrans_t wctrans(const char *name)
{
    return !strcmp(name, "tolower") ? 1 : !strcmp(name, "toupper") ? 2 : 0;
}

wint_t towctrans(wint_t wc, wctrans_t mapping)
{
    return mapping == 1 ? towlower(wc) : mapping == 2 ? towupper(wc) : wc;
}

/* (43a-2) Columns: 0 for NUL, -1 for controls (and in the C locale for
 * anything past ASCII that is not printable there); else the table's. */
int wcwidth(wchar_t wc)
{
    if (wc == 0)
        return 0;
    if (wc < 128 && wc >= 0)
        return wc < 32 || wc == 127 ? -1 : 1;
    if (!mb_utf8())
        return -1;
    if ((uint32_t)wc > 0x10FFFF)
        return -1;
    static const int widths[4] = { 1, 0, 2, -1 };
    unsigned b = bits_of((wint_t)wc);
    return b ? widths[CT_WIDTH(b)] : -1;
}

int wcswidth(const wchar_t *s, size_t n)
{
    int total = 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        int w = wcwidth(s[i]);
        if (w < 0)
            return -1;
        total += w;
    }
    return total;
}

/*
 * Test: uchar.h (22w), UTF-8 always. Prints the UTF-8 bytes and UTF-16
 * units of chosen code points (the test compares them with Python's own
 * encoders), checks invalid UTF-8 against the standard's rules, and every
 * code point there and back through each pair of calls, whole and a byte
 * at a time. Also wide characters above 255 in swprintf and swscanf (the
 * Mac's C library, libctest's reference, fails on those).
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <uchar.h>
#include <wchar.h>

#define T "uchartest: "

static int failures;

static void ok(int good, const char *what)
{
    if (!good) {
        printf(T "FAIL %s\n", what);
        failures++;
    }
}

static const unsigned points[] = { 0x24, 0x7F, 0x80, 0xA2, 0xE9, 0x7FF, 0x800, 0x939, 0x20AC,
                                   0xD55C, 0xD7FF, 0xE000, 0xFFFD, 0xFFFF, 0x10000, 0x10348,
                                   0x1F600, 0x10FFFF };

static void show(void)
{
    for (size_t k = 0; k < sizeof(points) / sizeof(points[0]); k++) {
        char out[4];
        mbstate_t st = { 0 };
        size_t len = c32rtomb(out, points[k], &st);
        printf(T "U+%04X utf8", points[k]);
        for (size_t i = 0; i < len; i++)
            printf(" %02X", (unsigned char)out[i]);
        printf(" utf16");
        char16_t u;
        mbstate_t st16 = { 0 };
        size_t r = mbrtoc16(&u, out, len, &st16);
        printf(" %04X", (unsigned)u);
        if (mbrtoc16(&u, out + r, 0, &st16) == (size_t)-3)
            printf(" %04X", (unsigned)u);
        printf("\n");
    }
}

/* Invalid UTF-8 is EILSEQ wherever it is seen; a cut character INCOMPLETE. */
static void invalid(void)
{
    static const struct { const char *bytes; size_t n; size_t want; const char *what; } cases[] = {
        { "\x80", 1, (size_t)-1, "a continuation byte alone" },
        { "\xC0\x80", 2, (size_t)-1, "overlong NUL" },
        { "\xC1\xBF", 2, (size_t)-1, "overlong 2-byte" },
        { "\xE0\x80\x80", 3, (size_t)-1, "overlong 3-byte" },
        { "\xE0\x9F\xBF", 3, (size_t)-1, "overlong U+07FF" },
        { "\xF0\x8F\xBF\xBF", 4, (size_t)-1, "overlong U+FFFF" },
        { "\xED\xA0\x80", 3, (size_t)-1, "surrogate D800" },
        { "\xED\xBF\xBF", 3, (size_t)-1, "surrogate DFFF" },
        { "\xF4\x90\x80\x80", 4, (size_t)-1, "above U+10FFFF" },
        { "\xF5\x80\x80\x80", 4, (size_t)-1, "lead F5" },
        { "\xFF", 1, (size_t)-1, "byte FF" },
        { "\xE2\x28\xA1", 3, (size_t)-1, "a second byte out of place" },
        { "\xE2\x82", 2, (size_t)-2, "a cut character" },
        { "\xF0\x9F\x98", 3, (size_t)-2, "a cut 4-byte character" },
        { "\xE0", 1, (size_t)-2, "a lead alone" },
    };
    int good = 0;
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        mbstate_t st = { 0 };
        char32_t c;
        errno = 0;
        size_t r = mbrtoc32(&c, cases[k].bytes, cases[k].n, &st);
        int right = r == cases[k].want && (r != (size_t)-1 || errno == EILSEQ);
        ok(right, cases[k].what);
        good += right;
    }
    char out[8];
    mbstate_t st = { 0 };
    size_t s1 = c32rtomb(out, 0xD800, &st), s2 = c32rtomb(out, 0x110000, &st);
    size_t l1 = c16rtomb(out, 0xDC00, &st);             /* a low half alone */
    memset(&st, 0, sizeof(st));
    size_t h1 = c16rtomb(out, 0xD83D, &st), h2 = c16rtomb(out, 0x41, &st);
    int refused = s1 == (size_t)-1 && s2 == (size_t)-1 && l1 == (size_t)-1 && h1 == 0 &&
                  h2 == (size_t)-1;
    ok(refused, "c32rtomb/c16rtomb refuse what is not a code point");
    printf(T "invalid: %d of %d cases right, unencodable refused %d\n", good,
           (int)(sizeof(cases) / sizeof(cases[0])), refused);
}

/* Every code point: c32rtomb, then back through mbrtoc32 (whole and a
 * byte at a time), mbrtoc16 + c16rtomb, mbrtoc8 + c8rtomb. */
static void roundtrip(void)
{
    long count = 0, wrong = 0;
    for (unsigned cp = 1; cp <= 0x10FFFF; cp++) {
        if (cp >= 0xD800 && cp <= 0xDFFF)
            continue;
        count++;
        char utf8[4], again[8];
        mbstate_t st = { 0 };
        size_t len = c32rtomb(utf8, cp, &st);
        char32_t c32 = 0;
        int bad = len < 1 || len > 4 || mbrtoc32(&c32, utf8, len, &st) != len || c32 != cp;

        memset(&st, 0, sizeof(st));
        for (size_t i = 0; i < len && !bad; i++) {
            size_t r = mbrtoc32(&c32, utf8 + i, 1, &st);
            bad = i + 1 < len ? r != (size_t)-2 : r != 1 || c32 != cp;
        }

        memset(&st, 0, sizeof(st));
        mbstate_t back = { 0 };
        char16_t u;
        size_t at = 0, r = mbrtoc16(&u, utf8, len, &st);
        bad |= r != len;
        size_t w = c16rtomb(again, u, &back);
        if (cp >= 0x10000) {
            bad |= w != 0 || mbrtoc16(&u, utf8 + len, 0, &st) != (size_t)-3;
            w = c16rtomb(again, u, &back);
        }
        bad |= w != len || memcmp(again, utf8, len);

        memset(&st, 0, sizeof(st));
        memset(&back, 0, sizeof(back));
        char8_t b;
        r = mbrtoc8(&b, utf8, len, &st);
        bad |= r != len;
        for (size_t i = 0; i < len && !bad; i++) {
            if (i > 0)
                bad |= mbrtoc8(&b, utf8 + len, 0, &st) != (size_t)-3;
            w = c8rtomb(again + at, b, &back);
            if (w)
                at += w;
        }
        bad |= at != len || memcmp(again, utf8, len);
        wrong += bad;
    }
    printf(T "roundtrip %ld code points, %ld wrong\n", count, wrong);
    ok(wrong == 0, "roundtrip");
}

/* Wide characters above 255 in the wide formatting and scanning (C's own
 * rule: wide characters go in and out as they are). */
static void wide(void)
{
    wchar_t out[32];
    int n = swprintf(out, 32, L"%lc|%ls|%5ls", (wint_t)0x3B1, L"\x20AC\x1F600", L"\x3B2");
    ok(n == 10 && !wcscmp(out, L"\x3B1|\x20AC\x1F600|    \x3B2"), "swprintf above 255");
    wchar_t word[8] = L"", c = 0, set[8] = L"";
    int got = swscanf(L"\x3B1\x3B2 \x20AC \x3B3\x3B3x", L"%ls %lc %l[\x3B0-\x3BF]", word, &c, set);
    ok(got == 3 && !wcscmp(word, L"\x3B1\x3B2") && c == 0x20AC && !wcscmp(set, L"\x3B3\x3B3"),
       "swscanf above 255");
    char bytes[8];
    errno = 0;
    int p = snprintf(bytes, sizeof(bytes), "%ls", L"\x3B1");
    ok(p == -1 && errno == EILSEQ, "printf's %ls above 255: EILSEQ");
    printf(T "wide above 255: swprintf %d, swscanf %d, printf %d\n", n, got, p);
}

int main(void)
{
    show();
    invalid();
    roundtrip();
    wide();
    printf(T "%s\n", failures ? "FAILED" : "all ok");
    return failures != 0;
}

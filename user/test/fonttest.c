/*
 * Test: fonts from /system/fonts (step 22c). Every font file there opens
 * with the cell its name says (WxH, a B for bold); 9x15 and 9x15B read from
 * their files draw exactly like the compiled-in faces, and the file has the
 * glyphs the compiled tables leave out (Greek Omega); a bold face takes the
 * glyphs it lacks from its regular face (9x18B has no U+0460, 9x18 has); a
 * missing font, a broken file and a name with a path give 0, so a program
 * falls back to the default. (27k) A face not named by its cell (Adobe's
 * helvR12, ...) is checked against its own file, read here: its line is
 * FONT_ASCENT + FONT_DESCENT high, "i" and "W" advance by their DWIDTHs,
 * and a W draws. (41c-3) Scalable fonts (wfont_open): FreeSans.ttf at 17
 * pixels by its bare name and its path is one font; its line and widest
 * advance (printed: the test computes them from the file's hhea); its text
 * width is the sum of its advances, rounded once; drawn white on black,
 * every pixel is the reader's mask of each glyph at its pen's quarter
 * pixel (the reader, user/lib/ttf.c, is built in here); a background fills
 * each advance's cell; an .otf face draws; a missing file, a bitmap font,
 * a bad size and a path with .. give 0. (41d) Lines wrapped (9x15's exact
 * cases; FreeSans: each line fits, the next word would not), laid out and
 * measured; size 0 is the user's text_size (`fonttest size N` after the
 * test sets it).
 */
#include <dirent.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "wfont.h"
#include "../lib/ttf.c"

#define W 1024                          /* the drawing buffer */
#define H 32

static int failures;

static void result(const char *name, int ok, const char *detail)
{
    printf("fonttest: %s %s%s%s\n", name, ok ? "ok" : "FAILED", detail[0] ? " " : "", detail);
    failures += !ok;
}

/* Draw text with a font (foreground 1 on 0); a buffer to compare. */
static uint32_t *draw(const struct font *font, const char *text)
{
    static uint32_t buffers[4][W * H];
    static int next;
    uint32_t *buf = buffers[next++ % 4];
    memset(buf, 0, sizeof(buffers[0]));
    wfont_text(buf, W, W, H, font, 0, 0, text, 1, 0);
    return buf;
}

static int same(const struct font *a, const char *ta, const struct font *b, const char *tb)
{
    return a && b && memcmp(draw(a, ta), draw(b, tb), sizeof(uint32_t) * W * H) == 0;
}

/* WxH from a font's name ("10x20", "9x18B"). */
static void cell_of(const char *name, int *w, int *h)
{
    *w = *h = 0;
    while (*name >= '0' && *name <= '9')
        *w = *w * 10 + (*name++ - '0');
    if (*name++ != 'x')
        return;
    while (*name >= '0' && *name <= '9')
        *h = *h * 10 + (*name++ - '0');
}

/* (27k) From a BDF file: FONT_ASCENT + FONT_DESCENT, and the DWIDTH of
 * the glyphs for 'i' and 'W'. */
static int bdf_facts(const char *name, int *line, int *wi, int *ww)
{
    char path[96];
    snprintf(path, sizeof(path), WFONT_DIR "/%s.bdf", name);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char text[256];
    int ascent = -1, descent = -1, enc = -1;
    *wi = *ww = -1;
    while (fgets(text, sizeof(text), f)) {
        if (!strncmp(text, "FONT_ASCENT ", 12))
            ascent = atoi(text + 12);
        else if (!strncmp(text, "FONT_DESCENT ", 13))
            descent = atoi(text + 13);
        else if (!strncmp(text, "ENCODING ", 9))
            enc = atoi(text + 9);
        else if (!strncmp(text, "DWIDTH ", 7) && (enc == 'i' || enc == 'W'))
            *(enc == 'i' ? wi : ww) = atoi(text + 7);
    }
    fclose(f);
    *line = ascent + descent;
    return ascent >= 0 && descent >= 0 && *wi > 0 && *ww > 0;
}

/* (41c-3) Scalable fonts, checked against the reader built in here. */
static void scalable_checks(void)
{
    const struct font *f = wfont_open("FreeSans.ttf", 17);
    result("FreeSans.ttf at 17 by its name and its path is one font",
           f && wfont_open(WFONT_DIR "/FreeSans.ttf", 17) == f && wfont_open("FreeSans.ttf", 18) != f, "");
    if (!f)
        return;
    printf("fonttest: FreeSans.ttf at 17: line %d, widest %d\n", wfont_height(f), wfont_width(f));
    FILE *file = fopen(WFONT_DIR "/FreeSans.ttf", "rb");
    static uint8_t data[2 << 20];
    size_t size = file ? fread(data, 1, sizeof(data), file) : 0;
    if (file)
        fclose(file);
    struct ttf t;
    if (!ttf_open(&t, data, size)) {
        result("the reader reads FreeSans.ttf", 0, "");
        return;
    }
    float scale = 17.0f / (float)t.upem, pen = 0;
    const char *text = "Hamburgefonstiv Ag";
    for (const char *c = text; *c; c++)
        pen += (float)ttf_advance(&t, ttf_glyph(&t, (uint8_t)*c)) * scale;
    char detail[48];
    snprintf(detail, sizeof(detail), "(%d)", wfont_text_width(f, text));
    result("its text width is the sum of its advances", wfont_text_width(f, text) == (int)floorf(pen + 0.5f), detail);

    /* "Ag" white on black at (3, 2): each pixel the masks' coverage */
    static uint32_t buf[W * H], want[W * H];
    memset(buf, 0, sizeof(buf));
    memset(want, 0, sizeof(want));
    int end = wfont_text(buf, W, W, H, f, 3, 2, "Ag", 0xFFFFFFFFu, WFONT_TRANSPARENT);
    int ascent = (int)floorf((float)t.ascender * scale + 0.5f);
    pen = 3;
    for (const char *c = "Ag"; *c; c++) {
        int g = ttf_glyph(&t, (uint8_t)*c), sub = (int)((pen - floorf(pen)) * 4);
        struct ttf_mask m;
        if (ttf_mask(&t, g, scale, (float)sub / 4, &m))
            for (int r = 0; r < m.h; r++)
                for (int col = 0; col < m.w; col++) {
                    int x = (int)floorf(pen) + m.left + col, y = 2 + ascent + m.top + r;
                    uint32_t a = m.bytes[r * m.w + col];
                    if (x >= 0 && x < W && y >= 0 && y < H && a) {
                        uint32_t old = want[y * W + x] & 255, v = (255 * a + old * (255 - a) + 127) / 255;
                        want[y * W + x] = v * 0x01010101u;
                    }
                }
        free(m.bytes);
        pen += (float)ttf_advance(&t, g) * scale;
    }
    int lit = 0, part = 0;
    for (int i = 0; i < W * H; i++)
        lit += buf[i] == 0xFFFFFFFFu, part += buf[i] && buf[i] != 0xFFFFFFFFu;
    snprintf(detail, sizeof(detail), "(%d full, %d partly)", lit, part);
    result("\"Ag\" drawn as its glyphs' masks", !memcmp(buf, want, sizeof(buf)) && lit > 0 && part > 0 &&
           end == (int)floorf(pen + 0.5f), detail);

    /* a background: each advance's cell, the line's height */
    memset(buf, 0, sizeof(buf));
    wfont_text(buf, W, W, H, f, 0, 0, "  ", 1, 2);
    int filled = 0;
    for (int i = 0; i < W * H; i++)
        filled += buf[i] == 2;
    int adv2 = (int)floorf(2 * (float)ttf_advance(&t, ttf_glyph(&t, ' ')) * scale + 0.5f);
    snprintf(detail, sizeof(detail), "(%d pixels)", filled);
    result("a background fills the advances' cells", filled == adv2 * wfont_height(f), detail);

    const struct font *o = wfont_open("FreeSerif.otf", 31);
    memset(buf, 0, sizeof(buf));
    lit = 0;
    if (o) {
        wfont_text(buf, W, W, H, o, 0, 0, "Wg", 0xFFFFFFFFu, WFONT_TRANSPARENT);
        for (int i = 0; i < W * H; i++)
            lit += buf[i] == 0xFFFFFFFFu;
    }
    result("FreeSerif.otf at 31 draws", o && lit > 0, "");
    result("a missing file, a bitmap font, a bad size or a path with .. gives 0",
           !wfont_open("nosuch.ttf", 17) && !wfont_open("9x15.bdf", 17) && !wfont_open("FreeSans.ttf", -1) &&
           !wfont_open("FreeSans.ttf", 2000) && !wfont_open("../fonts/FreeSans.ttf", 17) && !wfont_open(0, 17), "");
}

/* (41d) Text over several lines: with 9x15 (9-pixel cells) every answer is
 * exact; with FreeSans each line fits and the next word would not. */
static void wrap_checks(void)
{
    const struct font *f = wfont_regular();
    static const struct { const char *text; int width, bytes; } cases[] = {
        { "hello world foo", 99, 12 },                  /* (11 cells fit; the space hangs) */
        { "hello world foo", 98, 6 },                   /* (a pixel less: after "hello ") */
        { "abcdefghij", 45, 5 },                        /* (a word wider than a line: cut) */
        { "ab\ncd", 1000, 3 },                          /* (a newline ends a line, counted) */
        { "x", 0, 1 },                                  /* (at least a character) */
        { "\xC3\xA4\xC3\xB6 \xC3\xBC\xC3\x9F", 27, 5 },     /* (UTF-8: "äö ") */
        { "", 100, 0 },
    };
    int ok = 1;
    char detail[64] = "";
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int got = wfont_wrap(f, cases[i].text, cases[i].width);
        if (got != cases[i].bytes && ok) {
            snprintf(detail, sizeof(detail), "(case %u: %d bytes, not %d)", i, got, cases[i].bytes);
            ok = 0;
        }
    }
    result("lines wrap where they should", ok, detail);

    /* laid out: measured, and drawn as wfont_text draws each line */
    const char *para = "hello world foo bar baz";
    static uint32_t a[W * H], b[W * H];
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    int height = wfont_layout(a, W, W, H, f, 1, 0, 99, para, 1, WFONT_TRANSPARENT);
    wfont_text(b, W, W, H, f, 1, 0, "hello world ", 1, WFONT_TRANSPARENT);
    wfont_text(b, W, W, H, f, 1, 15, "foo bar baz", 1, WFONT_TRANSPARENT);
    snprintf(detail, sizeof(detail), "(%d pixels high)", height);
    result("text laid out in two lines, measured and drawn",
           height == 30 && wfont_layout(0, 0, 0, 0, f, 0, 0, 99, para, 0, 0) == 30 && !memcmp(a, b, sizeof(a)),
           detail);

    /* FreeSans at 17: every line fits in 120 pixels, and its next word would not */
    const struct font *s = wfont_open("FreeSans.ttf", 17);
    const char *text = "The quick brown fox jumps over the lazy dog, and the dog sleeps on in the sun.";
    int lines = 0;
    ok = s != 0;
    for (const char *p = text; s && *p; lines++) {
        int n = wfont_wrap(s, p, 120);
        char line[128], more[128];
        int shown = n;
        while (shown > 0 && p[shown - 1] == ' ')
            shown--;
        snprintf(line, sizeof(line), "%.*s", shown, p);
        const char *next = p + n;
        int word = 0;
        while (next[word] && next[word] != ' ')
            word++;
        snprintf(more, sizeof(more), "%.*s", (int)(next - p) + word, p);
        if (n <= 0 || wfont_text_width(s, line) > 120 || (*next && wfont_text_width(s, more) <= 120))
            ok = 0;
        p += n > 0 ? n : 1;
    }
    snprintf(detail, sizeof(detail), "(%d lines)", lines);
    result("FreeSans: each line fits and the next word would not", ok && lines > 2 &&
           wfont_layout(0, 0, 0, 0, s, 0, 0, 120, text, 0, 0) == lines * wfont_height(s), detail);

    /* the user's size: 15 with no text_size */
    result("size 0 is the user's text size (15 if not set)",
           wfont_open("FreeSans.ttf", 0) == wfont_open("FreeSans.ttf", 15), "");
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "size")) {        /* (41d) `fonttest size N`: the user's size is N */
        int n = atoi(argv[2]);
        printf("fonttest: the user's text size is %d %s\n", n,
               n && wfont_open("FreeSans.ttf", 0) == wfont_open("FreeSans.ttf", n) ? "ok" : "FAILED");
        return 0;
    }
    int fonts = 0;
    DIR *dir = opendir(WFONT_DIR);
    for (struct dirent *e; dir && (e = readdir(dir));) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".bdf") != 0)
            continue;
        char name[32], detail[48];
        snprintf(name, sizeof(name), "%.*s", (int)(n - 4), e->d_name);
        int w, h;
        cell_of(name, &w, &h);
        const struct font *f = wfont_find(name);
        if (w == 0) {                           /* (27k) checked against its file */
            int line, wi, ww, drawn = 0;
            bool facts = bdf_facts(name, &line, &wi, &ww);
            if (f) {
                uint32_t *buf = draw(f, "W");
                for (int i = 0; i < W * H; i++)
                    drawn += buf[i] == 1;
            }
            snprintf(detail, sizeof(detail), "(line %d, i %d, W %d, %d pixels)", f ? wfont_height(f) : 0,
                     f ? wfont_text_width(f, "i") : 0, f ? wfont_text_width(f, "W") : 0, drawn);
            result(name, f && facts && wfont_height(f) == line && wfont_text_width(f, "i") == wi &&
                         wfont_text_width(f, "W") == ww && drawn > 0, detail);
            fonts++;
            continue;
        }
        snprintf(detail, sizeof(detail), "(%dx%d)", f ? wfont_width(f) : 0, f ? wfont_height(f) : 0);
        result(name, f && wfont_width(f) == w && wfont_height(f) == h, detail);
        fonts++;
    }
    if (dir)
        closedir(dir);
    char count[32];
    snprintf(count, sizeof(count), "(%d)", fonts);
    result("fonts in " WFONT_DIR, fonts > 0, count);

    /* The compiled tables' characters: ASCII, Latin-1, punctuation, the euro,
     * arrows, box drawing, blocks, check marks. */
    const char *sample = "RitterOS: The quick brown fox! \xC3\xA4\xC3\xB6\xC3\xBC\xC3\x9F \xE2\x82\xAC "
                         "\xE2\x86\x90\xE2\x86\x92 \xE2\x94\x9C\xE2\x94\x80\xE2\x94\xA4 "
                         "\xE2\x96\x88\xE2\x96\x92 \xE2\x9C\x93 \xE2\x80\x9C\xE2\x80\x9D \xE2\x80\xA6";
    result("9x15 from its file draws as the compiled one", same(wfont_find("9x15"), sample, wfont_regular(), sample), "");
    result("9x15B from its file draws as the compiled one", same(wfont_find("9x15B"), sample, wfont_bold(), sample), "");
    const char *omega = "\xCE\xA9", *replacement = "\xEF\xBF\xBD";
    result("the file has more glyphs (Omega)",
           wfont_find("9x15") && !same(wfont_find("9x15"), omega, wfont_find("9x15"), replacement) &&
           same(wfont_regular(), omega, wfont_regular(), replacement), "");
    const char *omega_old = "\xD1\xA0";                  /* U+0460: in 9x18, not in 9x18B */
    result("a bold face borrows from its regular one",
           same(wfont_find("9x18B"), omega_old, wfont_find("9x18"), omega_old) &&
           !same(wfont_find("9x18B"), omega_old, wfont_find("9x18B"), replacement), "");
    result("text width", wfont_find("10x20") && wfont_text_width(wfont_find("10x20"), "abc\xC3\xA4") == 40 &&
           wfont_text_width(wfont_regular(), "abc") == 27, "");

    scalable_checks();
    wrap_checks();
    result("a missing font gives 0", wfont_find("nosuch") == 0, "");
    result("a name with a path gives 0", wfont_find("../fonts/9x15") == 0 && wfont_find("") == 0, "");
    int fd = open(WFONT_DIR "/broken.bdf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        const char *junk = "STARTFONT 2.1\nFONTBOUNDINGBOX 9\nCHARS 1\nSTARTCHAR x\n";
        write(fd, junk, strlen(junk));
        close(fd);
    }
    result("a broken file gives 0", fd >= 0 && wfont_find("broken") == 0, "");
    unlink(WFONT_DIR "/broken.bdf");
    printf("fonttest: %s\n", failures ? "FAILED" : "all ok");
    return failures != 0;
}

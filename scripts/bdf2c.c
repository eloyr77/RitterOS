/*
 * Convert BDF fonts into a C file of glyph tables (63; scripts/bdf2c.py
 * until then, whose output this writes byte for byte).
 *
 * bdf2c [--include=HEADER] OUTPUT.c NAME=FONT.bdf [NAME=FONT.bdf ...]
 *
 * For each font, emits `const struct font font_NAME` (see kernel/font.h)
 * with the characters in RANGES that the font has, sorted by code point.
 * Each glyph is `height` rows of uint16_t; bit 15 is the leftmost pixel.
 * Glyphs are placed in the font's cell as scripts/bdf.py places them
 * (FONTBOUNDINGBOX, BBX). Built for the machine that runs the build: the
 * Mac or RitterOS.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    uint32_t first, last;
} ranges[] = {
    { 0x0020, 0x007E },     /* printable ASCII */
    { 0x00A0, 0x00FF },     /* Latin-1 supplement: accented letters, symbols */
    { 0x0100, 0x017F },     /* Latin Extended-A: more European letters */
    { 0x2010, 0x205E },     /* general punctuation: dashes, quotes, bullet, ellipsis */
    { 0x20A0, 0x20BF },     /* currency signs: the euro */
    { 0x2100, 0x214F },     /* letterlike symbols: trade mark, numero */
    { 0x2190, 0x21FF },     /* arrows */
    { 0x2713, 0x2718 },     /* check marks and crosses */
    { 0x2500, 0x257F },     /* box drawing */
    { 0x2580, 0x259F },     /* block elements */
    { 0xFFFD, 0xFFFD },     /* replacement character for anything missing */
};

#define MAX_HEIGHT 64
#define MAX_CODE 0x110000

struct glyph {
    uint32_t rows[MAX_HEIGHT];
};

static struct glyph **glyphs;                   /* by code point; 0: none */
static int width, height, offset_y, ascent;

static _Noreturn void fail(const char *what, const char *path)
{
    fprintf(stderr, "bdf2c: %s: %s\n", path, what);
    exit(1);
}

/* a whole file as lines (each ended by its NUL) */
static char **read_lines(const char *path, int *count)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        fail("cannot read it", path);
    size_t cap = 1 << 16, len = 0;
    char *text = malloc(cap);
    for (;;) {
        if (len + 4096 > cap) {
            cap *= 2;
            text = realloc(text, cap);
        }
        if (!text)
            fail("out of memory", path);
        size_t n = fread(text + len, 1, cap - len - 1, f);
        if (n == 0)
            break;
        len += n;
    }
    fclose(f);
    text[len] = 0;
    int n = 0, room = 1024;
    char **lines = malloc(sizeof(char *) * (size_t)room);
    for (char *p = text; *p;) {
        if (n == room) {
            room *= 2;
            lines = realloc(lines, sizeof(char *) * (size_t)room);
        }
        lines[n++] = p;
        char *e = strchr(p, '\n');
        if (!e)
            break;
        if (e > p && e[-1] == '\r')
            e[-1] = 0;
        *e = 0;
        p = e + 1;
    }
    *count = n;
    return lines;
}

/* the first word of a line, and the rest after it */
static int starts(const char *line, const char *word)
{
    while (*line == ' ' || *line == '\t')
        line++;
    size_t n = strlen(word);
    return strncmp(line, word, n) == 0 && (line[n] == 0 || line[n] == ' ' || line[n] == '\t');
}

static const char *after(const char *line)
{
    while (*line == ' ' || *line == '\t')
        line++;
    while (*line && *line != ' ' && *line != '\t')
        line++;
    return line;
}

/* (bdf.py's place) a glyph's bitmap (BBX w h xoff yoff) in the font's cell */
static struct glyph *place(const uint32_t *rows, int nrows, const int bbx[4])
{
    int w = bbx[0], h = bbx[1], xoff = bbx[2], yoff = bbx[3];
    int row_bits = (w + 7) / 8 * 8;             /* BDF pads each row to whole bytes */
    int top = (height + offset_y) - (h + yoff); /* rows count downward from the cell's top */
    struct glyph *g = calloc(1, sizeof(*g));
    for (int r = 0; r < nrows; r++) {
        int y = top + r;
        if (y < 0 || y >= height)
            continue;
        for (int c = 0; c < w; c++)
            if (rows[r] >> (row_bits - 1 - c) & 1) {
                int x = xoff + c;
                if (x >= 0 && x < width)
                    g->rows[y] |= 1u << (width - 1 - x);
            }
    }
    return g;
}

static void load_bdf(const char *path)
{
    int n;
    char **lines = read_lines(path, &n);
    width = height = -1;
    offset_y = ascent = 0;
    int have_ascent = 0;
    memset(glyphs, 0, sizeof(struct glyph *) * MAX_CODE);
    for (int i = 0; i < n; i++) {
        const char *l = lines[i];
        if (starts(l, "FONTBOUNDINGBOX")) {
            int x;
            if (sscanf(after(l), "%d %d %d %d", &width, &height, &x, &offset_y) != 4)
                fail("a bad FONTBOUNDINGBOX", path);
            if (height > MAX_HEIGHT)
                fail("glyphs taller than 64 pixels are not supported", path);
        } else if (starts(l, "FONT_ASCENT")) {
            ascent = atoi(after(l));
            have_ascent = 1;
        } else if (starts(l, "STARTCHAR")) {
            long encoding = -1;
            int have_encoding = 0, have_bbx = 0, bbx[4] = { 0 }, nrows = 0;
            uint32_t rows[256];
            for (i++; i < n && !starts(lines[i], "ENDCHAR"); i++) {
                const char *w = lines[i];
                if (starts(w, "ENCODING")) {
                    encoding = atol(after(w));
                    have_encoding = 1;
                } else if (starts(w, "BBX")) {
                    if (sscanf(after(w), "%d %d %d %d", &bbx[0], &bbx[1], &bbx[2], &bbx[3]) != 4)
                        fail("a bad BBX", path);
                    have_bbx = 1;
                } else if (starts(w, "BITMAP")) {
                    for (i++; i < n && !starts(lines[i], "ENDCHAR"); i++)
                        if (nrows < 256)
                            rows[nrows++] = (uint32_t)strtoul(lines[i], 0, 16);
                    break;
                }
            }
            if (have_encoding && encoding >= 0 && encoding < MAX_CODE && have_bbx && width > 0) {
                free(glyphs[encoding]);
                glyphs[encoding] = place(rows, nrows, bbx);
            }
        }
    }
    if (width < 0 || !have_ascent)
        fail("missing FONTBOUNDINGBOX or FONT_ASCENT", path);
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static void emit_font(FILE *out, const char *name, const char *path)
{
    load_bdf(path);
    if (width > 16)
        fail("glyphs wider than 16 pixels are not supported", path);
    /* sorted (the kernel finds a glyph by binary search, kernel/font.c);
     * each range once even where ranges overlap */
    static uint32_t codes[MAX_CODE];
    static unsigned char seen[MAX_CODE];
    memset(seen, 0, sizeof(seen));
    for (size_t r = 0; r < sizeof(ranges) / sizeof(ranges[0]); r++)
        for (uint32_t c = ranges[r].first; c <= ranges[r].last; c++)
            if (glyphs[c])
                seen[c] = 1;
    int count = 0;
    for (uint32_t c = 0; c < MAX_CODE; c++)
        if (seen[c])
            codes[count++] = c;
    fprintf(out, "/* %s: %dx%d, %d glyphs */\n", base_name(path), width, height, count);
    fprintf(out, "static const uint32_t %s_codepoints[%d] = {\n", name, count);
    for (int i = 0; i < count; i += 8) {
        fprintf(out, "   ");
        for (int k = i; k < count && k < i + 8; k++)
            fprintf(out, " 0x%04X,", codes[k]);
        fprintf(out, "\n");
    }
    fprintf(out, "};\n\n");
    fprintf(out, "static const uint16_t %s_bitmaps[%d][%d] = {\n", name, count, height);
    int shift = 16 - width;                     /* the leftmost pixel to bit 15 */
    for (int i = 0; i < count; i++) {
        uint32_t c = codes[i];
        fprintf(out, "    {");
        for (int r = 0; r < height; r++)
            fprintf(out, "%s0x%04X", r ? ", " : "", glyphs[c]->rows[r] << shift);
        if (c > 0x20 && c < 0x7F && c != 0x5C)
            fprintf(out, "}, /* %c */\n", (char)c);
        else
            fprintf(out, "}, /* U+%04X */\n", c);
    }
    fprintf(out, "};\n\n");
    fprintf(out, "const struct font font_%s = {\n", name);
    fprintf(out, "    .width = %d,\n", width);
    fprintf(out, "    .height = %d,\n", height);
    fprintf(out, "    .ascent = %d,\n", ascent);
    fprintf(out, "    .count = %d,\n", count);
    fprintf(out, "    .codepoints = %s_codepoints,\n", name);
    fprintf(out, "    .bitmaps = &%s_bitmaps[0][0],\n", name);
    fprintf(out, "};\n");
}

int main(int argc, char **argv)
{
    const char *header = "font.h";
    int a = 1;
    if (a < argc && strncmp(argv[a], "--include=", 10) == 0)
        header = argv[a++] + 10;                /* which header defines struct font */
    if (argc - a < 2) {
        fprintf(stderr, "usage: bdf2c [--include=HEADER] OUTPUT.c NAME=FONT.bdf...\n");
        return 2;
    }
    glyphs = calloc(MAX_CODE, sizeof(struct glyph *));
    if (!glyphs)
        fail("out of memory", argv[a]);
    FILE *out = fopen(argv[a], "w");
    if (!out)
        fail("cannot write it", argv[a]);
    fprintf(out, "/* Generated by scripts/bdf2c.c -- do not edit. */\n#include \"%s\"\n\n", header);
    for (int i = a + 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq)
            fail("expected NAME=FONT.bdf", argv[i]);
        *eq = 0;
        emit_font(out, argv[i], eq + 1);
        if (i + 1 < argc)
            fprintf(out, "\n");
    }
    if (fclose(out) != 0)
        fail("cannot write it", argv[a]);
    return 0;
}

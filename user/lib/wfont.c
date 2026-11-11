/*
 * Text drawing for the window system (see wfont.h). Ported from the kernel's
 * font.c, but drawing into a plain 32-bit pixel buffer instead of a
 * framebuffer. The default glyph tables are the generated font data compiled
 * into the library; other fonts are read from WFONT_DIR (step 22c), mapped
 * (mmap: the file's pages come from the page cache) and parsed into tables of
 * the same layout, the way scripts/bdf.py places a BDF glyph in its cell.
 */
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "wfont.h"
#include "wfont_tables.h"
#include "ttf.h"

/* The compiled-in tables (build/gen/user_font_data.c, from fonts/9x15*.bdf). */
extern const struct font font_regular, font_bold;

const struct font *wfont_regular(void)
{
    return &font_regular;
}

const struct font *wfont_bold(void)
{
    return &font_bold;
}

/* (41c-3) A scalable font: its file (mapped), the reader's view of it, its
 * size in pixels, and the glyph masks drawn so far (each glyph at each
 * quarter-pixel pen place it was drawn at), GLYPHS_KEPT at most: past
 * them a mask is drawn for the one use. Masks are never changed or freed
 * once kept, so a pointer to one stays good; `lock` guards the lists. */
#define GLYPHS_KEPT 4096
#define KEPT_LISTS 256

struct kept {
    int glyph, sub;
    struct ttf_mask m;
    struct kept *next;
};

struct scalable {
    struct ttf ttf;
    void *data;
    size_t size;
    float scale;                        /* pixels a font unit */
    int ascent, line, width;            /* pixels */
    pthread_mutex_t lock;
    struct kept *kept[KEPT_LISTS];
    int count;
};

int wfont_width(const struct font *font)
{
    return font->scalable ? font->scalable->width : font->width;
}

int wfont_height(const struct font *font)
{
    if (font->scalable)
        return font->scalable->line;
    return font->line ? font->line : font->height;
}

/* The index of codepoint's glyph, or -1. */
static long glyph_index(const struct font *font, uint32_t codepoint)
{
    uint32_t lo = 0, hi = font->count;          /* binary search, sorted table */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (font->codepoints[mid] < codepoint)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < font->count && font->codepoints[lo] == codepoint)
        return (long)lo;
    return -1;
}

static const uint16_t *lookup(const struct font *font, uint32_t codepoint)
{
    long i = font->wide ? -1 : glyph_index(font, codepoint);
    return i >= 0 ? font->bitmaps + (uint64_t)i * font->height : 0;
}

/* (27k) A wide or proportional face's glyph: its index there, or in its
 * fallback, or U+FFFD's, or '?''s; *in is the font it is in. -1: none. */
static long wide_glyph(const struct font *font, uint32_t codepoint, const struct font **in)
{
    const uint32_t tries[3] = { codepoint, 0xFFFD, '?' };
    for (int t = 0; t < 3; t++)
        for (const struct font *f = font; f; f = f->fallback) {
            long i = glyph_index(f, tries[t]);
            if (i >= 0) {
                *in = f;
                return i;
            }
        }
    return -1;
}

/* How far the pen moves for codepoint. */
static int advance(const struct font *font, uint32_t codepoint)
{
    if (!font->advances)
        return font->width;
    const struct font *in;
    long i = wide_glyph(font, codepoint, &in);
    return i < 0 ? font->width / 2 : in->advances ? in->advances[i] : in->width;
}

/* A glyph, else its fallback face's (the compiled bold: the regular one),
 * else the replacement character U+FFFD. */
static const uint16_t *font_glyph(const struct font *font, uint32_t codepoint)
{
    const struct font *alt = font == &font_bold ? &font_regular : font->fallback;
    const uint16_t *g = lookup(font, codepoint);
    if (!g && alt)
        g = lookup(alt, codepoint);
    if (!g)
        g = lookup(alt ? alt : font, 0xFFFD);
    if (!g)
        g = lookup(font, 0xFFFD);
    return g;
}

/* --- fonts from files ------------------------------------------------------ */

#define MAX_LOADED      128             /* fonts one program reads (27k: a browser's many faces) */
#define MAX_HEIGHT      64

static struct {
    char name[32];
    const struct font *font;            /* 0: tried, and there was none */
} loaded[MAX_LOADED];
static int loaded_count;

/* The text of a mapped file, line by line (lines end with \n; \r is ignored). */
struct text {
    const char *p, *end;
};

static bool next_line(struct text *t, const char **line, const char **end)
{
    if (t->p >= t->end)
        return false;
    *line = t->p;
    const char *nl = memchr(t->p, '\n', (size_t)(t->end - t->p));
    *end = nl ? nl : t->end;
    t->p = nl ? nl + 1 : t->end;
    if (*end > *line && (*end)[-1] == '\r')
        (*end)--;
    return true;
}

/* Does the line start with the keyword (then a space or its end)? *rest is after it. */
static bool keyword(const char *line, const char *end, const char *word, const char **rest)
{
    size_t n = strlen(word);
    if ((size_t)(end - line) < n || memcmp(line, word, n) != 0 ||
        (line + n < end && line[n] != ' ' && line[n] != '\t'))
        return false;
    *rest = line + n;
    return true;
}

/* The next decimal number (with an optional sign) at *s before end. */
static bool number(const char **s, const char *end, long *out)
{
    const char *p = *s;
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    bool negative = p < end && *p == '-';
    if (p < end && (*p == '-' || *p == '+'))
        p++;
    if (p >= end || *p < '0' || *p > '9')
        return false;
    long v = 0;
    while (p < end && *p >= '0' && *p <= '9' && v < 1000000000)
        v = v * 10 + (*p++ - '0');
    *out = negative ? -v : v;
    *s = p;
    return true;
}

/* A bitmap row: hexadecimal digits, at most 64 bits. */
static bool hex_row(const char *line, const char *end, uint64_t *out, int *bits)
{
    uint64_t v = 0;
    int n = 0;
    for (const char *p = line; p < end; p++) {
        int d = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 :
                *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : -1;
        if (d < 0 || n == 16)
            return false;
        v = v << 4 | (uint64_t)d;
        n++;
    }
    *out = v;
    *bits = n * 4;
    return n > 0;
}

/* (27k) Does any glyph's DWIDTH differ from the cell width w? (Looks
 * ahead from where the header ended; t itself is not moved.) */
static bool proportional(struct text t, long w)
{
    const char *line, *end, *rest;
    while (next_line(&t, &line, &end)) {
        long d;
        if (keyword(line, end, "DWIDTH", &rest) && number(&rest, end, &d) && d != w)
            return true;
    }
    return false;
}

/* Parse a BDF font into a new struct font, or 0 if it is not a bitmap font
 * this library can draw (cell up to 16 x MAX_HEIGHT pixels). */
static struct font *parse_bdf(const char *data, size_t size)
{
    struct text t = { data, data + size };
    const char *line, *end, *rest;
    long w = 0, h = 0, offset_x = 0, offset_y = 0, ascent = -1, descent = -1, chars = -1, v[4];
    bool box = false;
    while (next_line(&t, &line, &end)) {        /* the header, up to CHARS */
        if (keyword(line, end, "FONTBOUNDINGBOX", &rest)) {
            for (int i = 0; i < 4; i++)
                if (!number(&rest, end, &v[i]))
                    return 0;
            w = v[0], h = v[1], offset_x = v[2], offset_y = v[3];
            box = true;
        } else if (keyword(line, end, "FONT_ASCENT", &rest)) {
            if (!number(&rest, end, &ascent))
                return 0;
        } else if (keyword(line, end, "FONT_DESCENT", &rest)) {
            if (!number(&rest, end, &descent) || descent < 0 || descent > 255)
                descent = -1;
        } else if (keyword(line, end, "CHARS", &rest)) {
            if (!number(&rest, end, &chars))
                return 0;
            break;
        }
    }
    if (!box || ascent < 0 || chars <= 0 || chars > 65536 || w < 1 || w > 64 || h < 1 ||
        h > MAX_HEIGHT || ascent > 255 || offset_x < -64 || offset_x > 64)
        return 0;
    /* (27k) Proportional faces (a DWIDTH not the cell's) and cells wider
     * than 16 keep rows of 64 bits and an advance per glyph. */
    bool wide = w > 16 || proportional(t, w);
    struct font *f = calloc(1, sizeof(*f));
    uint32_t *codepoints = calloc((size_t)chars, sizeof(uint32_t));
    uint16_t *bitmaps = wide ? 0 : calloc((size_t)chars * (size_t)h, sizeof(uint16_t));
    uint64_t *rows64 = wide ? calloc((size_t)chars * (size_t)h, sizeof(uint64_t)) : 0;
    uint8_t *advances = wide ? calloc((size_t)chars, 1) : 0;
    if (!f || !codepoints || (wide ? !rows64 || !advances : !bitmaps)) {
        free(f), free(codepoints), free(bitmaps), free(rows64), free(advances);
        return 0;
    }
    uint32_t count = 0;
    while (count < (uint32_t)chars && next_line(&t, &line, &end)) {
        if (!keyword(line, end, "STARTCHAR", &rest))
            continue;
        long encoding = -1, bbx[4] = { 0, 0, 0, 0 }, dwidth = w;
        bool have_bbx = false;
        uint16_t *cell = wide ? 0 : bitmaps + (size_t)count * (size_t)h;
        uint64_t *cell64 = wide ? rows64 + (size_t)count * (size_t)h : 0;
        while (next_line(&t, &line, &end) && !keyword(line, end, "ENDCHAR", &rest)) {
            if (keyword(line, end, "ENCODING", &rest)) {
                if (!number(&rest, end, &encoding))
                    encoding = -1;
            } else if (keyword(line, end, "DWIDTH", &rest)) {
                if (!number(&rest, end, &dwidth) || dwidth < 0 || dwidth > 255)
                    dwidth = w;
            } else if (keyword(line, end, "BBX", &rest)) {
                have_bbx = true;
                for (int i = 0; i < 4; i++)
                    if (!number(&rest, end, &bbx[i]))
                        have_bbx = false;
            } else if (keyword(line, end, "BITMAP", &rest)) {
                /* Rows count downward from the cell's top; its bottom is at offset_y. */
                long top = (h + offset_y) - (bbx[1] + bbx[3]);
                for (long r = 0; next_line(&t, &line, &end); r++) {
                    if (keyword(line, end, "ENDCHAR", &rest))
                        break;
                    uint64_t bits;
                    int row_bits;
                    long y = top + r;
                    if (!have_bbx || y < 0 || y >= h || !hex_row(line, end, &bits, &row_bits))
                        continue;
                    for (long c = 0; c < bbx[0] && c < row_bits; c++) {
                        long x = bbx[2] - offset_x + c;     /* (from the box's left edge) */
                        if (!(bits >> (row_bits - 1 - c) & 1) || x < 0 || x >= w)
                            continue;
                        if (wide)
                            cell64[y] |= (uint64_t)1 << (63 - x);
                        else
                            cell[y] |= (uint16_t)(0x8000 >> x);
                    }
                }
                break;                          /* (ENDCHAR was read) */
            }
        }
        if (encoding >= 0 && encoding <= 0x10FFFF && have_bbx) {
            if (wide)
                advances[count] = (uint8_t)dwidth;
            codepoints[count++] = (uint32_t)encoding;
        } else if (wide) {
            memset(cell64, 0, (size_t)h * sizeof(uint64_t));
        } else {
            memset(cell, 0, (size_t)h * sizeof(uint16_t));
        }
    }
    /* Sorted for the binary search: BDF files usually are already. */
    size_t row_size = wide ? sizeof(uint64_t) : sizeof(uint16_t);
    uint8_t *cells = wide ? (uint8_t *)rows64 : (uint8_t *)bitmaps;
    for (uint32_t i = 1; i < count; i++) {
        if (codepoints[i - 1] < codepoints[i])
            continue;
        uint32_t c = codepoints[i], j = i;
        uint8_t rows[MAX_HEIGHT * sizeof(uint64_t)], adv = wide ? advances[i] : 0;
        memcpy(rows, cells + (size_t)i * h * row_size, (size_t)h * row_size);
        while (j > 0 && codepoints[j - 1] > c) {
            codepoints[j] = codepoints[j - 1];
            if (wide)
                advances[j] = advances[j - 1];
            memcpy(cells + (size_t)j * h * row_size, cells + (size_t)(j - 1) * h * row_size, (size_t)h * row_size);
            j--;
        }
        codepoints[j] = c;
        if (wide)
            advances[j] = adv;
        memcpy(cells + (size_t)j * h * row_size, rows, (size_t)h * row_size);
    }
    if (count == 0) {
        free(f), free(codepoints), free(bitmaps), free(rows64), free(advances);
        return 0;
    }
    f->wide = rows64;
    f->advances = advances;
    f->left = (int8_t)offset_x;
    f->box_ascent = (uint8_t)(h + offset_y > 0 && h + offset_y < 256 ? h + offset_y : ascent);
    if (wide && descent >= 0 && ascent + descent > 0 && ascent + descent <= h)
        f->line = (uint8_t)(ascent + descent);  /* (27k: a line, not the tallest accent's cell) */
    f->width = (uint8_t)w;
    f->height = (uint8_t)h;
    f->ascent = (uint8_t)ascent;
    f->count = count;
    f->codepoints = codepoints;
    f->bitmaps = bitmaps;
    return f;
}

/* A font file's name: letters, digits, '-' and '_' (no paths). */
static bool valid_name(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n >= sizeof(loaded[0].name))
        return false;
    for (const char *p = name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '-' || *p == '_'))
            return false;
    return true;
}

static struct font *read_font(const char *name)
{
    char path[64];
    snprintf(path, sizeof(path), WFONT_DIR "/%s.bdf", name);
    struct stat st;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return 0;
    }
    void *data = mmap(0, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED)
        return 0;
    struct font *f = parse_bdf(data, (size_t)st.st_size);
    munmap(data, (size_t)st.st_size);
    return f;
}

const struct font *wfont_find(const char *name)
{
    if (!name || !valid_name(name))
        return 0;
    for (int i = 0; i < loaded_count; i++)
        if (strcmp(loaded[i].name, name) == 0)
            return loaded[i].font;
    if (loaded_count == MAX_LOADED)
        return 0;
    struct font *f = read_font(name);
    int i = loaded_count++;                     /* remembered, also when there is none */
    snprintf(loaded[i].name, sizeof(loaded[i].name), "%s", name);
    loaded[i].font = f;
    size_t n = strlen(name);
    if (f && n > 1 && name[n - 1] == 'B') {     /* a bold face: missing glyphs from NAME */
        char regular[32];
        snprintf(regular, sizeof(regular), "%.*s", (int)(n - 1), name);
        const struct font *r = wfont_find(regular);
        if (r && r->width == f->width && r->height == f->height)
            f->fallback = r;
    }
    return f;
}

/* --- drawing --------------------------------------------------------------- */

uint32_t utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = p[0];
    int extra;

    if (c == 0)
        return 0;
    if (c < 0x80) {
        *s += 1;
        return c;
    }
    if ((c & 0xE0) == 0xC0) {
        c &= 0x1F;
        extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
        c &= 0x0F;
        extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
        c &= 0x07;
        extra = 3;
    } else {
        *s += 1;
        return 0xFFFD;
    }
    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *s += i;
            return 0xFFFD;
        }
        c = (c << 6) | (p[i] & 0x3F);
    }
    *s += 1 + extra;
    return c;
}

static int scalable_glyph(const struct scalable *sc, uint32_t c);
static int scalable_text(uint32_t *buf, uint32_t stride, uint32_t bw, uint32_t bh, const struct font *font, int x,
                         int y, const char *utf8, uint32_t fg, uint32_t bg);

int wfont_text_width(const struct font *font, const char *utf8)
{
    if (font->scalable) {                       /* (the advances as they are, rounded once) */
        const struct scalable *sc = font->scalable;
        float pen = 0;
        uint32_t c;
        while ((c = utf8_next(&utf8)) != 0)
            pen += (float)ttf_advance(&sc->ttf, scalable_glyph(sc, c)) * sc->scale;
        return (int)floorf(pen + 0.5f);
    }
    int n = 0;
    uint32_t c;
    while ((c = utf8_next(&utf8)) != 0)
        n += advance(font, c);
    return n;
}

static void draw_char(uint32_t *buf, uint32_t stride, uint32_t bw, uint32_t bh,
                      const struct font *font, int x, int y, uint32_t codepoint,
                      uint32_t fg, uint32_t bg)
{
    const uint16_t *rows = font_glyph(font, codepoint);
    for (uint32_t r = 0; r < font->height; r++) {
        int py = y + (int)r;
        if (py < 0 || py >= (int)bh)
            continue;
        uint16_t bits = rows ? rows[r] : 0;
        for (uint32_t c = 0; c < font->width; c++) {
            int px = x + (int)c;
            if (px < 0 || px >= (int)bw)
                continue;
            if (bits & (0x8000 >> c))
                buf[(uint64_t)py * stride + px] = fg;
            else if (bg != WFONT_TRANSPARENT)
                buf[(uint64_t)py * stride + px] = bg;
        }
    }
}

/* (27k) A wide or proportional face's character: its set pixels only
 * (bg, when not transparent, fills its advance's cell first). */
static void draw_wide(uint32_t *buf, uint32_t stride, uint32_t bw, uint32_t bh, const struct font *font,
                      int x, int y, uint32_t codepoint, uint32_t fg, uint32_t bg)
{
    const struct font *in = font;
    long i = wide_glyph(font, codepoint, &in);
    int adv = i < 0 ? font->width / 2 : in->advances ? in->advances[i] : in->width;
    if (bg != WFONT_TRANSPARENT)
        for (int r = 0; r < wfont_height(font); r++)
            for (int c = 0; c < adv; c++)
                if (y + r >= 0 && y + r < (int)bh && x + c >= 0 && x + c < (int)bw)
                    buf[(uint64_t)(y + r) * stride + x + c] = bg;
    if (i < 0 || !in->wide)
        return;
    const uint64_t *rows = in->wide + (uint64_t)i * in->height;
    int top = y + font->ascent - in->box_ascent;        /* (its cell's top: the baseline less its rows above) */
    for (int r = 0; r < in->height; r++) {
        int py = top + r;
        if (py < 0 || py >= (int)bh || !rows[r])
            continue;
        for (int c = 0; c < in->width; c++) {
            int px = x + in->left + c;
            if (px >= 0 && px < (int)bw && (rows[r] >> (63 - c) & 1))
                buf[(uint64_t)py * stride + px] = fg;
        }
    }
}

int wfont_text(uint32_t *buf, uint32_t stride, uint32_t bw, uint32_t bh,
               const struct font *font, int x, int y, const char *utf8,
               uint32_t fg, uint32_t bg)
{
    if (font->scalable)
        return scalable_text(buf, stride, bw, bh, font, x, y, utf8, fg, bg);
    uint32_t c;
    while ((c = utf8_next(&utf8)) != 0) {
        if (font->wide)
            draw_wide(buf, stride, bw, bh, font, x, y, c, fg, bg);
        else
            draw_char(buf, stride, bw, bh, font, x, y, c, fg, bg);
        x += advance(font, c);
    }
    return x;
}

/* --- (41c-3) scalable fonts ---------------------------------------------- */

#define OPENED_MAX 64                   /* (file, size) pairs one program opens */

static struct {
    char path[256];
    int size;
    const struct font *font;            /* 0: tried, and there was none */
} opened[OPENED_MAX];
static int opened_count;
static pthread_mutex_t opened_lock = PTHREAD_MUTEX_INITIALIZER;

/* A bare file name (letters, digits, '-', '_', '.'; not starting with
 * '.'): one in WFONT_DIR. */
static bool bare_name(const char *name)
{
    if (!name[0] || name[0] == '.')
        return false;
    for (const char *p = name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' ||
              *p == '_' || *p == '.'))
            return false;
    return true;
}

static struct font *open_scalable(const char *path, int size)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0)
        return 0;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return 0;
    }
    void *data = mmap(0, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED)
        return 0;
    struct font *f = calloc(1, sizeof(*f));
    struct scalable *sc = calloc(1, sizeof(*sc));
    if (!f || !sc || !ttf_open(&sc->ttf, data, (size_t)st.st_size)) {
        free(f), free(sc);
        munmap(data, (size_t)st.st_size);
        return 0;
    }
    sc->data = data;
    sc->size = (size_t)st.st_size;
    sc->scale = (float)size / (float)sc->ttf.upem;
    int ascent = (int)floorf((float)sc->ttf.ascender * sc->scale + 0.5f);
    int descent = (int)floorf((float)-sc->ttf.descender * sc->scale + 0.5f);
    int gap = (int)floorf((float)sc->ttf.line_gap * sc->scale + 0.5f);
    sc->ascent = ascent;
    sc->line = ascent + descent + (gap > 0 ? gap : 0);
    sc->width = (int)floorf((float)sc->ttf.advance_max * sc->scale + 0.5f);
    if (sc->line < 1)
        sc->line = 1;
    if (sc->width < 1)
        sc->width = 1;
    pthread_mutex_init(&sc->lock, 0);
    f->scalable = sc;
    return f;
}

/* (41d) The user's text size: text_size in ~/.config/windows, else in
 * /system/config/windows (as windowd reads its settings), 8-64, 15 if not
 * set; read once. (windowd writes the user's file when it is missing: no
 * program draws without windowd.) */
#define TEXT_SIZE_DEFAULT 15

static int user_text_size(void)
{
    static int size;
    if (size)
        return size;
    char value[32], mine[300];
    int r = 0;
    struct passwd *pw = getpwuid(getuid());
    if (pw) {
        snprintf(mine, sizeof(mine), "%s/.config/windows", pw->pw_dir);
        r = config_get(mine, "text_size", value, sizeof(value));
    }
    if (r <= 0)
        r = config_get("/system/config/windows", "text_size", value, sizeof(value));
    int v = r > 0 ? atoi(value) : 0;
    size = v >= 8 && v <= 64 ? v : TEXT_SIZE_DEFAULT;
    return size;
}

const struct font *wfont_open(const char *path, int size)
{
    char full[256];
    if (size == 0)
        size = user_text_size();
    if (!path || size < 4 || size > 1000)
        return 0;
    if (strchr(path, '/'))
        snprintf(full, sizeof(full), "%s", path);
    else if (bare_name(path))
        snprintf(full, sizeof(full), WFONT_DIR "/%s", path);
    else
        return 0;
    if (strlen(path) >= sizeof(full) - sizeof(WFONT_DIR))
        return 0;
    pthread_mutex_lock(&opened_lock);
    const struct font *f = 0;
    int i = 0;
    for (; i < opened_count; i++)
        if (opened[i].size == size && strcmp(opened[i].path, full) == 0)
            break;
    if (i < opened_count)
        f = opened[i].font;
    else if (opened_count < OPENED_MAX) {
        f = open_scalable(full, size);
        snprintf(opened[opened_count].path, sizeof(opened[0].path), "%s", full);
        opened[opened_count].size = size;
        opened[opened_count++].font = f;        /* (remembered, also when there is none) */
    }
    pthread_mutex_unlock(&opened_lock);
    return f;
}

/* A character's glyph: its own, else U+FFFD's, else .notdef's (0). */
static int scalable_glyph(const struct scalable *sc, uint32_t c)
{
    int g = ttf_glyph(&sc->ttf, c);
    return g ? g : ttf_glyph(&sc->ttf, 0xFFFD);
}

/* A glyph's mask at quarter-pixel place sub: a kept one, or (*own) one
 * drawn for this use, which the caller frees; 0 when it cannot be drawn. */
static const struct ttf_mask *glyph_mask(struct scalable *sc, int glyph, int sub, struct ttf_mask *own)
{
    unsigned list = ((unsigned)glyph * 4 + (unsigned)sub) % KEPT_LISTS;
    pthread_mutex_lock(&sc->lock);
    for (struct kept *k = sc->kept[list]; k; k = k->next)
        if (k->glyph == glyph && k->sub == sub) {
            pthread_mutex_unlock(&sc->lock);
            return &k->m;
        }
    pthread_mutex_unlock(&sc->lock);
    struct ttf_mask m;                          /* (drawn without the lock) */
    if (!ttf_mask(&sc->ttf, glyph, sc->scale, (float)sub / 4, &m))
        return 0;
    struct kept *k = malloc(sizeof(*k));
    pthread_mutex_lock(&sc->lock);
    for (struct kept *o = sc->kept[list]; o && k; o = o->next)
        if (o->glyph == glyph && o->sub == sub) {   /* (another thread kept it meanwhile) */
            pthread_mutex_unlock(&sc->lock);
            free(k);
            free(m.bytes);
            return &o->m;
        }
    if (!k || sc->count >= GLYPHS_KEPT) {
        pthread_mutex_unlock(&sc->lock);
        free(k);
        *own = m;
        return own;
    }
    *k = (struct kept){ glyph, sub, m, sc->kept[list] };
    sc->kept[list] = k;
    sc->count++;
    pthread_mutex_unlock(&sc->lock);
    return &k->m;
}

/* A native pixel blended over another by a (0 to 255), byte by byte (each
 * channel is a byte of the pixel, whichever its order). */
static uint32_t blend(uint32_t fg, uint32_t d, uint32_t a)
{
    if (a == 255)
        return fg;
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        uint32_t f = fg >> shift & 255, b = d >> shift & 255;
        out |= (f * a + b * (255 - a) + 127) / 255 << shift;
    }
    return out;
}

static int scalable_text(uint32_t *buf, uint32_t stride, uint32_t bw, uint32_t bh, const struct font *font, int x,
                         int y, const char *utf8, uint32_t fg, uint32_t bg)
{
    struct scalable *sc = font->scalable;
    float pen = (float)x;
    int base = y + sc->ascent;
    uint32_t c;
    while ((c = utf8_next(&utf8)) != 0) {
        int g = scalable_glyph(sc, c);
        float next = pen + (float)ttf_advance(&sc->ttf, g) * sc->scale;
        if (bg != WFONT_TRANSPARENT)            /* (its advance's cell, the line's height) */
            for (int r = y; r < y + sc->line; r++)
                for (int col = (int)floorf(pen + 0.5f); col < (int)floorf(next + 0.5f); col++)
                    if (r >= 0 && r < (int)bh && col >= 0 && col < (int)bw)
                        buf[(uint64_t)r * stride + col] = bg;
        float whole = floorf(pen);
        int sub = (int)((pen - whole) * 4);
        sub = sub < 0 ? 0 : sub > 3 ? 3 : sub;
        struct ttf_mask own = { 0, 0, 0, 0, 0 };
        const struct ttf_mask *m = glyph_mask(sc, g, sub, &own);
        if (m && m->bytes) {
            int left = (int)whole + m->left, top = base + m->top;
            for (int r = 0; r < m->h; r++) {
                int py = top + r;
                if (py < 0 || py >= (int)bh)
                    continue;
                for (int col = 0; col < m->w; col++) {
                    int px = left + col;
                    uint32_t a = m->bytes[r * m->w + col];
                    if (a && px >= 0 && px < (int)bw)
                        buf[(uint64_t)py * stride + px] = blend(fg, buf[(uint64_t)py * stride + px], a);
                }
            }
        }
        free(own.bytes);
        pen = next;
    }
    return (int)floorf(pen + 0.5f);
}

/* --- (41d) text over several lines --------------------------------------- */

/* How far the pen moves for c, in pixels (a scalable font's exactly). */
static float pen_advance(const struct font *font, uint32_t c)
{
    if (font->scalable)
        return (float)ttf_advance(&font->scalable->ttf, scalable_glyph(font->scalable, c)) * font->scalable->scale;
    return (float)advance(font, c);
}

int wfont_wrap(const struct font *font, const char *utf8, int width)
{
    const char *p = utf8, *brk = 0;
    float pen = 0;
    bool after_space = false;
    for (;;) {
        const char *at = p;
        uint32_t c = utf8_next(&p);
        if (c == 0)
            return (int)(at - utf8);
        if (c == '\n')
            return (int)(p - utf8);
        if (c == ' ') {                         /* (spaces may hang past the edge) */
            pen += pen_advance(font, c);
            after_space = true;
            continue;
        }
        if (after_space) {                      /* a word starts: the line may break before it */
            brk = at;
            after_space = false;
        }
        pen += pen_advance(font, c);
        if (floorf(pen + 0.5f) > (float)width && at > utf8) {
            if (brk)
                return (int)(brk - utf8);
            return (int)(at - utf8);            /* (a word wider than a line: cut where it no longer fits) */
        }
    }
}

int wfont_layout(uint32_t *buf, uint32_t stride, uint32_t bw, uint32_t bh, const struct font *font, int x,
                 int y, int width, const char *utf8, uint32_t fg, uint32_t bg)
{
    int lines = 0, line = wfont_height(font);
    char small[256];
    for (const char *p = utf8; *p; lines++) {
        int n = wfont_wrap(font, p, width);
        if (buf) {
            int shown = n > 0 && p[n - 1] == '\n' ? n - 1 : n;
            char *text = shown < (int)sizeof(small) ? small : malloc((size_t)shown + 1);
            if (text) {
                memcpy(text, p, (size_t)shown);
                text[shown] = 0;
                wfont_text(buf, stride, bw, bh, font, x, y + lines * line, text, fg, bg);
                if (text != small)
                    free(text);
            }
        }
        p += n > 0 ? n : 1;
    }
    return lines * line;
}

/*
 * Test: the font file reader (41c). The reader (user/lib/ttf.c, inside
 * libwindow) is built into this program, so the Mac and RitterOS run the
 * same code. `ttftest dump FILE` prints a font's sizes, every glyph's
 * advance and outline as moves, lines and curves, and every character's
 * glyph: scripts/fontref.swift compares that with macOS's CoreText.
 * `ttftest [FILE...]` (default: the FreeFont files in /system/fonts, .ttf
 * and .otf) prints a checksum of each dump and (41c-3) of a sample line's
 * glyph masks: the test compares them with the Mac's. `ttftest masks`
 * prints masks for scripts/drawref.py's exact areas.
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../lib/ttf.c"

static uint64_t hash = 0xcbf29ce484222325ULL;
static int hashing;

/* Print, or (hashing) only add to the checksum (FNV-1a). */
static void out(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void out(const char *format, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    if (!hashing) {
        fputs(line, stdout);
        return;
    }
    for (int i = 0; i < n && i < (int)sizeof(line) - 1; i++) {
        hash ^= (unsigned char)line[i];
        hash *= 0x100000001b3ULL;
    }
}

static void point(const char *what, struct ttf_point p)
{
    out(" %s %.4f %.4f", what, (double)p.x, (double)p.y);
}

/* A contour as TrueType draws it: between two control points the curve
 * passes through their midpoint; it starts on its first point on the curve
 * (none: the first two points' midpoint). Cubic control points come in
 * pairs before their end point. */
static void contour(const struct ttf_point *p, int n)
{
    if (n < 1)
        return;
    int s = 0;
    while (s < n && p[s].kind != TTF_ON)
        s++;
    struct ttf_point start;
    int total = n;                                       /* (steps round, the last back to the start) */
    if (s < n)
        start = p[s];
    else {                                               /* (all control points: p[0] comes last too) */
        start = (struct ttf_point){ (p[0].x + p[1 % n].x) / 2, (p[0].y + p[1 % n].y) / 2, TTF_ON };
        s = 0;
        total = n + 1;
    }
    out("M");
    point("", start);
    out("\n");
    struct ttf_point control = { 0, 0, 0 }, c1 = { 0, 0, 0 };
    int pending = 0;                                     /* control points waiting: 1 quadratic, 2 cubic */
    for (int k = 1; k <= total; k++) {
        struct ttf_point q = k == total ? start : p[(s + k) % n];
        if (k == total && pending == 0)
            break;                                       /* (back to the start: the close does it) */
        if (q.kind == TTF_CUBIC) {
            if (pending == 0)
                c1 = q, pending = 2;
            else
                control = q, pending = 3;
            continue;
        }
        if (q.kind == TTF_OFF && k < total) {
            if (pending == 1) {                          /* (two in a row: their midpoint) */
                struct ttf_point mid = { (control.x + q.x) / 2, (control.y + q.y) / 2, TTF_ON };
                out("Q");
                point("", control);
                point("", mid);
                out("\n");
            }
            control = q, pending = 1;
            continue;
        }
        if (pending == 1) {
            out("Q");
            point("", control);
            point("", q);
        } else if (pending == 3) {
            out("C");
            point("", c1);
            point("", control);
            point("", q);
        } else
            out("L"), point("", q);
        out("\n");
        pending = 0;
    }
    out("Z\n");
}

/* (41c-3) The sample masks' checksum: the glyphs of a line of text at four
 * sizes and four quarter-pixel pen places. */
static const char *const SAMPLE = "Hamburgefonstiv 0123 @&g ff";
static const uint32_t SAMPLE_MORE[] = { 0x3A9, 0x436, 0x20AC, 0xFB01 };    /* (Omega, zhe, euro, fi) */

static uint64_t masks_sum(const struct ttf *f)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    static const int sizes[] = { 9, 13, 17, 31 };
    uint32_t chars[64];
    int n = 0;
    for (const char *c = SAMPLE; *c; c++)
        chars[n++] = (uint8_t)*c;
    for (int i = 0; i < 4; i++)
        chars[n++] = SAMPLE_MORE[i];
    for (int z = 0; z < 4; z++)
        for (int sub = 0; sub < 4; sub++)
            for (int k = 0; k < n; k++) {
                struct ttf_mask m;
                bool ok = ttf_mask(f, ttf_glyph(f, chars[k]), (float)sizes[z] / (float)f->upem, (float)sub / 4, &m);
                int head[5] = { ok, m.left, m.top, m.w, m.h };
                for (int i = 0; i < 5; i++)
                    for (int b = 0; b < 4; b++) {
                        h ^= (uint32_t)head[i] >> (8 * b) & 255;
                        h *= 0x100000001b3ULL;
                    }
                for (int i = 0; i < m.w * m.h; i++) {
                    h ^= m.bytes[i];
                    h *= 0x100000001b3ULL;
                }
                free(m.bytes);
            }
    return h;
}

static uint8_t *load(const char *path, size_t *size)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0)
        return 0;
    uint8_t *data = malloc((size_t)st.st_size);
    if (!data || read(fd, data, (size_t)st.st_size) != (ssize_t)st.st_size) {
        free(data);
        close(fd);
        return 0;
    }
    close(fd);
    *size = (size_t)st.st_size;
    return data;
}

/* `masks FILE SIZE DX GLYPH...`: each glyph's mask at SIZE pixels an em,
 * the pen DX of a pixel on: its box, then its rows in hexadecimal. */
static int masks(int argc, char **argv)
{
    size_t size;
    uint8_t *data = load(argv[2], &size);
    struct ttf f;
    if (!data || !ttf_open(&f, data, size)) {
        printf("ttftest: cannot read %s\n", argv[2]);
        return 1;
    }
    float scale = (float)atof(argv[3]) / (float)f.upem, dx = (float)atof(argv[4]);
    for (int i = 5; i < argc; i++) {
        struct ttf_mask m;
        int g = atoi(argv[i]);
        bool ok = ttf_mask(&f, g, scale, dx, &m);
        printf("mask %d %s %d %d %d %d\n", g, ok ? "ok" : "broken", m.left, m.top, m.w, m.h);
        for (int y = 0; y < m.h; y++) {
            for (int x = 0; x < m.w; x++)
                printf("%02x", m.bytes[y * m.w + x]);
            printf("\n");
        }
        free(m.bytes);
    }
    free(data);
    return 0;
}

static int dump(const char *path)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) {
        printf("ttftest: cannot read %s\n", path);
        return 1;
    }
    uint8_t *data = malloc((size_t)st.st_size);
    if (!data || read(fd, data, (size_t)st.st_size) != (ssize_t)st.st_size) {
        printf("ttftest: cannot read %s\n", path);
        return 1;
    }
    close(fd);
    struct ttf f;
    if (!ttf_open(&f, data, (size_t)st.st_size)) {
        printf("ttftest: %s is not a font this reader knows\n", path);
        return 1;
    }
    out("font upem %d glyphs %d ascender %d descender %d gap %d\n", f.upem, f.glyphs, f.ascender,
        f.descender, f.line_gap);
    int broken = 0;
    for (int g = 0; g < f.glyphs; g++) {
        struct ttf_shape s = { 0 };
        bool ok = ttf_shape(&f, g, &s);
        out("glyph %d advance %d%s\n", g, ttf_advance(&f, g), ok ? "" : " broken");
        broken += !ok;
        for (int c = 0, from = 0; c < s.contours; c++) {
            contour(s.p + from, s.ends[c] - from + 1);
            from = s.ends[c] + 1;
        }
        ttf_shape_free(&s);
    }
    for (uint32_t c = 0; c <= 0x10FFFF; c++) {
        int g = ttf_glyph(&f, c);
        if (g)
            out("char %u %d\n", (unsigned)c, g);
    }
    free(data);
    return broken ? 2 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "dump"))
        return dump(argv[2]);
    if (argc >= 5 && !strcmp(argv[1], "masks"))
        return masks(argc, argv);
    static const char *const ours[] = { "FreeSans.ttf", "FreeSansBold.ttf", "FreeSerif.ttf", "FreeSerifBold.ttf",
                                        "FreeMono.ttf", "FreeMonoBold.ttf", "FreeSans.otf", "FreeSansBold.otf",
                                        "FreeSerif.otf", "FreeSerifBold.otf", "FreeMono.otf", "FreeMonoBold.otf" };
    int n = argc > 1 ? argc - 1 : 12, failed = 0;
    for (int i = 0; i < n; i++) {
        char path[512];
        if (argc > 1)
            snprintf(path, sizeof(path), "%s", argv[i + 1]);
        else
            snprintf(path, sizeof(path), "/system/fonts/%s", ours[i]);
        hashing = 1;
        hash = 0xcbf29ce484222325ULL;
        int r = dump(path);
        hashing = 0;
        const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
        uint64_t glyphs = hash, sample = 0;
        size_t size;
        uint8_t *data = load(path, &size);
        struct ttf f;
        if (data && ttf_open(&f, data, size))
            sample = masks_sum(&f);
        free(data);
        printf("ttftest: %s %016llx masks %016llx%s\n", name, (unsigned long long)glyphs, (unsigned long long)sample,
               r ? " (broken glyphs)" : "");
        failed |= r;
    }
    printf("ttftest: done\n");
    return failed;
}

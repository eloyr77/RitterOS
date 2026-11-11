/*
 * The font file reader (41c, inside libwindow; not a public interface):
 * TrueType and OpenType files (TrueType or, 41c-2, CFF outlines), their
 * sizes and advances, which glyph a
 * character is, and each glyph's outline in font units (y up, as in the
 * file). Written from the OpenType specification; every read is checked
 * against the file's size, since a font is data from anywhere.
 */
#ifndef RITTEROS_TTF_H
#define RITTEROS_TTF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ttf {
    const uint8_t *data;
    uint32_t size;
    int upem;                           /* font units per em */
    int glyphs, hmetrics;
    int ascender, descender, line_gap;  /* hhea's, in font units (descender negative) */
    int advance_max;
    bool loca_long;
    uint32_t cmap, cmap_end;            /* the chosen character map (format 4 or 12) */
    int cmap_format;
    uint32_t loca, loca_end, glyf, glyf_end, hmtx, hmtx_end;
    /* (41c-2) CFF outlines (an 'OTTO' file): the table's bounds, its
     * CharStrings and global subroutines; a plain font's local subroutines,
     * a CID-keyed one's FDArray and FDSelect; FontMatrix's scale to font
     * units */
    bool cff;
    uint32_t cff_at, cff_end, charstrings, gsubrs, subrs, fdarray, fdselect;
    double cff_xscale, cff_yscale;
};

/* A point of an outline: on the curve, or a control point of a quadratic
 * (TrueType) or cubic (CFF) curve. */
enum { TTF_OFF = 0, TTF_ON = 1, TTF_CUBIC = 2 };

struct ttf_point {
    float x, y;
    uint8_t kind;
};

/* An outline: contours, each its points up to ends[i] (inclusive). */
struct ttf_shape {
    struct ttf_point *p;
    int n, cap;
    int *ends;
    int contours, ccap;
    bool failed;                        /* out of memory */
};

/* Read a font file's tables (the data stays the caller's): false when it
 * is not one this reader knows. */
bool ttf_open(struct ttf *f, const void *data, size_t size);

/* The glyph of a character (0, .notdef, when the font has none). */
int ttf_glyph(const struct ttf *f, uint32_t codepoint);

/* A glyph's advance in font units. */
int ttf_advance(const struct ttf *f, int glyph);

/* Add a glyph's outline to s: false when the glyph's data is broken (s
 * then holds what was read before it). */
bool ttf_shape(const struct ttf *f, int glyph, struct ttf_shape *s);

void ttf_shape_free(struct ttf_shape *s);

/* (41c-3) A glyph drawn at `scale` pixels a font unit with its pen dx
 * (0 to 1) of a pixel right of a whole pixel, on the baseline: each
 * pixel's coverage (0 to 255) over the glyph's box, w x h bytes, the box's
 * top left `left` pixels right of the pen's pixel and `top` below the
 * baseline (negative: above it). bytes is malloc'd (0 for a glyph without
 * an outline); false for a broken glyph or one too big to draw. */
struct ttf_mask {
    int left, top, w, h;
    uint8_t *bytes;
};

bool ttf_mask(const struct ttf *f, int glyph, float scale, float dx, struct ttf_mask *m);

#endif

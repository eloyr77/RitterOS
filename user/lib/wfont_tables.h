/*
 * The layout of a font: libwindow's own (programs hold only pointers, see
 * wfont.h). The compiled-in tables (build/gen/user_font_data.c, made by
 * scripts/bdf2c.c) and the fonts read from /system/fonts (wfont.c) both use
 * it. Each glyph is `height` rows of uint16_t, bit 15 the leftmost pixel, so
 * at most 16 pixels wide; or (27k, wider or proportional faces) rows of
 * uint64_t with an advance per glyph.
 */
#ifndef RITTEROS_WFONT_TABLES_H
#define RITTEROS_WFONT_TABLES_H

#include <stdint.h>

struct scalable;

struct font {
    uint8_t width;                  /* cell width in pixels (at most 16) */
    uint8_t height;                 /* cell height in pixels */
    uint8_t ascent;                 /* pixels above the baseline */
    uint32_t count;                 /* number of glyphs */
    const uint32_t *codepoints;     /* sorted Unicode code points */
    const uint16_t *bitmaps;        /* count * height rows */
    const struct font *fallback;    /* where missing glyphs come from (a bold
                                     * face's regular one), or 0 */
    /* (27k) proportional and wide faces (read from files only; 0 in the
     * compiled tables): each glyph's advance, and rows of up to 64 pixels
     * (bit 63 the leftmost) instead of `bitmaps`; a glyph's column 0 is
     * `left` pixels from the pen (the font's bounding box offset). */
    const uint8_t *advances;
    const uint64_t *wide;
    int8_t left;
    uint8_t box_ascent;             /* rows of a glyph cell above the baseline (accents too) */
    uint8_t line;                   /* FONT_ASCENT + FONT_DESCENT: a line's height (0: height) */
    struct scalable *scalable;      /* (41c-3) a TrueType/OpenType file's font (wfont_open), else 0 */
};

#endif

/*
 * Bitmap fonts and text drawing. The glyph tables are generated at build time
 * from the X11 misc-fixed BDF fonts in kernel/font/ by scripts/bdf2c.c.
 */
#ifndef RITTEROS_FONT_H
#define RITTEROS_FONT_H

#include <stdint.h>
#include "framebuffer.h"

struct font {
    uint8_t width;                 /* cell width in pixels (at most 16) */
    uint8_t height;                /* cell height in pixels */
    uint8_t ascent;                /* pixels above the baseline */
    uint32_t count;                /* number of glyphs */
    const uint32_t *codepoints;    /* sorted Unicode code points */
    const uint16_t *bitmaps;       /* count * height rows; bit 15 = leftmost pixel */
};

extern const struct font font_regular;   /* 9x15 */
extern const struct font font_bold;      /* 9x15B, falls back to regular */

/* Background value meaning "leave the pixels behind the text unchanged". */
#define FB_TRANSPARENT 0xFFFFFFFFu

/*
 * Rows of the glyph for codepoint: from font, else from font_regular, else
 * U+FFFD (the replacement character).
 */
const uint16_t *font_glyph(const struct font *font, uint32_t codepoint);

/* Draw one character cell with its top-left corner at (x, y). */
void fb_draw_char(const struct framebuffer *fb, const struct font *font,
                  uint32_t x, uint32_t y, uint32_t codepoint, uint32_t fg, uint32_t bg);

/* Draw a UTF-8 string on one line; returns the x position after it. */
uint32_t fb_draw_text(const struct framebuffer *fb, const struct font *font,
                      uint32_t x, uint32_t y, const char *utf8, uint32_t fg, uint32_t bg);

/* Decode one UTF-8 character at *s and advance *s past it. Invalid bytes
 * become U+FFFD. Returns 0 at the end of the string. */
uint32_t utf8_next(const char **s);

#endif

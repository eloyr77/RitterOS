#include "font.h"

static const uint16_t *lookup(const struct font *font, uint32_t codepoint)
{
    /* Binary search in the sorted code point table. */
    uint32_t lo = 0, hi = font->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (font->codepoints[mid] < codepoint)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < font->count && font->codepoints[lo] == codepoint)
        return font->bitmaps + (uint64_t)lo * font->height;
    return 0;
}

const uint16_t *font_glyph(const struct font *font, uint32_t codepoint)
{
    const uint16_t *g = lookup(font, codepoint);
    if (!g && font != &font_regular)
        g = lookup(&font_regular, codepoint);
    if (!g)
        g = lookup(&font_regular, 0xFFFD);
    return g;
}

void fb_draw_char(const struct framebuffer *fb, const struct font *font,
                  uint32_t x, uint32_t y, uint32_t codepoint, uint32_t fg, uint32_t bg)
{
    const uint16_t *rows = font_glyph(font, codepoint);
    for (uint32_t r = 0; r < font->height; r++) {
        uint16_t bits = rows ? rows[r] : 0;
        for (uint32_t c = 0; c < font->width; c++) {
            if (bits & (0x8000 >> c))
                fb_put_pixel(fb, x + c, y + r, fg);
            else if (bg != FB_TRANSPARENT)
                fb_put_pixel(fb, x + c, y + r, bg);
        }
    }
}

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
        *s += 1;                    /* stray continuation or invalid byte */
        return 0xFFFD;
    }
    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *s += i;                /* truncated sequence */
            return 0xFFFD;
        }
        c = (c << 6) | (p[i] & 0x3F);
    }
    *s += 1 + extra;
    return c;
}

uint32_t fb_draw_text(const struct framebuffer *fb, const struct font *font,
                      uint32_t x, uint32_t y, const char *utf8, uint32_t fg, uint32_t bg)
{
    uint32_t c;
    while ((c = utf8_next(&utf8)) != 0) {
        fb_draw_char(fb, font, x, y, c, fg, bg);
        x += font->width;
    }
    return x;
}

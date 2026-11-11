#include "framebuffer.h"

void fb_fill_rect(const struct framebuffer *fb, uint32_t x, uint32_t y,
                  uint32_t w, uint32_t h, uint32_t color)
{
    if (x >= fb->width || y >= fb->height)
        return;
    if (w > fb->width - x)
        w = fb->width - x;
    if (h > fb->height - y)
        h = fb->height - y;

    for (uint32_t row = y; row < y + h; row++) {
        volatile uint32_t *p = fb->base + (uint64_t)row * fb->stride + x;
        for (uint32_t i = 0; i < w; i++)
            p[i] = color;
    }
}

void fb_draw_rect(const struct framebuffer *fb, uint32_t x, uint32_t y,
                  uint32_t w, uint32_t h, uint32_t thickness, uint32_t color)
{
    if (thickness * 2 > w || thickness * 2 > h) {
        fb_fill_rect(fb, x, y, w, h, color);
        return;
    }
    fb_fill_rect(fb, x, y, w, thickness, color);                     /* top */
    fb_fill_rect(fb, x, y + h - thickness, w, thickness, color);     /* bottom */
    fb_fill_rect(fb, x, y, thickness, h, color);                     /* left */
    fb_fill_rect(fb, x + w - thickness, y, thickness, h, color);     /* right */
}

void fb_progress_mark(const struct framebuffer *fb, unsigned stage)
{
    fb_fill_rect(fb, 8 + (stage - 1) * 24, 8, 16, 16, 0x00C0C0C0);
}

void fb_progress_dot(const struct framebuffer *fb, unsigned index)
{
    fb_fill_rect(fb, 8 + index * 8, 30, 4, 4, fb_rgb(fb, 0xF0, 0x90, 0x20));
}

void fb_progress_failed(const struct framebuffer *fb, unsigned stage)
{
    fb_fill_rect(fb, 8 + (stage - 1) * 24, 8, 16, 16, fb_rgb(fb, 0xE0, 0x30, 0x30));
}

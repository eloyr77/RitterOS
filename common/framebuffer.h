/*
 * Linear framebuffer drawing. No UEFI dependency: the bootloader fills in a
 * struct framebuffer from GOP, and the kernel will receive the same struct.
 */
#ifndef RITTEROS_FRAMEBUFFER_H
#define RITTEROS_FRAMEBUFFER_H

#include <stdint.h>

struct framebuffer {
    volatile uint32_t *base;   /* first pixel, 32 bits per pixel */
    uint32_t width;            /* visible pixels per row */
    uint32_t height;           /* rows */
    uint32_t stride;           /* pixels from one row to the next (>= width) */
    uint8_t red_shift;         /* bit position of each 8-bit color channel */
    uint8_t green_shift;
    uint8_t blue_shift;
};

/* Encode a color in the framebuffer's native pixel layout. */
static inline uint32_t fb_rgb(const struct framebuffer *fb, uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << fb->red_shift) | ((uint32_t)g << fb->green_shift) |
           ((uint32_t)b << fb->blue_shift);
}

static inline void fb_put_pixel(const struct framebuffer *fb, uint32_t x, uint32_t y,
                                uint32_t color)
{
    if (x < fb->width && y < fb->height)
        fb->base[(uint64_t)y * fb->stride + x] = color;
}

void fb_fill_rect(const struct framebuffer *fb, uint32_t x, uint32_t y,
                  uint32_t w, uint32_t h, uint32_t color);

/*
 * Boot progress: a small gray square per stage in the top left corner,
 * drawn by the bootloader (stages 1-4) and the kernel (5) before the console
 * exists. On a PC without a serial port, the number of squares on a dark
 * screen tells how far the boot got. Gray is the same in every pixel layout.
 */
void fb_progress_mark(const struct framebuffer *fb, unsigned stage);

/* A small orange dot below the squares (e.g. one per attempt), and a red
 * square in place of a stage that failed. */
void fb_progress_dot(const struct framebuffer *fb, unsigned index);
void fb_progress_failed(const struct framebuffer *fb, unsigned stage);

/* Rectangle outline of the given thickness, drawn inside (x, y, w, h). */
void fb_draw_rect(const struct framebuffer *fb, uint32_t x, uint32_t y,
                  uint32_t w, uint32_t h, uint32_t thickness, uint32_t color);

#endif

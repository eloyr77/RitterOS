/*
 * libdraw (41b): 2D drawing on any pixel buffer: a window's (window_pixels),
 * an image's or one of the program's own, described by a struct
 * draw_canvas. Every shape is anti-aliased: a pixel takes the shape's colour
 * in proportion to how much of its square the shape covers, by area, and
 * every colour has an alpha (0 transparent, 255 opaque) blended over what
 * is there. Coordinates are floats in pixels: pixel (i, j) is the square
 * from (i, j) to (i + 1, j + 1), so its centre is (i + 0.5, j + 0.5).
 *
 * A `line` of 0 fills a shape; above 0 it is the width of the shape's
 * outline: a rectangle's, a circle's or an ellipse's lies inside the shape
 * (as a CSS border, so a 1-pixel one on whole coordinates is sharp), a
 * polygon's is centred on its edges (bevelled corners), as is a line's.
 * Curves are drawn as straight pieces within 1/1024 of a pixel of them.
 */
#ifndef RITTEROS_DRAW_H
#define RITTEROS_DRAW_H

#include <stdint.h>
#include "media.h"                  /* (struct image, for draw_image) */

/* A pixel buffer: w x h pixels of 32 bits, rows `stride` pixels apart.
 * Each 8-bit channel lies at its shift (a window's: window_shifts; an
 * RGBA8888 image's in memory: red 0, green 8, blue 16); `opaque`'s bits
 * are set in every pixel written (0xFF000000 for an RGBA8888 image's
 * alpha, 0 for a window). Fill in the first eight fields; draw_clip sets
 * the rest (all 0: the whole canvas). */
struct draw_canvas {
    uint32_t *pixels;
    int w, h, stride;
    int red_shift, green_shift, blue_shift;
    uint32_t opaque;
    int clipped;                /* 1: only inside the clip rectangle */
    int clip_x, clip_y, clip_w, clip_h;
};

/* A colour: 8 bits each of red, green, blue and alpha (as 0xAARRGGBB). */
uint32_t draw_rgba(int r, int g, int b, int a);

/* Draw only inside x, y, w, h from now on (a part of it outside the canvas
 * is left out); draw_clip(c, 0, 0, c->w, c->h) draws everywhere again. */
void draw_clip(struct draw_canvas *c, int x, int y, int w, int h);

/* A rectangle from (x, y), w x h: filled (line 0), or its outline `line`
 * wide inside it. */
void draw_rect(struct draw_canvas *c, float x, float y, float w, float h, float line, uint32_t color);

/* A line from (x0, y0) to (x1, y1), `width` wide, its ends cut square at
 * the two points. */
void draw_line(struct draw_canvas *c, float x0, float y0, float x1, float y1, float width,
               uint32_t color);

/* A polygon of n points (xy: x0, y0, x1, y1, ...; the last joined to the
 * first): filled (line 0; where its edges cross themselves, a part wound
 * round twice is filled once), or its outline `line` wide. */
void draw_polygon(struct draw_canvas *c, const float *xy, int n, float line, uint32_t color);

/* (41b-2) A circle around (cx, cy) of radius r, an ellipse of radii rx
 * (across) and ry (down), and a rectangle whose corners are quarter
 * circles of `radius` (at most half its width and height): filled, or
 * their outline `line` wide inside them (an ellipse's inner edge is the
 * ellipse of radii rx - line, ry - line; a rounded rectangle's has corners
 * of radius - line). */
void draw_circle(struct draw_canvas *c, float cx, float cy, float r, float line, uint32_t color);
void draw_ellipse(struct draw_canvas *c, float cx, float cy, float rx, float ry, float line,
                  uint32_t color);
void draw_round_rect(struct draw_canvas *c, float x, float y, float w, float h, float radius,
                     float line, uint32_t color);

/* (41b-3) The rectangle x, y, w, h filled with a gradient: `from` at
 * (x0, y0) to `to` at (x1, y1) along the line through them (each pixel by
 * where its centre falls on that line; before the first point `from`,
 * past the second `to`), alpha included. */
void draw_gradient(struct draw_canvas *c, float x, float y, float w, float h, float x0, float y0,
                   uint32_t from, float x1, float y1, uint32_t to);

/* An image (libimage's struct image: RGBA8888, RGB888 or GRAY8) scaled to
 * w x h at x, y and turned `angle` degrees clockwise about that
 * rectangle's centre; its alpha blends it over what is there. Between its
 * pixels it is read linearly (shrunk: averaged over each canvas pixel);
 * drawn at its own size on whole coordinates, or turned by right angles,
 * it is copied exactly. */
void draw_image(struct draw_canvas *c, const struct image *img, float x, float y, float w, float h,
                float angle);

#endif

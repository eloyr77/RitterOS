/*
 * Test: libdraw (41b). With no argument it draws fixed scenes (rectangles,
 * outlines, lines, polygons with a self-crossing star, curves, gradients,
 * images scaled and turned, transparency,
 * clipping, coordinates far outside, an RGBA image's layout, a canvas
 * drawn in several bands) and prints a checksum of each canvas: the test
 * compares them with the same program built on the Mac, so every machine
 * must draw the same pixels. `drawtest read` (on the Mac) takes shapes from
 * standard input and prints the canvas's coverage for scripts/drawref.py,
 * which computes every pixel's exact area. `drawtest window` (RitterOS)
 * draws on a window's own pixels (window_pixels) for a screenshot, and
 * (41c-3) a line of FreeSans at 24 pixels (wfont_open) beside it.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "draw.h"
#ifndef DRAW_HOST
#include <unistd.h>
#include "wfont.h"
#include "window.h"
#endif

static uint32_t *pixels;

/* A w x h canvas in a 0x00RRGGBB layout, black. */
static struct draw_canvas canvas(int w, int h)
{
    free(pixels);
    pixels = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    struct draw_canvas c = { pixels, w, h, w, 16, 8, 0, 0, 0, 0, 0, 0, 0 };
    return c;
}

/* (41b-3) An image of w x h in memory: kind 0 white, 1 a gray pattern
 * (pixel (i, j) is (7i + 13j) mod 256), 2 the pattern in RGBA with its
 * alpha going down by row, 3 the pattern in RGB. */
static struct image picture(int kind, int w, int h)
{
    int bpp = kind == 1 ? 1 : kind == 3 ? 3 : 4;
    struct image img = { w, h, w * bpp, kind == 1 ? PIXEL_GRAY8 : kind == 3 ? PIXEL_RGB888 : PIXEL_RGBA8888,
                         malloc((size_t)w * (size_t)h * (size_t)bpp) };
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint8_t v = (uint8_t)((7 * i + 13 * j) & 255), *p = img.pixels + (size_t)j * (size_t)img.stride +
                                                                (size_t)i * (size_t)bpp;
            if (kind == 0)
                p[0] = p[1] = p[2] = p[3] = 255;
            else if (kind == 1)
                p[0] = v;
            else {
                p[0] = v, p[1] = (uint8_t)(255 - v), p[2] = (uint8_t)(i * 16);
                if (kind == 2)
                    p[3] = (uint8_t)(255 - j * 255 / (h > 1 ? h - 1 : 1));
            }
        }
    return img;
}

static void checksum(const char *name, const struct draw_canvas *c)
{
    uint64_t hash = 0xcbf29ce484222325ULL;              /* FNV-1a over the pixels' values */
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) {
            uint32_t v = c->pixels[(size_t)y * (size_t)c->stride + (size_t)x];
            for (int k = 0; k < 4; k++) {
                hash ^= v >> (8 * k) & 255;
                hash *= 0x100000001b3ULL;
            }
        }
    printf("drawtest: %s %016llx\n", name, (unsigned long long)hash);
}

static void scenes(void)
{
    uint32_t white = draw_rgba(255, 255, 255, 255), red = draw_rgba(255, 0, 0, 255);
    struct draw_canvas c = canvas(64, 64);
    draw_rect(&c, 5.25f, 7.5f, 20.5f, 10.75f, 0, white);
    draw_rect(&c, 30, 30, 25, 20, 1, red);
    draw_rect(&c, 2.5f, 30.25f, 22, 30, 2.5f, draw_rgba(0, 200, 80, 255));
    draw_rect(&c, 40, 2, 6, 6, 4, white);                       /* (the outline fills it) */
    checksum("rects", &c);

    c = canvas(64, 64);
    draw_line(&c, 2, 2, 60, 9, 1, white);
    draw_line(&c, 5.5f, 60, 30.25f, 12.75f, 2.5f, red);
    draw_line(&c, 40, 20, 40, 60, 0.5f, white);
    draw_line(&c, 35, 40, 62, 40, 3, draw_rgba(30, 60, 255, 255));
    draw_line(&c, 10, 10, 10, 10, 2, white);                    /* (no length: nothing) */
    checksum("lines", &c);

    c = canvas(64, 64);
    float tri[] = { 3.5f, 4, 28, 9.25f, 12, 30 };
    float star[] = { 46, 2, 53, 24, 34, 10, 58, 10, 39, 24 };    /* (its middle wound twice) */
    float bent[] = { 4, 36, 30, 36, 30, 60, 18, 46, 4, 60 };
    float box[] = { 36, 34, 60, 34, 60, 60, 36, 60 };
    draw_polygon(&c, tri, 3, 0, white);
    draw_polygon(&c, star, 5, 0, red);
    draw_polygon(&c, bent, 5, 0, draw_rgba(0, 255, 255, 255));
    draw_polygon(&c, box, 4, 3, white);
    draw_polygon(&c, star, 5, 1.5f, draw_rgba(255, 255, 0, 255));
    checksum("polygons", &c);

    c = canvas(64, 64);
    draw_rect(&c, 0, 0, 64, 64, 0, draw_rgba(0, 0, 255, 255));
    draw_rect(&c, 8, 8, 40, 40, 0, draw_rgba(255, 0, 0, 128));
    draw_rect(&c, 20.5f, 20.5f, 40, 40, 0, draw_rgba(0, 255, 0, 64));
    draw_line(&c, 0, 63, 63, 0, 4, draw_rgba(255, 255, 255, 200));
    draw_rect(&c, 50, 2, 10, 10, 0, draw_rgba(255, 255, 255, 0));   /* (alpha 0: nothing) */
    checksum("alpha", &c);

    c = canvas(64, 64);
    draw_clip(&c, 10, 12, 30, 25);
    float big[] = { -20, -10, 90, 20, 30, 80 };
    draw_polygon(&c, big, 3, 0, white);
    draw_clip(&c, 50, 50, 40, 40);                              /* (half outside the canvas) */
    draw_rect(&c, 40, 40, 30, 30, 0, red);
    draw_clip(&c, 0, 0, 0, 0);                                  /* (empty: nothing) */
    draw_rect(&c, 0, 0, 64, 64, 0, red);
    draw_clip(&c, 0, 0, c.w, c.h);
    draw_line(&c, 0, 50, 63, 45, 1, draw_rgba(0, 255, 0, 255));
    checksum("clip", &c);

    c = canvas(64, 64);
    float huge[] = { -1e7f, -1e7f, 1e7f, -3e6f, 32, 1e9f };
    draw_polygon(&c, huge, 3, 0, draw_rgba(90, 90, 90, 255));
    float nan = strtof("nan", 0);
    draw_line(&c, nan, 3, 20, 40, 2, white);
    draw_rect(&c, 100, 100, 5, 5, 0, white);                    /* (outside) */
    draw_rect(&c, -30, 20, 40.5f, 10, 0, red);                  /* (partly left of it) */
    draw_line(&c, -1e5f, 50, 1e5f, 55, 1, white);
    checksum("far", &c);

    c = canvas(48, 32);
    c.red_shift = 0, c.green_shift = 8, c.blue_shift = 16, c.opaque = 0xFF000000u;  /* RGBA8888 */
    draw_rect(&c, 4, 4, 30, 20, 0, draw_rgba(200, 100, 50, 255));
    draw_rect(&c, 10.5f, 10.5f, 30, 15, 0, draw_rgba(10, 20, 250, 100));
    checksum("rgba", &c);

    c = canvas(96, 64);                                         /* (41b-2) */
    draw_circle(&c, 16, 16, 12.5f, 0, white);
    draw_circle(&c, 46.5f, 16.25f, 13, 2, red);
    draw_circle(&c, 70, 10, 0.4f, 0, white);                    /* (smaller than a pixel) */
    draw_circle(&c, 85, 20, 6, 9, draw_rgba(0, 255, 0, 255));   /* (the outline fills it) */
    draw_ellipse(&c, 20, 46, 18, 9.5f, 0, draw_rgba(0, 160, 255, 255));
    draw_ellipse(&c, 58, 46, 14, 16, 1.5f, white);
    draw_round_rect(&c, 76, 34, 18, 26, 5, 0, draw_rgba(255, 200, 0, 180));
    draw_round_rect(&c, 72.5f, 30.5f, 22, 30, 40, 1, white);    /* (radius more than half) */
    draw_circle(&c, 40, 300, 1000, 2, red);                     /* (a large one's edge) */
    checksum("curves", &c);

    c = canvas(96, 64);                                         /* (41b-3) */
    draw_gradient(&c, 2, 2, 40, 20, 2, 0, draw_rgba(255, 0, 0, 255), 42, 0, draw_rgba(0, 0, 255, 255));
    draw_gradient(&c, 50.5f, 2.25f, 40, 26, 0, 2, draw_rgba(0, 0, 0, 255), 0, 28, draw_rgba(255, 255, 255, 255));
    draw_gradient(&c, 2, 30, 60, 30, 10, 35, draw_rgba(0, 255, 0, 255), 50, 55, draw_rgba(255, 0, 255, 64));
    draw_gradient(&c, 66, 34, 26, 26, 70, 40, white, 70, 40, red);   /* (one point: `from`) */
    checksum("gradients", &c);

    c = canvas(128, 96);
    struct image gray = picture(1, 16, 12), rgba = picture(2, 16, 12), rgb = picture(3, 10, 6);
    draw_rect(&c, 0, 0, 128, 96, 0, draw_rgba(40, 40, 120, 255));
    draw_image(&c, &gray, 2, 2, 16, 12, 0);                     /* (its own size: a copy) */
    draw_image(&c, &rgba, 22, 2, 48, 36, 0);                    /* (three times as big) */
    draw_image(&c, &rgba, 74, 2, 5, 4, 0);                      /* (a third: 4 x 4 samples) */
    draw_image(&c, &rgb, 84.5f, 4.25f, 30, 20, 30);
    draw_image(&c, &gray, 4, 44, 16, 12, 90);
    draw_image(&c, &rgba, 30, 44, 16, 12, -90);
    draw_image(&c, &rgba, 56, 44, 40, 30, 200.5f);
    draw_image(&c, &gray, 100, 60, 0, 10, 0);                   /* (no width: nothing) */
    checksum("images", &c);
    free(gray.pixels), free(rgba.pixels), free(rgb.pixels);

    c = canvas(1000, 700);                                      /* (65 rows a band) */
    for (int i = 0; i < 40; i++) {
        float x = (float)(i * 23 % 900), y = (float)(i * 37 % 640);
        float poly[] = { x, y, x + 90.5f, y + 13, x + 40, y + 70.25f, x + 5, y + 40 };
        draw_polygon(&c, poly, 4, i % 3 ? 0 : 2, draw_rgba(i * 6, 255 - i * 6, 128, 160 + i));
        draw_line(&c, x, y, 999 - x, 699 - y, 0.75f + (float)(i % 4), draw_rgba(255, i * 6, 0, 255));
    }
    draw_rect(&c, 0, 0, 1000, 700, 3, white);
    checksum("bands", &c);
}

/* Shapes from standard input, one a line: canvas W H, clip X Y W H, rect X Y
 * W H LINE, line X0 Y0 X1 Y1 WIDTH, poly LINE N X Y..., circle X Y R LINE,
 * ellipse X Y RX RY LINE, rrect X Y W H RADIUS LINE, gradient X Y W H X0 Y0
 * X1 Y1 (black to white), image KIND IW IH X Y W H ANGLE (picture's kinds), dump (each pixel's
 * red channel, the coverage of white shapes on black, a row a line). */
static int read_shapes(void)
{
    char word[16];
    struct draw_canvas c = canvas(1, 1);
    uint32_t white = draw_rgba(255, 255, 255, 255);
    while (scanf("%15s", word) == 1) {
        if (!strcmp(word, "canvas")) {
            int w, h;
            if (scanf("%d %d", &w, &h) != 2)
                return 1;
            c = canvas(w, h);
        } else if (!strcmp(word, "clip")) {
            int x, y, w, h;
            if (scanf("%d %d %d %d", &x, &y, &w, &h) != 4)
                return 1;
            draw_clip(&c, x, y, w, h);
        } else if (!strcmp(word, "rect")) {
            float x, y, w, h, line;
            if (scanf("%f %f %f %f %f", &x, &y, &w, &h, &line) != 5)
                return 1;
            draw_rect(&c, x, y, w, h, line, white);
        } else if (!strcmp(word, "line")) {
            float x0, y0, x1, y1, width;
            if (scanf("%f %f %f %f %f", &x0, &y0, &x1, &y1, &width) != 5)
                return 1;
            draw_line(&c, x0, y0, x1, y1, width, white);
        } else if (!strcmp(word, "circle")) {
            float x, y, r, line;
            if (scanf("%f %f %f %f", &x, &y, &r, &line) != 4)
                return 1;
            draw_circle(&c, x, y, r, line, white);
        } else if (!strcmp(word, "ellipse")) {
            float x, y, rx, ry, line;
            if (scanf("%f %f %f %f %f", &x, &y, &rx, &ry, &line) != 5)
                return 1;
            draw_ellipse(&c, x, y, rx, ry, line, white);
        } else if (!strcmp(word, "rrect")) {
            float x, y, w, h, r, line;
            if (scanf("%f %f %f %f %f %f", &x, &y, &w, &h, &r, &line) != 6)
                return 1;
            draw_round_rect(&c, x, y, w, h, r, line, white);
        } else if (!strcmp(word, "gradient")) {
            float x, y, w, h, x0, y0, x1, y1;
            if (scanf("%f %f %f %f %f %f %f %f", &x, &y, &w, &h, &x0, &y0, &x1, &y1) != 8)
                return 1;
            draw_gradient(&c, x, y, w, h, x0, y0, draw_rgba(0, 0, 0, 255), x1, y1, white);
        } else if (!strcmp(word, "image")) {
            int kind, iw, ih;
            float x, y, w, h, angle;
            if (scanf("%d %d %d %f %f %f %f %f", &kind, &iw, &ih, &x, &y, &w, &h, &angle) != 8 ||
                iw < 1 || ih < 1 || iw > 1024 || ih > 1024)
                return 1;
            struct image img = picture(kind, iw, ih);
            draw_image(&c, &img, x, y, w, h, angle);
            free(img.pixels);
        } else if (!strcmp(word, "poly")) {
            float line;
            int n;
            if (scanf("%f %d", &line, &n) != 2 || n < 1 || n > 4096)
                return 1;
            float *xy = malloc(sizeof(float) * 2 * (size_t)n);
            for (int i = 0; i < 2 * n; i++)
                if (scanf("%f", &xy[i]) != 1)
                    return 1;
            draw_polygon(&c, xy, n, line, white);
            free(xy);
        } else if (!strcmp(word, "dump")) {
            printf("canvas %d %d\n", c.w, c.h);
            for (int y = 0; y < c.h; y++) {
                for (int x = 0; x < c.w; x++)
                    printf("%02x", (unsigned)(c.pixels[(size_t)y * (size_t)c.w + (size_t)x] >> 16 & 255));
                printf("\n");
            }
        } else
            return 1;
    }
    return 0;
}

#ifndef DRAW_HOST
/* On a window's own pixels: a white square with a red half-transparent one
 * over its corner and a 3-pixel green line, until the window closes or
 * SECONDS pass. */
static int window_scene(int seconds)
{
    struct window *win = window_open(200, 150, "drawtest");
    if (!win) {
        printf("drawtest: cannot open a window\n");
        return 1;
    }
    struct draw_canvas c = { window_pixels(win), window_width(win), window_height(win),
                             window_width(win), 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    window_shifts(win, &c.red_shift, &c.green_shift, &c.blue_shift);
    draw_rect(&c, 0, 0, 200, 150, 0, draw_rgba(0, 0, 0, 255));
    draw_rect(&c, 20, 20, 80, 60, 0, draw_rgba(255, 255, 255, 255));
    draw_rect(&c, 60, 50, 80, 60, 0, draw_rgba(255, 0, 0, 128));
    draw_line(&c, 10, 130, 190, 130, 3, draw_rgba(0, 255, 0, 255));
    const struct font *sans = wfont_open("FreeSans.ttf", 24);   /* (41c-3) smooth text, top right */
    if (sans)
        window_text_font(win, sans, 108, 4, "Ritter", window_rgb(win, 255, 255, 255), WINDOW_TRANSPARENT);
    window_flush(win);
    printf("drawtest: window drawn\n");
    struct window_event ev;
    for (int left = seconds * 1000; left > 0; left -= 100)
        if (window_wait_timeout(win, &ev, 100) < 0)
            break;
    window_close(win);
    return 0;
}
#endif

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "read"))
        return read_shapes();
#ifndef DRAW_HOST
    if (argc > 1 && !strcmp(argv[1], "window"))
        return window_scene(argc > 2 ? atoi(argv[2]) : 30);
#endif
    scenes();
    printf("drawtest: done\n");
    return 0;
}

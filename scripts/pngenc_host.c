/*
 * pngenc_host (41h-2): libimage's DEFLATE compressor (user/lib/deflate.c)
 * and PNG writer (user/lib/png.c's encode_png) built on the Mac, under
 * ASan and UBSan, by test.sh's pngenc_host_test; scripts/pngcheck.py reads
 * what it writes with Python's zlib, an inflater not ours.
 *
 *   pngenc_host deflate IN OUT              IN's bytes as a zlib stream
 *   pngenc_host png KIND W H SEED OUT RAW   a picture of KIND as OUT (PNG)
 *                                           and its RGBA bytes as RAW
 *
 * KINDs: noise (random, opaque), alpha (random with transparency), flat,
 * gradient, stripes (a 7-pixel period), screen (rectangles and dots, as a
 * desktop), far (rows repeated 5 rows apart: matches past 32 KiB on a wide
 * picture).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "image.h"
#include "imagedec.h"

int image_alloc(struct image *out, int w, int h)
{
    out->w = w, out->h = h, out->stride = w * 4, out->format = PIXEL_RGBA8888;
    out->pixels = calloc((size_t)w * (size_t)h, 4);
    return out->pixels ? 0 : -1;
}

void image_free(struct image *img)
{
    free(img->pixels);
    memset(img, 0, sizeof(*img));
}

static uint32_t seed;

static uint32_t rnd(void)
{
    seed = seed * 1103515245u + 12345u;
    return seed >> 8;
}

static int write_file(const char *path, const void *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(p, 1, n, f) != n) {
        perror(path);
        return -1;
    }
    return fclose(f);
}

static void paint(struct image *img, const char *kind)
{
    int w = img->w, h = img->h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *p = img->pixels + (size_t)y * img->stride + (size_t)x * 4;
            uint32_t r = 0, g = 0, b = 0, a = 255;
            if (!strcmp(kind, "noise") || !strcmp(kind, "alpha")) {
                uint32_t v = rnd();
                r = v & 255, g = v >> 8 & 255, b = v >> 16 & 255;
                if (kind[0] == 'a')
                    a = rnd() & 255;
            } else if (!strcmp(kind, "flat")) {
                r = 46, g = 52, b = 64;
            } else if (!strcmp(kind, "gradient")) {
                r = (uint32_t)(x * 255 / (w > 1 ? w - 1 : 1)), g = (uint32_t)(y * 255 / (h > 1 ? h - 1 : 1));
                b = (r + g) / 2;
            } else if (!strcmp(kind, "stripes")) {
                r = g = b = (uint32_t)(x % 7) * 36;
            } else if (!strcmp(kind, "screen")) {
                r = 46, g = 52, b = 64;                     /* the desktop */
                if (x > w / 8 && x < w / 2 && y > h / 8 && y < h / 2) {
                    r = 76, g = 86, b = 106;                /* a window's frame */
                    if (y > h / 8 + 24)
                        r = 236, g = 239, b = 244;          /* its content */
                    if ((x * 7 + y * 3) % 11 == 0)
                        r = g = b = 30;                     /* "text" */
                }
            } else if (!strcmp(kind, "far")) {
                uint32_t v = (uint32_t)(x * 2654435761u ^ (uint32_t)(y % 5) * 40503u);
                r = v & 255, g = v >> 8 & 255, b = v >> 16 & 255;
            }
            p[0] = (uint8_t)r, p[1] = (uint8_t)g, p[2] = (uint8_t)b, p[3] = (uint8_t)a;
        }
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "deflate")) {
        FILE *f = fopen(argv[2], "rb");
        if (!f) {
            perror(argv[2]);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *in = malloc(n ? (size_t)n : 1), *out;
        if (!in || fread(in, 1, (size_t)n, f) != (size_t)n)
            return 1;
        fclose(f);
        long zn = deflate_zlib(in, (size_t)n, &out);
        if (zn < 0 || write_file(argv[3], out, (size_t)zn) < 0)
            return 1;
        free(in);
        free(out);
        return 0;
    }
    if (argc == 8 && !strcmp(argv[1], "png")) {
        struct image img;
        seed = (uint32_t)atoi(argv[5]);
        if (image_alloc(&img, atoi(argv[3]), atoi(argv[4])) < 0)
            return 1;
        paint(&img, argv[2]);
        uint8_t *png;
        long n = encode_png(&img, &png);
        if (n < 0 || write_file(argv[6], png, (size_t)n) < 0 ||
            write_file(argv[7], img.pixels, (size_t)img.w * img.h * 4) < 0)
            return 1;
        free(png);
        image_free(&img);
        return 0;
    }
    fprintf(stderr, "usage: pngenc_host deflate IN OUT | png KIND W H SEED OUT RAW\n");
    return 2;
}

/*
 * libimage (see image.h). PPM (P6), BMP (24/32-bit, uncompressed), PNG
 * (png.c, with inflate.c) and JPEG (jpeg.c), each decoded to RGBA8888.
 * Parsers are defensive about their input (bounds checked), since images
 * come from other machines.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "image.h"
#include "imagedec.h"

static int fail(int e)
{
    errno = e;
    return -1;
}

/* Read a whole file into a malloc'd buffer; 0 on success, -1/errno. */
static int read_all(const char *path, uint8_t **out, long *n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    long cap = 65536, len = 0;
    uint8_t *buf = malloc((size_t)cap);
    if (!buf) {
        close(fd);
        return fail(ENOMEM);
    }
    for (;;) {
        if (len == cap) {
            long ncap = cap * 2;
            uint8_t *nb = realloc(buf, (size_t)ncap);
            if (!nb) {
                free(buf);
                close(fd);
                return fail(ENOMEM);
            }
            buf = nb, cap = ncap;
        }
        ssize_t r = read(fd, buf + len, (size_t)(cap - len));
        if (r < 0) {
            free(buf);
            close(fd);
            return -1;
        }
        if (r == 0)
            break;
        len += r;
    }
    close(fd);
    *out = buf, *n = len;
    return 0;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Allocate out->pixels for w x h RGBA8888. */
int image_alloc(struct image *out, int w, int h)
{
    if (w <= 0 || h <= 0 || w > 1 << 15 || h > 1 << 15)
        return fail(EINVAL);
    long bytes = (long)w * h * 4;
    uint8_t *px = malloc((size_t)bytes);
    if (!px)
        return fail(ENOMEM);
    out->w = w, out->h = h, out->stride = w * 4;
    out->format = PIXEL_RGBA8888;
    out->pixels = px;
    return 0;
}

/* --- PPM (P6) ------------------------------------------------------------- */

/* Advance *at over whitespace and # comments; false if the header runs out. */
static int ppm_skip(const uint8_t *d, long n, long *at)
{
    for (;;) {
        while (*at < n && (d[*at] == ' ' || d[*at] == '\t' || d[*at] == '\n' || d[*at] == '\r'))
            (*at)++;
        if (*at < n && d[*at] == '#') {
            while (*at < n && d[*at] != '\n')
                (*at)++;
        } else {
            return *at < n;
        }
    }
}

static int ppm_num(const uint8_t *d, long n, long *at, int *out)
{
    if (!ppm_skip(d, n, at) || d[*at] < '0' || d[*at] > '9')
        return 0;
    long v = 0;
    while (*at < n && d[*at] >= '0' && d[*at] <= '9') {
        v = v * 10 + (d[*at] - '0');
        if (v > 1 << 20)
            return 0;
        (*at)++;
    }
    *out = (int)v;
    return 1;
}

static int decode_ppm(const uint8_t *d, long n, struct image *out, int header_only)
{
    long at = 2;                                /* past "P6" */
    int w, h, maxval;
    if (!ppm_num(d, n, &at, &w) || !ppm_num(d, n, &at, &h) || !ppm_num(d, n, &at, &maxval))
        return fail(EINVAL);
    if (maxval != 255)
        return fail(EINVAL);                    /* 8-bit only */
    at++;                                       /* the single whitespace before the data */
    if (header_only) {
        out->w = w, out->h = h, out->stride = w * 4;
        out->format = PIXEL_RGBA8888, out->pixels = 0;
        return 0;
    }
    if (n - at < (long)w * h * 3)
        return fail(EINVAL);
    if (image_alloc(out, w, h) < 0)
        return -1;
    const uint8_t *s = d + at;
    uint8_t *p = out->pixels;
    for (long i = 0; i < (long)w * h; i++) {
        *p++ = s[i * 3 + 0];
        *p++ = s[i * 3 + 1];
        *p++ = s[i * 3 + 2];
        *p++ = 255;
    }
    return 0;
}

/* --- BMP (24/32-bit, uncompressed, BI_RGB) -------------------------------- */

static int decode_bmp(const uint8_t *d, long n, struct image *out, int header_only)
{
    if (n < 54)
        return fail(EINVAL);
    uint32_t offset = le32(d + 10);
    int32_t w = (int32_t)le32(d + 18);
    int32_t h = (int32_t)le32(d + 22);
    uint16_t bpp = le16(d + 28);
    uint32_t comp = le32(d + 30);
    int top_down = h < 0;
    if (top_down)
        h = -h;
    if (comp != 0 || (bpp != 24 && bpp != 32) || w <= 0 || h <= 0)
        return fail(EINVAL);
    if (header_only) {
        out->w = w, out->h = h, out->stride = w * 4;
        out->format = PIXEL_RGBA8888, out->pixels = 0;
        return 0;
    }
    int bypp = bpp / 8;
    long row = ((long)w * bypp + 3) & ~3L;      /* rows padded to 4 bytes */
    if ((long)offset + row * h > n)
        return fail(EINVAL);
    if (image_alloc(out, w, h) < 0)
        return -1;
    for (int y = 0; y < h; y++) {
        int sy = top_down ? y : h - 1 - y;      /* BMP rows are bottom-up by default */
        const uint8_t *s = d + offset + (long)sy * row;
        uint8_t *p = out->pixels + (long)y * w * 4;
        for (int x = 0; x < w; x++) {
            p[0] = s[2];                        /* BGR(A) -> RGBA */
            p[1] = s[1];
            p[2] = s[0];
            p[3] = bypp == 4 ? s[3] : 255;
            p += 4, s += bypp;
        }
    }
    return 0;
}

/* --- the public entry points ---------------------------------------------- */

static int decode(const uint8_t *d, long n, struct image *out, int header_only)
{
    if (n >= 2 && d[0] == 'P' && d[1] == '6')
        return decode_ppm(d, n, out, header_only);
    if (n >= 2 && d[0] == 'B' && d[1] == 'M')
        return decode_bmp(d, n, out, header_only);
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G')
        return decode_png(d, n, out, header_only);
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
        return decode_jpeg(d, n, out, header_only);
    return fail(EINVAL);
}

int image_load_memory(const void *data, long n, struct image *out)
{
    return decode(data, n, out, 0);
}

int image_load(const char *path, struct image *out)
{
    uint8_t *data;
    long n;
    if (read_all(path, &data, &n) < 0)
        return -1;
    int r = decode(data, n, out, 0);
    free(data);
    return r;
}

int image_info(const char *path, struct image *out)
{
    uint8_t *data;
    long n;
    if (read_all(path, &data, &n) < 0)
        return -1;
    int r = decode(data, n, out, 1);
    free(data);
    return r;
}

void image_free(struct image *img)
{
    if (img && img->pixels) {
        free(img->pixels);
        memset(img, 0, sizeof(*img));
    }
}

int image_scale(const struct image *src, int w, int h, struct image *out)
{
    if (!src || !src->pixels)
        return fail(EINVAL);
    if (image_alloc(out, w, h) < 0)
        return -1;
    for (int y = 0; y < h; y++) {
        int sy = (int)((long)y * src->h / h);
        const uint8_t *srow = src->pixels + (long)sy * src->stride;
        uint8_t *drow = out->pixels + (long)y * out->stride;
        for (int x = 0; x < w; x++) {
            int sx = (int)((long)x * src->w / w);
            memcpy(drow + x * 4, srow + sx * 4, 4);
        }
    }
    return 0;
}

static int write_all(int fd, const void *p, long n)
{
    const uint8_t *b = p;
    while (n > 0) {
        ssize_t w = write(fd, b, (size_t)n);
        if (w <= 0)
            return -1;
        b += w, n -= w;
    }
    return 0;
}

int image_save(const char *path, const struct image *img)
{
    if (!img || !img->pixels)
        return fail(EINVAL);
    size_t pl = strlen(path);
    int bmp = pl >= 4 && strcmp(path + pl - 4, ".bmp") == 0;
    int png = pl >= 4 && strcmp(path + pl - 4, ".png") == 0;   /* (41h-2) */
    uint8_t *png_data = 0;
    long png_n = png ? encode_png(img, &png_data) : 0;
    if (png_n < 0)
        return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        free(png_data);
        return -1;
    }
    int rc = 0;
    if (png) {
        rc = write_all(fd, png_data, png_n);
        free(png_data);
    } else if (bmp) {
        long row = ((long)img->w * 3 + 3) & ~3L;
        long datasz = row * img->h;
        uint8_t hdr[54] = {0};
        hdr[0] = 'B', hdr[1] = 'M';
        uint32_t total = 54 + (uint32_t)datasz;
        hdr[2] = total, hdr[3] = total >> 8, hdr[4] = total >> 16, hdr[5] = total >> 24;
        hdr[10] = 54;
        hdr[14] = 40;                                   /* DIB header size */
        hdr[18] = img->w, hdr[19] = img->w >> 8, hdr[20] = img->w >> 16, hdr[21] = img->w >> 24;
        hdr[22] = img->h, hdr[23] = img->h >> 8, hdr[24] = img->h >> 16, hdr[25] = img->h >> 24;
        hdr[26] = 1;                                    /* planes */
        hdr[28] = 24;                                   /* bpp */
        rc = write_all(fd, hdr, 54);
        uint8_t *line = calloc(1, (size_t)row);         /* (63e: a write per row, not per pixel) */
        if (!line)
            rc = -1;
        for (int y = img->h - 1; y >= 0 && rc == 0; y--) {   /* (bottom row first; padded to 4) */
            const uint8_t *p = img->pixels + (long)y * img->stride;
            for (int x = 0; x < img->w; x++) {
                line[x * 3] = p[x * 4 + 2], line[x * 3 + 1] = p[x * 4 + 1], line[x * 3 + 2] = p[x * 4];
            }
            rc = write_all(fd, line, row);
        }
        free(line);
    } else {                                            /* PPM (P6) */
        char h[64];
        int hn = 0;
        const char *fmt = "P6\n";
        for (const char *c = fmt; *c; c++) h[hn++] = *c;
        hn += 0;
        /* "W H\n255\n" built by hand (no snprintf dependency assumptions) */
        char num[32];
        int k;
        int vals[2] = { img->w, img->h };
        for (int vi = 0; vi < 2; vi++) {
            int v = vals[vi], d = 0;
            char tmp[16];
            if (v == 0) tmp[d++] = '0';
            while (v > 0) { tmp[d++] = (char)('0' + v % 10); v /= 10; }
            for (k = d - 1; k >= 0; k--) h[hn++] = tmp[k];
            h[hn++] = vi == 0 ? ' ' : '\n';
        }
        const char *mx = "255\n";
        for (const char *c = mx; *c; c++) h[hn++] = *c;
        (void)num;
        rc = write_all(fd, h, hn);
        uint8_t *line = rc == 0 ? malloc((size_t)img->w * 3 + 1) : 0;
        if (!line)
            rc = -1;
        for (int y = 0; y < img->h && rc == 0; y++) {  /* (63e: a write per row) */
            const uint8_t *p = img->pixels + (long)y * img->stride;
            for (int x = 0; x < img->w; x++)
                memcpy(line + x * 3, p + x * 4, 3);
            rc = write_all(fd, line, (long)img->w * 3);
        }
        free(line);
    }
    if (close(fd) < 0)
        rc = -1;
    return rc;
}

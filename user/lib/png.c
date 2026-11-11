/*
 * PNG (the W3C's Portable Network Graphics, second edition), decoded to
 * RGBA8888: every colour type (grey, RGB, palette, grey + alpha, RGBA), bit
 * depths 1-16, transparency (tRNS), Adam7 interlacing. Each chunk's CRC is
 * checked, the image data is inflated (inflate.c) into exactly the size the
 * header promises, then unfiltered row by row. 16-bit samples keep their
 * high byte. Not decoded: the colour-management chunks (gAMA, cHRM, sRGB,
 * iCCP: the pixels are shown as stored) and APNG's animation (its first
 * frame, the IDAT image, is what shows).
 *
 * (41h-2) Encoded too (encode_png): 8-bit RGBA, or RGB when nothing is
 * transparent, not interlaced, each row's filter the one whose bytes sum
 * smallest as signed values (the specification's suggestion), the data
 * compressed by deflate.c.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "image.h"
#include "imagedec.h"
#include "ritteros.h"

static int fail(int e)
{
    errno = e;
    return -1;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

struct png {
    uint32_t w, h;
    int depth, type, interlace;
    int channels;                               /* samples per pixel */
    uint8_t palette[256][4];                    /* RGBA (alpha from tRNS) */
    int colours;                                /* palette entries */
    bool key;                                   /* tRNS for grey/RGB: this colour is */
    uint16_t key_rgb[3];                        /* transparent (grey: key_rgb[0]) */
};

/* The colour types and depths the specification allows. */
static bool valid(const struct png *p)
{
    switch (p->type) {
    case 0: return p->depth == 1 || p->depth == 2 || p->depth == 4 || p->depth == 8 ||
                   p->depth == 16;
    case 3: return p->depth == 1 || p->depth == 2 || p->depth == 4 || p->depth == 8;
    case 2: case 4: case 6: return p->depth == 8 || p->depth == 16;
    }
    return false;
}

static const int channels_of[7] = { 1, 0, 3, 1, 2, 0, 4 };

/* Adam7's seven passes: where each starts and its steps (x0, y0, dx, dy). */
static const uint8_t adam7[7][4] = {
    { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 },
    { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 } };

static size_t row_bytes(const struct png *p, uint32_t w)
{
    return ((size_t)w * (size_t)p->channels * (size_t)p->depth + 7) / 8;
}

/* A pass's size (the whole image when not interlaced). */
static void pass_size(const struct png *p, int pass, uint32_t *pw, uint32_t *ph)
{
    if (!p->interlace) {
        *pw = p->w, *ph = p->h;
        return;
    }
    const uint8_t *a = adam7[pass];
    *pw = p->w > a[0] ? (p->w - a[0] + a[2] - 1) / a[2] : 0;
    *ph = p->h > a[1] ? (p->h - a[1] + a[3] - 1) / a[3] : 0;
}

static int magnitude(int v)
{
    return v < 0 ? -v : v;
}

static int paeth(int a, int b, int c)
{
    int pa = magnitude(b - c), pb = magnitude(a - c), pc = magnitude(a + b - 2 * c);
    if (pa <= pb && pa <= pc)
        return a;
    return pb <= pc ? b : c;
}

/* Undo row filters in place: rows of rb bytes, each after its filter type
 * byte; bpp the bytes per pixel the filters step by (at least 1). */
static bool unfilter(uint8_t *data, uint32_t rows, size_t rb, size_t bpp)
{
    uint8_t *prev = 0;
    for (uint32_t y = 0; y < rows; y++) {
        uint8_t *row = data + (size_t)y * (rb + 1);
        uint8_t type = row[0];
        uint8_t *r = row + 1;
        uint8_t *u = prev ? prev + 1 : 0;       /* the row above (unfiltered), or none */
        switch (type) {
        case 0:
            break;
        case 1:                                 /* Sub */
            for (size_t i = bpp; i < rb; i++)
                r[i] = (uint8_t)(r[i] + r[i - bpp]);
            break;
        case 2:                                 /* Up */
            if (u)
                for (size_t i = 0; i < rb; i++)
                    r[i] = (uint8_t)(r[i] + u[i]);
            break;
        case 3:                                 /* Average */
            for (size_t i = 0; i < rb; i++) {
                int left = i >= bpp ? r[i - bpp] : 0, up = u ? u[i] : 0;
                r[i] = (uint8_t)(r[i] + ((left + up) >> 1));
            }
            break;
        case 4:                                 /* Paeth */
            for (size_t i = 0; i < rb; i++) {
                int left = i >= bpp ? r[i - bpp] : 0, up = u ? u[i] : 0;
                int corner = (i >= bpp && u) ? u[i - bpp] : 0;
                r[i] = (uint8_t)(r[i] + paeth(left, up, corner));
            }
            break;
        default:
            return false;
        }
        prev = row;
    }
    return true;
}

/* Sample k of a row (any depth), as stored. */
static unsigned sample(const struct png *p, const uint8_t *r, size_t k)
{
    switch (p->depth) {
    case 16: return (unsigned)(r[2 * k] << 8 | r[2 * k + 1]);
    case 8: return r[k];
    default: {
        size_t bit = k * (size_t)p->depth;
        unsigned byte = r[bit / 8];
        int shift = 8 - p->depth - (int)(bit % 8);
        return (byte >> shift) & ((1u << p->depth) - 1);
    }
    }
}

/* A sample scaled to 8 bits (16-bit: its high byte; below 8: stretched). */
static uint8_t to8(const struct png *p, unsigned v)
{
    if (p->depth == 16)
        return (uint8_t)(v >> 8);
    if (p->depth == 8)
        return (uint8_t)v;
    return (uint8_t)(v * 255 / ((1u << p->depth) - 1));
}

/* One pass's unfiltered rows into the image's RGBA pixels. */
static void place(const struct png *p, const uint8_t *data, int pass, uint32_t pw,
                  uint32_t ph, struct image *out)
{
    size_t rb = row_bytes(p, pw);
    const uint8_t *a = p->interlace ? adam7[pass] : (const uint8_t[4]){ 0, 0, 1, 1 };
    for (uint32_t y = 0; y < ph; y++) {
        const uint8_t *r = data + (size_t)y * (rb + 1) + 1;
        uint8_t *dst = out->pixels + (size_t)(a[1] + y * a[3]) * (size_t)out->stride;
        for (uint32_t x = 0; x < pw; x++) {
            uint8_t *px = dst + (size_t)(a[0] + x * a[2]) * 4;
            size_t k = (size_t)x * (size_t)p->channels;
            unsigned v0 = sample(p, r, k), v1, v2;
            switch (p->type) {
            case 0:                             /* grey */
                px[0] = px[1] = px[2] = to8(p, v0);
                px[3] = p->key && v0 == p->key_rgb[0] ? 0 : 255;
                break;
            case 2:                             /* RGB */
                v1 = sample(p, r, k + 1), v2 = sample(p, r, k + 2);
                px[0] = to8(p, v0), px[1] = to8(p, v1), px[2] = to8(p, v2);
                px[3] = p->key && v0 == p->key_rgb[0] && v1 == p->key_rgb[1] &&
                        v2 == p->key_rgb[2] ? 0 : 255;
                break;
            case 3:                             /* palette (an index past it: black) */
                memcpy(px, v0 < (unsigned)p->colours ? p->palette[v0] :
                       (const uint8_t[4]){ 0, 0, 0, 255 }, 4);
                break;
            case 4:                             /* grey + alpha */
                px[0] = px[1] = px[2] = to8(p, v0);
                px[3] = to8(p, sample(p, r, k + 1));
                break;
            case 6:                             /* RGBA */
                px[0] = to8(p, v0);
                px[1] = to8(p, sample(p, r, k + 1));
                px[2] = to8(p, sample(p, r, k + 2));
                px[3] = to8(p, sample(p, r, k + 3));
                break;
            }
        }
    }
}

int decode_png(const uint8_t *d, long n, struct image *out, int header_only)
{
    static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (n < 8 + 25 || memcmp(d, signature, 8))
        return fail(EINVAL);
    struct png p;
    memset(&p, 0, sizeof(p));
    for (int i = 0; i < 256; i++)
        p.palette[i][3] = 255;
    uint8_t *idat = 0;                          /* the joined image data */
    size_t idat_n = 0, idat_cap = 0;
    bool header = false, end = false;
    long at = 8;
    while (at + 12 <= n && !end) {
        uint32_t len = be32(d + at);
        const uint8_t *type = d + at + 4, *body = d + at + 8;
        if (len > (uint32_t)(n - at - 12))
            goto bad;                           /* runs past the file */
        if (crc32(type, (size_t)len + 4) != be32(body + len))
            goto bad;                           /* damaged */
        if (!memcmp(type, "IHDR", 4)) {
            if (len != 13 || header)
                goto bad;
            p.w = be32(body), p.h = be32(body + 4);
            p.depth = body[8], p.type = body[9], p.interlace = body[12];
            if (!p.w || !p.h || p.w > 0x7FFFFFFF || p.h > 0x7FFFFFFF || !valid(&p) ||
                body[10] || body[11] || p.interlace > 1)
                goto bad;
            if ((uint64_t)p.w * p.h > (uint64_t)IMAGE_MAX_PIXELS)
                goto bad;                       /* too big to hold */
            p.channels = channels_of[p.type];
            header = true;
            if (header_only) {
                free(idat);
                out->w = (int)p.w, out->h = (int)p.h, out->stride = (int)p.w * 4;
                out->format = PIXEL_RGBA8888, out->pixels = 0;
                return 0;
            }
        } else if (!header) {
            goto bad;                           /* IHDR must come first */
        } else if (!memcmp(type, "PLTE", 4)) {
            if (len % 3 || len / 3 > 256 || len == 0)
                goto bad;
            p.colours = (int)(len / 3);
            for (int i = 0; i < p.colours; i++)
                memcpy(p.palette[i], body + 3 * i, 3);
        } else if (!memcmp(type, "tRNS", 4)) {
            if (p.type == 3) {                  /* alpha per palette entry */
                for (uint32_t i = 0; i < len && i < 256; i++)
                    p.palette[i][3] = body[i];
            } else if (p.type == 0 && len >= 2) {
                p.key = true, p.key_rgb[0] = (uint16_t)(body[0] << 8 | body[1]);
            } else if (p.type == 2 && len >= 6) {
                p.key = true;
                for (int c = 0; c < 3; c++)
                    p.key_rgb[c] = (uint16_t)(body[2 * c] << 8 | body[2 * c + 1]);
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            if (idat_n + len > idat_cap) {
                size_t cap = idat_cap ? idat_cap * 2 : 65536;
                while (cap < idat_n + len)
                    cap *= 2;
                uint8_t *grown = realloc(idat, cap);
                if (!grown)
                    goto nomem;
                idat = grown, idat_cap = cap;
            }
            memcpy(idat + idat_n, body, len);
            idat_n += len;
        } else if (!memcmp(type, "IEND", 4)) {
            end = true;
        } else if (!(type[0] & 0x20)) {
            goto bad;                           /* a critical chunk we do not know */
        }
        at += 12 + (long)len;
    }
    if (!header || !idat_n || (p.type == 3 && !p.colours))
        goto bad;

    /* How many bytes the decompressed data must be: each pass's rows, each
     * with its filter byte. */
    size_t raw_n = 0;
    for (int pass = 0; pass < (p.interlace ? 7 : 1); pass++) {
        uint32_t pw, ph;
        pass_size(&p, pass, &pw, &ph);
        if (pw && ph)
            raw_n += (size_t)ph * (row_bytes(&p, pw) + 1);
    }
    uint8_t *raw = malloc(raw_n ? raw_n : 1);
    if (!raw)
        goto nomem;
    long got = inflate_zlib(idat, idat_n, raw, raw_n);
    free(idat);
    idat = 0;
    if (got != (long)raw_n) {
        free(raw);
        goto bad;
    }
    if (image_alloc(out, (int)p.w, (int)p.h) < 0) {
        free(raw);
        return -1;
    }
    size_t bpp = ((size_t)p.channels * (size_t)p.depth + 7) / 8;
    uint8_t *pass_data = raw;
    for (int pass = 0; pass < (p.interlace ? 7 : 1); pass++) {
        uint32_t pw, ph;
        pass_size(&p, pass, &pw, &ph);
        if (!pw || !ph)
            continue;
        size_t rb = row_bytes(&p, pw);
        if (!unfilter(pass_data, ph, rb, bpp)) {
            free(raw);
            image_free(out);
            return fail(EINVAL);
        }
        place(&p, pass_data, pass, pw, ph, out);
        pass_data += (size_t)ph * (rb + 1);
    }
    free(raw);
    return 0;
bad:
    free(idat);
    return fail(EINVAL);
nomem:
    free(idat);
    return fail(ENOMEM);
}

/* --- encoding (41h-2) -------------------------------------------------- */

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

/* A chunk of `len` bytes of data at out + 8 (its type and data already
 * there, from out + 4): its length before them and its CRC after. */
static size_t chunk_close(uint8_t *out, uint32_t len)
{
    put_be32(out, len);
    put_be32(out + 8 + len, crc32(out + 4, (size_t)len + 4));
    return (size_t)len + 12;
}

long encode_png(const struct image *img, uint8_t **out)
{
    size_t w = (size_t)img->w, h = (size_t)img->h;
    bool alpha = false;
    for (size_t y = 0; y < h && !alpha; y++)
        for (size_t x = 0; x < w; x++)
            if (img->pixels[y * (size_t)img->stride + x * 4 + 3] != 0xFF) {
                alpha = true;
                break;
            }
    size_t bpp = alpha ? 4 : 3, rb = w * bpp;
    uint8_t *rows = malloc(h * (rb + 1)), *cur = malloc(rb), *prior = calloc(1, rb ? rb : 1);
    uint8_t *trial = malloc(rb + 1);
    if (!rows || !cur || !prior || !trial) {
        free(rows), free(cur), free(prior), free(trial);
        return fail(ENOMEM);
    }
    for (size_t y = 0; y < h; y++) {
        const uint8_t *p = img->pixels + y * (size_t)img->stride;
        for (size_t x = 0; x < w; x++)
            memcpy(cur + x * bpp, p + x * 4, bpp);
        uint8_t *dst = rows + y * (rb + 1);
        unsigned long best_sum = ~0ul;
        for (int f = 0; f < 5; f++) {           /* each filter; the smallest sum wins */
            unsigned long sum = 0;
            trial[0] = (uint8_t)f;
            for (size_t i = 0; i < rb; i++) {
                int a = i >= bpp ? cur[i - bpp] : 0, b = prior[i], c = i >= bpp ? prior[i - bpp] : 0;
                int pred = f == 0 ? 0 : f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : paeth(a, b, c);
                uint8_t v = (uint8_t)(cur[i] - pred);
                trial[i + 1] = v;
                sum += v < 128 ? v : 256 - v;
            }
            if (sum < best_sum) {
                best_sum = sum;
                memcpy(dst, trial, rb + 1);
            }
        }
        memcpy(prior, cur, rb);
    }
    free(cur), free(prior), free(trial);
    uint8_t *z;
    long zn = deflate_zlib(rows, h * (rb + 1), &z);
    free(rows);
    if (zn < 0)
        return fail(ENOMEM);
    size_t total = 8 + 25 + ((size_t)zn + 12) + 12;
    uint8_t *f = malloc(total);
    if (!f) {
        free(z);
        return fail(ENOMEM);
    }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    size_t at = 0;
    memcpy(f, sig, 8);
    at = 8;
    memcpy(f + at + 4, "IHDR", 4);
    put_be32(f + at + 8, (uint32_t)w);
    put_be32(f + at + 12, (uint32_t)h);
    f[at + 16] = 8;                             /* bits a sample */
    f[at + 17] = alpha ? 6 : 2;                 /* RGBA or RGB */
    f[at + 18] = f[at + 19] = f[at + 20] = 0;   /* deflate, adaptive filters, not interlaced */
    at += chunk_close(f + at, 13);
    memcpy(f + at + 4, "IDAT", 4);
    memcpy(f + at + 8, z, (size_t)zn);
    at += chunk_close(f + at, (uint32_t)zn);
    free(z);
    memcpy(f + at + 4, "IEND", 4);
    at += chunk_close(f + at, 0);
    *out = f;
    return (long)at;
}

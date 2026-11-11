/*
 * JPEG (ITU-T T.81, with JFIF's and Adobe's colour conventions): baseline,
 * extended sequential and progressive images with Huffman coding and 8-bit
 * samples, grey (one component) or colour (three: YCbCr, or RGB when the
 * file says so), any sampling factors (4:4:4, 4:2:2, 4:2:0, ...),
 * interleaved or not, restart markers. Arithmetic coding, 12-bit samples,
 * lossless and hierarchical modes and four-component (CMYK) files are
 * refused (EINVAL). Our own code from the standard. EXIF's orientation is
 * applied (as browsers and photo viewers do): a portrait photo, stored
 * landscape by the camera, comes out portrait.
 *
 * Decoding: a sequential scan's blocks are Huffman decoded, dequantized and
 * turned back into 8x8 samples at once; a progressive image's scans each add
 * a band of coefficients or one more bit of them (G.1.2: DC first and
 * refined, AC bands with end-of-band runs, AC refined), kept per block until
 * the end of the image. Samples come from an integer inverse DCT
 * (the Loeffler-Ligtenberg-Moschytz factorisation, constants scaled by
 * 4096), into one plane per component; then planes with fewer samples are
 * upsampled (a triangle filter for the usual halves, as libjpeg and the
 * browsers do) and YCbCr becomes RGB. Every read is bounds-checked: a damaged
 * or hostile file ends in an error or in grey blocks (entropy data cut short
 * reads as zeros, as other decoders show a partial picture), never outside
 * the buffers.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "image.h"
#include "imagedec.h"

static int fail(int e)
{
    errno = e;
    return -1;
}

static unsigned be16(const uint8_t *p)
{
    return (unsigned)p[0] << 8 | p[1];
}

/* The natural (row by row) index of each coefficient in zigzag order. */
static const uint8_t zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

/* --- Huffman codes (T.81 Annex C and F.2.2.3) ------------------------------ */

#define LOOK 9                                  /* bits looked up in one step */

struct huffman {
    bool present;
    uint8_t look_len[1 << LOOK];                /* code length, 0 when longer than LOOK */
    uint8_t look_value[1 << LOOK];
    int32_t maxcode[17];                        /* the largest code of each length, -1: none */
    int32_t delta[17];                          /* value index = code + delta[length] */
    uint8_t value[256];
};

/* Build a table from its 16 counts and values; false if they over-subscribe. */
static bool huffman_build(struct huffman *h, const uint8_t *counts, const uint8_t *values, int n)
{
    int code = 0, k = 0;
    memset(h->look_len, 0, sizeof(h->look_len));
    for (int len = 1; len <= 16; len++) {
        int c = counts[len - 1];
        h->delta[len] = k - code;
        for (int i = 0; i < c; i++, k++, code++) {
            if (code >= 1 << len)
                return false;                   /* more codes than the lengths allow */
            if (len <= LOOK) {
                int shift = LOOK - len;
                for (int f = 0; f < 1 << shift; f++) {
                    h->look_len[code << shift | f] = (uint8_t)len;
                    h->look_value[code << shift | f] = values[k];
                }
            }
        }
        h->maxcode[len] = c ? code - 1 : -1;
        code <<= 1;
    }
    memcpy(h->value, values, (size_t)n);
    h->present = true;
    return true;
}

/* --- Entropy-coded data ---------------------------------------------------- */

struct reader {
    const uint8_t *d;
    long n, at;
    uint32_t buf;                               /* the next `count` bits, oldest highest */
    int count;
    bool marker;                                /* stopped at a marker (or the end) */
};

/* At least 25 bits in the buffer: a 0xFF 0x00 is a data byte 0xFF; at a
 * marker or the end of the file zeros follow. */
static void fill(struct reader *r)
{
    while (r->count <= 24) {
        unsigned byte = 0;
        if (!r->marker && r->at < r->n) {
            byte = r->d[r->at];
            if (byte == 0xFF) {
                if (r->at + 1 < r->n && r->d[r->at + 1] == 0)
                    r->at += 2;
                else {
                    r->marker = true;
                    byte = 0;
                }
            } else
                r->at++;
        } else
            r->marker = true;
        r->buf = r->buf << 8 | byte;
        r->count += 8;
    }
}

static unsigned peek(struct reader *r, int k)
{
    return (r->buf >> (r->count - k)) & ((1u << k) - 1);
}

/* The next value of code h; -1 for a code it does not have. */
static int huffman_decode(struct reader *r, const struct huffman *h)
{
    fill(r);
    unsigned look = peek(r, LOOK);
    int len = h->look_len[look];
    if (len) {
        r->count -= len;
        return h->look_value[look];
    }
    unsigned code = peek(r, 16);
    for (len = LOOK + 1; len <= 16; len++) {
        int c = (int)(code >> (16 - len));
        if (c <= h->maxcode[len]) {
            r->count -= len;
            return h->value[c + h->delta[len]];
        }
    }
    return -1;
}

/* s (1-15) more bits, as the signed value of category s (F.2.2.1 EXTEND). */
static int receive_extend(struct reader *r, int s)
{
    fill(r);
    int v = (int)peek(r, s);
    r->count -= s;
    return v < 1 << (s - 1) ? v - (1 << s) + 1 : v;
}

/* --- The inverse DCT -------------------------------------------------------- */

/* One 8-point inverse DCT (rows or columns) of in[0], in[step], ... into the
 * even part x0..x3 and the odd part t0..t3; the outputs are x0 + t3, x1 + t2,
 * x2 + t1, x3 + t0, x3 - t0, x2 - t1, x1 - t2, x0 - t3. Constants are
 * 4096 times sqrt(2) times sums of cos(k pi / 16). */
#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7)                                       \
    int64_t i0 = (s0), i1 = (s1), i2 = (s2), i3 = (s3);                              \
    int64_t i4 = (s4), i5 = (s5), i6 = (s6), i7 = (s7);                              \
    int64_t p1 = (i2 + i6) * 2217;                                                   \
    int64_t e2 = p1 - i6 * 7568, e3 = p1 + i2 * 3135;                                \
    int64_t e0 = (i0 + i4) * 4096, e1 = (i0 - i4) * 4096;                            \
    int64_t x0 = e0 + e3, x3 = e0 - e3, x1 = e1 + e2, x2 = e1 - e2;                  \
    int64_t t0 = i7, t1 = i5, t2 = i3, t3 = i1;                                      \
    int64_t q3 = t0 + t2, q4 = t1 + t3, q1 = t0 + t3, q2 = t1 + t2;                  \
    int64_t q5 = (q3 + q4) * 4816;                                                   \
    t0 *= 1223, t1 *= 8410, t2 *= 12586, t3 *= 6149;                                 \
    q1 = q5 - q1 * 3686, q2 = q5 - q2 * 10498;                                       \
    q3 *= -8035, q4 *= -1598;                                                        \
    t3 += q1 + q4, t2 += q2 + q3, t1 += q2 + q4, t0 += q1 + q3;

static uint8_t clamp8(int64_t v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

/* Coefficients c (natural order, dequantized) to 8x8 samples at out. */
static void idct(const int32_t *c, uint8_t *out, int stride)
{
    int64_t w[64];
    for (int i = 0; i < 8; i++) {               /* columns, keeping 2 more bits than the input */
        const int32_t *s = c + i;
        if (!(s[8] | s[16] | s[24] | s[32] | s[40] | s[48] | s[56])) {
            for (int k = 0; k < 8; k++)         /* (only DC: a flat column) */
                w[i + 8 * k] = (int64_t)s[0] * 4;
            continue;
        }
        IDCT_1D(s[0], s[8], s[16], s[24], s[32], s[40], s[48], s[56])
        w[i] = (x0 + t3 + 512) >> 10, w[i + 56] = (x0 - t3 + 512) >> 10;
        w[i + 8] = (x1 + t2 + 512) >> 10, w[i + 48] = (x1 - t2 + 512) >> 10;
        w[i + 16] = (x2 + t1 + 512) >> 10, w[i + 40] = (x2 - t1 + 512) >> 10;
        w[i + 24] = (x3 + t0 + 512) >> 10, w[i + 32] = (x3 - t0 + 512) >> 10;
    }
    for (int i = 0; i < 8; i++, out += stride) { /* rows: the 2 bits, the 8 of the DCT, +128 */
        const int64_t *s = w + 8 * i;
        IDCT_1D(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7])
        const int64_t round = (int64_t)1 << 16, shift = (int64_t)128 << 17;
        out[0] = clamp8((x0 + t3 + round + shift) >> 17);
        out[7] = clamp8((x0 - t3 + round + shift) >> 17);
        out[1] = clamp8((x1 + t2 + round + shift) >> 17);
        out[6] = clamp8((x1 - t2 + round + shift) >> 17);
        out[2] = clamp8((x2 + t1 + round + shift) >> 17);
        out[5] = clamp8((x2 - t1 + round + shift) >> 17);
        out[3] = clamp8((x3 + t0 + round + shift) >> 17);
        out[4] = clamp8((x3 - t0 + round + shift) >> 17);
    }
}

/* --- The decoder ------------------------------------------------------------ */

struct component {
    int id, h, v, tq;                           /* sampling factors, quantization table */
    int dc, ac;                                 /* this scan's tables */
    int width, height;                          /* its samples (before upsampling) */
    int bw, bh;                                 /* blocks in its plane (whole MCUs) */
    uint8_t *plane;                             /* bw * 8 samples per row */
    int16_t *coef;                              /* progressive: 64 per block, natural order */
    int pred;                                   /* the DC prediction */
};

struct jpeg {
    int w, h, ncomp, hmax, vmax, mcux, mcuy;
    struct component comp[3];
    uint16_t quant[4][64];                      /* in zigzag order, as the file has them */
    bool quant_present[4];
    struct huffman dc[4], ac[4];
    unsigned restart;                           /* MCUs between restart markers, 0: none */
    int adobe;                                  /* Adobe's colour transform, -1: no APP14 */
    bool frame, scanned, progressive;
    int orientation;                            /* EXIF's 1-8 (1: as stored), 0: none */
    int ss, se, ah, al;                         /* this progressive scan's band and bits */
    unsigned eobrun;                            /* blocks left with nothing more in this band */
};

static int16_t sat16(int v)
{
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

/* --- Progressive scans (G.1.2) ------------------------------------------- */

static unsigned bits(struct reader *r, int k)
{
    fill(r);
    unsigned v = peek(r, k);
    r->count -= k;
    return v;
}

/* DC, first scan: the difference, shifted up by the scan's Al. */
static void dc_first(struct jpeg *j, struct reader *r, struct component *c, int16_t *b, bool *bad)
{
    int t = huffman_decode(r, &j->dc[c->dc]);
    if (t < 0 || t > 11) {
        *bad = true;
        return;
    }
    c->pred += t ? receive_extend(r, t) : 0;
    if (c->pred < -32768 || c->pred > 32767)
        c->pred = c->pred < 0 ? -32768 : 32767;
    b[0] = sat16(c->pred * (1 << j->al));
}

/* DC, refinement: one more bit. */
static void dc_refine(struct jpeg *j, struct reader *r, int16_t *b)
{
    if (bits(r, 1))
        b[0] = (int16_t)(b[0] | 1 << j->al);
}

/* AC, first scan of the band ss..se: values shifted up by Al; EOBn starts a
 * run of blocks with nothing more in the band. */
static void ac_first(struct jpeg *j, struct reader *r, struct component *c, int16_t *b, bool *bad)
{
    if (j->eobrun) {
        j->eobrun--;
        return;
    }
    for (int k = j->ss; k <= j->se; k++) {
        int rs = huffman_decode(r, &j->ac[c->ac]);
        if (rs < 0) {
            *bad = true;
            return;
        }
        int run = rs >> 4, s = rs & 15;
        if (s == 0) {
            if (run == 15) {
                k += 15;                        /* sixteen zeros (with the k++) */
                continue;
            }
            j->eobrun = (1u << run) - 1 + (run ? bits(r, run) : 0);
            return;                             /* (this block is the run's first) */
        }
        k += run;
        if (k > j->se) {
            *bad = true;
            return;
        }
        b[zigzag[k]] = sat16(receive_extend(r, s) * (1 << j->al));
    }
}

/* A coefficient already nonzero gets its correction bit (G.1.2.3): when set
 * and that bit is not yet there, the magnitude grows by it. */
static void correct(struct reader *r, int16_t *v, int p1)
{
    if (bits(r, 1) && !(*v & p1))
        *v = sat16(*v + (*v >= 0 ? p1 : -p1));
}

/* AC, refinement of the band: correction bits for the nonzero coefficients,
 * and new coefficients of magnitude 1 << Al after runs of zero ones. */
static void ac_refine(struct jpeg *j, struct reader *r, struct component *c, int16_t *b, bool *bad)
{
    int p1 = 1 << j->al, k = j->ss;
    if (!j->eobrun) {
        for (; k <= j->se; k++) {
            int rs = huffman_decode(r, &j->ac[c->ac]);
            if (rs < 0) {
                *bad = true;
                return;
            }
            int run = rs >> 4, s = rs & 15, value = 0;
            if (s) {
                if (s != 1) {
                    *bad = true;                /* a new coefficient is +-1 << Al */
                    return;
                }
                value = bits(r, 1) ? p1 : -p1;
            } else if (run != 15) {
                j->eobrun = (1u << run) + (run ? bits(r, run) : 0);
                break;                          /* (this block too: corrected below) */
            }
            /* Skip run zero coefficients (correcting the nonzero ones on the
             * way); the new one goes where the run ends. */
            for (; k <= j->se; k++) {
                int16_t *v = &b[zigzag[k]];
                if (*v)
                    correct(r, v, p1);
                else if (run-- == 0)
                    break;
            }
            if (value) {
                if (k > j->se) {
                    *bad = true;
                    return;
                }
                b[zigzag[k]] = (int16_t)value;
            }
        }
    }
    if (j->eobrun) {                            /* the rest of the band: corrections only */
        for (; k <= j->se; k++)
            if (b[zigzag[k]])
                correct(r, &b[zigzag[k]], p1);
        j->eobrun--;
    }
}

/* One block of the current scan, whichever kind it is. */
static void decode_block(struct jpeg *j, struct reader *r, struct component *c, int bx, int by,
                         bool *bad);

static void scan_block(struct jpeg *j, struct reader *r, struct component *c, int bx, int by,
                       bool *bad)
{
    if (!j->progressive) {
        decode_block(j, r, c, bx, by, bad);
        return;
    }
    int16_t *b = c->coef + ((long)by * c->bw + bx) * 64;
    if (j->ss == 0)
        j->ah ? dc_refine(j, r, b) : dc_first(j, r, c, b, bad);
    else
        j->ah ? ac_refine(j, r, c, b, bad) : ac_first(j, r, c, b, bad);
}

/* The end of a progressive image: every block's coefficients, dequantized,
 * through the inverse DCT. */
static void progressive_finish(struct jpeg *j)
{
    for (int i = 0; i < j->ncomp; i++) {
        struct component *c = &j->comp[i];
        int32_t q[64], coef[64];
        for (int k = 0; k < 64; k++)
            q[zigzag[k]] = j->quant[c->tq][k];
        for (int by = 0; by < c->bh; by++)
            for (int bx = 0; bx < c->bw; bx++) {
                const int16_t *b = c->coef + ((long)by * c->bw + bx) * 64;
                for (int k = 0; k < 64; k++)
                    coef[k] = b[k] * q[k];
                idct(coef, c->plane + (long)by * 8 * c->bw * 8 + bx * 8, c->bw * 8);
            }
    }
}

static void decode_block(struct jpeg *j, struct reader *r, struct component *c, int bx, int by,
                         bool *bad)
{
    int32_t coef[64];
    memset(coef, 0, sizeof(coef));
    const uint16_t *q = j->quant[c->tq];
    int t = huffman_decode(r, &j->dc[c->dc]);
    if (t < 0 || t > 11) {
        *bad = true;
        return;
    }
    int diff = t ? receive_extend(r, t) : 0;
    c->pred += diff;
    if (c->pred < -32768 || c->pred > 32767)    /* (hostile data: keep the sums in range) */
        c->pred = c->pred < 0 ? -32768 : 32767;
    coef[0] = c->pred * q[0];
    for (int k = 1; k < 64;) {
        int rs = huffman_decode(r, &j->ac[c->ac]);
        if (rs < 0) {
            *bad = true;
            return;
        }
        int run = rs >> 4, s = rs & 15;
        if (s == 0) {
            if (run != 15)
                break;                          /* end of block */
            k += 16;                            /* sixteen zeros */
            continue;
        }
        k += run;
        if (k > 63) {
            *bad = true;
            return;
        }
        coef[zigzag[k]] = receive_extend(r, s) * q[k];
        k++;
    }
    idct(coef, c->plane + (long)by * 8 * c->bw * 8 + bx * 8, c->bw * 8);
}

/* After a restart interval: the marker RSTn, the predictions back to 0. */
static void restart(struct jpeg *j, struct reader *r)
{
    r->buf = 0, r->count = 0;
    long at = r->at;
    while (at + 1 < r->n && !(r->d[at] == 0xFF && r->d[at + 1] != 0 && r->d[at + 1] != 0xFF))
        at++;
    if (at + 1 < r->n && r->d[at + 1] >= 0xD0 && r->d[at + 1] <= 0xD7) {
        r->at = at + 2;
        r->marker = false;
    } else {
        r->at = at;
        r->marker = true;                       /* another marker: zeros from here */
    }
    for (int i = 0; i < j->ncomp; i++)
        j->comp[i].pred = 0;
    j->eobrun = 0;
}

/* One scan from d[at] (after its header): returns where its data ends. */
static long decode_scan(struct jpeg *j, const uint8_t *d, long n, long at,
                        struct component **sc, int ns, bool *bad)
{
    struct reader r = { d, n, at, 0, 0, false };
    for (int i = 0; i < j->ncomp; i++)
        j->comp[i].pred = 0;
    j->eobrun = 0;
    long across, down;
    if (ns == 1) {                              /* one component: its blocks one by one */
        across = (sc[0]->width + 7) / 8;
        down = (sc[0]->height + 7) / 8;
    } else
        across = j->mcux, down = j->mcuy;
    unsigned done = 0;
    for (long my = 0; my < down && !*bad; my++)
        for (long mx = 0; mx < across && !*bad; mx++) {
            if (j->restart && done && done % j->restart == 0)
                restart(j, &r);
            if (ns == 1)
                scan_block(j, &r, sc[0], (int)mx, (int)my, bad);
            else
                for (int i = 0; i < ns && !*bad; i++)
                    for (int v = 0; v < sc[i]->v; v++)
                        for (int h = 0; h < sc[i]->h; h++)
                            scan_block(j, &r, sc[i], (int)mx * sc[i]->h + h,
                                       (int)my * sc[i]->v + v, bad);
            done++;
        }
    /* The next marker: past the data, any padding and RSTs. */
    at = r.at;
    while (at + 1 < n && !(d[at] == 0xFF && d[at + 1] != 0 && d[at + 1] != 0xFF &&
                           !(d[at + 1] >= 0xD0 && d[at + 1] <= 0xD7)))
        at++;
    return at;
}

static int frame(struct jpeg *j, const uint8_t *s, unsigned len)
{
    if (j->frame || len < 6 || s[0] != 8)
        return -1;                              /* a second frame, or not 8-bit samples */
    j->h = (int)be16(s + 1), j->w = (int)be16(s + 3), j->ncomp = s[5];
    if (j->w == 0 || j->h == 0 || (j->ncomp != 1 && j->ncomp != 3) || len < 6 + 3u * j->ncomp)
        return -1;                              /* (height 0: given later by DNL, not taken) */
    if ((long)j->w * j->h > IMAGE_MAX_PIXELS)
        return -1;
    j->hmax = j->vmax = 1;
    for (int i = 0; i < j->ncomp; i++) {
        struct component *c = &j->comp[i];
        c->id = s[6 + 3 * i], c->h = s[7 + 3 * i] >> 4, c->v = s[7 + 3 * i] & 15;
        c->tq = s[8 + 3 * i];
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4 || c->tq > 3)
            return -1;
        for (int k = 0; k < i; k++)
            if (j->comp[k].id == c->id)
                return -1;
        if (c->h > j->hmax)
            j->hmax = c->h;
        if (c->v > j->vmax)
            j->vmax = c->v;
    }
    j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
    j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
    j->frame = true;
    return 0;
}

static int planes(struct jpeg *j)
{
    for (int i = 0; i < j->ncomp; i++) {
        struct component *c = &j->comp[i];
        c->width = (j->w * c->h + j->hmax - 1) / j->hmax;
        c->height = (j->h * c->v + j->vmax - 1) / j->vmax;
        c->bw = j->mcux * c->h, c->bh = j->mcuy * c->v;
        c->plane = calloc((size_t)c->bw * 8 * c->bh * 8, 1);
        if (!c->plane)
            return -1;
        if (j->progressive) {
            c->coef = calloc((size_t)c->bw * c->bh * 64, sizeof(int16_t));
            if (!c->coef)
                return -1;
        }
    }
    return 0;
}

static int tables(struct jpeg *j, unsigned marker, const uint8_t *s, unsigned len)
{
    unsigned at = 0;
    while (at < len) {
        if (marker == 0xDB) {                   /* DQT: precision (8 or 16 bits), table */
            unsigned pq = s[at] >> 4, tq = s[at] & 15, size = pq ? 129 : 65;
            if (pq > 1 || tq > 3 || at + size > len)
                return -1;
            for (int k = 0; k < 64; k++)
                j->quant[tq][k] = (uint16_t)(pq ? be16(s + at + 1 + 2 * k) : s[at + 1 + k]);
            j->quant_present[tq] = true;
            at += size;
        } else {                                /* DHT: class (DC 0, AC 1), table, counts */
            if (at + 17 > len)
                return -1;
            unsigned tc = s[at] >> 4, th = s[at] & 15, total = 0;
            for (int i = 0; i < 16; i++)
                total += s[at + 1 + i];
            if (tc > 1 || th > 3 || total > 256 || at + 17 + total > len)
                return -1;
            if (!huffman_build(tc ? &j->ac[th] : &j->dc[th], s + at + 1, s + at + 17, (int)total))
                return -1;
            at += 17 + total;
        }
    }
    return 0;
}

/* A component's row y (output pixels) at full width into row: copied, or
 * upsampled -- halves by a triangle filter (3/4 of the nearer sample, 1/4 of
 * the next), other ratios by repeating samples. */
static void component_row(const struct jpeg *j, const struct component *c, int y, uint8_t *row,
                          uint16_t *wide)
{
    int stride = c->bw * 8;
    int sx = j->hmax / c->h, sy = j->vmax / c->v;
    bool halves = j->hmax == sx * c->h && j->vmax == sy * c->v && sx <= 2 && sy <= 2;
    if (!halves) {
        for (int x = 0; x < j->w; x++)
            row[x] = c->plane[(long)(y * c->v / j->vmax) * stride + x * c->h / j->hmax];
        return;
    }
    const uint8_t *near = c->plane + (long)(y / sy) * stride;
    if (sx == 1 && sy == 1) {
        memcpy(row, near, (size_t)j->w);
        return;
    }
    int cw = c->width;
    if (sy == 2) {                              /* vertical: 3 near + 1 far, scaled by 4 */
        int far_y = (y & 1) ? y / 2 + 1 : y / 2 - 1;
        if (far_y < 0)
            far_y = 0;
        if (far_y > c->height - 1)
            far_y = c->height - 1;
        const uint8_t *far = c->plane + (long)far_y * stride;
        for (int x = 0; x < cw; x++)
            wide[x] = (uint16_t)(3 * near[x] + far[x]);
    } else
        for (int x = 0; x < cw; x++)
            wide[x] = (uint16_t)(4 * near[x]);
    if (sx == 1) {
        for (int x = 0; x < j->w; x++)
            row[x] = (uint8_t)((wide[x] + 2) >> 2);
        return;
    }
    for (int x = 0; x < j->w; x++) {            /* horizontal: the same, now scaled by 16 */
        int i = x >> 1;
        int other = (x & 1) ? (i + 1 < cw ? i + 1 : cw - 1) : (i > 0 ? i - 1 : 0);
        row[x] = (uint8_t)((3 * wide[i] + wide[other] + ((x & 1) ? 7 : 8)) >> 4);
    }
}

/* EXIF's orientation (APP1 "Exif\0\0", a TIFF structure: byte order,
 * 42, the first directory's offset; tag 0x0112 there, a SHORT 1-8). How
 * cameras store a portrait photo: landscape, with the turn to make. */
static int exif_orientation(const uint8_t *s, unsigned len)
{
    if (len < 6 + 8 || memcmp(s, "Exif\0\0", 6))
        return 0;
    const uint8_t *t = s + 6;
    unsigned n = len - 6;
    bool le = t[0] == 'I' && t[1] == 'I';
    if (!le && !(t[0] == 'M' && t[1] == 'M'))
        return 0;
#define G16(p) (le ? (unsigned)(p)[0] | (unsigned)(p)[1] << 8 : (unsigned)(p)[0] << 8 | (p)[1])
#define G32(p) (le ? G16(p) | G16((p) + 2) << 16 : G16(p) << 16 | G16((p) + 2))
    if (G16(t + 2) != 42)
        return 0;
    uint32_t ifd = G32(t + 4);
    if (ifd > n - 2 || ifd < 8)
        return 0;
    unsigned count = G16(t + ifd);
    for (unsigned i = 0; i < count; i++) {
        uint32_t e = ifd + 2 + 12 * i;
        if (e > n - 12)
            return 0;
        if (G16(t + e) == 0x0112 && G16(t + e + 2) == 3) {   /* Orientation, SHORT */
            unsigned v = G16(t + e + 8);
            return v >= 1 && v <= 8 ? (int)v : 0;
        }
    }
    return 0;
#undef G16
#undef G32
}

/* Turn out as orientation o says (2 mirror, 3 half a turn, 4 upside down
 * mirrored, 5 mirrored along the diagonal, 6 a quarter turn clockwise, 7
 * the other diagonal, 8 a quarter turn anticlockwise): each new pixel taken
 * from where it lies in the stored picture. */
static int orient(struct image *out, int o)
{
    if (o <= 1)
        return 0;
    int w = out->w, h = out->h;
    bool swap = o >= 5;
    struct image t;
    if (image_alloc(&t, swap ? h : w, swap ? w : h) < 0) {
        image_free(out);
        return -1;
    }
    for (int y = 0; y < t.h; y++)
        for (int x = 0; x < t.w; x++) {
            int sx = x, sy = y;
            switch (o) {
            case 2: sx = w - 1 - x; break;
            case 3: sx = w - 1 - x, sy = h - 1 - y; break;
            case 4: sy = h - 1 - y; break;
            case 5: sx = y, sy = x; break;
            case 6: sx = y, sy = h - 1 - x; break;
            case 7: sx = w - 1 - y, sy = h - 1 - x; break;
            case 8: sx = w - 1 - y, sy = x; break;
            }
            memcpy(t.pixels + ((size_t)y * t.w + x) * 4, out->pixels + ((size_t)sy * w + sx) * 4, 4);
        }
    image_free(out);
    *out = t;
    return 0;
}

static int convert(struct jpeg *j, struct image *out)
{
    if (image_alloc(out, j->w, j->h) < 0)
        return -1;
    uint8_t *rows = malloc((size_t)j->w * 3);
    uint16_t *wide = malloc(((size_t)j->w + 8) * sizeof(uint16_t));
    if (!rows || !wide) {
        free(rows), free(wide);
        image_free(out);
        return fail(ENOMEM);
    }
    /* RGB without a transform: Adobe's transform 0, or JFIF-less files
     * naming their components R, G, B. */
    bool rgb = j->ncomp == 3 && (j->adobe == 0 || (j->adobe < 0 && j->comp[0].id == 'R' &&
                                                   j->comp[1].id == 'G' && j->comp[2].id == 'B'));
    for (int y = 0; y < j->h; y++) {
        for (int i = 0; i < j->ncomp; i++)
            component_row(j, &j->comp[i], y, rows + (size_t)i * j->w, wide);
        uint8_t *p = out->pixels + (size_t)y * out->stride;
        const uint8_t *c0 = rows, *c1 = rows + j->w, *c2 = rows + 2 * j->w;
        for (int x = 0; x < j->w; x++, p += 4) {
            if (j->ncomp == 1)
                p[0] = p[1] = p[2] = c0[x];
            else if (rgb)
                p[0] = c0[x], p[1] = c1[x], p[2] = c2[x];
            else {                              /* JFIF's YCbCr, in 16-bit fixed point */
                int yy = c0[x], cb = c1[x] - 128, cr = c2[x] - 128;
                p[0] = clamp8(yy + ((91881 * cr + 32768) >> 16));
                p[1] = clamp8(yy + ((-22554 * cb - 46802 * cr + 32768) >> 16));
                p[2] = clamp8(yy + ((116130 * cb + 32768) >> 16));
            }
            p[3] = 255;
        }
    }
    free(rows), free(wide);
    return 0;
}

int decode_jpeg(const uint8_t *d, long n, struct image *out, int header_only)
{
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8)
        return fail(EINVAL);
    struct jpeg *j = calloc(1, sizeof(*j));
    if (!j)
        return fail(ENOMEM);
    j->adobe = -1;
    int result = -1, error = EINVAL;
    long at = 2;
    for (;;) {
        while (at + 1 < n && (d[at] != 0xFF || d[at + 1] == 0xFF || d[at + 1] == 0))
            at++;                               /* (fill bytes; stray data is skipped) */
        if (at + 1 >= n)
            break;                              /* no EOI: what was decoded stands */
        unsigned marker = d[at + 1];
        at += 2;
        if (marker == 0xD9)
            break;                              /* EOI */
        if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01)
            continue;                           /* markers without a length */
        if (at + 2 > n)
            goto done;
        unsigned len = be16(d + at);
        if (len < 2 || at + len > n)
            goto done;
        const uint8_t *s = d + at + 2;
        len -= 2;
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            j->progressive = marker == 0xC2;    /* (else baseline, extended sequential) */
            if (frame(j, s, len) < 0)
                goto done;
            if (header_only) {
                bool swap = j->orientation >= 5;    /* (the size as it will be shown) */
                out->w = swap ? j->h : j->w, out->h = swap ? j->w : j->h;
                out->stride = out->w * 4;
                out->format = PIXEL_RGBA8888, out->pixels = 0;
                result = 0;
                goto done;
            }
            if (planes(j) < 0) {
                error = ENOMEM;
                goto done;
            }
        } else if (marker >= 0xC3 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
                   marker != 0xCC)
            goto done;                          /* lossless, hierarchical, arithmetic */
        else if (marker == 0xC4 || marker == 0xDB) {
            if (tables(j, marker, s, len) < 0)
                goto done;
        } else if (marker == 0xDD) {            /* DRI */
            if (len < 2)
                goto done;
            j->restart = be16(s);
        } else if (marker == 0xE1) {            /* APP1: EXIF, for the orientation */
            if (!j->orientation)
                j->orientation = exif_orientation(s, len);
        } else if (marker == 0xEE) {            /* APP14: Adobe's colour transform */
            if (len >= 12 && !memcmp(s, "Adobe", 5))
                j->adobe = s[11];
        } else if (marker == 0xDA) {            /* SOS */
            if (!j->frame || len < 1)
                goto done;
            int ns = s[0];
            if (ns < 1 || ns > j->ncomp || len < 4 + 2u * ns)
                goto done;
            j->ss = s[1 + 2 * ns], j->se = s[2 + 2 * ns];
            j->ah = s[3 + 2 * ns] >> 4, j->al = s[3 + 2 * ns] & 15;
            if (j->progressive && (j->ah > 13 || j->al > 13 || j->se > 63 || j->se < j->ss ||
                                   (j->ss == 0 && j->se != 0) || (j->ss > 0 && ns != 1)))
                goto done;                      /* (a band of DC or of one component's AC) */
            /* The tables a scan uses: sequential both; progressive DC first
             * only DC's, AC scans only AC's, DC refinement none. */
            bool need_dc = !j->progressive || (j->ss == 0 && j->ah == 0);
            bool need_ac = !j->progressive || j->ss > 0;
            struct component *sc[3];
            int blocks = 0;
            for (int i = 0; i < ns; i++) {
                struct component *c = 0;
                for (int k = 0; k < j->ncomp; k++)
                    if (j->comp[k].id == s[1 + 2 * i])
                        c = &j->comp[k];
                if (!c)
                    goto done;
                c->dc = s[2 + 2 * i] >> 4, c->ac = s[2 + 2 * i] & 15;
                if (c->dc > 3 || c->ac > 3 || (need_dc && !j->dc[c->dc].present) ||
                    (need_ac && !j->ac[c->ac].present) || !j->quant_present[c->tq])
                    goto done;
                for (int k = 0; k < i; k++)
                    if (sc[k] == c)
                        goto done;
                sc[i] = c;
                blocks += c->h * c->v;
            }
            if (ns > 1 && blocks > 10)
                goto done;                      /* T.81 B.2.3: at most 10 blocks per MCU */
            bool bad = false;
            at = decode_scan(j, d, n, at + 2 + len, sc, ns, &bad);
            if (bad)
                goto done;
            j->scanned = true;
            continue;
        } else if (marker == 0xDC)
            goto done;                          /* DNL: not taken */
        at += 2 + len;                          /* (APPn, COM and others: skipped) */
    }
    if (j->scanned) {
        if (j->progressive)
            progressive_finish(j);
        result = convert(j, out), error = errno;
        if (result == 0 && orient(out, j->orientation) < 0)
            result = -1, error = ENOMEM;
    }
done:
    for (int i = 0; i < 3; i++)
        free(j->comp[i].plane), free(j->comp[i].coef);
    free(j);
    if (result < 0)
        return fail(error);
    return 0;
}

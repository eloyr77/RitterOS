/*
 * The rasteriser libdraw and the font reader share (41c-3; internal, never
 * a public header): straight edges into cells, each edge adding the signed
 * area between it and the cell's right side (see draw.c's header), so a
 * row's running sum is each pixel's covered area exactly. Static functions
 * in a header: each library that includes it has its own copy, the code
 * one. Floats in a fixed order (its users build with -ffp-contract=off).
 */
#ifndef RITTEROS_RASTER_H
#define RITTEROS_RASTER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* (each user takes what it needs: the rest is not a warning) */
#define RASTER static __attribute__((unused))

#define FAR 1e6f                        /* coordinates beyond are clamped (ints stay in range) */
#define BAND_CELLS 65536                /* cells worked on at once (256 KiB) */

struct edge { float x0, y0, x1, y1; };

struct path {
    struct edge *e;
    int n, cap;
    int failed;
};

RASTER float near(float v)
{
    if (!(v > -FAR))                    /* (NaN too) */
        return -FAR;
    return v < FAR ? v : FAR;
}

RASTER int ffloor(float v)
{
    int i = (int)v;
    return (float)i > v ? i - 1 : i;
}

RASTER int fceil(float v)
{
    int i = (int)v;
    return (float)i < v ? i + 1 : i;
}

RASTER void add_edge(struct path *p, float x0, float y0, float x1, float y1)
{
    if (p->n == p->cap) {
        int cap = p->cap ? p->cap * 2 : 16;
        struct edge *e = realloc(p->e, sizeof(*e) * (size_t)cap);
        if (!e) {
            p->failed = 1;
            return;
        }
        p->e = e;
        p->cap = cap;
    }
    p->e[p->n++] = (struct edge){ near(x0), near(y0), near(x1), near(y1) };
}

/* A closed run of n points, in their order or (backwards) the other way. */
RASTER void add_loop(struct path *p, const float *xy, int n, int backwards)
{
    for (int i = 0; i < n; i++) {
        int a = backwards ? n - 1 - i : i, b = backwards ? (a + n - 1) % n : (a + 1) % n;
        add_edge(p, xy[2 * a], xy[2 * a + 1], xy[2 * b], xy[2 * b + 1]);
    }
}

/* A convex piece of an outline, always wound the same way (by its area's
 * sign), so overlapping pieces add instead of cancelling. */
RASTER void add_piece(struct path *p, const float *xy, int n)
{
    float area = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += xy[2 * i] * xy[2 * j + 1] - xy[2 * j] * xy[2 * i + 1];
    }
    add_loop(p, xy, n, area < 0);
}

/* One cell of a band's row: what lies right of the box only reaches pixels
 * right of it (dropped); what lies left of it reaches all of the row. */
RASTER void cell(float *row, int w, int i, float v)
{
    if (i >= w)
        return;
    row[i < 0 ? 0 : i] += v;
}

/* Edge (x0, y0)-(x1, y1), in the box's coordinates, into rows y_from to
 * y_to - 1 of the band starting at row `top` (rows w cells long). */
RASTER void accumulate(float *band, int w, int top, int y_from, int y_to, struct edge e)
{
    float x0 = e.x0, y0 = e.y0, x1 = e.x1, y1 = e.y1, dir = 1;
    if (y0 == y1)
        return;
    if (y0 > y1) {
        float t = x0;
        x0 = x1, x1 = t;
        t = y0;
        y0 = y1, y1 = t;
        dir = -1;
    }
    float dxdy = (x1 - x0) / (y1 - y0);
    int ys = ffloor(y0), ye = fceil(y1);
    if (ys < y_from)
        ys = y_from;
    if (ye > y_to)
        ye = y_to;
    for (int y = ys; y < ye; y++) {
        float *row = band + (size_t)(y - top) * (size_t)w;
        float t = (float)y > y0 ? (float)y : y0, b = (float)(y + 1) < y1 ? (float)(y + 1) : y1;
        float xa = x0 + (t - y0) * dxdy, xb = x0 + (b - y0) * dxdy;
        float d = (b - t) * dir;
        float lo = xa < xb ? xa : xb, hi = xa < xb ? xb : xa;
        int i0 = ffloor(lo), i1 = fceil(hi);
        if (i1 <= i0 + 1) {                     /* within one cell */
            float mid = 0.5f * (xa + xb) - (float)i0;
            cell(row, w, i0, d - d * mid);
            cell(row, w, i0 + 1, d * mid);
            continue;
        }
        float s = 1 / (hi - lo);
        float f0 = lo - (float)i0, f1 = hi - (float)i1 + 1;
        float a0 = 0.5f * s * (1 - f0) * (1 - f0), am = 0.5f * s * f1 * f1;
        cell(row, w, i0, d * a0);
        if (i1 == i0 + 2)
            cell(row, w, i0 + 1, d * (1 - a0 - am));
        else {
            float a1 = s * (1.5f - f0);
            cell(row, w, i0 + 1, d * (a1 - a0));
            int from = i0 + 2, to = i1 - 2;     /* the cells it crosses whole: d * s each */
            if (from < 0) {
                int left = (to < -1 ? to : -1) - from + 1;
                cell(row, w, 0, (float)left * (d * s));
                from = 0;
            }
            if (to > w - 1)
                to = w - 1;
            for (int i = from; i <= to; i++)
                row[i] += d * s;
            float a2 = a1 + (float)(i1 - i0 - 3) * s;
            cell(row, w, i1 - 1, d * (1 - a2 - am));
        }
        cell(row, w, i1, d * am);
    }
}

/* (41c-3) The coverage of the path's edges over the box x0, y0, w x h
 * (in the path's coordinates) into mask (w x h bytes, 0 to 255, rows w
 * apart); false without memory. The edges are moved into the box. */
RASTER bool raster_mask(struct path *p, int x0, int y0, int w, int h, uint8_t *mask)
{
    memset(mask, 0, (size_t)w * (size_t)h);
    if (p->failed)
        return false;
    if (!p->n || w <= 0 || h <= 0)
        return true;
    int rows = BAND_CELLS / w > 0 ? BAND_CELLS / w : 1;
    if (rows > h)
        rows = h;
    float *band = malloc(sizeof(float) * (size_t)w * (size_t)rows);
    if (!band)
        return false;
    for (int i = 0; i < p->n; i++) {
        p->e[i].x0 -= (float)x0, p->e[i].x1 -= (float)x0;
        p->e[i].y0 -= (float)y0, p->e[i].y1 -= (float)y0;
    }
    for (int top = 0; top < h; top += rows) {
        int n = top + rows < h ? rows : h - top;
        memset(band, 0, sizeof(float) * (size_t)w * (size_t)n);
        for (int i = 0; i < p->n; i++)
            accumulate(band, w, top, top, top + n, p->e[i]);
        for (int y = 0; y < n; y++) {
            const float *row = band + (size_t)y * (size_t)w;
            uint8_t *out = mask + (size_t)(top + y) * (size_t)w;
            float sum = 0;
            for (int i = 0; i < w; i++) {
                sum += row[i];
                float cov = sum < 0 ? -sum : sum;
                out[i] = (uint8_t)((cov > 1 ? 1 : cov) * 255.0f + 0.5f);
            }
        }
    }
    free(band);
    return true;
}

#endif

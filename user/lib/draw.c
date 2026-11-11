/*
 * libdraw (see draw.h). Every shape becomes straight edges, and one
 * rasteriser fills them: each edge adds, to the cells of the rows it
 * crosses, the signed area between it and the cell's right side (the
 * accumulation of font-rs and of stb_truetype's second rasteriser), so a
 * row's running sum is each pixel's covered area, exactly. Its absolute
 * value, at most 1, is the coverage: pieces wound the same way add up (an
 * outline's overlapping corners count once inside it), a hole wound the
 * other way cancels its part. Only the shape's box within the clip is
 * worked on, BAND_CELLS cells at a time.
 *
 * The arithmetic is single-precision floats in a fixed order and without
 * contraction (-ffp-contract=off), so every machine draws the same pixels:
 * drawtest compares RitterOS's with the Mac's, and scripts/drawref.py's
 * exact areas with both.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "draw.h"
#include "raster.h"

uint32_t draw_rgba(int r, int g, int b, int a)
{
#define BYTE(v) ((uint32_t)((v) < 0 ? 0 : (v) > 255 ? 255 : (v)))
    return BYTE(a) << 24 | BYTE(r) << 16 | BYTE(g) << 8 | BYTE(b);
#undef BYTE
}

void draw_clip(struct draw_canvas *c, int x, int y, int w, int h)
{
    c->clipped = 1;
    c->clip_x = x;
    c->clip_y = y;
    c->clip_w = w < 0 ? 0 : w;
    c->clip_h = h < 0 ? 0 : h;
}

/* (41b-3) A colour for each pixel (0xAARRGGBB), asked only where the
 * shape covers some of it: gradients and images. */
typedef uint32_t paint_fn(void *ctx, int x, int y);

static void fill_paint(struct draw_canvas *c, struct path *p, uint32_t color, paint_fn *paint, void *ctx);

/* Fill the path in colour (0xAARRGGBB), and let it go. */
static void fill(struct draw_canvas *c, struct path *p, uint32_t color)
{
    fill_paint(c, p, color, 0, 0);
}

/* The same with each pixel's colour from paint (when given). */
static void fill_paint(struct draw_canvas *c, struct path *p, uint32_t color, paint_fn *paint, void *ctx)
{
    if (p->failed || !p->n || !c || !c->pixels) {
        free(p->e);
        return;
    }
    float minx = p->e[0].x0, maxx = minx, miny = p->e[0].y0, maxy = miny;
    for (int i = 0; i < p->n; i++) {
        struct edge e = p->e[i];
        minx = fminf(minx, fminf(e.x0, e.x1));
        maxx = fmaxf(maxx, fmaxf(e.x0, e.x1));
        miny = fminf(miny, fminf(e.y0, e.y1));
        maxy = fmaxf(maxy, fmaxf(e.y0, e.y1));
    }
    int bx0 = ffloor(minx), by0 = ffloor(miny), bx1 = fceil(maxx), by1 = fceil(maxy);
    int cx0 = 0, cy0 = 0, cx1 = c->w, cy1 = c->h;
    if (c->clipped) {
        cx0 = c->clip_x > 0 ? c->clip_x : 0;
        cy0 = c->clip_y > 0 ? c->clip_y : 0;
        if (c->clip_x + c->clip_w < cx1)
            cx1 = c->clip_x + c->clip_w;
        if (c->clip_y + c->clip_h < cy1)
            cy1 = c->clip_y + c->clip_h;
    }
    bx0 = bx0 > cx0 ? bx0 : cx0;
    by0 = by0 > cy0 ? by0 : cy0;
    bx1 = bx1 < cx1 ? bx1 : cx1;
    by1 = by1 < cy1 ? by1 : cy1;
    int w = bx1 - bx0;
    if (w <= 0 || by1 <= by0) {
        free(p->e);
        return;
    }
    int rows = BAND_CELLS / w > 0 ? BAND_CELLS / w : 1;
    if (rows > by1 - by0)
        rows = by1 - by0;
    float *band = malloc(sizeof(float) * (size_t)w * (size_t)rows);
    if (!band) {
        free(p->e);
        return;
    }
    for (int i = 0; i < p->n; i++) {            /* (the box's coordinates) */
        p->e[i].x0 -= (float)bx0, p->e[i].x1 -= (float)bx0;
        p->e[i].y0 -= (float)by0, p->e[i].y1 -= (float)by0;
    }
    uint32_t ca = color >> 24, cr = color >> 16 & 255, cg = color >> 8 & 255, cb = color & 255;
    uint32_t solid = cr << c->red_shift | cg << c->green_shift | cb << c->blue_shift | c->opaque;
    for (int top = 0; top < by1 - by0; top += rows) {
        int n = top + rows < by1 - by0 ? rows : by1 - by0 - top;
        memset(band, 0, sizeof(float) * (size_t)w * (size_t)n);
        for (int i = 0; i < p->n; i++)
            accumulate(band, w, top, top, top + n, p->e[i]);
        for (int y = 0; y < n; y++) {
            const float *row = band + (size_t)y * (size_t)w;
            uint32_t *px = c->pixels + (size_t)(by0 + top + y) * (size_t)c->stride + bx0;
            float sum = 0;
            for (int i = 0; i < w; i++) {
                sum += row[i];
                float cov = sum < 0 ? -sum : sum;
                if (cov > 1)
                    cov = 1;
                uint32_t cov8 = (uint32_t)(cov * 255.0f + 0.5f);
                if (!cov8)
                    continue;
                if (paint) {
                    color = paint(ctx, bx0 + i, by0 + top + y);
                    ca = color >> 24, cr = color >> 16 & 255, cg = color >> 8 & 255, cb = color & 255;
                    solid = cr << c->red_shift | cg << c->green_shift | cb << c->blue_shift | c->opaque;
                }
                uint32_t a = (cov8 * ca + 127) / 255;
                if (a == 255)
                    px[i] = solid;
                else if (a) {
                    uint32_t d = px[i], k = 255 - a;
                    uint32_t r = (cr * a + (d >> c->red_shift & 255) * k + 127) / 255;
                    uint32_t g = (cg * a + (d >> c->green_shift & 255) * k + 127) / 255;
                    uint32_t b = (cb * a + (d >> c->blue_shift & 255) * k + 127) / 255;
                    px[i] = r << c->red_shift | g << c->green_shift | b << c->blue_shift | c->opaque;
                }
            }
        }
    }
    free(band);
    free(p->e);
}

void draw_rect(struct draw_canvas *c, float x, float y, float w, float h, float line, uint32_t color)
{
    if (!(w > 0 && h > 0))
        return;
    struct path p = { 0 };
    float outer[8] = { x, y, x + w, y, x + w, y + h, x, y + h };
    add_loop(&p, outer, 4, 0);
    if (line > 0 && line * 2 < w && line * 2 < h) {     /* (else the outline fills it) */
        float inner[8] = { x + line, y + line, x + w - line, y + line, x + w - line, y + h - line,
                           x + line, y + h - line };
        add_loop(&p, inner, 4, 1);
    }
    fill(c, &p, color);
}

/* The two corners on each side of a's end of segment a-b, half wide. */
static int side(float ax, float ay, float bx, float by, float half, float *nx, float *ny)
{
    float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
    if (!(len > 0))
        return 0;
    *nx = -dy / len * half;
    *ny = dx / len * half;
    return 1;
}

void draw_line(struct draw_canvas *c, float x0, float y0, float x1, float y1, float width,
               uint32_t color)
{
    float nx, ny;
    if (!(width > 0) || !side(x0, y0, x1, y1, width / 2, &nx, &ny))
        return;
    struct path p = { 0 };
    float q[8] = { x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny };
    add_piece(&p, q, 4);
    fill(c, &p, color);
}

void draw_polygon(struct draw_canvas *c, const float *xy, int n, float line, uint32_t color)
{
    if (!xy || n < 3)
        return;
    struct path p = { 0 };
    if (!(line > 0)) {
        add_loop(&p, xy, n, 0);
        fill(c, &p, color);
        return;
    }
    /* An outline: each edge a band `line` wide, and at each corner the
     * wedge between two bands on its outer side (a bevel). */
    float half = line / 2;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n, k = (i + 2) % n;
        float ax = xy[2 * i], ay = xy[2 * i + 1], bx = xy[2 * j], by = xy[2 * j + 1];
        float cx = xy[2 * k], cy = xy[2 * k + 1], nx, ny, mx, my;
        if (!side(ax, ay, bx, by, half, &nx, &ny))
            continue;
        float q[8] = { ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny, ax - nx, ay - ny };
        add_piece(&p, q, 4);
        if (!side(bx, by, cx, cy, half, &mx, &my))
            continue;
        float turn = (bx - ax) * (cy - by) - (by - ay) * (cx - bx);
        if (turn == 0)
            continue;
        float s = turn > 0 ? -1 : 1;                    /* (the outer side) */
        float t[6] = { bx, by, bx + s * nx, by + s * ny, bx + s * mx, by + s * my };
        add_piece(&p, t, 3);
    }
    fill(c, &p, color);
}

/* (41b-2) Curves. A quarter circle is m straight pieces, m chosen so a
 * piece strays at most SAGITTA from the curve (a chord of angle a strays
 * r(1 - cos(a/2)); a step of 2 sqrt(2 SAGITTA / r) is no larger than the
 * exact one). The points' sines and cosines are series in doubles, the
 * C library's sin and cos left alone, so every machine draws the same. */
#define SAGITTA (1.0 / 1024)
#define QUARTER_MAX 2048                /* pieces of a quarter at most */
#define HALF_PI 1.57079632679489661923

static int quarter_steps(float r)
{
    if (!(r > 0))
        return 1;
    double step = 2 * sqrt(2 * SAGITTA / (double)r);
    double m = HALF_PI / step;
    return m >= QUARTER_MAX ? QUARTER_MAX : m < 1 ? 1 : (int)m + ((double)(int)m < m);
}

/* cos and sin of a, 0 <= a <= pi/2 (Taylor's series to the 23rd power:
 * within 1e-16 there). */
static void cos_sin(double a, double *c, double *s)
{
    double a2 = a * a, ct = 1, st = a, cs = 1, ss = a;
    for (int k = 1; k <= 11; k++) {
        ct = -ct * a2 / (double)((2 * k - 1) * (2 * k));
        st = -st * a2 / (double)((2 * k) * (2 * k + 1));
        cs += ct;
        ss += st;
    }
    *c = cs;
    *s = ss;
}

/* Point k of m on quarter q (0: from 0 to 90 degrees, y down, ...: x
 * right, then down, left, up) of an ellipse's unit circle. */
static void quarter_point(int q, int k, int m, double *x, double *y)
{
    double c, s;
    if (k == 0)
        c = 1, s = 0;
    else if (k == m)
        c = 0, s = 1;
    else
        cos_sin(HALF_PI * (double)k / (double)m, &c, &s);
    double px[4] = { c, -s, -c, s }, py[4] = { s, c, -s, -c };
    *x = px[q];
    *y = py[q];
}

/* An ellipse's loop of 4m points (forwards or backwards); 0 without memory. */
static int add_ellipse(struct path *p, float cx, float cy, float rx, float ry, int backwards)
{
    int m = quarter_steps(rx > ry ? rx : ry), n = 4 * m;
    float *xy = malloc(sizeof(float) * 2 * (size_t)n);
    if (!xy) {
        p->failed = 1;
        return 0;
    }
    for (int q = 0; q < 4; q++)
        for (int k = 0; k < m; k++) {
            double x, y;
            quarter_point(q, k, m, &x, &y);
            xy[2 * (q * m + k)] = (float)((double)cx + (double)rx * x);
            xy[2 * (q * m + k) + 1] = (float)((double)cy + (double)ry * y);
        }
    add_loop(p, xy, n, backwards);
    free(xy);
    return 1;
}

void draw_ellipse(struct draw_canvas *c, float cx, float cy, float rx, float ry, float line,
                  uint32_t color)
{
    if (!(rx > 0 && ry > 0))
        return;
    struct path p = { 0 };
    add_ellipse(&p, cx, cy, rx, ry, 0);
    if (line > 0 && line < rx && line < ry)        /* (else the outline fills it) */
        add_ellipse(&p, cx, cy, rx - line, ry - line, 1);
    fill(c, &p, color);
}

void draw_circle(struct draw_canvas *c, float cx, float cy, float r, float line, uint32_t color)
{
    draw_ellipse(c, cx, cy, r, r, line, color);
}

/* A rounded rectangle's loop: its corners' quarters from the top left one
 * round (a radius of 0: the rectangle). */
static void add_round_rect(struct path *p, float x, float y, float w, float h, float r, int backwards)
{
    if (!(r > 0)) {
        float xy[8] = { x, y, x + w, y, x + w, y + h, x, y + h };
        add_loop(p, xy, 4, backwards);
        return;
    }
    int m = quarter_steps(r), n = 4 * (m + 1);
    float *xy = malloc(sizeof(float) * 2 * (size_t)n);
    if (!xy) {
        p->failed = 1;
        return;
    }
    /* corners' centres, and the quarter each one draws (top left: 180 to
     * 270 degrees, the quarter going up then right) */
    float ccx[4] = { x + r, x + w - r, x + w - r, x + r }, ccy[4] = { y + r, y + r, y + h - r, y + h - r };
    int quarter[4] = { 2, 3, 0, 1 };
    for (int corner = 0; corner < 4; corner++)
        for (int k = 0; k <= m; k++) {
            double px, py;
            quarter_point(quarter[corner], k, m, &px, &py);
            int i = corner * (m + 1) + k;
            xy[2 * i] = (float)((double)ccx[corner] + (double)r * px);
            xy[2 * i + 1] = (float)((double)ccy[corner] + (double)r * py);
        }
    add_loop(p, xy, n, backwards);
    free(xy);
}

void draw_round_rect(struct draw_canvas *c, float x, float y, float w, float h, float radius,
                     float line, uint32_t color)
{
    if (!(w > 0 && h > 0))
        return;
    float r = radius > 0 ? radius : 0;
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;
    struct path p = { 0 };
    add_round_rect(&p, x, y, w, h, r, 0);
    if (line > 0 && line * 2 < w && line * 2 < h)
        add_round_rect(&p, x + line, y + line, w - 2 * line, h - 2 * line, r > line ? r - line : 0, 1);
    fill(c, &p, color);
}

/* (41b-3) Gradients: each pixel's colour where its centre falls between
 * the two points (projected on the line through them, 0 to 1 beyond),
 * every channel and alpha in a straight line. */
struct gradient {
    float x0, y0, dx, dy, len2;
    uint32_t from, to;
};

static uint32_t gradient_at(void *ctx, int x, int y)
{
    const struct gradient *g = ctx;
    float t = 0;
    if (g->len2 > 0)
        t = (((float)x + 0.5f - g->x0) * g->dx + ((float)y + 0.5f - g->y0) * g->dy) / g->len2;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        float a = (float)(g->from >> shift & 255), b = (float)(g->to >> shift & 255);
        out |= (uint32_t)(a + (b - a) * t + 0.5f) << shift;
    }
    return out;
}

void draw_gradient(struct draw_canvas *c, float x, float y, float w, float h, float x0, float y0,
                   uint32_t from, float x1, float y1, uint32_t to)
{
    if (!(w > 0 && h > 0))
        return;
    struct gradient g = { near(x0), near(y0), near(x1) - near(x0), near(y1) - near(y0), 0, from, to };
    g.len2 = g.dx * g.dx + g.dy * g.dy;
    struct path p = { 0 };
    float xy[8] = { x, y, x + w, y, x + w, y + h, x, y + h };
    add_loop(&p, xy, 4, 0);
    fill_paint(c, &p, 0, gradient_at, &g);
}

/* Images: a pixel's centre taken back through the rotation and the scale
 * to the image, which is read there between its four nearest pixels
 * (weighted by alpha, so transparent ones give no dark fringe; the edge
 * pixels repeat beyond it); shrunk more than twice, k x k such samples
 * over the pixel (k the shrinking, at most SAMPLES_MAX) are averaged. */
#define SAMPLES_MAX 8

struct picture {
    const struct image *img;
    float cx, cy, cos, sin, sx, sy;     /* the centre; back-rotation; image pixels a canvas pixel */
    int k;
};

/* Pixel (i, j)'s red, green, blue and alpha, i and j within the image. */
static void texel(const struct image *img, int i, int j, float *rgba)
{
    const uint8_t *p = img->pixels + (size_t)j * (size_t)img->stride;
    if (img->format == PIXEL_RGBA8888) {
        p += 4 * (size_t)i;
        rgba[0] = p[0], rgba[1] = p[1], rgba[2] = p[2], rgba[3] = p[3];
    } else if (img->format == PIXEL_RGB888) {
        p += 3 * (size_t)i;
        rgba[0] = p[0], rgba[1] = p[1], rgba[2] = p[2], rgba[3] = 255;
    } else {
        rgba[0] = rgba[1] = rgba[2] = p[i];
        rgba[3] = 255;
    }
}

/* The image at (u, v) in its own pixels, colours multiplied by alpha. */
static void sample(const struct image *img, float u, float v, float *acc)
{
    u -= 0.5f;
    v -= 0.5f;
    int i0 = ffloor(u), j0 = ffloor(v);
    float fu = u - (float)i0, fv = v - (float)j0;
    float wt[4] = { (1 - fu) * (1 - fv), fu * (1 - fv), (1 - fu) * fv, fu * fv };
    for (int n = 0; n < 4; n++) {
        if (wt[n] == 0)
            continue;
        int i = i0 + (n & 1), j = j0 + (n >> 1);
        i = i < 0 ? 0 : i >= img->w ? img->w - 1 : i;
        j = j < 0 ? 0 : j >= img->h ? img->h - 1 : j;
        float t[4];
        texel(img, i, j, t);
        float a = t[3] / 255 * wt[n];
        acc[0] += t[0] * a;
        acc[1] += t[1] * a;
        acc[2] += t[2] * a;
        acc[3] += t[3] * wt[n];
    }
}

static uint32_t picture_at(void *ctx, int x, int y)
{
    const struct picture *pc = ctx;
    float acc[4] = { 0, 0, 0, 0 };
    for (int sj = 0; sj < pc->k; sj++)
        for (int si = 0; si < pc->k; si++) {
            float px = (float)x + ((float)si + 0.5f) / (float)pc->k - pc->cx;
            float py = (float)y + ((float)sj + 0.5f) / (float)pc->k - pc->cy;
            float u = (px * pc->cos + py * pc->sin) * pc->sx + (float)pc->img->w / 2;
            float v = (py * pc->cos - px * pc->sin) * pc->sy + (float)pc->img->h / 2;
            sample(pc->img, u, v, acc);
        }
    float n = (float)(pc->k * pc->k), a = acc[3] / n;
    if (!(a > 0))
        return 0;
    uint32_t out = (uint32_t)(a + 0.5f) << 24;
    for (int ch = 0; ch < 3; ch++) {                    /* (back from multiplied by alpha) */
        float v = acc[ch] / n * 255 / a;
        out |= (uint32_t)(v > 255 ? 255 : v + 0.5f) << (16 - 8 * ch);
    }
    return out;
}

/* cos and sin of `degrees`, exact at every right angle. */
static void turn(float degrees, float *c, float *s)
{
    float a = fmodf(near(degrees), 360);
    if (a < 0)
        a += 360;
    int q = (int)(a / 90);
    if (q > 3)
        q = 3;
    double co, si;
    cos_sin((double)(a - 90 * (float)q) * (HALF_PI / 90), &co, &si);
    double cq[4] = { co, -si, -co, si }, sq[4] = { si, co, -si, -co };
    *c = (float)cq[q];
    *s = (float)sq[q];
}

void draw_image(struct draw_canvas *c, const struct image *img, float x, float y, float w, float h,
                float angle)
{
    if (!img || !img->pixels || img->w <= 0 || img->h <= 0 || !(w > 0 && h > 0))
        return;
    struct picture pc = { img, x + w / 2, y + h / 2, 1, 0, (float)img->w / w, (float)img->h / h, 1 };
    turn(angle, &pc.cos, &pc.sin);
    float shrink = pc.sx > pc.sy ? pc.sx : pc.sy;
    pc.k = shrink > 2 ? (int)shrink + ((float)(int)shrink < shrink) : 1;
    if (pc.k > SAMPLES_MAX)
        pc.k = SAMPLES_MAX;
    /* the rectangle's corners turned about its centre (clockwise on the screen) */
    float hw = w / 2, hh = h / 2, xy[8];
    float corner[8] = { -hw, -hh, hw, -hh, hw, hh, -hw, hh };
    for (int i = 0; i < 4; i++) {
        xy[2 * i] = pc.cx + corner[2 * i] * pc.cos - corner[2 * i + 1] * pc.sin;
        xy[2 * i + 1] = pc.cy + corner[2 * i] * pc.sin + corner[2 * i + 1] * pc.cos;
    }
    struct path p = { 0 };
    add_loop(&p, xy, 4, 0);
    fill_paint(c, &p, 0, picture_at, &pc);
}

/*
 * SVG (27j): an <svg> tree, styled by css.c like the page (SVG's
 * presentation attributes are hints there), drawn into a picture: paths,
 * rectangles, circles, ellipses, lines, polylines, polygons and text, in
 * groups and nested <svg>s with transforms and viewBoxes, <use> of an
 * element or a <symbol> by its id; filled (nonzero or evenodd) and stroked
 * (round joins, butt, round or square caps). Curves are flattened into
 * lines; a pixel's coverage comes from four sub-rows, each with the exact
 * parts of the columns it crosses (anti-aliased edges). Not: gradients and
 * patterns (their fallback colour, else grey), clipping, masks, filters,
 * markers, dashes, embedded pictures.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "browser.h"
#include "image.h"
#include "wfont.h"

#define SVG_DEPTH 32                    /* groups and <use>s within each other */
#define POINTS_MAX 1000000              /* per drawing: what a hostile file may make us keep */

struct matrix {                         /* x' = a x + c y + e, y' = b x + d y + f */
    double a, b, c, d, e, f;
};

static const struct matrix IDENTITY = { 1, 0, 0, 1, 0, 0 };

static struct matrix multiply(struct matrix m, struct matrix n)     /* m then n inside it */
{
    return (struct matrix){ m.a * n.a + m.c * n.b, m.b * n.a + m.d * n.b,
                            m.a * n.c + m.c * n.d, m.b * n.c + m.d * n.d,
                            m.a * n.e + m.c * n.f + m.e, m.b * n.e + m.d * n.f + m.f };
}

static void apply(const struct matrix *m, double x, double y, double *ox, double *oy)
{
    *ox = m->a * x + m->c * y + m->e;
    *oy = m->b * x + m->d * y + m->f;
}

/* --- polylines (in the picture's pixels) --- */

struct line {
    float *p;                           /* x, y pairs */
    int n, room;
    bool closed;
};

struct lines {
    struct line *l;
    int n, room;
    long points;                        /* (all of them, against POINTS_MAX) */
};

static struct line *new_line(struct lines *ls)
{
    if (ls->n == ls->room) {
        int room = ls->room ? 2 * ls->room : 8;
        struct line *l = realloc(ls->l, sizeof(*l) * (size_t)room);
        if (!l)
            return 0;
        ls->l = l, ls->room = room;
    }
    struct line *l = &ls->l[ls->n++];
    memset(l, 0, sizeof(*l));
    return l;
}

static void add_point(struct lines *ls, double x, double y)
{
    if (!ls->n || ls->points >= POINTS_MAX)
        return;
    struct line *l = &ls->l[ls->n - 1];
    if (!isfinite(x) || !isfinite(y))
        return;
    x = x > 1e6 ? 1e6 : x < -1e6 ? -1e6 : x;           /* (far outside any picture: as far as any) */
    y = y > 1e6 ? 1e6 : y < -1e6 ? -1e6 : y;
    if (l->n && fabs(l->p[2 * l->n - 2] - x) < 0.01 && fabs(l->p[2 * l->n - 1] - y) < 0.01)
        return;                         /* (the same point again) */
    if (l->n == l->room) {
        int room = l->room ? 2 * l->room : 16;
        float *p = realloc(l->p, sizeof(float) * 2 * (size_t)room);
        if (!p)
            return;
        l->p = p, l->room = room;
    }
    l->p[2 * l->n] = (float)x, l->p[2 * l->n + 1] = (float)y;
    l->n++;
    ls->points++;
}

static void free_lines(struct lines *ls)
{
    for (int i = 0; i < ls->n; i++)
        free(ls->l[i].p);
    free(ls->l);
    memset(ls, 0, sizeof(*ls));
}

/* --- filling --- */

struct edge {
    float x0, y0, x1, y1;               /* y0 < y1 */
    int dir;
};

struct edges {
    struct edge *e;
    int n, room;
};

static void add_edge(struct edges *es, float x0, float y0, float x1, float y1)
{
    if (y0 == y1)
        return;
    if (es->n == es->room) {
        int room = es->room ? 2 * es->room : 64;
        struct edge *e = realloc(es->e, sizeof(*e) * (size_t)room);
        if (!e)
            return;
        es->e = e, es->room = room;
    }
    if (y0 < y1)
        es->e[es->n++] = (struct edge){ x0, y0, x1, y1, 1 };
    else
        es->e[es->n++] = (struct edge){ x1, y1, x0, y0, -1 };
}

static int by_top(const void *a, const void *b)
{
    float d = ((const struct edge *)a)->y0 - ((const struct edge *)b)->y0;
    return d < 0 ? -1 : d > 0;
}

/* Coverage `amount` over [xa, xb) of a row (the ends in part). */
static void cover(float *row, int w, float xa, float xb, float amount)
{
    if (!(xb > xa) || xb <= 0 || xa >= w)
        return;                                 /* (also NaN: no comparison holds) */
    if (xa < 0)
        xa = 0;
    if (xb > w)
        xb = (float)w;
    int ia = (int)xa, ib = (int)xb;
    if (ia == ib) {
        row[ia] += amount * (xb - xa);
        return;
    }
    row[ia] += amount * ((float)(ia + 1) - xa);
    for (int x = ia + 1; x < ib; x++)
        row[x] += amount;
    if (ib < w)
        row[ib] += amount * (xb - (float)ib);
}

/* src (0xRRGGBB at alpha 0-255) over the picture's pixel. */
static void blend(uint8_t *p, uint32_t rgb, float alpha)
{
    if (alpha <= 0)
        return;
    if (alpha > 1)
        alpha = 1;
    float da = p[3] / 255.0f, oa = alpha + da * (1 - alpha);
    if (oa <= 0)
        return;
    float comp[3] = { (float)(rgb >> 16 & 255), (float)(rgb >> 8 & 255), (float)(rgb & 255) };
    for (int k = 0; k < 3; k++)
        p[k] = (uint8_t)((comp[k] * alpha + p[k] * da * (1 - alpha)) / oa + 0.5f);
    p[3] = (uint8_t)(oa * 255 + 0.5f);
}

static void fill_edges(struct image *img, struct edges *es, bool evenodd, uint32_t rgb, float alpha)
{
    if (!es->n || alpha <= 0)
        return;
    qsort(es->e, (size_t)es->n, sizeof(*es->e), by_top);
    float top = es->e[0].y0, bottom = es->e[0].y1;
    for (int i = 1; i < es->n; i++)
        if (es->e[i].y1 > bottom)
            bottom = es->e[i].y1;
    if (bottom <= 0 || top >= img->h)
        return;
    int y0 = top < 0 ? 0 : (int)top, y1 = bottom > img->h ? img->h : (int)ceilf(bottom);
    float *row = calloc((size_t)img->w + 1, sizeof(float));
    float *xs = malloc(sizeof(float) * (size_t)es->n);
    int *dirs = malloc(sizeof(int) * (size_t)es->n);
    if (!row || !xs || !dirs) {
        free(row), free(xs), free(dirs);
        return;
    }
    int from = 0;
    for (int y = y0; y < y1; y++) {
        memset(row, 0, sizeof(float) * (size_t)img->w);
        bool any = false;
        while (from < es->n && es->e[from].y1 <= (float)y)
            from++;                             /* (edges sorted by top: these ended above) */
        for (int sub = 0; sub < 4; sub++) {
            float sy = (float)y + (sub + 0.5f) / 4;
            int k = 0;
            for (int i = from; i < es->n && es->e[i].y0 <= sy; i++) {
                const struct edge *e = &es->e[i];
                if (sy >= e->y1)
                    continue;
                float x = e->x0 + (sy - e->y0) * (e->x1 - e->x0) / (e->y1 - e->y0);
                int at = k++;                   /* (kept sorted by x) */
                while (at > 0 && xs[at - 1] > x) {
                    xs[at] = xs[at - 1], dirs[at] = dirs[at - 1];
                    at--;
                }
                xs[at] = x, dirs[at] = e->dir;
            }
            int wind = 0;
            for (int i = 0; i + 1 < k; i++) {
                wind += dirs[i];
                if (evenodd ? (i + 1) % 2 : wind != 0) {
                    cover(row, img->w, xs[i], xs[i + 1], 0.25f);
                    any = true;
                }
            }
        }
        if (!any)
            continue;
        uint8_t *p = img->pixels + (size_t)y * img->stride;
        for (int x = 0; x < img->w; x++)
            if (row[x] > 0.001f)
                blend(p + 4 * x, rgb, alpha * (row[x] > 1 ? 1 : row[x]));
    }
    free(row), free(xs), free(dirs);
}

/* --- strokes --- */

static void polygon(struct edges *es, const float *p, int n)
{
    double area = 0;                            /* (each the same way round: their union fills) */
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += (double)p[2 * i] * p[2 * j + 1] - (double)p[2 * j] * p[2 * i + 1];
    }
    for (int i = 0; i < n; i++) {
        int a = area >= 0 ? i : n - 1 - i, b = area >= 0 ? (i + 1) % n : (2 * n - 2 - i) % n;
        add_edge(es, p[2 * a], p[2 * a + 1], p[2 * b], p[2 * b + 1]);
    }
}

static void dot(struct edges *es, float x, float y, float r)
{
    float p[2 * 16];
    int n = r < 2 ? 8 : 16;
    for (int i = 0; i < n; i++) {
        double t = 2 * M_PI * i / n;
        p[2 * i] = x + r * (float)cos(t), p[2 * i + 1] = y + r * (float)sin(t);
    }
    polygon(es, p, n);
}

/* A polyline's outline `width` wide: a box per segment, a dot at each
 * joint (round joins), its ends by `cap` (0 butt, 1 round, 2 square). */
static void stroke_line(struct edges *es, const struct line *l, float width, int cap)
{
    float h = width / 2;
    int n = l->n;
    if (n < 2)
        return;
    int segments = l->closed ? n : n - 1;
    for (int i = 0; i < segments; i++) {
        float x0 = l->p[2 * i], y0 = l->p[2 * i + 1];
        float x1 = l->p[2 * ((i + 1) % n)], y1 = l->p[2 * ((i + 1) % n) + 1];
        float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
        if (len < 1e-6f)
            continue;
        float nx = -dy / len * h, ny = dx / len * h;
        if (!l->closed && cap == 2) {           /* (square: the ends half the width further) */
            float ex = dx / len * h, ey = dy / len * h;
            if (i == 0)
                x0 -= ex, y0 -= ey;
            if (i == segments - 1)
                x1 += ex, y1 += ey;
        }
        float q[8] = { x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny };
        polygon(es, q, 4);
    }
    for (int i = 0; i < n; i++) {
        bool end = !l->closed && (i == 0 || i == n - 1);
        if ((!end || cap == 1) && h > 0.6f)
            dot(es, l->p[2 * i], l->p[2 * i + 1], h);
    }
}

/* --- paths --- */

struct pen {
    struct lines *ls;
    struct matrix m;
    double x, y, sx, sy;                /* where it is, and where the subpath began (user units) */
    double scale;                       /* the transform's size (for how finely to flatten) */
};

static void move_to(struct pen *p, double x, double y)
{
    double dx, dy;
    p->x = p->sx = x, p->y = p->sy = y;
    if (!new_line(p->ls))
        return;
    apply(&p->m, x, y, &dx, &dy);
    add_point(p->ls, dx, dy);
}

static void line_to(struct pen *p, double x, double y)
{
    double dx, dy;
    if (!p->ls->n)
        move_to(p, p->x, p->y);
    apply(&p->m, x, y, &dx, &dy);
    add_point(p->ls, dx, dy);
    p->x = x, p->y = y;
}

static void close_path(struct pen *p)
{
    if (p->ls->n)
        p->ls->l[p->ls->n - 1].closed = true;
    p->x = p->sx, p->y = p->sy;
}

static void cubic_to(struct pen *p, double x1, double y1, double x2, double y2, double x, double y)
{
    double len = (fabs(x1 - p->x) + fabs(y1 - p->y) + fabs(x2 - x1) + fabs(y2 - y1) + fabs(x - x2) + fabs(y - y2)) *
                 p->scale;
    double pieces = len / 3;
    int n = !(pieces > 2) ? 2 : pieces > 64 ? 64 : (int)pieces;      /* (NaN and infinity too) */
    double x0 = p->x, y0 = p->y;
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        line_to(p, u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x,
                   u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y);
    }
}

static void quad_to(struct pen *p, double x1, double y1, double x, double y)
{
    cubic_to(p, p->x + 2.0 / 3 * (x1 - p->x), p->y + 2.0 / 3 * (y1 - p->y), x + 2.0 / 3 * (x1 - x),
             y + 2.0 / 3 * (y1 - y), x, y);
}

/* An elliptical arc to (x, y) (SVG's endpoint form, its appendix B.2.4). */
static void arc_to(struct pen *p, double rx, double ry, double angle, bool large, bool sweep, double x, double y)
{
    double x0 = p->x, y0 = p->y;
    rx = fabs(rx), ry = fabs(ry);
    if (rx < 1e-9 || ry < 1e-9 || (x0 == x && y0 == y)) {
        line_to(p, x, y);
        return;
    }
    double phi = angle * M_PI / 180, cs = cos(phi), sn = sin(phi);
    double dx = (x0 - x) / 2, dy = (y0 - y) / 2;
    double x1 = cs * dx + sn * dy, y1 = -sn * dx + cs * dy;
    double lambda = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
    if (lambda > 1)
        rx *= sqrt(lambda), ry *= sqrt(lambda);
    double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    double k = den > 0 && num > 0 ? sqrt(num / den) : 0;
    if (large == sweep)
        k = -k;
    double cx1 = k * rx * y1 / ry, cy1 = -k * ry * x1 / rx;
    double cx = cs * cx1 - sn * cy1 + (x0 + x) / 2, cy = sn * cx1 + cs * cy1 + (y0 + y) / 2;
    double t1 = atan2((y1 - cy1) / ry, (x1 - cx1) / rx);
    double t2 = atan2((-y1 - cy1) / ry, (-x1 - cx1) / rx);
    double dt = t2 - t1;
    if (sweep && dt < 0)
        dt += 2 * M_PI;
    else if (!sweep && dt > 0)
        dt -= 2 * M_PI;
    double pieces = fabs(dt) * (rx > ry ? rx : ry) * p->scale / 3;
    int n = !(pieces > 4) ? 4 : pieces > 128 ? 128 : (int)pieces;
    for (int i = 1; i <= n; i++) {
        double t = t1 + dt * i / n;
        double ex = rx * cos(t), ey = ry * sin(t);
        line_to(p, cs * ex - sn * ey + cx, sn * ex + cs * ey + cy);
    }
    p->x = x, p->y = y;
}

/* The next number of a path or a list (commas and blanks between). */
static bool number(const char **s, double *out)
{
    const char *p = *s;
    while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    char *end;
    double v = strtod(p, &end);
    if (end == p)
        return false;
    *out = v;
    *s = end;
    return true;
}

static bool flag(const char **s, bool *out)     /* (an arc's flags may run together: "11") */
{
    const char *p = *s;
    while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p != '0' && *p != '1')
        return false;
    *out = *p == '1';
    *s = p + 1;
    return true;
}

static void path_data(struct pen *pen, const char *d)
{
    char cmd = 0;
    double lx = 0, ly = 0;                      /* the last control point (S, T) */
    char last = 0;
    for (const char *s = d; *s;) {
        while (*s == ' ' || *s == ',' || *s == '\t' || *s == '\n' || *s == '\r')
            s++;
        if (!*s)
            break;
        bool lettered = (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z');
        if (lettered)
            cmd = *s++;
        else if (!cmd)
            return;                             /* (numbers before any command: wrong) */
        bool rel = cmd >= 'a';
        double ox = rel ? pen->x : 0, oy = rel ? pen->y : 0, v[7];
        bool f1, f2;
        const char *before = s;
        switch (cmd | 0x20) {
        case 'm':
            if (!number(&s, &v[0]) || !number(&s, &v[1]))
                return;
            move_to(pen, ox + v[0], oy + v[1]);
            cmd = rel ? 'l' : 'L';              /* (more pairs after a moveto are linetos) */
            break;
        case 'l':
            if (!number(&s, &v[0]) || !number(&s, &v[1]))
                return;
            line_to(pen, ox + v[0], oy + v[1]);
            break;
        case 'h':
            if (!number(&s, &v[0]))
                return;
            line_to(pen, ox + v[0], pen->y);
            break;
        case 'v':
            if (!number(&s, &v[0]))
                return;
            line_to(pen, pen->x, oy + v[0]);
            break;
        case 'c':
            for (int i = 0; i < 6; i++)
                if (!number(&s, &v[i]))
                    return;
            cubic_to(pen, ox + v[0], oy + v[1], ox + v[2], oy + v[3], ox + v[4], oy + v[5]);
            lx = ox + v[2], ly = oy + v[3];
            break;
        case 's':
            for (int i = 0; i < 4; i++)
                if (!number(&s, &v[i]))
                    return;
            {
                bool smooth = (last | 0x20) == 'c' || (last | 0x20) == 's';
                double cx = smooth ? 2 * pen->x - lx : pen->x, cy = smooth ? 2 * pen->y - ly : pen->y;
                cubic_to(pen, cx, cy, ox + v[0], oy + v[1], ox + v[2], oy + v[3]);
                lx = ox + v[0], ly = oy + v[1];
            }
            break;
        case 'q':
            for (int i = 0; i < 4; i++)
                if (!number(&s, &v[i]))
                    return;
            quad_to(pen, ox + v[0], oy + v[1], ox + v[2], oy + v[3]);
            lx = ox + v[0], ly = oy + v[1];
            break;
        case 't':
            if (!number(&s, &v[0]) || !number(&s, &v[1]))
                return;
            {
                bool smooth = (last | 0x20) == 'q' || (last | 0x20) == 't';
                double cx = smooth ? 2 * pen->x - lx : pen->x, cy = smooth ? 2 * pen->y - ly : pen->y;
                quad_to(pen, cx, cy, ox + v[0], oy + v[1]);
                lx = cx, ly = cy;
            }
            break;
        case 'a':
            if (!number(&s, &v[0]) || !number(&s, &v[1]) || !number(&s, &v[2]) || !flag(&s, &f1) ||
                !flag(&s, &f2) || !number(&s, &v[3]) || !number(&s, &v[4]))
                return;
            arc_to(pen, v[0], v[1], v[2], f1, f2, ox + v[3], oy + v[4]);
            break;
        case 'z':
            if (!lettered)
                return;                         /* (numbers after a Z: wrong, and nothing to take) */
            close_path(pen);
            break;
        default:
            return;                             /* (a letter that is no command) */
        }
        last = cmd;
        if (s == before && (cmd | 0x20) != 'z')
            return;                             /* (nothing taken: stop, never loop) */
    }
}

/* --- elements --- */

struct drawing {
    struct image *img;
    const struct node *root;            /* (for <use>'s ids) */
    double vw, vh;                      /* the viewport in user units (percentages) */
    int depth;
};

/* A length attribute in user units: a number (px), or a percentage of
 * `of`; fallback when there is none. */
static double length_attr(const struct node *e, const char *name, double of, double fallback)
{
    const char *v = node_attr(e, name);
    if (!v)
        return fallback;
    char *end;
    double x = strtod(v, &end);
    if (end == v)
        return fallback;
    if (*end == '%')
        return x * of / 100;
    if (end[0] == 'e' && end[1] == 'm')
        return x * 16;
    if (end[0] == 'p' && end[1] == 't')
        return x * 4 / 3;
    return x;
}

static struct matrix transform_of(const char *t)
{
    struct matrix m = IDENTITY;
    for (const char *s = t; s && *s;) {
        while (*s == ' ' || *s == ',' || *s == '\t' || *s == '\n')
            s++;
        const char *name = s;
        while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z'))
            s++;
        size_t n = (size_t)(s - name);
        while (*s == ' ')
            s++;
        if (!n || *s != '(')
            break;
        s++;
        double v[6] = { 0 };
        int k = 0;
        while (k < 6 && number(&s, &v[k]))
            k++;
        while (*s && *s != ')')
            s++;
        if (*s)
            s++;
        struct matrix t2 = IDENTITY;
        if (n == 6 && !strncmp(name, "matrix", 6) && k == 6)
            t2 = (struct matrix){ v[0], v[1], v[2], v[3], v[4], v[5] };
        else if (n == 9 && !strncmp(name, "translate", 9))
            t2.e = v[0], t2.f = k > 1 ? v[1] : 0;
        else if (n == 5 && !strncmp(name, "scale", 5))
            t2.a = v[0], t2.d = k > 1 ? v[1] : v[0];
        else if (n == 6 && !strncmp(name, "rotate", 6)) {
            double r = v[0] * M_PI / 180;
            struct matrix rot = { cos(r), sin(r), -sin(r), cos(r), 0, 0 };
            if (k == 3) {                       /* (about a point) */
                struct matrix to = { 1, 0, 0, 1, v[1], v[2] }, back = { 1, 0, 0, 1, -v[1], -v[2] };
                t2 = multiply(multiply(to, rot), back);
            } else
                t2 = rot;
        } else if (n == 5 && !strncmp(name, "skewX", 5))
            t2.c = tan(v[0] * M_PI / 180);
        else if (n == 5 && !strncmp(name, "skewY", 5))
            t2.b = tan(v[0] * M_PI / 180);
        m = multiply(m, t2);
    }
    return m;
}

/* A viewBox of (x, y, w, h) into a viewport w x h, by preserveAspectRatio. */
static struct matrix view_box(const struct node *e, double width, double height, double *vw, double *vh)
{
    const char *v = node_attr(e, "viewbox");
    double b[4];
    const char *s = v;
    if (!v || !number(&s, &b[0]) || !number(&s, &b[1]) || !number(&s, &b[2]) || !number(&s, &b[3]) ||
        b[2] <= 0 || b[3] <= 0) {
        *vw = width, *vh = height;
        return IDENTITY;
    }
    *vw = b[2], *vh = b[3];
    double sx = width / b[2], sy = height / b[3];
    const char *par = node_attr(e, "preserveaspectratio");
    if (par && strstr(par, "none"))
        return (struct matrix){ sx, 0, 0, sy, -b[0] * sx, -b[1] * sy };
    bool slice = par && strstr(par, "slice");
    double k = slice ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
    double fx = 0.5, fy = 0.5;                  /* (xMidYMid) */
    if (par && strstr(par, "xMin"))
        fx = 0;
    if (par && strstr(par, "xMax"))
        fx = 1;
    if (par && strstr(par, "YMin"))
        fy = 0;
    if (par && strstr(par, "YMax"))
        fy = 1;
    return (struct matrix){ k, 0, 0, k, (width - b[2] * k) * fx - b[0] * k, (height - b[3] * k) * fy - b[1] * k };
}

static const struct node *by_id(const struct node *root, const char *id)
{
    for (const struct node *n = root; n; n = node_next((struct node *)n, (struct node *)root)) {
        const char *v = n->kind == NODE_ELEMENT ? node_attr(n, "id") : 0;
        if (v && !strcmp(v, id))
            return n;
    }
    return 0;
}

static void draw_children(struct drawing *g, const struct node *e, struct matrix m, float opacity);

/* Fill then stroke what the pen made, in e's paint. */
static void paint(struct drawing *g, const struct node *e, struct lines *ls, struct matrix m, float opacity,
                  bool fill_ok)
{
    const struct style *s = e->style;
    if (!s || !ls->n)
        return;
    if (fill_ok && s->fill_alpha > 0) {
        struct edges es = { 0 };
        for (int i = 0; i < ls->n; i++) {
            const struct line *l = &ls->l[i];
            for (int k = 0; k < l->n; k++) {    /* (every subpath closed for filling) */
                int j = (k + 1) % l->n;
                add_edge(&es, l->p[2 * k], l->p[2 * k + 1], l->p[2 * j], l->p[2 * j + 1]);
            }
        }
        fill_edges(g->img, &es, s->evenodd, s->fill_current ? s->color : s->fill, opacity * s->fill_alpha / 255.0f);
        free(es.e);
    }
    if (s->stroke_alpha > 0 && s->stroke_width > 0) {
        double scale = sqrt(fabs(m.a * m.d - m.b * m.c));
        float width = (float)(s->stroke_width * scale);
        if (width < 0.5f)
            width = 0.5f;                       /* (hairlines: still seen) */
        const char *cap = node_attr(e, "stroke-linecap");
        int c = cap && !strcmp(cap, "round") ? 1 : cap && !strcmp(cap, "square") ? 2 : 0;
        struct edges es = { 0 };
        for (int i = 0; i < ls->n; i++)
            stroke_line(&es, &ls->l[i], width, c);
        fill_edges(g->img, &es, false, s->stroke_current ? s->color : s->stroke,
                   opacity * s->stroke_alpha / 255.0f);
        free(es.e);
    }
}

/* <text>: its characters in the system's font at its size, from (x, y),
 * its baseline there (only moved by the transform, not scaled). */
static void text(struct drawing *g, const struct node *e, struct matrix m, float opacity)
{
    const struct style *s = e->style;
    if (!s || s->fill_alpha == 0)
        return;
    char *t = node_text(e);
    if (!t || !*t) {
        free(t);
        return;
    }
    double x = length_attr(e, "x", g->vw, 0), y = length_attr(e, "y", g->vh, 0);
    double px, py;
    apply(&m, x, y, &px, &py);
    int scale;
    bool fake;
    const struct font *f = style_font(s, &scale, &fake);
    int tw = wfont_text_width(f, t), th = wfont_height(f);
    const char *anchor = node_attr(e, "text-anchor");
    if (anchor && !strcmp(anchor, "middle"))
        px -= tw * scale / 2.0;
    else if (anchor && !strcmp(anchor, "end"))
        px -= tw * scale;
    uint32_t *buf = calloc((size_t)(tw + 1) * th, 4);
    if (buf) {
        wfont_text(buf, (uint32_t)(tw + 1), (uint32_t)(tw + 1), (uint32_t)th, f, 0, 0, t, 1, 0);
        if (!(px > -1e5 && px < 1e5 && py > -1e5 && py < 1e5)) {
            free(buf);
            free(t);
            return;
        }
        int ox = (int)px, oy = (int)py - (th - 3) * scale;     /* (the baseline about 3 above the bottom) */
        uint32_t rgb = s->fill_current ? s->color : s->fill;
        float a = opacity * s->fill_alpha / 255.0f;
        for (int r = 0; r < th * scale; r++)
            for (int c = 0; c < (tw + 1) * scale; c++) {
                int X = ox + c, Y = oy + r;
                if (X < 0 || Y < 0 || X >= g->img->w || Y >= g->img->h)
                    continue;
                if (buf[(r / scale) * (tw + 1) + c / scale] == 1)
                    blend(g->img->pixels + (size_t)Y * g->img->stride + 4 * X, rgb, a);
            }
        free(buf);
    }
    free(t);
}

static void draw(struct drawing *g, const struct node *e, struct matrix m, float opacity)
{
    if (e->kind != NODE_ELEMENT || !e->style || e->style->display == DISPLAY_NONE || g->depth >= SVG_DEPTH)
        return;
    static const char *const SKIP[] = { "defs", "symbol", "clippath", "mask", "title", "desc", "metadata",
                                        "style", "script", "lineargradient", "radialgradient", "pattern",
                                        "marker", "filter", "image", "foreignobject", 0 };
    for (int i = 0; SKIP[i]; i++)
        if (!strcmp(e->tag, SKIP[i]))
            return;
    const char *t = node_attr(e, "transform");
    if (t)
        m = multiply(m, transform_of(t));
    opacity *= e->style->opacity / 255.0f;
    if (opacity <= 0)
        return;
    g->depth++;
    struct lines ls = { 0 };
    struct pen pen = { &ls, m, 0, 0, 0, 0, sqrt(fabs(m.a * m.d - m.b * m.c)) };
    double vw = g->vw, vh = g->vh, diag = sqrt((vw * vw + vh * vh) / 2);
    bool fill_ok = true, hidden = e->style->hidden;
    if (!strcmp(e->tag, "g") || !strcmp(e->tag, "a") || !strcmp(e->tag, "switch")) {
        draw_children(g, e, m, opacity);
    } else if (!strcmp(e->tag, "svg")) {        /* a nested viewport */
        double x = length_attr(e, "x", vw, 0), y = length_attr(e, "y", vh, 0);
        double w = length_attr(e, "width", vw, vw), h = length_attr(e, "height", vh, vh);
        double nvw, nvh;
        struct matrix inner = view_box(e, w, h, &nvw, &nvh);
        struct matrix at = { 1, 0, 0, 1, x, y };
        double saved_w = g->vw, saved_h = g->vh;
        g->vw = nvw, g->vh = nvh;
        draw_children(g, e, multiply(multiply(m, at), inner), opacity);
        g->vw = saved_w, g->vh = saved_h;
    } else if (!strcmp(e->tag, "use")) {
        const char *href = node_attr(e, "href");
        if (!href)
            href = node_attr(e, "xlink:href");
        const struct node *target = href && href[0] == '#' ? by_id(g->root, href + 1) : 0;
        if (target) {
            struct matrix at = { 1, 0, 0, 1, length_attr(e, "x", vw, 0), length_attr(e, "y", vh, 0) };
            m = multiply(m, at);
            if (!strcmp(target->tag, "symbol")) {
                double w = length_attr(e, "width", vw, vw), h = length_attr(e, "height", vh, vh);
                double nvw, nvh;
                struct matrix inner = view_box(target, w, h, &nvw, &nvh);
                double saved_w = g->vw, saved_h = g->vh;
                g->vw = nvw, g->vh = nvh;
                draw_children(g, target, multiply(m, inner), opacity);
                g->vw = saved_w, g->vh = saved_h;
            } else {
                draw(g, target, m, opacity);
            }
        }
    } else if (hidden) {
        /* (visibility: hidden: nothing of its own) */
    } else if (!strcmp(e->tag, "path")) {
        const char *d = node_attr(e, "d");
        if (d)
            path_data(&pen, d);
    } else if (!strcmp(e->tag, "rect")) {
        double x = length_attr(e, "x", vw, 0), y = length_attr(e, "y", vh, 0);
        double w = length_attr(e, "width", vw, 0), h = length_attr(e, "height", vh, 0);
        double rx = length_attr(e, "rx", vw, -1), ry = length_attr(e, "ry", vh, -1);
        if (rx < 0)
            rx = ry < 0 ? 0 : ry;
        if (ry < 0)
            ry = rx;
        rx = rx > w / 2 ? w / 2 : rx, ry = ry > h / 2 ? h / 2 : ry;
        if (w > 0 && h > 0) {
            move_to(&pen, x + rx, y);
            line_to(&pen, x + w - rx, y);
            if (rx > 0)
                arc_to(&pen, rx, ry, 0, false, true, x + w, y + ry);
            line_to(&pen, x + w, y + h - ry);
            if (rx > 0)
                arc_to(&pen, rx, ry, 0, false, true, x + w - rx, y + h);
            line_to(&pen, x + rx, y + h);
            if (rx > 0)
                arc_to(&pen, rx, ry, 0, false, true, x, y + h - ry);
            line_to(&pen, x, y + ry);
            if (rx > 0)
                arc_to(&pen, rx, ry, 0, false, true, x + rx, y);
            close_path(&pen);
        }
    } else if (!strcmp(e->tag, "circle") || !strcmp(e->tag, "ellipse")) {
        bool circle = e->tag[0] == 'c';
        double cx = length_attr(e, "cx", vw, 0), cy = length_attr(e, "cy", vh, 0);
        double rx = circle ? length_attr(e, "r", diag, 0) : length_attr(e, "rx", vw, 0);
        double ry = circle ? rx : length_attr(e, "ry", vh, 0);
        if (rx > 0 && ry > 0) {
            move_to(&pen, cx + rx, cy);
            arc_to(&pen, rx, ry, 0, false, true, cx - rx, cy);
            arc_to(&pen, rx, ry, 0, false, true, cx + rx, cy);
            close_path(&pen);
        }
    } else if (!strcmp(e->tag, "line")) {
        move_to(&pen, length_attr(e, "x1", vw, 0), length_attr(e, "y1", vh, 0));
        line_to(&pen, length_attr(e, "x2", vw, 0), length_attr(e, "y2", vh, 0));
        fill_ok = false;
    } else if (!strcmp(e->tag, "polyline") || !strcmp(e->tag, "polygon")) {
        const char *pts = node_attr(e, "points");
        double x, y;
        bool first = true;
        for (const char *s = pts; s && number(&s, &x) && number(&s, &y); first = false)
            if (first)
                move_to(&pen, x, y);
            else
                line_to(&pen, x, y);
        if (!strcmp(e->tag, "polygon"))
            close_path(&pen);
    } else if (!strcmp(e->tag, "text")) {
        text(g, e, m, opacity);
    }
    if (ls.n)
        paint(g, e, &ls, m, opacity, fill_ok);
    free_lines(&ls);
    g->depth--;
}

static void draw_children(struct drawing *g, const struct node *e, struct matrix m, float opacity)
{
    for (const struct node *k = e->first; k; k = k->next)
        draw(g, k, m, opacity);
}

void svg_size(const struct node *svg, int *w, int *h)
{
    double vb[4];
    const char *v = node_attr(svg, "viewbox"), *s = v;
    bool has_box = v && number(&s, &vb[0]) && number(&s, &vb[1]) && number(&s, &vb[2]) && number(&s, &vb[3]) &&
                   vb[2] > 0 && vb[3] > 0;
    const char *wa = node_attr(svg, "width"), *ha = node_attr(svg, "height");
    double W = wa && !strchr(wa, '%') ? length_attr(svg, "width", 0, -1) : -1;
    double H = ha && !strchr(ha, '%') ? length_attr(svg, "height", 0, -1) : -1;
    if (W < 0 && H < 0) {
        W = has_box ? vb[2] : 300, H = has_box ? vb[3] : 150;
    } else if (W < 0) {
        W = has_box ? H * vb[2] / vb[3] : 300;
    } else if (H < 0) {
        H = has_box ? W * vb[3] / vb[2] : 150;
    }
    *w = !(W >= 1) ? 1 : W > 4096 ? 4096 : (int)(W + 0.5);      /* (NaN: 1) */
    *h = !(H >= 1) ? 1 : H > 4096 ? 4096 : (int)(H + 0.5);
}

struct image *svg_draw(const struct node *svg, int w, int h)
{
    if (w < 1 || h < 1 || w > 4096 || h > 4096)
        return 0;
    struct image *img = calloc(1, sizeof(*img));
    if (!img)
        return 0;
    img->w = w, img->h = h, img->stride = 4 * w, img->format = PIXEL_RGBA8888;
    img->pixels = calloc((size_t)w * h, 4);
    if (!img->pixels) {
        free(img);
        return 0;
    }
    struct drawing g = { img, svg, 0, 0, 0 };
    double vw, vh;
    struct matrix m = view_box(svg, w, h, &vw, &vh);
    g.vw = vw, g.vh = vh;
    if (svg->style && svg->style->display != DISPLAY_NONE)
        draw_children(&g, svg, m, svg->style->opacity / 255.0f);
    return img;
}

bool svg_looks_like(const char *text, size_t n, const char *type)
{
    if (type && strstr(type, "svg"))
        return true;
    size_t i = 0;
    if (n >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB)
        i = 3;                                  /* (a byte order mark) */
    while (i < n && is_space(text[i]))
        i++;
    if (i + 4 < n && !strncmp(text + i, "<svg", 4))
        return true;
    if (i + 5 < n && (!strncmp(text + i, "<?xml", 5) || !strncmp(text + i, "<!DOC", 5))) {
        size_t look = n - i < 2048 ? n - i : 2048;     /* (an XML prologue, then the svg) */
        for (size_t k = i; k + 4 < i + look; k++)
            if (!strncmp(text + k, "<svg", 4))
                return true;
    }
    return false;
}

struct image *svg_load(const char *text, size_t n, int w, int h)
{
    struct node *tree = html_parse(text, n);
    if (!tree)
        return 0;
    struct node *svg = node_find(tree, "svg");
    struct image *img = 0;
    if (svg) {
        struct sheet *sheet = 0;                /* (its own <style>s) */
        for (struct node *k = tree; k; k = node_next(k, tree))
            if (k->kind == NODE_ELEMENT && !strcmp(k->tag, "style"))
                for (struct node *t = k->first; t; t = t->next)
                    if (t->kind == NODE_TEXT)
                        sheet = css_parse(t->text, sheet);
        css_apply(tree, sheet, 0, 0);
        int sw, sh;
        svg_size(svg, &sw, &sh);
        if (w > 0 && h <= 0)
            h = (int)((long)sh * w / sw);
        else if (h > 0 && w <= 0)
            w = (int)((long)sw * h / sh);
        img = svg_draw(svg, w > 0 ? w : sw, h > 0 ? h : sh);
        css_free(sheet);
    }
    html_free(tree);
    return img;
}

/*
 * The font file reader (see ttf.h), from the OpenType specification
 * (Microsoft's, 1.9): the table directory; head (units per em, loca's
 * form), maxp (glyphs), hhea and hmtx (sizes, advances); cmap (formats 4
 * and 12, Unicode subtables); loca and glyf (TrueType outlines: simple
 * glyphs, and composite ones made of others moved, scaled or matched by
 * points, COMPONENTS_DEEPEST deep); (41c-2) CFF (Adobe's technical notes
 * 5176 and 5177: its INDEXes and DICTs, Type 2 charstrings with their
 * subroutines, CID-keyed fonts' FDArray and FDSelect). Every value is read
 * through `u16`/`u32` at a checked place: a short or lying file gives
 * false, never a read outside it.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ttf.h"
#include "raster.h"

#define COMPONENTS_DEEPEST 8            /* composites inside composites */
#define POINTS_MAX 65536                /* points of one outline */

static bool fits(const struct ttf *f, uint32_t at, uint32_t n)
{
    return at <= f->size && n <= f->size - at;
}

static uint32_t u8(const struct ttf *f, uint32_t at)
{
    return fits(f, at, 1) ? f->data[at] : 0;
}

static uint32_t u16(const struct ttf *f, uint32_t at)
{
    return fits(f, at, 2) ? (uint32_t)f->data[at] << 8 | f->data[at + 1] : 0;
}

static int s16(const struct ttf *f, uint32_t at)
{
    return (int16_t)u16(f, at);
}

static uint32_t u32(const struct ttf *f, uint32_t at)
{
    return fits(f, at, 4) ? u16(f, at) << 16 | u16(f, at + 2) : 0;
}

/* A table's place and length; false when it is missing or outside the file. */
static bool table(const struct ttf *f, const char *tag, uint32_t *at, uint32_t *len)
{
    uint32_t n = u16(f, 4);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = 12 + 16 * i;
        if (!fits(f, r, 16))
            return false;
        if (memcmp(f->data + r, tag, 4) == 0) {
            *at = u32(f, r + 8);
            *len = u32(f, r + 12);
            return fits(f, *at, *len);
        }
    }
    return false;
}

/* The Unicode character map: format 12 of (3, 10) or (0, 4+), else format 4
 * of (3, 1) or (0, 0-3); Windows's (platform 3) before Unicode's (0) when
 * both are there and differ (as CoreText and FreeType choose). */
static void choose_cmap(struct ttf *f, uint32_t at, uint32_t len)
{
    uint32_t n = u16(f, at + 2), best = 0;
    for (uint32_t i = 0; i < n && fits(f, at + 4 + 8 * i, 8); i++) {
        uint32_t r = at + 4 + 8 * i, platform = u16(f, r), encoding = u16(f, r + 2), off = u32(f, r + 4);
        if (off >= len)
            continue;
        uint32_t sub = at + off, format = u16(f, sub), score = 0;
        uint32_t sublen = format == 12 ? u32(f, sub + 4) : u16(f, sub + 2);
        if (!fits(f, sub, sublen) || sub + sublen > at + len)
            continue;
        if (format == 12 && ((platform == 3 && encoding == 10) || (platform == 0 && encoding >= 4)))
            score = platform == 3 ? 4 : 3;
        else if (format == 4 && ((platform == 3 && encoding == 1) || (platform == 0 && encoding <= 3)))
            score = platform == 3 ? 2 : 1;
        if (score > best) {
            best = score;
            f->cmap = sub;
            f->cmap_end = sub + sublen;
            f->cmap_format = (int)format;
        }
    }
}

static bool cff_open(struct ttf *f, uint32_t at, uint32_t len);

bool ttf_open(struct ttf *f, const void *data, size_t size)
{
    memset(f, 0, sizeof(*f));
    f->data = data;
    f->size = size > 0x7FFFFFFF ? 0x7FFFFFFF : (uint32_t)size;
    uint32_t version = u32(f, 0), at, len;
    if (version != 0x00010000 && version != 0x74727565 && version != 0x4F54544F)  /* (1.0, 'true', 'OTTO') */
        return false;
    if (!table(f, "head", &at, &len) || len < 54)
        return false;
    f->upem = (int)u16(f, at + 18);
    f->loca_long = s16(f, at + 50) == 1;
    if (f->upem < 16 || f->upem > 16384)
        return false;
    if (!table(f, "maxp", &at, &len) || len < 6)
        return false;
    f->glyphs = (int)u16(f, at + 4);
    if (!table(f, "hhea", &at, &len) || len < 36)
        return false;
    f->ascender = s16(f, at + 4);
    f->descender = s16(f, at + 6);
    f->line_gap = s16(f, at + 8);
    f->advance_max = (int)u16(f, at + 10);
    f->hmetrics = (int)u16(f, at + 34);
    if (!table(f, "hmtx", &at, &len) || f->hmetrics < 1 || (uint32_t)f->hmetrics * 4 > len)
        return false;
    f->hmtx = at;
    f->hmtx_end = at + len;
    if (version == 0x4F54544F) {
        if (!table(f, "CFF ", &at, &len) || !cff_open(f, at, len))
            return false;
    } else {
        if (!table(f, "loca", &at, &len))
            return false;
        f->loca = at;
        f->loca_end = at + len;
        if (!table(f, "glyf", &at, &len))
            return false;
        f->glyf = at;
        f->glyf_end = at + len;
    }
    if (table(f, "cmap", &at, &len))
        choose_cmap(f, at, len);
    return f->cmap_format != 0;
}

int ttf_glyph(const struct ttf *f, uint32_t c)
{
    uint32_t at = f->cmap;
    if (f->cmap_format == 12) {
        uint32_t lo = 0, hi = u32(f, at + 12);
        if (hi > (f->cmap_end - at - 16) / 12)
            hi = (f->cmap_end - at - 16) / 12;
        while (lo < hi) {                               /* (groups sorted by their start) */
            uint32_t mid = lo + (hi - lo) / 2, g = at + 16 + 12 * mid;
            if (c < u32(f, g))
                hi = mid;
            else if (c > u32(f, g + 4))
                lo = mid + 1;
            else {
                uint32_t glyph = u32(f, g + 8) + (c - u32(f, g));
                return glyph < (uint32_t)f->glyphs ? (int)glyph : 0;
            }
        }
        return 0;
    }
    if (f->cmap_format != 4 || c > 0xFFFF)
        return 0;
    uint32_t segs = u16(f, at + 6) / 2;
    uint32_t ends = at + 14, starts = ends + 2 * segs + 2, deltas = starts + 2 * segs, ranges = deltas + 2 * segs;
    if (ranges + 2 * segs > f->cmap_end)
        return 0;
    uint32_t lo = 0, hi = segs;
    while (lo < hi) {                                   /* the first segment ending at or after c */
        uint32_t mid = lo + (hi - lo) / 2;
        if (u16(f, ends + 2 * mid) < c)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == segs || u16(f, starts + 2 * lo) > c)
        return 0;
    uint32_t delta = u16(f, deltas + 2 * lo), range = u16(f, ranges + 2 * lo), glyph;
    if (range == 0)
        glyph = (c + delta) & 0xFFFF;
    else {
        uint32_t g = ranges + 2 * lo + range + 2 * (c - u16(f, starts + 2 * lo));
        if (g + 2 > f->cmap_end)
            return 0;
        glyph = u16(f, g);
        if (glyph)
            glyph = (glyph + delta) & 0xFFFF;
    }
    return glyph < (uint32_t)f->glyphs ? (int)glyph : 0;
}

int ttf_advance(const struct ttf *f, int glyph)
{
    int i = glyph < f->hmetrics ? glyph : f->hmetrics - 1;
    return i < 0 ? 0 : (int)u16(f, f->hmtx + 4 * (uint32_t)i);
}

static bool add_point(struct ttf_shape *s, float x, float y, uint8_t kind)
{
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 64;
        struct ttf_point *p = cap > POINTS_MAX ? 0 : realloc(s->p, sizeof(*p) * (size_t)cap);
        if (!p) {
            s->failed = true;
            return false;
        }
        s->p = p;
        s->cap = cap;
    }
    s->p[s->n++] = (struct ttf_point){ x, y, kind };
    return true;
}

static bool end_contour(struct ttf_shape *s)
{
    if (s->contours == s->ccap) {
        int cap = s->ccap ? s->ccap * 2 : 16;
        int *e = realloc(s->ends, sizeof(*e) * (size_t)cap);
        if (!e) {
            s->failed = true;
            return false;
        }
        s->ends = e;
        s->ccap = cap;
    }
    s->ends[s->contours++] = s->n - 1;
    return true;
}

/* A glyph's data in glyf: its place and length (0: an empty glyph). */
static bool glyph_data(const struct ttf *f, int glyph, uint32_t *at, uint32_t *len)
{
    if (glyph < 0 || glyph >= f->glyphs)
        return false;
    uint32_t a, b, g = (uint32_t)glyph;
    if (f->loca_long) {
        if (f->loca + 4 * g + 8 > f->loca_end)
            return false;
        a = u32(f, f->loca + 4 * g);
        b = u32(f, f->loca + 4 * g + 4);
    } else {
        if (f->loca + 2 * g + 4 > f->loca_end)
            return false;
        a = 2 * u16(f, f->loca + 2 * g);
        b = 2 * u16(f, f->loca + 2 * g + 2);
    }
    if (b < a || b > f->glyf_end - f->glyf)
        return false;
    *at = f->glyf + a;
    *len = b - a;
    return true;
}

static bool simple_glyph(const struct ttf *f, uint32_t at, uint32_t end, int contours, struct ttf_shape *s)
{
    uint32_t ends = at + 10, n = 0;
    if (ends + 2 * (uint32_t)contours + 2 > end)
        return false;
    for (int i = 0; i < contours; i++) {
        uint32_t e = u16(f, ends + 2 * (uint32_t)i) + 1;
        if (e < n)                                      /* (ends only go up) */
            return false;
        n = e;
    }
    if (n > POINTS_MAX)
        return false;
    uint32_t p = ends + 2 * (uint32_t)contours;
    p += 2 + u16(f, p);                                 /* (past the instructions) */
    uint8_t *flags = malloc(n ? n : 1);
    if (!flags) {
        s->failed = true;
        return false;
    }
    for (uint32_t i = 0; i < n;) {
        if (p >= end) {
            free(flags);
            return false;
        }
        uint8_t fl = (uint8_t)u8(f, p++);
        flags[i++] = fl;
        if (fl & 8) {                                   /* (repeated) */
            uint32_t times = u8(f, p++);
            if (p > end || times > n - i) {
                free(flags);
                return false;
            }
            while (times--)
                flags[i++] = fl;
        }
    }
    /* x, then y: a byte (its sign from bit 4 or 5), the same as before, or a word */
    int base = s->n;
    int32_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (flags[i] & 2) {
            int d = (int)u8(f, p++);
            v += flags[i] & 16 ? d : -d;
        } else if (!(flags[i] & 16)) {
            v += s16(f, p);
            p += 2;
        }
        if (p > end || !add_point(s, (float)v, 0, flags[i] & 1 ? TTF_ON : TTF_OFF)) {
            free(flags);
            return false;
        }
    }
    v = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (flags[i] & 4) {
            int d = (int)u8(f, p++);
            v += flags[i] & 32 ? d : -d;
        } else if (!(flags[i] & 32)) {
            v += s16(f, p);
            p += 2;
        }
        if (p > end) {
            free(flags);
            return false;
        }
        s->p[base + (int)i].y = (float)v;
    }
    free(flags);
    for (int i = 0; i < contours; i++) {
        s->n = base + (int)u16(f, ends + 2 * (uint32_t)i) + 1;
        if (!end_contour(s))
            return false;
    }
    s->n = base + (int)n;
    return true;
}

static bool glyph_shape(const struct ttf *f, int glyph, struct ttf_shape *s, int depth);

/* F2Dot14: a 2.14 fixed-point number. */
static float f2dot14(const struct ttf *f, uint32_t at)
{
    return (float)s16(f, at) / 16384.0f;
}

static bool composite_glyph(const struct ttf *f, uint32_t at, uint32_t end, struct ttf_shape *s, int depth)
{
    uint32_t p = at + 10, flags;
    int base = s->n;                                    /* (this glyph's own first point) */
    do {
        if (p + 4 > end)
            return false;
        flags = u16(f, p);
        int component = (int)u16(f, p + 2);
        p += 4;
        int32_t a1, a2;
        if (flags & 1) {                                /* ARG_1_AND_2_ARE_WORDS */
            a1 = flags & 2 ? s16(f, p) : (int32_t)u16(f, p);
            a2 = flags & 2 ? s16(f, p + 2) : (int32_t)u16(f, p + 2);
            p += 4;
        } else {
            a1 = flags & 2 ? (int8_t)u8(f, p) : (int32_t)u8(f, p);
            a2 = flags & 2 ? (int8_t)u8(f, p + 1) : (int32_t)u8(f, p + 1);
            p += 2;
        }
        float xx = 1, xy = 0, yx = 0, yy = 1;            /* x' = xx x + yx y, y' = xy x + yy y */
        if (flags & 8) {                                /* WE_HAVE_A_SCALE */
            xx = yy = f2dot14(f, p);
            p += 2;
        } else if (flags & 0x40) {                      /* WE_HAVE_AN_X_AND_Y_SCALE */
            xx = f2dot14(f, p);
            yy = f2dot14(f, p + 2);
            p += 4;
        } else if (flags & 0x80) {                      /* WE_HAVE_A_TWO_BY_TWO */
            xx = f2dot14(f, p);
            xy = f2dot14(f, p + 2);
            yx = f2dot14(f, p + 4);
            yy = f2dot14(f, p + 6);
            p += 8;
        }
        if (p > end)
            return false;
        int first = s->n;
        if (!glyph_shape(f, component, s, depth + 1))
            return false;
        for (int i = first; i < s->n; i++) {
            float x = s->p[i].x, y = s->p[i].y;
            s->p[i].x = xx * x + yx * y;
            s->p[i].y = xy * x + yy * y;
        }
        float dx, dy;
        if (flags & 2) {                                /* ARGS_ARE_XY_VALUES: an offset */
            dx = (float)a1;
            dy = (float)a2;
            if ((flags & 0x800) && !(flags & 0x1000)) { /* SCALED_COMPONENT_OFFSET: transformed */
                dx = xx * (float)a1 + yx * (float)a2;
                dy = xy * (float)a1 + yy * (float)a2;
            }
        } else {                /* its point a2 onto point a1 of the components before it */
            if (a1 < 0 || base + a1 >= first || a2 < 0 || first + a2 >= s->n)
                return false;
            dx = s->p[base + a1].x - s->p[first + a2].x;
            dy = s->p[base + a1].y - s->p[first + a2].y;
        }
        for (int i = first; i < s->n; i++) {
            s->p[i].x += dx;
            s->p[i].y += dy;
        }
    } while (flags & 0x20);                             /* MORE_COMPONENTS */
    return true;
}

/* A glyph's left side bearing in hmtx. */
static int left_bearing(const struct ttf *f, int glyph)
{
    uint32_t at = glyph < f->hmetrics ? f->hmtx + 4 * (uint32_t)glyph + 2
                                      : f->hmtx + 4 * (uint32_t)f->hmetrics + 2 * (uint32_t)(glyph - f->hmetrics);
    return at + 2 <= f->hmtx_end ? s16(f, at) : 0;
}

static bool glyph_shape(const struct ttf *f, int glyph, struct ttf_shape *s, int depth)
{
    uint32_t at, len;
    if (depth > COMPONENTS_DEEPEST || !glyph_data(f, glyph, &at, &len))
        return false;
    if (len == 0)
        return true;                                    /* (no outline: a space) */
    if (len < 10)
        return false;
    int contours = s16(f, at);
    if (contours < 0)
        return composite_glyph(f, at, at + len, s, depth);
    int first = s->n;
    if (!simple_glyph(f, at, at + len, contours, s))
        return false;
    /* A simple glyph's origin is its leftmost x (its xMin) less hmtx's left
     * side bearing: where the two differ the outline moves (TrueType's
     * first phantom point; CoreText does so, also for a composite's
     * components, never for the composite itself). */
    float shift = (float)(left_bearing(f, glyph) - s16(f, at + 2));
    for (int i = first; i < s->n && shift != 0; i++)
        s->p[i].x += shift;
    return true;
}

static bool cff_shape(const struct ttf *f, int glyph, struct ttf_shape *s);

bool ttf_shape(const struct ttf *f, int glyph, struct ttf_shape *s)
{
    if (f->cff)
        return cff_shape(f, glyph, s) && !s->failed;
    return glyph_shape(f, glyph, s, 0) && !s->failed;
}

void ttf_shape_free(struct ttf_shape *s)
{
    free(s->p);
    free(s->ends);
    memset(s, 0, sizeof(*s));
}

/* --- (41c-2) CFF -------------------------------------------------------- */

#define CFF_STACK 48                    /* Type 2's argument stack */
#define CFF_DEEPEST 10                  /* subroutines within subroutines */
#define CFF_OPS 20000                   /* operators a glyph may run */

/* An offset of n bytes (an INDEX's, 1 to 4). */
static uint32_t offset_n(const struct ttf *f, uint32_t at, uint32_t n)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++)
        v = v << 8 | u8(f, at + i);
    return v;
}

/* An INDEX at `at`: its item i's place and length (false outside it or
 * the table), and where the INDEX ends. */
static bool index_item(const struct ttf *f, uint32_t at, uint32_t i, uint32_t *item, uint32_t *len)
{
    uint32_t count = u16(f, at), size = u8(f, at + 2);
    if (i >= count || size < 1 || size > 4)
        return false;
    uint32_t offsets = at + 3, data = offsets + (count + 1) * size - 1;
    uint32_t a = offset_n(f, offsets + i * size, size), b = offset_n(f, offsets + (i + 1) * size, size);
    if (a < 1 || b < a || data > f->cff_end || a > f->cff_end - data || b > f->cff_end - data)
        return false;
    *item = data + a;
    *len = b - a;
    return true;
}

static uint32_t index_end(const struct ttf *f, uint32_t at)
{
    uint32_t count = u16(f, at), size = u8(f, at + 2);
    if (count == 0)
        return at + 2;
    if (size < 1 || size > 4)
        return f->cff_end;
    uint32_t offsets = at + 3, data = offsets + (count + 1) * size - 1;
    return data + offset_n(f, offsets + count * size, size);
}

/* A DICT's operands before operator op (12 x is 1200 + x): up to n of
 * them into v; how many, or -1 when it has no such operator. */
static int dict_find(const struct ttf *f, uint32_t at, uint32_t len, int op, double *v, int n)
{
    double stack[CFF_STACK];
    int sp = 0;
    uint32_t p = at, end = at + len;
    while (p < end) {
        uint32_t b = u8(f, p++);
        if (b <= 21) {                                  /* an operator */
            int o = (int)b;
            if (b == 12)
                o = 1200 + (int)u8(f, p++);
            if (o == op) {
                for (int i = 0; i < sp && i < n; i++)
                    v[i] = stack[i];
                return sp;
            }
            sp = 0;
            continue;
        }
        double x;
        if (b >= 32 && b <= 246)
            x = (double)b - 139;
        else if (b >= 247 && b <= 250)
            x = (double)((b - 247) * 256 + u8(f, p++) + 108);
        else if (b >= 251 && b <= 254)
            x = -(double)((b - 251) * 256 + u8(f, p++) + 108);
        else if (b == 28) {
            x = s16(f, p);
            p += 2;
        } else if (b == 29) {
            x = (int32_t)u32(f, p);
            p += 4;
        } else if (b == 30) {                           /* a real: its digits by nibbles */
            char text[64];
            int n_ = 0;
            for (bool done = false; !done && p < end;) {
                uint32_t byte = u8(f, p++);
                for (int half = 0; half < 2 && !done; half++) {
                    uint32_t nib = half ? byte & 15 : byte >> 4;
                    const char *put = nib <= 9 ? 0 : nib == 10 ? "." : nib == 11 ? "E" : nib == 12 ? "E-"
                                    : nib == 14 ? "-" : "";
                    if (nib == 15) {
                        done = true;
                        break;
                    }
                    char digit[2] = { (char)('0' + nib), 0 };
                    for (const char *c = put ? put : digit; *c && n_ < (int)sizeof(text) - 1; c++)
                        text[n_++] = *c;
                }
            }
            text[n_] = 0;
            x = strtod(text, 0);
        } else
            return -1;                                  /* (reserved bytes: a broken DICT) */
        if (sp < CFF_STACK)
            stack[sp++] = x;
    }
    return -1;
}

/* A Private DICT (size, offset from the CFF table's start): its local
 * subroutines' INDEX, or 0. */
static uint32_t private_subrs(const struct ttf *f, double size, double offset)
{
    if (!(size >= 0 && offset >= 0) || offset > f->cff_end - f->cff_at || size > f->cff_end - f->cff_at - offset)
        return 0;
    uint32_t at = f->cff_at + (uint32_t)offset;
    double v[1];
    if (dict_find(f, at, (uint32_t)size, 19, v, 1) != 1 || !(v[0] > 0) || v[0] > f->cff_end - at)
        return 0;
    return at + (uint32_t)v[0];
}

static bool cff_open(struct ttf *f, uint32_t at, uint32_t len)
{
    f->cff = true;
    f->cff_at = at;
    f->cff_end = at + len;
    if (u8(f, at) != 1)                                 /* (major version 1) */
        return false;
    uint32_t names = at + u8(f, at + 2), tops = index_end(f, names);
    uint32_t strings = index_end(f, tops);
    f->gsubrs = index_end(f, strings);
    uint32_t top, top_len;
    if (f->gsubrs > f->cff_end || !index_item(f, tops, 0, &top, &top_len))
        return false;
    double v[6];
    if (dict_find(f, top, top_len, 1206, v, 1) >= 1 && v[0] != 2)
        return false;                                   /* (CharstringType 2 only) */
    if (dict_find(f, top, top_len, 17, v, 1) != 1 || !(v[0] > 0) || v[0] >= len)
        return false;
    f->charstrings = at + (uint32_t)v[0];
    f->cff_xscale = f->cff_yscale = 1;
    if (dict_find(f, top, top_len, 1207, v, 6) == 6) { /* FontMatrix: units in ems */
        f->cff_xscale = v[0] * f->upem;
        f->cff_yscale = v[3] * f->upem;
    }
    if (dict_find(f, top, top_len, 1230, v, 3) >= 0) {  /* ROS: CID-keyed */
        double fa, fs;
        if (dict_find(f, top, top_len, 1236, &fa, 1) != 1 || dict_find(f, top, top_len, 1237, &fs, 1) != 1 ||
            !(fa > 0 && fa < len && fs > 0 && fs < len))
            return false;
        f->fdarray = at + (uint32_t)fa;
        f->fdselect = at + (uint32_t)fs;
    } else if (dict_find(f, top, top_len, 18, v, 2) == 2)
        f->subrs = private_subrs(f, v[0], v[1]);
    return u16(f, f->charstrings) >= (uint32_t)f->glyphs || u16(f, f->charstrings) > 0;
}

/* A CID-keyed font's local subroutines for a glyph (FDSelect format 0 or
 * 3, then its Font DICT's Private DICT). */
static uint32_t cid_subrs(const struct ttf *f, int glyph)
{
    uint32_t g = (uint32_t)glyph, fd = 0xFFFF, p = f->fdselect;
    if (u8(f, p) == 0)
        fd = u8(f, p + 1 + g);
    else if (u8(f, p) == 3) {
        uint32_t ranges = u16(f, p + 1);
        for (uint32_t i = 0; i < ranges; i++) {
            uint32_t r = p + 3 + 3 * i;
            if (g >= u16(f, r) && g < u16(f, r + 3)) {
                fd = u8(f, r + 2);
                break;
            }
        }
    }
    uint32_t dict, len;
    double v[2];
    if (fd == 0xFFFF || !index_item(f, f->fdarray, fd, &dict, &len) || dict_find(f, dict, len, 18, v, 2) != 2)
        return 0;
    return private_subrs(f, v[0], v[1]);
}

/* Type 2 charstrings (technical note 5177). */
struct charstring {
    const struct ttf *f;
    struct ttf_shape *s;
    double stack[CFF_STACK], store[32];
    int sp, stems, ops, contour_from;
    bool width_seen, open, ended;
    double x, y;
    uint32_t subrs;
};

static void cs_point(struct charstring *c, double x, double y, uint8_t kind)
{
    add_point(c->s, (float)(x * c->f->cff_xscale), (float)(y * c->f->cff_yscale), kind);
}

static void cs_close(struct charstring *c)
{
    if (c->open && c->s->n > c->contour_from)
        end_contour(c->s);
    c->open = false;
}

static void cs_move(struct charstring *c, double dx, double dy)
{
    cs_close(c);
    c->x += dx;
    c->y += dy;
    c->contour_from = c->s->n;
    cs_point(c, c->x, c->y, TTF_ON);
    c->open = true;
}

static void cs_line(struct charstring *c, double dx, double dy)
{
    c->x += dx;
    c->y += dy;
    cs_point(c, c->x, c->y, TTF_ON);
}

static void cs_curve(struct charstring *c, double dx1, double dy1, double dx2, double dy2, double dx3, double dy3)
{
    double x1 = c->x + dx1, y1 = c->y + dy1, x2 = x1 + dx2, y2 = y1 + dy2;
    c->x = x2 + dx3;
    c->y = y2 + dy3;
    cs_point(c, x1, y1, TTF_CUBIC);
    cs_point(c, x2, y2, TTF_CUBIC);
    cs_point(c, c->x, c->y, TTF_ON);
}

static uint32_t bias(const struct ttf *f, uint32_t index)
{
    uint32_t n = u16(f, index);
    return n < 1240 ? 107 : n < 33900 ? 1131 : 32768;
}

/* The width before a stack-clearing operator's arguments: the first one,
 * when there is one more than the operator takes (only the first such
 * operator of a glyph; the advance comes from hmtx). */
static int args_from(struct charstring *c, bool extra)
{
    int from = 0;
    if (!c->width_seen && extra)
        from = 1;
    c->width_seen = true;
    return from;
}

/* Run a charstring; false when it is broken. */
static bool run(struct charstring *c, uint32_t at, uint32_t len, int depth)
{
    uint32_t p = at, end = at + len;
    double *st = c->stack;
    while (p < end && !c->ended) {
        if (++c->ops > CFF_OPS || c->s->failed)
            return false;
        uint32_t b = u8(c->f, p++);
        if (b >= 32 || b == 28) {                       /* a number */
            double v;
            if (b == 28) {
                v = s16(c->f, p);
                p += 2;
            } else if (b <= 246)
                v = (double)b - 139;
            else if (b <= 250)
                v = (double)((b - 247) * 256 + u8(c->f, p++) + 108);
            else if (b <= 254)
                v = -(double)((b - 251) * 256 + u8(c->f, p++) + 108);
            else {
                v = (double)(int32_t)u32(c->f, p) / 65536;
                p += 4;
            }
            if (c->sp >= CFF_STACK)
                return false;
            st[c->sp++] = v;
            continue;
        }
        int n = c->sp, i = 0;
        switch (b) {
        case 1: case 3: case 18: case 23:               /* hstem, vstem, hstemhm, vstemhm */
            i = args_from(c, n % 2 == 1);
            c->stems += (n - i) / 2;
            c->sp = 0;
            break;
        case 19: case 20: {                             /* hintmask, cntrmask: the stems before as vstems */
            i = args_from(c, n % 2 == 1);
            c->stems += (n - i) / 2;
            p += (uint32_t)(c->stems + 7) / 8;
            c->sp = 0;
            break;
        }
        case 21:                                        /* rmoveto */
            i = args_from(c, n > 2);
            if (n - i < 2)
                return false;
            cs_move(c, st[i], st[i + 1]);
            c->sp = 0;
            break;
        case 22: case 4:                                /* hmoveto, vmoveto */
            i = args_from(c, n > 1);
            if (n - i < 1)
                return false;
            cs_move(c, b == 22 ? st[i] : 0, b == 22 ? 0 : st[i]);
            c->sp = 0;
            break;
        case 5:                                         /* rlineto */
            for (; i + 1 < n; i += 2)
                cs_line(c, st[i], st[i + 1]);
            c->sp = 0;
            break;
        case 6: case 7: {                               /* hlineto, vlineto: alternating */
            bool across = b == 6;
            for (; i < n; i++, across = !across)
                cs_line(c, across ? st[i] : 0, across ? 0 : st[i]);
            c->sp = 0;
            break;
        }
        case 8:                                         /* rrcurveto */
            for (; i + 5 < n; i += 6)
                cs_curve(c, st[i], st[i + 1], st[i + 2], st[i + 3], st[i + 4], st[i + 5]);
            c->sp = 0;
            break;
        case 24:                                        /* rcurveline */
            for (; i + 5 < n - 2; i += 6)
                cs_curve(c, st[i], st[i + 1], st[i + 2], st[i + 3], st[i + 4], st[i + 5]);
            if (i + 1 < n)
                cs_line(c, st[i], st[i + 1]);
            c->sp = 0;
            break;
        case 25:                                        /* rlinecurve */
            for (; i + 1 < n - 6; i += 2)
                cs_line(c, st[i], st[i + 1]);
            if (i + 5 < n)
                cs_curve(c, st[i], st[i + 1], st[i + 2], st[i + 3], st[i + 4], st[i + 5]);
            c->sp = 0;
            break;
        case 26: {                                      /* vvcurveto: [dx1] {dya dxb dyb dyc}+ */
            double dx1 = 0;
            if (n % 4 == 1)
                dx1 = st[i++];
            for (; i + 3 < n; i += 4, dx1 = 0)
                cs_curve(c, dx1, st[i], st[i + 1], st[i + 2], 0, st[i + 3]);
            c->sp = 0;
            break;
        }
        case 27: {                                      /* hhcurveto: [dy1] {dxa dxb dyb dxc}+ */
            double dy1 = 0;
            if (n % 4 == 1)
                dy1 = st[i++];
            for (; i + 3 < n; i += 4, dy1 = 0)
                cs_curve(c, st[i], dy1, st[i + 1], st[i + 2], st[i + 3], 0);
            c->sp = 0;
            break;
        }
        case 30: case 31: {                             /* vhcurveto, hvcurveto: alternating */
            bool across = b == 31;
            for (; i + 3 < n; i += 4, across = !across) {
                double last = n - i == 5 ? st[i + 4] : 0;           /* (the final curve's extra) */
                if (across)
                    cs_curve(c, st[i], 0, st[i + 1], st[i + 2], last, st[i + 3]);
                else
                    cs_curve(c, 0, st[i], st[i + 1], st[i + 2], st[i + 3], last);
            }
            c->sp = 0;
            break;
        }
        case 10: case 29: {                             /* callsubr, callgsubr */
            uint32_t index = b == 10 ? c->subrs : c->f->gsubrs, item, size;
            if (n < 1 || !index || depth >= CFF_DEEPEST)
                return false;
            double k = st[--c->sp] + bias(c->f, index);
            if (!(k >= 0) || k > 65535 || !index_item(c->f, index, (uint32_t)k, &item, &size) ||
                !run(c, item, size, depth + 1))
                return false;
            break;
        }
        case 11:                                        /* return */
            return true;
        case 14:                                        /* endchar */
            args_from(c, n == 1 || n == 5);
            cs_close(c);
            c->ended = true;
            return true;
        case 12: {                                      /* two bytes: flex and arithmetic */
            uint32_t e = u8(c->f, p++);
            double *a = st;
            switch (e) {
            case 35:                                    /* flex: two curves, then fd */
                if (n < 13)
                    return false;
                cs_curve(c, a[0], a[1], a[2], a[3], a[4], a[5]);
                cs_curve(c, a[6], a[7], a[8], a[9], a[10], a[11]);
                c->sp = 0;
                break;
            case 34:                                    /* hflex */
                if (n < 7)
                    return false;
                cs_curve(c, a[0], 0, a[1], a[2], a[3], 0);
                cs_curve(c, a[4], 0, a[5], -a[2], a[6], 0);
                c->sp = 0;
                break;
            case 36:                                    /* hflex1 */
                if (n < 9)
                    return false;
                cs_curve(c, a[0], a[1], a[2], a[3], a[4], 0);
                cs_curve(c, a[5], 0, a[6], a[7], a[8], -(a[1] + a[3] + a[7]));
                c->sp = 0;
                break;
            case 37: {                                  /* flex1 */
                if (n < 11)
                    return false;
                double dx = a[0] + a[2] + a[4] + a[6] + a[8], dy = a[1] + a[3] + a[5] + a[7] + a[9];
                cs_curve(c, a[0], a[1], a[2], a[3], a[4], a[5]);
                if (fabs(dx) > fabs(dy))
                    cs_curve(c, a[6], a[7], a[8], a[9], a[10], -dy);
                else
                    cs_curve(c, a[6], a[7], a[8], a[9], -dx, a[10]);
                c->sp = 0;
                break;
            }
            case 3: case 4: case 10: case 11: case 12: case 15: case 24:    /* and, or, add, sub, div, eq, mul */
                if (n < 2)
                    return false;
                {
                    double x = a[n - 2], y = a[n - 1], r = e == 3 ? (x != 0 && y != 0) : e == 4 ? (x != 0 || y != 0)
                             : e == 10 ? x + y : e == 11 ? x - y : e == 12 ? (y != 0 ? x / y : 0)
                             : e == 15 ? (x == y) : x * y;
                    a[n - 2] = r;
                    c->sp = n - 1;
                }
                break;
            case 5: case 9: case 14: case 26:           /* not, abs, neg, sqrt */
                if (n < 1)
                    return false;
                a[n - 1] = e == 5 ? (a[n - 1] == 0) : e == 9 ? fabs(a[n - 1]) : e == 14 ? -a[n - 1]
                         : sqrt(fabs(a[n - 1]));
                break;
            case 18:                                    /* drop */
                if (n < 1)
                    return false;
                c->sp--;
                break;
            case 27:                                    /* dup */
                if (n < 1 || n >= CFF_STACK)
                    return false;
                a[n] = a[n - 1];
                c->sp++;
                break;
            case 28:                                    /* exch */
                if (n < 2)
                    return false;
                {
                    double t = a[n - 1];
                    a[n - 1] = a[n - 2];
                    a[n - 2] = t;
                }
                break;
            case 29: {                                  /* index */
                if (n < 1)
                    return false;
                int k = (int)a[n - 1];
                k = k < 0 ? 0 : k;
                if (k > n - 2)
                    return false;
                a[n - 1] = a[n - 2 - k];
                break;
            }
            case 20: case 21: {                         /* put, get */
                int k = n >= 1 ? (int)a[n - 1] : -1;
                if (k < 0 || k >= 32 || (e == 20 && n < 2))
                    return false;
                if (e == 20) {
                    c->store[k] = a[n - 2];
                    c->sp = n - 2;
                } else
                    a[n - 1] = c->store[k];
                break;
            }
            case 22:                                    /* ifelse */
                if (n < 4)
                    return false;
                a[n - 4] = a[n - 2] <= a[n - 1] ? a[n - 4] : a[n - 3];
                c->sp = n - 3;
                break;
            default:                                    /* (random, roll, reserved) */
                return false;
            }
            break;
        }
        default:                                        /* (reserved operators) */
            return false;
        }
    }
    return true;
}

static bool cff_shape(const struct ttf *f, int glyph, struct ttf_shape *s)
{
    uint32_t at, len;
    if (glyph < 0 || glyph >= f->glyphs || !index_item(f, f->charstrings, (uint32_t)glyph, &at, &len))
        return false;
    struct charstring c = { .f = f, .s = s, .subrs = f->fdarray ? cid_subrs(f, glyph) : f->subrs,
                            .contour_from = s->n };
    bool ok = run(&c, at, len, 0);
    cs_close(&c);
    return ok;
}

/* --- (41c-3) glyphs as coverage ----------------------------------------- */

#define FLAT (1.0 / 1024)               /* a curve's pieces stray at most this (pixels) */
#define PIECES_MAX 512                  /* pieces of one curve */
#define MASK_MAX (4096 * 4096)          /* bytes of one glyph's mask */

struct pen {
    struct path *p;
    double x, y;                        /* where the outline is (pixels) */
};

static void pen_line(struct pen *q, double x, double y)
{
    add_edge(q->p, (float)q->x, (float)q->y, (float)x, (float)y);
    q->x = x;
    q->y = y;
}

/* Pieces for a curve whose second differences are dd pixels (a piece of
 * 1/n strays at most dd / (8 n^2) for a quadratic, 3 dd / (4 n^2) for a
 * cubic). */
static int pieces_for(double dd, double k)
{
    double n = ceil(sqrt(k * dd / FLAT));
    return n < 1 ? 1 : n > PIECES_MAX ? PIECES_MAX : (int)n;
}

static void pen_quad(struct pen *q, double cx, double cy, double x, double y)
{
    double x0 = q->x, y0 = q->y;
    int n = pieces_for(hypot(x0 - 2 * cx + x, y0 - 2 * cy + y), 1.0 / 8);
    for (int k = 1; k <= n; k++) {
        double t = (double)k / n, u = 1 - t;
        pen_line(q, u * u * x0 + 2 * u * t * cx + t * t * x, u * u * y0 + 2 * u * t * cy + t * t * y);
    }
}

static void pen_cubic(struct pen *q, double c1x, double c1y, double c2x, double c2y, double x, double y)
{
    double x0 = q->x, y0 = q->y;
    double dd = fmax(hypot(x0 - 2 * c1x + c2x, y0 - 2 * c1y + c2y), hypot(c1x - 2 * c2x + x, c1y - 2 * c2y + y));
    int n = pieces_for(dd, 3.0 / 4);
    for (int k = 1; k <= n; k++) {
        double t = (double)k / n, u = 1 - t;
        pen_line(q, u * u * u * x0 + 3 * u * u * t * c1x + 3 * u * t * t * c2x + t * t * t * x,
                 u * u * u * y0 + 3 * u * u * t * c1y + 3 * u * t * t * c2y + t * t * t * y);
    }
}

/* A contour's edges: from its first point on the curve (none: the midpoint
 * of its first two), the midpoint between two quadratic control points on
 * the curve, cubic control points in pairs, back to the start. */
static void contour_edges(struct pen *q, const struct ttf_point *p, int n, double scale, double dx)
{
    if (n < 2)
        return;
#define PX(pt) (dx + (double)(pt).x * scale)
#define PY(pt) (-(double)(pt).y * scale)
    int s = 0;
    while (s < n && p[s].kind != TTF_ON)
        s++;
    double sx, sy;
    int total = n;
    if (s < n)
        sx = PX(p[s]), sy = PY(p[s]);
    else {
        sx = (PX(p[0]) + PX(p[1])) / 2, sy = (PY(p[0]) + PY(p[1])) / 2;
        s = 0;
        total = n + 1;
    }
    q->x = sx;
    q->y = sy;
    double cx = 0, cy = 0, c1x = 0, c1y = 0;
    int pending = 0;
    for (int k = 1; k <= total; k++) {
        bool closing = k == total;
        struct ttf_point pt = p[(s + k) % n];
        double x = closing ? sx : PX(pt), y = closing ? sy : PY(pt);
        uint8_t kind = closing ? TTF_ON : pt.kind;
        if (kind == TTF_CUBIC) {
            if (pending == 0)
                c1x = x, c1y = y, pending = 2;
            else
                cx = x, cy = y, pending = 3;
            continue;
        }
        if (kind == TTF_OFF) {
            if (pending == 1)
                pen_quad(q, cx, cy, (cx + x) / 2, (cy + y) / 2);
            cx = x, cy = y, pending = 1;
            continue;
        }
        if (pending == 1)
            pen_quad(q, cx, cy, x, y);
        else if (pending == 3)
            pen_cubic(q, c1x, c1y, cx, cy, x, y);
        else
            pen_line(q, x, y);
        pending = 0;
    }
#undef PX
#undef PY
}

bool ttf_mask(const struct ttf *f, int glyph, float scale, float dx, struct ttf_mask *m)
{
    memset(m, 0, sizeof(*m));
    struct ttf_shape s = { 0 };
    if (!ttf_shape(f, glyph, &s)) {
        ttf_shape_free(&s);
        return false;
    }
    if (s.n == 0) {
        ttf_shape_free(&s);
        return true;                                    /* (no outline: a space) */
    }
    struct path p = { 0 };
    struct pen q = { &p, 0, 0 };
    for (int c = 0, from = 0; c < s.contours; c++) {
        contour_edges(&q, s.p + from, s.ends[c] - from + 1, scale, dx);
        from = s.ends[c] + 1;
    }
    ttf_shape_free(&s);
    if (p.failed || !p.n) {
        free(p.e);
        return !p.failed;
    }
    float minx = p.e[0].x0, maxx = minx, miny = p.e[0].y0, maxy = miny;
    for (int i = 0; i < p.n; i++) {
        minx = fminf(minx, fminf(p.e[i].x0, p.e[i].x1));
        maxx = fmaxf(maxx, fmaxf(p.e[i].x0, p.e[i].x1));
        miny = fminf(miny, fminf(p.e[i].y0, p.e[i].y1));
        maxy = fmaxf(maxy, fmaxf(p.e[i].y0, p.e[i].y1));
    }
    int x0 = ffloor(minx), y0 = ffloor(miny), w = fceil(maxx) - x0, h = fceil(maxy) - y0;
    if (w <= 0 || h <= 0 || (long)w * h > MASK_MAX || !(m->bytes = malloc((size_t)w * (size_t)h))) {
        free(p.e);
        return w > 0 && h > 0 ? false : true;
    }
    bool ok = raster_mask(&p, x0, y0, w, h, m->bytes);
    free(p.e);
    if (!ok) {
        free(m->bytes);
        m->bytes = 0;
        return false;
    }
    m->left = x0;
    m->top = y0;
    m->w = w;
    m->h = h;
    return true;
}

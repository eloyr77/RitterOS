/*
 * Layout: the styled tree into what is drawn. Blocks stack downwards, each
 * with its margins (a block's top margin and the one above it collapse into
 * the larger), border, padding and background; inline content (texts,
 * links, pictures, line breaks) flows into lines inside its block, broken
 * between words, aligned left, centred or right; white space collapses
 * except in <pre>. A list item gets its marker in the margin. A table row
 * puts its cells side by side in equal columns. Everything is items with
 * page coordinates; words in one line with the same look are one item.
 * Elements with an id (and <a name>) are anchors: where "#part" scrolls to.
 * Form controls are boxes on the line (27c), sized as browsers size them:
 * a text field by its size="" (20 characters), a textarea by cols and
 * rows, a select by its longest option, a button by its label; they are
 * drawn by the program from their state (form.c), in the system's font.
 *
 * Floats and positioning (27d). A float is laid out at its line's top
 * against the left or right edge, beside the floats already there (or
 * below them when it does not fit), as wide as its width or, without one,
 * its contents ("shrink to fit": laid out once in a scratch page to
 * measure); lines beside it are shorter (`edges`), a line too narrow for
 * its first word goes down past it, `clear` puts a block below it, and a
 * box that floats, is an inline-block, is positioned absolutely or clips
 * its overflow grows to hold its floats. An inline-block is one box on its
 * line, its items riding on it (their y follows the box's when the line is
 * aligned). position: relative moves an element's items when everything
 * is laid out; absolute and fixed ones are laid out last, in their
 * containing block (the nearest positioned ancestor's box, or the view;
 * fixed: the window, and they do not scroll). The items are painted in
 * CSS's order (`order`): negative z-index, in-flow backgrounds and
 * borders, floats, in-flow content, then positioned boxes by z-index.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "browser.h"
#include "image.h"
#include "wfont.h"

struct line_state {
    int first;                          /* the line's first item */
    int left, right;                    /* its edges: the block's, less the floats beside it */
    int x;                              /* where the next item goes (page x) */
    int height;
    bool space;                         /* a space before the next word */
    bool any;                           /* something on the line */
};

#define FLOATS 128
#define BAND_FLOAT 200000               /* (in-flow decorations 100000, content 300000) */
#define BAND_POSITIONED 400000

struct floated {
    int x, y, w, h;                     /* its margin box */
    bool right;
};

struct later {                          /* an absolute or fixed box, laid out at the end */
    struct node *e, *holder;            /* holder: the positioned ancestor, or 0 (the view) */
    int x, y;                           /* where it would have been ("static position") */
    int band;
};

struct shift {                          /* position: relative: items [from, to) move */
    int from, to, dx, dy;
};

struct ctx {
    struct page *page;
    const char *base;
    int left, right;                    /* the current block's content edges */
    int y;                              /* the top of the current line */
    enum align align;
    struct line_state line;
    int margin;                         /* a bottom margin not yet added (it collapses) */
    int link;                           /* the link being laid out, or -1 */
    int line_number;
    int depth;
    int marker;                         /* a list marker waiting for its line's place, or -1 */
    /* (27d) */
    struct floated floats[FLOATS];
    int nfloats, floats_from;           /* lines look at floats[floats_from .. nfloats) */
    int band;                           /* the layer of what is added now (0: in the flow) */
    bool contain;                       /* the next block() holds its floats */
    bool measuring;                     /* a scratch layout: positioned boxes are left out */
    int view_h;
    struct later *later;                /* (27m: as many as the page has; 64 dropped apple.com's) */
    int nlater, later_room;
    struct shift *shifts;
    int nshifts, shift_room;
    /* (27g) for the next block() only: a flex item's box is the room given
     * (its width, max-width and side margins are the container's to set),
     * at least min_h high (stretched) */
    bool exact;
    int min_h;
    int push;                           /* (27i) a table cell's contents this far down (vertical-align) */
    bool whole_words;                   /* (27i) measuring the narrowest: words are not broken */
    bool no_break;                      /* (27n) place() keeps this on its line (white-space: nowrap) */
    /* (27m) heights */
    int cb_h;                           /* the containing block's content height, or -1 (unknown) */
    bool has_given;                     /* for the next block(): its border box this high */
    int given_h;
    /* (27h) */
    uint32_t under;                     /* the colour beneath what is laid out now (alpha mixes) */
    int group, groups;                  /* the inline box being laid out (item.group), and a count */
    int raise;                          /* vertical-align: pixels up for what is laid out now */
    struct node *owner;                 /* (27q) the element being laid out (item.owner) */
};

/* (27k) The Adobe face for a style: Times, Helvetica or Courier at the
 * pixel size nearest its font size (11, 14, 17, 20, 25, 34; larger ones at
 * twice a smaller face), its bold and italic; 0 when the files are not
 * there (an older disk: the misc-fixed faces then). */
static const struct font *adobe_font(const struct style *s, int *scale)
{
    static const struct { int px; const char *pt; } SIZES[] = {
        { 11, "08" }, { 14, "10" }, { 17, "12" }, { 20, "14" }, { 25, "18" }, { 34, "24" }
    };
    int size = s ? s->font_size : 16;
    *scale = 1;
    if (size > 44)                              /* (beyond the largest: twice a smaller one) */
        *scale = 2, size /= 2;
    int best = 0;
    for (int i = 1; i < 6; i++)
        if (abs(SIZES[i].px - size) < abs(SIZES[best].px - size))
            best = i;
    enum font_family fam = s ? s->family : FAMILY_SERIF;
    if (fam == FAMILY_FIXED)
        return 0;                               /* (the misc-fixed faces, as before 27k) */
    const char *face = fam == FAMILY_SANS ? "helv" : fam == FAMILY_MONO ? "cour" : "tim";
    bool bold = s && s->bold, italic = s && s->italic;
    const char *slant = fam == FAMILY_SERIF ? "I" : "O";
    char name[16];
    snprintf(name, sizeof(name), "%s%s%s%s", face, bold ? "B" : italic ? "" : "R",
             italic ? slant : "", SIZES[best].pt);
    return wfont_find(name);
}

const struct font *style_font(const struct style *s, int *scale, bool *fake_bold)
{
    const struct font *adobe = adobe_font(s, scale);
    if (adobe) {
        *fake_bold = false;
        return adobe;
    }
    static const struct { int up_to; const char *name, *bold; } FACES[] = {
        { 11, "6x13", "6x13B" }, { 14, "7x14", "7x14B" }, { 17, 0, 0 }, { 21, "9x18", "9x18B" },
        { 27, "10x20", 0 }, { 1000, 0, 0 }
    };
    int size = s ? s->font_size : 16;
    bool bold = s && s->bold;
    *scale = 1;
    *fake_bold = false;
    int i = 0;
    while (size > FACES[i].up_to)
        i++;
    if (FACES[i].up_to == 1000)                 /* the largest: 9x15 at twice its size */
        *scale = 2;
    const struct font *f = 0;
    const char *name = bold ? FACES[i].bold : FACES[i].name;
    if (name)
        f = wfont_find(name);
    if (!f && bold && FACES[i].name && !FACES[i].bold) {
        f = wfont_find(FACES[i].name);
        *fake_bold = f != 0;
    }
    if (!f)
        f = bold ? wfont_bold() : wfont_regular();
    return f;
}

static void add_translate(struct ctx *c, const struct style *s, int from, int w, int h);

/* (27k) A space's width in a face (proportional faces: narrower than the cell). */
static int space_width(const struct font *f)
{
    return wfont_text_width(f, " ");
}

/* --- items --- */

static struct item *add_item(struct ctx *c, enum item_kind kind)
{
    struct page *p = c->page;
    if (p->count == p->room) {
        int room = p->room ? p->room * 2 : 256;
        struct item *items = realloc(p->items, sizeof(*items) * (size_t)room);
        if (!items)
            return 0;
        p->items = items, p->room = room;
    }
    struct item *it = &p->items[p->count++];
    memset(it, 0, sizeof(*it));
    it->kind = kind;
    it->link = -1;
    it->band = c->band;
    it->leader = -1;
    it->scale = 1;
    it->line = c->line_number;
    it->owner = c->owner;
    return it;
}

static int add_link(struct page *p, const char *href, const char *base)
{
    char absolute[URL_MAX];
    if (!url_resolve(base, href, absolute, sizeof(absolute)))
        return -1;
    if (p->link_count == p->link_room) {
        int room = p->link_room ? p->link_room * 2 : 32;
        struct link *l = realloc(p->links, sizeof(*l) * (size_t)room);
        if (!l)
            return -1;
        p->links = l, p->link_room = room;
    }
    p->links[p->link_count].href = copy_text(absolute, strlen(absolute));
    if (!p->links[p->link_count].href)
        return -1;
    return p->link_count++;
}

/* Note where e begins (y: a block's top, or its line's for inline ones).
 * Only <a> gives a name="" (the old form); an id counts on every element. */
static void add_anchor(struct ctx *c, const struct node *e, int y)
{
    struct page *p = c->page;
    const char *names[2] = { node_attr(e, "id"), !strcmp(e->tag, "a") ? node_attr(e, "name") : 0 };
    for (int i = 0; i < 2; i++) {
        if (!names[i] || !names[i][0])
            continue;
        if (p->anchor_count == p->anchor_room) {
            int room = p->anchor_room ? p->anchor_room * 2 : 32;
            struct anchor *a = realloc(p->anchors, sizeof(*a) * (size_t)room);
            if (!a)
                return;
            p->anchors = a, p->anchor_room = room;
        }
        p->anchors[p->anchor_count].name = copy_text(names[i], strlen(names[i]));
        if (!p->anchors[p->anchor_count].name)
            return;
        p->anchors[p->anchor_count++].y = y;
    }
}

/* --- lines --- */

/* The room a line at y (h high) has between the floats beside it. */
static void edges(struct ctx *c, int y, int h, int *l, int *r)
{
    *l = c->left, *r = c->right;
    for (int i = c->floats_from; i < c->nfloats; i++) {
        const struct floated *f = &c->floats[i];
        if (y + h <= f->y || y >= f->y + f->h || f->x + f->w <= c->left || f->x >= c->right)
            continue;
        if (!f->right && f->x + f->w > *l)
            *l = f->x + f->w;
        if (f->right && f->x < *r)
            *r = f->x;
    }
    if (*r < *l)
        *r = *l;
}

/* The lowest bottom of the floats beside y that is below it, or -1. */
static int next_float_bottom(struct ctx *c, int y, int sides)
{
    int best = -1;
    for (int i = c->floats_from; i < c->nfloats; i++) {
        const struct floated *f = &c->floats[i];
        int side = f->right ? CLEAR_RIGHT : CLEAR_LEFT;
        if (!(sides & side) || f->x + f->w <= c->left || f->x >= c->right || f->y + f->h <= y)
            continue;
        if (f->y <= y && (best < 0 || f->y + f->h < best))
            best = f->y + f->h;
    }
    return best;
}

static void start_line(struct ctx *c)
{
    c->line.first = c->page->count;
    edges(c, c->y + c->margin, 1, &c->line.left, &c->line.right);
    c->line.x = c->line.left;
    c->line.height = 0;
    c->line.space = false;
    c->line.any = false;
}

/* End the line: its items get their y (bottoms aligned) and the alignment. */
static void end_line(struct ctx *c, bool even_empty, int empty_height)
{
    if (!c->line.any && !even_empty) {
        start_line(c);
        return;
    }
    if (c->margin) {                            /* (the margin above comes first) */
        c->y += c->margin;
        c->margin = 0;
    }
    int height = c->line.any ? c->line.height : empty_height;
    int shift = 0;
    int used = c->line.x - c->line.left, room = c->line.right - c->line.left;
    if (c->measuring)
        ;                                       /* (27i: a measure is of what it needs, unaligned) */
    else if (c->align == ALIGN_CENTER)
        shift = (room - used) / 2;
    else if (c->align == ALIGN_RIGHT)
        shift = room - used;
    if (shift < 0)
        shift = 0;
    for (int i = c->line.first; i < c->page->count; i++) {
        if (i == c->marker)
            continue;
        struct item *it = &c->page->items[i];
        if (it->rider) {                        /* (inside an inline-block: with its box) */
            int dy = c->page->items[it->leader].y + it->ry - it->y;
            it->y += dy, it->tile_y += dy;
            if (it->clipped)
                it->clip[1] += dy, it->clip[3] += dy;
        } else if (it->valign == VALIGN_TOP) {
            it->y = c->y;
        } else if (it->valign == VALIGN_MIDDLE) {
            it->y = c->y + (height - it->h) / 2;
        } else {                                /* (on the line's bottom, less its leading) */
            it->y = c->y + height - it->h - it->lead - it->raise;
        }
        it->x += shift, it->tile_x += shift;
        if (it->clipped)
            it->clip[0] += shift, it->clip[2] += shift;
    }
    if (c->marker >= 0 && c->line.any) {        /* the list item's first line */
        struct item *m = &c->page->items[c->marker];
        m->y = c->y + height - m->h;
        m->line = c->line_number;
        c->marker = -1;
    }
    c->y += height;
    c->line_number++;
    start_line(c);
}

/* An empty line too narrow for w (floats beside it) goes down past them. */
static void make_room(struct ctx *c, int w)
{
    while (!c->line.any && w > c->line.right - c->line.left &&
           (c->line.left > c->left || c->line.right < c->right)) {
        int below = next_float_bottom(c, c->y + c->margin, CLEAR_LEFT | CLEAR_RIGHT);
        if (below < 0)
            break;
        c->y += c->margin;
        c->margin = 0;
        c->y = below > c->y ? below : c->y + 1;
        start_line(c);
    }
}

/* Place a box of w x h on the line (wrapping first if it does not fit). */
static int place(struct ctx *c, int w, int h)
{
    if (c->line.any && c->line.x + w > c->line.right && !c->no_break)
        end_line(c, false, 0);
    make_room(c, w);
    int x = c->line.x;
    c->line.x += w;
    if (h > c->line.height)
        c->line.height = h;
    c->line.any = true;
    return x;
}

/* rgb at alpha (0-255) over under. */
static uint32_t mix(uint32_t rgb, int alpha, uint32_t under)
{
    if (alpha >= 255 || under & 0xFF000000u)
        return rgb;
    if (alpha <= 0)
        return under;
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        int a = (int)(rgb >> shift & 255), b = (int)(under >> shift & 255);
        out |= (uint32_t)((a * alpha + b * (255 - alpha)) / 255) << shift;
    }
    return out;
}

static uint32_t text_color(struct ctx *c, const struct style *s)
{
    return s ? mix(s->color, s->color_alpha, c->under) : 0;
}

/* (27h) A line's height for text in style s at font height ch: its
 * line-height (as a number, of its font size), or ch. */
static int line_height(const struct style *s, int ch)
{
    if (!s)
        return ch;
    if (s->line_factor > 0)
        return (int)(s->line_factor * (float)s->font_size + 0.5f);
    return s->line_px > 0 ? s->line_px : ch;
}

/* One word (or a run of pre text) in style s. */
static void word(struct ctx *c, const struct style *s, const char *text, size_t n)
{
    int scale;
    bool fake;
    const struct font *f = style_font(s, &scale, &fake);
    int cw = space_width(f) * scale, ch = wfont_height(f) * scale + 2;
    int lh = line_height(s, ch), lead = (lh - ch) / 2;     /* (27h: half the leading below) */
    uint32_t ink = text_color(c, s);
    char buf[512];
    while (n > 0) {
        size_t take = n < sizeof(buf) - 1 ? n : sizeof(buf) - 1;
        while (take < n && take > 0 && ((unsigned char)text[take] & 0xC0) == 0x80)
            take--;                             /* (whole UTF-8 characters) */
        memcpy(buf, text, take);
        buf[take] = 0;
        int w = wfont_text_width(f, buf) * scale;
        int space = c->line.space && c->line.any ? cw : 0;
        if (c->line.any && c->line.x + space + w > c->line.right && !(s && s->nowrap)) {
            end_line(c, false, 0);
            space = 0;
        }
        make_room(c, w);
        /* A word wider than the whole line: as much as fits, the rest after
         * (not when measuring the narrowest a box can be: a word is whole). */
        if (!c->line.any && w > c->line.right - c->line.left && take > 1 && !c->whole_words &&
            !(s && s->nowrap)) {
            size_t fit = 0;
            const char *p = buf;
            int width = 0;
            while (*p) {
                const char *q = p;
                utf8_next(&q);
                char one[8];                    /* (27k: each character its own width) */
                size_t k = (size_t)(q - p) < sizeof(one) ? (size_t)(q - p) : sizeof(one) - 1;
                memcpy(one, p, k);
                one[k] = 0;
                int each = wfont_text_width(f, one) * scale;
                if (width + each > c->line.right - c->line.left && fit > 0)
                    break;
                width += each;
                fit = (size_t)(q - buf);
                p = q;
            }
            take = fit;
            buf[take] = 0;
            w = wfont_text_width(f, buf) * scale;
        }
        struct item *prev = c->line.any && c->page->count > c->line.first
                          ? &c->page->items[c->page->count - 1] : 0;
        if (prev && prev->kind == ITEM_TEXT && prev->font == f && prev->scale == scale &&
            prev->color == ink && prev->link == c->link && prev->group == c->group &&
            prev->lead == lead && prev->raise == c->raise &&
            prev->underline == (s && s->underline) && prev->strike == (s && s->strike) &&
            prev->x + prev->w == c->line.x && strlen(prev->text) + take + 2 < 4096) {
            char *joined = malloc(strlen(prev->text) + take + 2);
            if (joined) {                       /* the same look: one item */
                strcpy(joined, prev->text);
                if (space)
                    strcpy(joined + strlen(joined), " ");
                strcpy(joined + strlen(joined), buf);
                free(prev->text);
                prev->text = joined;
                prev->w += space + w;
                c->line.x += space + w;
                c->line.space = false;
                text += take, n -= take;
                continue;
            }
        }
        c->line.x += space;
        c->no_break = s && s->nowrap;
        int x = place(c, w, lh);
        c->no_break = false;
        struct item *it = add_item(c, ITEM_TEXT);
        if (!it)
            return;
        it->x = x, it->w = w, it->h = ch;
        it->lead = lead, it->raise = c->raise, it->group = c->group;
        it->hidden = s && s->hidden;
        it->font = f, it->scale = scale, it->fake_bold = fake;
        it->color = ink;
        it->text = copy_text(buf, take);
        it->underline = s && s->underline;
        it->strike = s && s->strike;
        it->link = c->link;
        c->line.space = false;
        text += take, n -= take;
    }
}

/* (27h) text-transform: upper or lower case (ASCII and Latin-1), or
 * capitals at the words' starts. malloc'ed. */
static char *transformed(const char *text, enum text_transform t)
{
    char *out = copy_text(text, strlen(text));
    if (!out)
        return 0;
    bool start = true;
    for (unsigned char *p = (unsigned char *)out; *p; p++) {
        bool upper = t == TRANSFORM_UPPER || (t == TRANSFORM_CAPITALIZE && start);
        bool lower_it = t == TRANSFORM_LOWER;
        if (*p == 0xC3 && p[1]) {               /* (Latin-1's letters: C3 80-9E and A0-BE) */
            if (upper && p[1] >= 0xA0 && p[1] <= 0xBE && p[1] != 0xB7)
                p[1] -= 0x20;
            else if (lower_it && p[1] >= 0x80 && p[1] <= 0x9E && p[1] != 0x97)
                p[1] += 0x20;
            p++;
            start = false;
            continue;
        }
        if (upper && *p >= 'a' && *p <= 'z')
            *p = (unsigned char)(*p - 32);
        else if (lower_it && *p >= 'A' && *p <= 'Z')
            *p = (unsigned char)(*p + 32);
        start = is_space(*p) || *p == '-' || *p == '(' || *p == '"';
    }
    return out;
}

/* A text node's words, white space collapsed (or kept in pre). */
static void text_run(struct ctx *c, const struct style *s, const char *text)
{
    if (s && s->transform != TRANSFORM_NONE && text) {
        char *t = transformed(text, s->transform);
        if (t) {
            struct style plain = *s;            /* (the same style but done) */
            plain.transform = TRANSFORM_NONE;
            text_run(c, &plain, t);
            free(t);
            return;
        }
    }
    if (s && s->pre) {
        const char *p = text;
        while (*p) {
            size_t n = strcspn(p, "\n\t");
            if (n)
                word(c, s, p, n);
            p += n;
            if (*p == '\t') {                   /* to the next multiple of 8 characters */
                int scale;
                bool fake;
                const struct font *f = style_font(s, &scale, &fake);
                int cw = space_width(f) * scale;
                int col = (c->line.x - c->line.left) / cw;
                word(c, s, "        ", (size_t)(8 - col % 8));
                p++;
            } else if (*p == '\n') {
                int scale;
                bool fake;
                const struct font *f = style_font(s, &scale, &fake);
                end_line(c, true, wfont_height(f) * scale + 2);
                p++;
            }
        }
        return;
    }
    const char *p = text;
    while (*p) {
        if (is_space(*p)) {
            c->line.space = true;
            p++;
            continue;
        }
        size_t n = 0;
        while (p[n] && !is_space(p[n]))
            n++;
        word(c, s, p, n);
        p += n;
    }
}

/* --- pictures --- */

static int attr_px(const struct node *e, const char *name)
{
    const char *v = node_attr(e, name);
    if (!v || !(v[0] >= '0' && v[0] <= '9'))
        return -1;
    int n = atoi(v);
    return n > 0 && n < 20000 ? n : -1;
}

static void picture(struct ctx *c, struct node *e)
{
    const struct style *s = e->style;
    int w = s && s->width > 0 ? s->width : attr_px(e, "width");
    int h = s && s->height > 0 ? s->height : attr_px(e, "height");
    if (s && s->height_pct > 0 && c->cb_h >= 0)        /* (27m) of the box it is in */
        h = c->cb_h * s->height_pct / 100 + s->height_add;
    if (s && s->width < -1)
        w = (c->right - c->left) * (-s->width - 1) / 100 + s->width_add;
    const struct image *img = e->picture;
    if (img) {
        if (w < 0 && h < 0)
            w = img->w, h = img->h;
        else if (w < 0)
            w = (int)((long)img->w * h / (img->h ? img->h : 1));
        else if (h < 0)
            h = (int)((long)img->h * w / (img->w ? img->w : 1));
    } else {
        const char *alt = node_attr(e, "alt");
        if (w < 0 || h < 0) {                   /* no size known: the alternative text */
            if (alt && *alt) {
                struct style dim = s ? *s : (struct style){ .font_size = 16 };
                dim.color = 0x606060;
                c->line.space = c->line.space;
                char shown[300];
                snprintf(shown, sizeof(shown), "[%s]", alt);
                word(c, &dim, shown, strlen(shown));
                return;
            }
            w = w < 0 ? 16 : w, h = h < 0 ? 16 : h;
        }
    }
    int room = c->right - c->left;
    if (w > room && room > 0) {                 /* wider than the column: scaled down */
        h = (int)((long)h * room / w);
        w = room;
    }
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    if (c->line.space && c->line.any) {
        int scale;
        bool fake;
        c->line.x += space_width(style_font(s, &scale, &fake)) * scale;
    }
    int x = place(c, w, h);
    c->line.space = false;
    struct item *it = add_item(c, img ? ITEM_PICTURE : ITEM_FILL);
    if (!it)
        return;
    it->x = x, it->w = w, it->h = h;
    it->picture = img;
    it->color = 0xC8C8C8;                       /* (not loaded: a gray box) */
    it->link = c->link;
    if (s) {                                    /* (27h) */
        it->valign = s->valign, it->hidden = s->hidden, it->raise = c->raise;
        for (int k = 0; k < 4; k++)
            it->radius[k] = s->radius[k] < 0 ? (w < h ? w : h) * -s->radius[k] / 100 : s->radius[k];
        add_translate(c, s, c->page->count - 1, w, h);
    }
}

/* --- form controls (27c) --- */

/* The size of a control's box: the text inside in the regular font. */
void control_box(struct node *e, int *w, int *h)
{
    const struct font *f = wfont_regular();
    int cw = wfont_width(f), ch = wfont_height(f);
    struct control *k = control_of(e);
    *w = 20 * cw + 8, *h = ch + 6;              /* (as window_field_height: the editor fits) */
    if (!k)
        return;
    int n;
    switch (k->kind) {
    case CONTROL_TEXT:
    case CONTROL_PASSWORD:
        n = attr_px(e, "size");
        *w = (n > 0 && n < 200 ? n : 20) * cw + 8;
        break;
    case CONTROL_CHECKBOX:
    case CONTROL_RADIO:
        *w = *h = 13;
        break;
    case CONTROL_SUBMIT:
    case CONTROL_RESET:
    case CONTROL_BUTTON: {
        char *label = control_label(e);
        *w = (label ? wfont_text_width(f, label) : 0) + 16;
        free(label);
        break;
    }
    case CONTROL_SELECT: {
        struct node *options[256];
        int count = select_options(e, options, 256), widest = 4 * cw;
        for (int i = 0; i < count && i < 256; i++) {
            char *t = node_text(options[i]);
            int tw = t ? wfont_text_width(f, t) : 0;
            widest = tw > widest ? tw : widest;
            free(t);
        }
        *w = widest + 8 + 16;                   /* (and the arrow's room) */
        break;
    }
    case CONTROL_TEXTAREA: {
        int cols = attr_px(e, "cols"), rows = attr_px(e, "rows");
        *w = (cols > 0 && cols < 200 ? cols : 20) * cw + 8;
        *h = (rows > 0 && rows < 100 ? rows : 2) * ch + 8;
        break;
    }
    case CONTROL_HIDDEN:
        *w = *h = 0;
        break;
    }
    const struct style *s = e->style;
    if (s && s->width > 0 && k->kind != CONTROL_CHECKBOX && k->kind != CONTROL_RADIO)
        *w = s->width;
}

static int resolved_width(int value, int add, int room);

/* (27j) An inline <svg>: a picture of its drawing, as large as its width
 * and height say (CSS, its attributes, its viewBox), drawn again when that
 * changes (not while only measuring: its box is enough then). */
static void svg_inline(struct ctx *c, struct node *e)
{
    const struct style *s = e->style;
    int iw, ih, room = c->right - c->left;
    svg_size(e, &iw, &ih);
    int w = s->width >= 0 || s->width < -1 ? resolved_width(s->width, s->width_add, room) : -1;
    if (w >= 0 && s->border_box)
        w -= s->border[1] + s->border[3] + (s->padding[1] > 0 ? s->padding[1] : 0) +
             (s->padding[3] > 0 ? s->padding[3] : 0);
    int h = s->height > 0 ? s->height : -1;
    const char *wa = node_attr(e, "width");
    if (w < 0 && wa && strchr(wa, '%'))         /* (width="100%": of the room) */
        w = room * atoi(wa) / 100;
    if (w < 0 && h < 0)
        w = iw, h = ih;
    else if (w < 0)
        w = (int)((long)iw * h / ih);
    else if (h < 0)
        h = (int)((long)ih * w / iw);
    if (w > room && room > 0)
        h = (int)((long)h * room / w), w = room;
    w = w < 1 ? 1 : w > 4096 ? 4096 : w;
    h = h < 1 ? 1 : h > 4096 ? 4096 : h;
    if (!c->measuring && (!e->svg_drawn || !e->picture || e->picture->w != w || e->picture->h != h)) {
        if (e->picture) {
            image_free(e->picture);
            free(e->picture);
        }
        e->picture = svg_draw(e, w, h);
        e->svg_drawn = true;
    }
    if (c->line.space && c->line.any) {
        int scale;
        bool fake;
        c->line.x += space_width(style_font(s, &scale, &fake)) * scale;
    }
    int x = place(c, w, h);
    c->line.space = false;
    bool drawn = !c->measuring && e->picture;
    struct item *it = add_item(c, drawn ? ITEM_PICTURE : ITEM_FILL);
    if (!it)
        return;
    it->x = x, it->w = w, it->h = h;
    it->picture = drawn ? e->picture : 0;
    it->color = c->under, it->decoration = !drawn;
    it->link = c->link;
    it->valign = s->valign, it->hidden = s->hidden, it->raise = c->raise;
}

static void control(struct ctx *c, struct node *e)
{
    struct control *k = control_of(e);
    if (!k || k->kind == CONTROL_HIDDEN)
        return;
    int w, h;
    control_box(e, &w, &h);
    if (w > c->right - c->left && c->right - c->left > 20)
        w = c->right - c->left;
    if (c->line.space && c->line.any) {
        int scale;
        bool fake;
        c->line.x += space_width(style_font(e->style, &scale, &fake)) * scale;
    }
    int x = place(c, w + 2, h + 2);             /* (a pixel around it on every side) */
    c->line.space = false;
    struct item *it = add_item(c, ITEM_CONTROL);
    if (!it)
        return;
    it->x = x + 1, it->w = w, it->h = h;
    it->node = e;
    it->font = wfont_regular();
    if (e->style)
        it->valign = e->style->valign, it->hidden = e->style->hidden;
}

/* --- blocks --- */

static bool is_block(const struct node *n)
{
    return n->kind == NODE_ELEMENT && n->style &&
           (n->style->display == DISPLAY_BLOCK || n->style->display == DISPLAY_LIST_ITEM ||
            n->style->display == DISPLAY_TABLE_ROW || n->style->display == DISPLAY_TABLE_CELL);
}

static void block(struct ctx *c, struct node *e);
static void flex_contents(struct ctx *c, struct node *e, int definite);
static void grid_contents(struct ctx *c, struct node *e, int definite);
struct table;
static struct table *table_build(struct ctx *c, struct node *e, int avail, int given);
static void table_lay(struct ctx *c, struct node *e, struct table *t);
static void table_free(struct table *t);
static void defer(struct ctx *c, struct node *e);
static void float_box(struct ctx *c, struct node *e);
static void inline_block(struct ctx *c, struct node *e);
static int positioned_band(const struct style *s);
static void add_shift(struct ctx *c, const struct style *s, int from, int room);

static void list_marker(struct ctx *c, struct node *e)
{
    const struct style *s = e->style;
    if (!s || s->list == LIST_NONE)
        return;
    char marker[16];
    if (s->list == LIST_DECIMAL) {
        int number = 1;
        const struct node *list = e->parent;
        const char *start = list ? node_attr(list, "start") : 0;
        if (start)
            number = atoi(start);
        for (const struct node *n = e->prev; n; n = n->prev)
            if (n->kind == NODE_ELEMENT && n->style && n->style->display == DISPLAY_LIST_ITEM)
                number++;
        const char *value = node_attr(e, "value");
        if (value)
            number = atoi(value);
        snprintf(marker, sizeof(marker), "%d.", number);
    } else {
        snprintf(marker, sizeof(marker), "%s",
                 s->list == LIST_CIRCLE ? "\xE2\x97\xA6" : s->list == LIST_SQUARE ? "\xE2\x96\xAA"
                                                                                  : "\xE2\x80\xA2");
    }
    if (c->marker >= 0) {                       /* (an outer item's, still waiting: here) */
        c->page->items[c->marker].y = c->y;
        c->marker = -1;
    }
    int scale;
    bool fake;
    const struct font *f = style_font(s, &scale, &fake);
    int w = wfont_text_width(f, marker) * scale;
    struct item *it = add_item(c, ITEM_TEXT);
    if (!it)
        return;
    it->x = c->left - w - space_width(f) * scale;
    it->w = w, it->h = wfont_height(f) * scale + 2;
    it->font = f, it->scale = scale, it->fake_bold = fake;
    it->color = s->color;
    it->text = copy_text(marker, strlen(marker));
    it->line = c->line_number;
    c->marker = c->page->count - 1;             /* (its y: the first line's, when it ends) */
}

/* (27h) An inline element's background and borders: a box behind each of
 * its words' items (from `first`; one per line, as the words are merged
 * per line), grown by its padding, that rides on the item when the item's
 * line is not laid yet. */
static void inline_decorations(struct ctx *c, struct node *n, int first)
{
    const struct style *s = n->style;
    bool bg = s->background != COLOR_NONE;
    bool border = s->border[0] || s->border[1] || s->border[2] || s->border[3];
    if ((!bg && !border) || s->hidden)
        return;
    int end = c->page->count;
    int pad[4];
    for (int k = 0; k < 4; k++)
        pad[k] = s->padding[k] > 0 ? s->padding[k] : 0;
    int lo = first, hi = end;                   /* (its first and last word: the side padding) */
    for (int i = first; i < end; i++) {
        struct item *it = &c->page->items[i];
        if (it->kind != ITEM_TEXT && it->kind != ITEM_PICTURE)
            continue;
        if (it->decoration || it->rider)
            continue;
        bool at_start = i == lo, at_end = true;
        for (int k = i + 1; k < hi; k++)
            if ((c->page->items[k].kind == ITEM_TEXT || c->page->items[k].kind == ITEM_PICTURE) &&
                !c->page->items[k].decoration && !c->page->items[k].rider)
                at_end = false;
        int x0 = it->x - (at_start ? pad[3] + s->border[3] : 0);
        int x1 = it->x + it->w + (at_end ? pad[1] + s->border[1] : 0);
        int top = -pad[0] - s->border[0], bottom = it->h + pad[2] + s->border[2];
        bool placed = i < c->line.first;        /* (its line is laid: y is final) */
        int boxes[5][4], nb = 0;                /* x, dy, w, h (dy from the item's top) */
        uint32_t colors[5];
        if (bg)
            colors[nb] = mix(s->background, s->background_alpha, c->under),
            boxes[nb][0] = x0, boxes[nb][1] = top, boxes[nb][2] = x1 - x0, boxes[nb][3] = bottom - top, nb++;
        for (int k = 0; k < 4; k++) {
            if (!s->border[k] || s->border_colors[k] == COLOR_CLEAR || ((k == 3 && !at_start) || (k == 1 && !at_end)))
                continue;
            uint32_t bc = s->border_colors[k] == COLOR_NONE ? s->color : s->border_colors[k];
            colors[nb] = bc;
            if (k == 0)
                boxes[nb][0] = x0, boxes[nb][1] = top, boxes[nb][2] = x1 - x0, boxes[nb][3] = s->border[0];
            else if (k == 2)
                boxes[nb][0] = x0, boxes[nb][1] = bottom - s->border[2], boxes[nb][2] = x1 - x0,
                boxes[nb][3] = s->border[2];
            else if (k == 3)
                boxes[nb][0] = x0, boxes[nb][1] = top, boxes[nb][2] = s->border[3], boxes[nb][3] = bottom - top;
            else
                boxes[nb][0] = x1 - s->border[1], boxes[nb][1] = top, boxes[nb][2] = s->border[1],
                boxes[nb][3] = bottom - top;
            nb++;
        }
        int leader = i;
        for (int b = 0; b < nb; b++) {
            struct item *d = add_item(c, ITEM_FILL);
            if (!d)
                return;
            it = &c->page->items[leader];       /* (add_item may have moved the items) */
            d->decoration = true;
            d->x = boxes[b][0], d->w = boxes[b][2], d->h = boxes[b][3], d->color = colors[b];
            d->line = it->line;
            for (int k = 0; k < 4; k++)
                d->radius[k] = b == 0 && bg && s->radius[k] > 0 ? s->radius[k] : 0;
            if (placed) {
                d->y = it->y + boxes[b][1];
            } else {
                d->rider = true, d->leader = leader, d->ry = boxes[b][1];
                d->y = boxes[b][1];
            }
        }
    }
}

/* The inline content of e (and its inline children), then its blocks. */
static void contents(struct ctx *c, struct node *e)
{
    struct node *outer = c->owner, *self = e->tag[0] == ':' ? outer : e;  /* (27q: ::before is its parent's) */
    for (struct node *n = e->first; n; n = n->next) {
        c->owner = n->kind == NODE_TEXT || n->tag[0] == ':' ? self : n;
        if (n->kind == NODE_TEXT) {
            text_run(c, e->style, n->text);
            continue;
        }
        const struct style *s = n->style;
        if (!s || s->display == DISPLAY_NONE)
            continue;
        if (!strcmp(n->tag, "br")) {
            int scale;
            bool fake;
            const struct font *f = style_font(e->style, &scale, &fake);
            end_line(c, true, wfont_height(f) * scale + 2);
            continue;
        }
        if (s->display == DISPLAY_CONTENTS) {   /* (no box: its children in its place) */
            if (c->depth < 200) {
                c->depth++;
                contents(c, n);
                c->depth--;
            }
            continue;
        }
        if (s->position == POSITION_ABSOLUTE || s->position == POSITION_FIXED) {
            defer(c, n);                        /* (27d) out of the flow: laid out last */
            continue;
        }
        if (s->floating != FLOAT_NONE) {
            float_box(c, n);
            continue;
        }
        if (s->display == DISPLAY_INLINE_BLOCK && strcmp(n->tag, "img") && !is_control(n)) {
            inline_block(c, n);
            continue;
        }
        if (!strcmp(n->tag, "img")) {
            picture(c, n);
            continue;
        }
        if (!strcmp(n->tag, "svg")) {           /* (27j) */
            svg_inline(c, n);
            continue;
        }
        if (is_control(n)) {
            control(c, n);
            continue;
        }
        if (is_block(n)) {
            block(c, n);
            continue;
        }
        add_anchor(c, n, c->y + c->margin);       /* (the line's top, once it is laid) */
        int saved = c->link, saved_band = c->band, first = c->page->count;
        int saved_group = c->group, saved_raise = c->raise;
        const char *href = !strcmp(n->tag, "a") ? node_attr(n, "href") : 0;
        if (href)
            c->link = add_link(c->page, href, c->base);
        if (s->position == POSITION_RELATIVE)
            c->band = positioned_band(s);
        /* (27h) an inline box: its own words, raised or lowered, with the
         * room of its side margins, borders and padding */
        c->group = ++c->groups;
        if (s->valign == VALIGN_SUPER)
            c->raise += s->font_size / 3;
        else if (s->valign == VALIGN_SUB)
            c->raise -= s->font_size / 5;
        else if (s->valign == VALIGN_LENGTH)
            c->raise += s->raise;
        int lead_in = (s->margin[3] == LENGTH_AUTO ? 0 : s->margin[3]) + s->border[3] +
                      (s->padding[3] > 0 ? s->padding[3] : 0);
        int lead_out = (s->margin[1] == LENGTH_AUTO ? 0 : s->margin[1]) + s->border[1] +
                       (s->padding[1] > 0 ? s->padding[1] : 0);
        if (lead_in) {
            if (c->line.space && c->line.any) {
                int scale;
                bool fake;
                c->line.x += space_width(style_font(e->style, &scale, &fake)) * scale;
                c->line.space = false;
            }
            c->line.x += lead_in;
        }
        if (c->depth < 200) {
            c->depth++;
            contents(c, n);
            c->depth--;
        }
        c->line.x += lead_out;
        inline_decorations(c, n, first);
        if (s->position == POSITION_RELATIVE)
            add_shift(c, s, first, c->right - c->left);
        c->link = saved;
        c->band = saved_band;
        c->group = saved_group, c->raise = saved_raise;
    }
    c->owner = outer;
}

static int resolved_width(int value, int add, int room)
{
    if (value == -1 || value == LENGTH_AUTO)
        return -1;
    if (value < -1) {
        int w = room * (-value - 1) / 100 + add;        /* (27f: calc(100% - 20px)) */
        return w < 0 ? 0 : w;
    }
    return value;
}

static void fill(struct ctx *c, int x, int y, int w, int h, uint32_t color)
{
    if (w <= 0 || h <= 0)
        return;
    struct item *it = add_item(c, ITEM_FILL);
    if (it)
        it->x = x, it->y = y, it->w = w, it->h = h, it->color = color, it->decoration = true;
}

/* --- a box's look (27h) --- */

static int corner(int r, int w, int h)
{
    int v = r < 0 ? (w < h ? w : h) * -r / 100 : r;
    int most = (w < h ? w : h) / 2;
    return v > most ? most : v;
}

/* The decorations of e's border box (x, y, w, h) into the items reserved
 * for them (shadow, background, picture: -1 if none), and its borders. */
static void decorate(struct ctx *c, struct node *e, int x, int y, int w, int h, int shadow, int background,
                     int picture_at)
{
    const struct style *s = e->style;
    const int *bd = s->border;
    int radius[4];
    bool round = false;
    for (int k = 0; k < 4; k++)
        radius[k] = corner(s->radius[k], w, h), round = round || radius[k] > 0;
    if (shadow >= 0) {
        struct item *it = &c->page->items[shadow];
        int sp = s->shadow_spread;
        it->x = x + s->shadow_x - sp, it->y = y + s->shadow_y - sp;
        it->w = w + 2 * sp, it->h = h + 2 * sp;
        if (it->w < 0)
            it->w = 0;
        if (it->h < 0)
            it->h = 0;
        it->color = mix(s->shadow_color, s->shadow_alpha, c->under);
        it->soft = s->shadow_blur;
        it->color2 = mix(s->shadow_color, s->shadow_alpha / 3, c->under);     /* (its blurred edge) */
        for (int k = 0; k < 4; k++)
            it->radius[k] = radius[k] ? radius[k] + sp : 0;
    }
    if (background >= 0) {
        struct item *bg = &c->page->items[background];
        bg->x = x, bg->y = y, bg->w = w, bg->h = h;
        bg->color = mix(s->background, s->background_alpha, c->under);
        bg->hidden = s->hidden;
        memcpy(bg->radius, radius, sizeof(radius));
    }
    if (picture_at >= 0) {
        struct item *it = &c->page->items[picture_at];
        it->x = x, it->y = y, it->w = w, it->h = h, it->hidden = s->hidden;
        memcpy(it->radius, radius, sizeof(radius));
        uint32_t beneath = background >= 0 ? c->page->items[background].color : c->under;
        if (it->kind == ITEM_FILL) {            /* a gradient */
            it->gradient = calloc(1, sizeof(*it->gradient));
            if (!it->gradient || !css_gradient(s->background_image, it->gradient, beneath)) {
                free(it->gradient);
                it->gradient = 0;
                it->w = it->h = 0;
            }
        } else {                                /* a picture, placed in the padding box */
            const struct image *img = e->background;
            int px = x + bd[3], py = y + bd[0], pw = w - bd[1] - bd[3], ph = h - bd[0] - bd[2];
            int tw = img->w, th = img->h;
            if (s->bg_size == BG_SIZE_COVER || s->bg_size == BG_SIZE_CONTAIN) {
                bool wide = (long)pw * img->h > (long)ph * img->w;     /* (the box wider than it) */
                bool by_width = s->bg_size == BG_SIZE_COVER ? wide : !wide;
                if (by_width)
                    tw = pw, th = (int)((long)img->h * pw / (img->w ? img->w : 1));
                else
                    th = ph, tw = (int)((long)img->w * ph / (img->h ? img->h : 1));
            } else if (s->bg_size == BG_SIZE_GIVEN) {
                int gw = s->bg_w == LENGTH_AUTO ? -1 : s->bg_w_pct ? pw * s->bg_w / 100 : s->bg_w;
                int gh = s->bg_h == LENGTH_AUTO ? -1 : s->bg_h_pct ? ph * s->bg_h / 100 : s->bg_h;
                if (gw >= 0 && gh >= 0)
                    tw = gw, th = gh;
                else if (gw >= 0)
                    tw = gw, th = (int)((long)img->h * gw / (img->w ? img->w : 1));
                else if (gh >= 0)
                    th = gh, tw = (int)((long)img->w * gh / (img->h ? img->h : 1));
            }
            if (tw > 2048)                      /* (a scaled tile: at most 16 MiB) */
                tw = 2048;
            if (th > 2048)
                th = 2048;
            it->picture = img, it->tiled = true;
            it->tile_w = tw < 1 ? 1 : tw, it->tile_h = th < 1 ? 1 : th;
            it->tile_x = px + (s->bg_x_pct ? (pw - it->tile_w) * s->bg_x / 100 : s->bg_x);
            it->tile_y = py + (s->bg_y_pct ? (ph - it->tile_h) * s->bg_y / 100 : s->bg_y);
            it->repeat = s->bg_repeat;
        }
    }
    if (s->hidden)
        return;
    /* The borders: one rounded ring when they are alike, else each side. */
    bool alike = true;
    for (int k = 1; k < 4; k++)
        alike = alike && bd[k] == bd[0] && s->border_colors[k] == s->border_colors[0];
    if (round && alike && bd[0]) {
        if (s->border_colors[0] == COLOR_CLEAR)
            return;
        fill(c, x, y, w, h, s->border_colors[0] == COLOR_NONE ? s->color : s->border_colors[0]);
        struct item *ring = &c->page->items[c->page->count - 1];
        if (ring->kind == ITEM_FILL && ring->x == x && ring->y == y) {
            memcpy(ring->radius, radius, sizeof(radius));
            ring->ring = bd[0];
        }
        return;
    }
    for (int k = 0; k < 4; k++) {
        if (!bd[k] || s->border_colors[k] == COLOR_CLEAR)
            continue;
        uint32_t bc = s->border_colors[k] == COLOR_NONE ? s->color : s->border_colors[k];
        if (k == 0)
            fill(c, x, y, w, bd[0], bc);
        else if (k == 2)
            fill(c, x, y + h - bd[2], w, bd[2], bc);
        else if (k == 3)
            fill(c, x, y, bd[3], h, bc);
        else
            fill(c, x + w - bd[1], y, bd[1], h, bc);
    }
}

/* Items [from, end) seen only inside x0..x1, y0..y1 (overflow: hidden). */
static void clip_items(struct ctx *c, int from, int x0, int y0, int x1, int y1)
{
    for (int i = from; i < c->page->count; i++) {
        struct item *it = &c->page->items[i];
        if (it->fixed)
            continue;
        if (!it->clipped) {
            it->clipped = true;
            it->clip[0] = x0, it->clip[1] = y0, it->clip[2] = x1, it->clip[3] = y1;
            continue;
        }
        if (it->clip[0] < x0) it->clip[0] = x0;
        if (it->clip[1] < y0) it->clip[1] = y0;
        if (it->clip[2] > x1) it->clip[2] = x1;
        if (it->clip[3] > y1) it->clip[3] = y1;
    }
}

/* opacity: the colours of items [from, end) mixed with what is beneath
 * (pictures stay as they are); 0 hides them. */
static void fade_items(struct ctx *c, int from, int opacity, uint32_t under)
{
    for (int i = from; i < c->page->count; i++) {
        struct item *it = &c->page->items[i];
        if (opacity == 0) {
            it->hidden = true;
            continue;
        }
        it->color = mix(it->color, opacity, under);
        it->color2 = mix(it->color2, opacity, under);
        if (it->gradient)
            for (int k = 0; k < it->gradient->count; k++)
                it->gradient->colors[k] = mix(it->gradient->colors[k], opacity, under);
    }
}

/* --- floats, inline-blocks, positioned boxes (27d) --- */

static int positioned_band(const struct style *s)
{
    if (!s->z_auto && s->z_index < 0)
        return s->z_index;                      /* (under the flow: -9999 .. -1) */
    return BAND_POSITIONED + (s->z_auto ? 0 : s->z_index);
}

static int offset_px(int v, int room)
{
    if (v <= OFFSET_PERCENT(0) + 100 && v >= OFFSET_PERCENT(100))
        return room * (OFFSET_PERCENT(0) - v) / 100;
    return v;
}

static void add_shift(struct ctx *c, const struct style *s, int from, int room)
{
    int dx = s->offset[3] != LENGTH_AUTO ? offset_px(s->offset[3], room)
           : s->offset[1] != LENGTH_AUTO ? -offset_px(s->offset[1], room) : 0;
    int dy = s->offset[0] != LENGTH_AUTO ? s->offset[0]
           : s->offset[2] != LENGTH_AUTO ? -s->offset[2] : 0;
    if ((!dx && !dy) || from >= c->page->count)
        return;
    if (c->nshifts == c->shift_room) {
        int room2 = c->shift_room ? 2 * c->shift_room : 16;
        struct shift *sh = realloc(c->shifts, sizeof(*sh) * (size_t)room2);
        if (!sh)
            return;
        c->shifts = sh, c->shift_room = room2;
    }
    c->shifts[c->nshifts++] = (struct shift){ from, c->page->count, dx, dy };
}

/* (27m) transform: translate(): items [from, end) moved by its pixels and
 * percentages of the element's own box (w x h), when everything is laid. */
static void add_translate(struct ctx *c, const struct style *s, int from, int w, int h)
{
    if (!s || !s->translated || from >= c->page->count)
        return;
    int dx = s->tx + s->tx_pct * w / 100, dy = s->ty + s->ty_pct * h / 100;
    if (!dx && !dy)
        return;
    if (c->nshifts == c->shift_room) {
        int room = c->shift_room ? 2 * c->shift_room : 16;
        struct shift *sh = realloc(c->shifts, sizeof(*sh) * (size_t)room);
        if (!sh)
            return;
        c->shifts = sh, c->shift_room = room;
    }
    c->shifts[c->nshifts++] = (struct shift){ from, c->page->count, dx, dy };
}

/* The space around a box's content: margins (auto: 0), borders, padding. */
static int sides_w(const struct style *s)
{
    int w = s->border[1] + s->border[3];
    for (int i = 1; i <= 3; i += 2)
        w += (s->margin[i] == LENGTH_AUTO ? 0 : s->margin[i]) + (s->padding[i] > 0 ? s->padding[i] : 0);
    return w;
}

/* The margin box of a box whose width is `width` (its content's, or with
 * box-sizing: border-box its border box's). */
static int outer_of(const struct style *s, int width)
{
    int w = width + sides_w(s);
    if (s->border_box)
        w -= s->border[1] + s->border[3] + (s->padding[1] > 0 ? s->padding[1] : 0) +
             (s->padding[3] > 0 ? s->padding[3] : 0);
    return w;
}

/* The inside of a box laid out by lay_box or measured: a picture or a
 * control on its own line, a text (27g: a flex container's text is an
 * anonymous item, in its parent's style), else the element as a block. */
static void lay_content(struct ctx *c, struct node *e)
{
    if (e->kind == NODE_TEXT) {
        text_run(c, e->parent ? e->parent->style : 0, e->text);
        end_line(c, false, 0);
    } else if (!strcmp(e->tag, "img")) {
        picture(c, e);
        end_line(c, false, 0);
    } else if (!strcmp(e->tag, "svg")) {
        svg_inline(c, e);
        end_line(c, false, 0);
    } else if (is_control(e)) {
        control(c, e);
        end_line(c, false, 0);
    } else {
        c->contain = true;
        block(c, e);
        end_line(c, false, 0);
    }
    c->exact = false, c->min_h = 0;
}

/* e laid out once in a scratch page `room` wide (exact: as a flex item of
 * that border-box width): its height (margins included), and in *widest
 * how far right its contents reach (margin box). Remembered for this
 * layout (memo slot: 0 the max-content width, 1 the min-content one, 2 the
 * height at a width), as flex boxes measure their items again and again. */
static int measure(struct ctx *c, struct node *e, int room, bool exact, int slot, int *widest)
{
    if (room < 1)
        room = 1;                               /* (a box squeezed below nothing: measured at 1) */
    if (room > 1 << 24)
        room = 1 << 24;
    int key = (room << 1 | exact) + 1;
    if (e->memo[slot][0] == key) {
        if (widest)
            *widest = e->memo[slot][1];
        return e->memo[slot][1];
    }
    struct page scratch;
    memset(&scratch, 0, sizeof(scratch));
    scratch.width = room;
    struct ctx *m = calloc(1, sizeof(*m));
    if (!m) {
        if (widest)
            *widest = room;
        return 0;
    }
    m->page = &scratch, m->base = c->base, m->left = 0, m->right = room;
    m->link = -1, m->marker = -1, m->view_h = c->view_h, m->measuring = true;
    m->depth = c->depth + 1;
    m->whole_words = slot == 1;
    m->cb_h = c->cb_h;
    start_line(m);
    m->exact = exact;
    lay_content(m, e);
    int h = m->y + m->margin, far = 0;
    for (int i = 0; i < scratch.count; i++)
        if (!scratch.items[i].decoration && scratch.items[i].x + scratch.items[i].w > far)
            far = scratch.items[i].x + scratch.items[i].w;
    for (int i = 0; i < m->nfloats; i++) {
        if (m->floats[i].x + m->floats[i].w > far)
            far = m->floats[i].x + m->floats[i].w;
        if (m->floats[i].y + m->floats[i].h > h)
            h = m->floats[i].y + m->floats[i].h;
    }
    if (e->kind == NODE_ELEMENT && e->style && strcmp(e->tag, "img") && !is_control(e)) {
        const struct style *s = e->style;
        int width = exact ? -1 : resolved_width(s->width, s->width_add, room);
        if (width >= 0)                         /* (a box of its own width) */
            far = (s->margin[3] == LENGTH_AUTO ? 0 : s->margin[3]) + width +
                  (s->border_box ? 0 : s->border[1] + s->border[3] + (s->padding[1] > 0 ? s->padding[1] : 0) +
                                       (s->padding[3] > 0 ? s->padding[3] : 0));
        far += (exact || s->margin[1] == LENGTH_AUTO ? 0 : s->margin[1]) +      /* (exact: no side margins) */
               (width >= 0 ? 0 : (s->padding[1] > 0 ? s->padding[1] : 0) + s->border[1]);
    }
    free(m->shifts);
    free(m);
    page_free(&scratch);
    int value = slot == 2 ? h : far;
    e->memo[slot][0] = key, e->memo[slot][1] = value;
    if (widest)
        *widest = far;
    return value;
}

/* The width e's box takes when nothing says how wide (CSS's "shrink to
 * fit"): what its contents need, at most `room`. */
static int shrink_to_fit(struct ctx *c, struct node *e, int room)
{
    int widest;
    measure(c, e, room, false, room == 1 ? 1 : 0, &widest);
    return widest > room ? room : widest < 1 ? 1 : widest;
}

/* The margin box's width e takes in `room`: its width (and the space
 * around), or what it needs. */
static int box_width(struct ctx *c, struct node *e, int room)
{
    const struct style *s = e->style;
    int width = resolved_width(s->width, s->width_add, room);
    if (width >= 0 && strcmp(e->tag, "img"))
        return outer_of(s, width);
    return shrink_to_fit(c, e, room);
}

/* Lay e out as a block in [x, x + w) from y (its own margins inside):
 * its bottom (margin included). `fresh`: floats outside do not touch its
 * lines (a positioned box or an inline-block: its own context). */
static int lay_box(struct ctx *c, struct node *e, int x, int w, int y, bool fresh)
{
    int left = c->left, right = c->right, cy = c->y, margin = c->margin;
    int from = c->floats_from, nfloats = c->nfloats, link = c->link, marker = c->marker;
    enum align align = c->align;
    struct line_state line = c->line;
    c->left = x, c->right = x + w, c->y = y, c->margin = 0;
    c->marker = -1;                             /* (a list item's marker waits for its own line) */
    if (fresh)
        c->floats_from = c->nfloats;
    start_line(c);
    lay_content(c, e);
    int bottom = c->y + c->margin;
    if (fresh)                                  /* (it holds its own floats) */
        for (int i = c->floats_from; i < c->nfloats; i++)
            if (c->floats[i].y + c->floats[i].h > bottom)
                bottom = c->floats[i].y + c->floats[i].h;
    if (fresh)
        c->floats_from = from, c->nfloats = nfloats;     /* (its floats were its own) */
    c->left = left, c->right = right, c->y = cy, c->margin = margin;
    c->align = align, c->line = line, c->link = link, c->marker = marker;
    return bottom;
}

static void float_box(struct ctx *c, struct node *e)
{
    const struct style *s = e->style;
    if (c->line.any)                            /* (here a float begins a line) */
        end_line(c, false, 0);
    int outer = box_width(c, e, c->right - c->left);
    int y = c->y + c->margin, l, r;
    for (;;) {                                  /* beside the floats there, or below them */
        edges(c, y, 1, &l, &r);
        if (r - l >= outer || (l == c->left && r == c->right))
            break;
        int below = next_float_bottom(c, y, CLEAR_LEFT | CLEAR_RIGHT);
        if (below < 0)
            break;
        y = below;
    }
    int x = s->floating == FLOAT_LEFT ? l : r - outer;
    if (x < c->left)
        x = c->left;
    int band = c->band;
    if (!c->band)
        c->band = BAND_FLOAT;
    int bottom = lay_box(c, e, x, outer, y, false);
    c->band = band;
    if (c->nfloats < FLOATS)
        c->floats[c->nfloats++] = (struct floated){ x, y, outer, bottom - y,
                                                     s->floating == FLOAT_RIGHT };
    start_line(c);                              /* (the line's edges have changed) */
}

static void inline_block(struct ctx *c, struct node *e)
{
    int outer = box_width(c, e, c->right - c->left);
    /* Its height first (laid out in a scratch page at that width), so the
     * line knows whether it fits. */
    int h = measure(c, e, outer, false, 2, 0);
    if (c->line.space && c->line.any) {
        int scale;
        bool fake;
        c->line.x += space_width(style_font(e->parent ? e->parent->style : 0, &scale, &fake)) * scale;
    }
    int x = place(c, outer, h);
    c->line.space = false;
    struct item *box = add_item(c, ITEM_BOX);
    if (!box)
        return;
    box->x = x, box->w = outer, box->h = h, box->decoration = true;
    box->valign = e->style->valign;
    int leader = c->page->count - 1, number = c->line_number;
    lay_box(c, e, x, outer, 0, true);           /* (at y 0: its items ride on the box) */
    c->line_number = number;
    for (int i = leader + 1; i < c->page->count; i++) {     /* (inner boxes' too: their y is
                                                               final inside this one by now) */
        struct item *it = &c->page->items[i];
        it->line = number;                      /* (read as part of the line it is on) */
        it->rider = true, it->leader = leader, it->ry = it->y;
    }
}

static void defer(struct ctx *c, struct node *e)
{
    if (c->measuring || c->nlater >= 100000)
        return;
    if (c->nlater == c->later_room) {
        int room = c->later_room ? 2 * c->later_room : 64;
        struct later *l = realloc(c->later, sizeof(*l) * (size_t)room);
        if (!l)
            return;
        c->later = l, c->later_room = room;
    }
    struct node *holder = 0;
    for (struct node *p = e->parent; p && !holder; p = p->parent)
        if (p->style && p->style->position != POSITION_STATIC)
            holder = p;
    c->later[c->nlater++] = (struct later){ e, holder, c->line.x, c->y + c->margin,
                                            positioned_band(e->style) };
}

/* An absolute or fixed box, after everything else. Its containing block:
 * the holder's box less its border (moved as the holder is, if relative),
 * or the view. Returns its bottom. */
static int lay_positioned(struct ctx *c, const struct later *d)
{
    struct node *e = d->e;
    const struct style *s = e->style;
    int cx = 0, cy = 0, cw = c->page->width, ch = c->view_h;
    struct node *h = s->position == POSITION_FIXED ? 0 : d->holder;
    while (h && !h->box[2] && !h->box[3])
        h = h->parent;                          /* (an inline holder: its block's box) */
    if (h) {
        const int *b = h->style ? h->style->border : (const int[4]){ 0, 0, 0, 0 };
        cx = h->box[0] + b[3], cy = h->box[1] + b[0], cw = h->box[2] - b[1] - b[3], ch = h->box[3] - b[0] - b[2];
        if (h->style && h->style->position == POSITION_RELATIVE) {
            const struct style *hs = h->style;
            cx += hs->offset[3] != LENGTH_AUTO ? offset_px(hs->offset[3], cw)
                : hs->offset[1] != LENGTH_AUTO ? -offset_px(hs->offset[1], cw) : 0;
            cy += hs->offset[0] != LENGTH_AUTO ? hs->offset[0]
                : hs->offset[2] != LENGTH_AUTO ? -hs->offset[2] : 0;
        }
    }
    int off[4];
    for (int i = 0; i < 4; i++)
        off[i] = s->offset[i] == LENGTH_AUTO ? LENGTH_AUTO : offset_px(s->offset[i], cw);
    int outer, width = resolved_width(s->width, s->width_add, cw);
    if (width >= 0)
        outer = outer_of(s, width);
    else if (off[1] != LENGTH_AUTO && off[3] != LENGTH_AUTO)
        outer = cw - off[1] - off[3];
    else
        outer = shrink_to_fit(c, e, cw - (off[1] != LENGTH_AUTO ? off[1] : 0) -
                                    (off[3] != LENGTH_AUTO ? off[3] : 0));
    if (outer < 1)
        outer = 1;
    int x = off[3] != LENGTH_AUTO ? cx + off[3] : off[1] != LENGTH_AUTO ? cx + cw - off[1] - outer : d->x;
    int y = off[0] != LENGTH_AUTO ? cy + off[0] : d->y;
    int first = c->page->count, band = c->band, saved_cb_h = c->cb_h;
    c->band = d->band;
    c->cb_h = ch;                               /* (27m) its percentages are of its container */
    if (s->height <= 0 && !s->height_pct && off[0] != LENGTH_AUTO && off[2] != LENGTH_AUTO) {
        int m0 = s->margin[0] == LENGTH_AUTO ? 0 : s->margin[0], m2 = s->margin[2] == LENGTH_AUTO ? 0 : s->margin[2];
        c->has_given = true;                    /* (top and bottom: as high as between them) */
        c->given_h = ch - off[0] - off[2] - m0 - m2;
    }
    int later_from = c->nlater;
    int bottom = lay_box(c, e, x, outer, y, true);
    c->has_given = false;
    c->cb_h = saved_cb_h;
    c->band = band;
    if (off[0] == LENGTH_AUTO && off[2] != LENGTH_AUTO) {       /* by its bottom */
        int dy = cy + ch - off[2] - bottom;
        for (int i = first; i < c->page->count; i++) {
            struct item *it = &c->page->items[i];
            it->y += dy, it->tile_y += dy;
            if (it->clipped)
                it->clip[1] += dy, it->clip[3] += dy;
        }
        for (struct node *n = e; n; n = node_next(n, e))
            if (n->kind == NODE_ELEMENT && (n->box[2] || n->box[3]))
                n->box[1] += dy;                /* (its boxes too: what is positioned in it) */
        for (int i = later_from; i < c->nlater; i++)
            c->later[i].y += dy;                /* (and where those would have been) */
        bottom += dy;
    }
    if (s->position == POSITION_FIXED) {
        for (int i = first; i < c->page->count; i++)
            c->page->items[i].fixed = true;
        return 0;                               /* (it does not make the page longer) */
    }
    return bottom;
}

/* --- flexible boxes (27g) --- */

/* The next of e's children in order, going into those with display:
 * contents (whose children stand in their place) and out of them again. */
static struct node *contents_next(struct node *k, struct node *e)
{
    if (k->kind == NODE_ELEMENT && k->style && k->style->display == DISPLAY_CONTENTS && k->first)
        return k->first;
    while (k && k != e) {
        if (k->next)
            return k->next;
        k = k->parent;
        if (k == e)
            return 0;
    }
    return 0;
}

struct flex_item {
    struct node *n;                     /* an element, or a text (an anonymous item) */
    const struct style *s;              /* its style (a text: 0) */
    int m[4];                           /* margins (auto: 0) */
    bool auto_m[4];
    int around;                         /* padding and border along the main axis */
    int base, hypo, min, max, size;     /* main sizes, border box */
    float grow, shrink;
    bool frozen;
    int cross, outer_cross;             /* cross size: border box, and with the margins */
    int pos;                            /* main position of its margin box (from the start) */
    int line;
    enum flex_align align;
};

static const struct style NO_STYLE = { .width = -1, .height = -1, .max_width = -1, .min_width = -1,
                                       .basis = -1, .shrink = 1, .align_self = FLEX_AUTO };

/* A length given as a width (-1, px, or a percentage of room): the border
 * box it makes, or -1. */
static int flex_length(const struct style *s, int value, int add, int room, int around)
{
    int v = resolved_width(value, add, room);
    if (v < 0)
        return -1;
    return s->border_box ? (v > around ? v : around) : v + around;
}

/* The flex items of e's lines laid out (CSS Flexible Box Layout 9: the
 * main sizes from the bases, flexed to fill each line or shrunk to fit,
 * then the cross sizes from laying them out at those). `definite`: e's
 * content height when known, else -1. */
static void flex_contents(struct ctx *c, struct node *e, int definite)
{
    const struct style *s = e->style;
    bool row = s->direction == FLEX_ROW || s->direction == FLEX_ROW_REVERSE;
    bool wrap = row && s->wrap != FLEX_NOWRAP;  /* (columns are one line) */
    int W = c->right - c->left, x0 = c->left, y0 = c->y + c->margin;
    c->margin = 0;
    int main_room = row ? W : definite;         /* (-1: as long as its items) */
    int gap = row ? s->column_gap : s->row_gap, cross_gap = row ? s->row_gap : s->column_gap;
    int count = 0;
    for (struct node *k = e->first; k; k = contents_next(k, e))
        count++;
    struct flex_item *it = calloc((size_t)(count ? count : 1), sizeof(*it));
    if (!it)
        return;
    int n = 0;
    for (struct node *k = e->first; k && n < count; k = contents_next(k, e)) {
        if (k->kind == NODE_ELEMENT && k->style && k->style->display == DISPLAY_CONTENTS)
            continue;                           /* (its children are the items) */
        if (k->kind == NODE_TEXT) {
            bool blank = true;
            for (const char *p = k->text; p && *p && blank; p++)
                blank = is_space(*p);
            if (blank)
                continue;
            it[n].n = k, it[n].s = 0;
        } else {
            const struct style *ks = k->style;
            if (!ks || ks->display == DISPLAY_NONE || !strcmp(k->tag, "br"))
                continue;
            if (ks->position == POSITION_ABSOLUTE || ks->position == POSITION_FIXED) {
                c->line.x = x0, c->y = y0;
                defer(c, k);
                continue;
            }
            it[n].n = k, it[n].s = ks;
        }
        n++;
    }
    for (int i = 1; i < n; i++)                 /* by `order` (stable) */
        for (int k = i; k > 0; k--) {
            int a = it[k - 1].s ? it[k - 1].s->order : 0, b = it[k].s ? it[k].s->order : 0;
            if (a <= b)
                break;
            struct flex_item t = it[k];
            it[k] = it[k - 1], it[k - 1] = t;
        }
    /* The sizes along the main axis. */
    for (int i = 0; i < n; i++) {
        struct flex_item *f = &it[i];
        const struct style *fs = f->s ? f->s : &NO_STYLE;
        for (int k = 0; k < 4; k++) {
            f->auto_m[k] = fs->margin[k] == LENGTH_AUTO;
            f->m[k] = f->auto_m[k] ? 0 : fs->margin[k];
        }
        int pad[4];
        for (int k = 0; k < 4; k++)
            pad[k] = fs->padding[k] > 0 ? fs->padding[k] : 0;
        f->around = row ? fs->border[1] + fs->border[3] + pad[1] + pad[3]
                        : fs->border[0] + fs->border[2] + pad[0] + pad[2];
        f->grow = fs->grow, f->shrink = fs->shrink;
        enum flex_align a = fs->align_self != FLEX_AUTO ? fs->align_self : s->align_items;
        f->align = a == FLEX_NORMAL ? FLEX_STRETCH : a;
        int room = row ? W : W - f->m[1] - f->m[3];
        if (row) {
            int basis = flex_length(fs, fs->basis, fs->basis_add, W, f->around);
            if (basis < 0)
                basis = flex_length(fs, fs->width, fs->width_add, W, f->around);
            if (basis < 0) {                    /* its contents' width */
                int widest;
                measure(c, f->n, W, false, 0, &widest);
                basis = widest - f->m[1] - f->m[3];
            }
            f->base = basis < 0 ? 0 : basis;
            f->max = flex_length(fs, fs->max_width, fs->max_width_add, W, f->around);
            f->min = flex_length(fs, fs->min_width, fs->min_width_add, W, f->around);
            if (f->min < 0 && !fs->holds_floats) {      /* auto: no narrower than its longest word */
                int widest;                     /* (its contents', its own width aside) */
                measure(c, f->n, 1, true, 1, &widest);
                f->min = widest;
                int w = flex_length(fs, fs->width, fs->width_add, W, f->around);
                if (w >= 0 && w < f->min)
                    f->min = w;
            }
        } else {                                /* (a column: the cross size first) */
            int w = flex_length(fs, fs->width, fs->width_add, W, fs->border[1] + fs->border[3] + pad[1] + pad[3]);
            bool stretch = f->align == FLEX_STRETCH && w < 0 && !f->auto_m[1] && !f->auto_m[3];
            if (w < 0)
                w = stretch ? room : shrink_to_fit(c, f->n, room) - f->m[1] - f->m[3];
            f->cross = w < 0 ? 0 : w;
            int pv = pad[0] + pad[2] + fs->border[0] + fs->border[2];
            int basis = -1;
            if (fs->basis >= 0)                 /* (a percentage of an unknown height: auto) */
                basis = fs->border_box ? fs->basis : fs->basis + pv;
            int content = measure(c, f->n, f->cross, true, 2, 0) - f->m[0] - f->m[2];
            if (basis < 0 && fs->height > 0)
                basis = fs->border_box ? fs->height : fs->height + pv;
            if (basis < 0)
                basis = content;
            f->base = basis < 0 ? 0 : basis;
            f->max = -1;
            f->min = fs->holds_floats ? 0 : (content < f->base || fs->height > 0 ? content : f->base);
            if (fs->height > 0 && f->min > f->base)
                f->min = f->base;
        }
        if (f->max >= 0 && f->max < f->min)
            f->max = f->min;
        f->hypo = f->base;
        if (f->max >= 0 && f->hypo > f->max)
            f->hypo = f->max;
        if (f->min >= 0 && f->hypo < f->min)
            f->hypo = f->min;
    }
    /* Into lines. */
    int lines = 0;
    for (int i = 0, used = 0; i < n; i++) {
        int outer = it[i].hypo + (row ? it[i].m[1] + it[i].m[3] : it[i].m[0] + it[i].m[2]);
        if (i == 0 || (wrap && used + gap + outer > W))
            lines++, used = outer;              /* (a new line; one item at least on each) */
        else
            used += gap + outer;
        it[i].line = lines - 1;
    }
    /* Each line's items flexed (9.7). */
    for (int l = 0; l < lines; l++) {
        int from = 0, to;
        while (from < n && it[from].line != l)
            from++;
        for (to = from; to < n && it[to].line == l; to++)
            ;
        int outer_sum = 0, margins = 0;
        for (int i = from; i < to; i++) {
            int mm = row ? it[i].m[1] + it[i].m[3] : it[i].m[0] + it[i].m[2];
            outer_sum += it[i].hypo + mm;
            margins += mm;
        }
        margins += gap * (to - from - 1);
        for (int i = from; i < to; i++)
            it[i].size = it[i].hypo, it[i].frozen = false;
        if (main_room < 0)
            continue;                           /* (no room to fill: the sizes stay) */
        bool growing = outer_sum + gap * (to - from - 1) < main_room;
        int initial = main_room - margins;
        for (int i = from; i < to; i++) {
            struct flex_item *f = &it[i];
            if ((growing ? f->grow : f->shrink) == 0 || (growing && f->base > f->hypo) ||
                (!growing && f->base < f->hypo))
                f->frozen = true;
            initial -= f->frozen ? f->size : f->base;
        }
        for (int round = 0; round < 64; round++) {
            int used = margins;
            float factors = 0, scaled = 0;
            bool open = false;
            for (int i = from; i < to; i++) {
                struct flex_item *f = &it[i];
                used += f->frozen ? f->size : f->base;
                if (!f->frozen) {
                    open = true;
                    factors += growing ? f->grow : f->shrink;
                    scaled += f->shrink * (float)f->base;
                }
            }
            if (!open)
                break;
            float free_space = (float)(main_room - used);
            if (factors < 1 && factors > 0) {   /* (factors below 1 take only their part) */
                float part = (float)initial * factors;
                if ((part < 0 ? -part : part) < (free_space < 0 ? -free_space : free_space))
                    free_space = part;
            }
            int violation = 0;
            float sum = 0;                      /* (rounded as a running sum: no pixel lost or added) */
            for (int i = from; i < to; i++) {
                struct flex_item *f = &it[i];
                if (f->frozen)
                    continue;
                float target = (float)f->base;
                if (growing && factors > 0)
                    target += free_space * f->grow / factors;
                else if (!growing && scaled > 0)
                    target += free_space * f->shrink * (float)f->base / scaled;
                int before = (int)(sum + 0.5f);
                sum += target;
                int t = (int)(sum + 0.5f) - before;
                int clamped = t;
                if (f->max >= 0 && clamped > f->max)
                    clamped = f->max;
                if (f->min >= 0 && clamped < f->min)
                    clamped = f->min;
                if (clamped < 0)
                    clamped = 0;
                violation += clamped - t;
                f->size = clamped;
                f->pos = clamped - t;           /* (the violation, for now) */
            }
            for (int i = from; i < to; i++) {
                struct flex_item *f = &it[i];
                if (f->frozen)
                    continue;
                if (violation == 0 || (violation > 0 && f->pos > 0) || (violation < 0 && f->pos < 0))
                    f->frozen = true;
            }
        }
    }
    /* Cross sizes, and each line's. */
    int *line_cross = calloc((size_t)(lines ? lines : 1), sizeof(int));
    if (!line_cross) {
        free(it);
        return;
    }
    for (int i = 0; i < n; i++) {
        struct flex_item *f = &it[i];
        if (row) {
            f->outer_cross = measure(c, f->n, f->size, true, 2, 0);
            f->cross = f->outer_cross - f->m[0] - f->m[2];
        } else {
            f->outer_cross = f->cross + f->m[1] + f->m[3];
        }
        if (f->outer_cross > line_cross[f->line])
            line_cross[f->line] = f->outer_cross;
    }
    int cross_room = row ? definite : W;        /* (-1: as tall as its lines) */
    if (!wrap && cross_room >= 0 && lines == 1)
        line_cross[0] = cross_room;             /* (one line fills the container) */
    int total_cross = 0;
    for (int l = 0; l < lines; l++)
        total_cross += line_cross[l] + (l ? cross_gap : 0);
    int line_start = 0, line_gap = cross_gap;   /* (align-content) */
    if (wrap && cross_room >= 0 && cross_room > total_cross && lines) {
        int extra = cross_room - total_cross;
        switch (s->align_content) {
        case FLEX_END: line_start = extra; break;
        case FLEX_CENTER: line_start = extra / 2; break;
        case FLEX_BETWEEN: if (lines > 1) line_gap += extra / (lines - 1); break;
        case FLEX_AROUND: line_start = extra / (2 * lines), line_gap += extra / lines; break;
        case FLEX_EVENLY: line_start = extra / (lines + 1), line_gap += extra / (lines + 1); break;
        case FLEX_START: break;
        default:                                /* (normal, stretch: the lines share it) */
            for (int l = 0; l < lines; l++)
                line_cross[l] += extra / lines;
        }
        total_cross = cross_room;
    }
    /* Places along the main axis (auto margins first, then justify-content). */
    int main_end = 0;
    for (int l = 0; l < lines; l++) {
        int from = 0, to;
        while (from < n && it[from].line != l)
            from++;
        for (to = from; to < n && it[to].line == l; to++)
            ;
        int used = gap * (to - from - 1), autos = 0;
        for (int i = from; i < to; i++) {
            used += it[i].size + (row ? it[i].m[1] + it[i].m[3] : it[i].m[0] + it[i].m[2]);
            autos += row ? it[i].auto_m[1] + it[i].auto_m[3] : it[i].auto_m[0] + it[i].auto_m[2];
        }
        int room = main_room >= 0 ? main_room : used;
        int free_space = room - used, start = 0, between = gap;
        if (autos && free_space > 0) {
            int each = free_space / autos;
            for (int i = from; i < to; i++) {
                int a = row ? 3 : 0, b2 = row ? 1 : 2;
                if (it[i].auto_m[a])
                    it[i].m[a] = each;
                if (it[i].auto_m[b2])
                    it[i].m[b2] = each;
            }
        } else if (free_space > 0 || s->justify == FLEX_CENTER || s->justify == FLEX_END) {
            int k = to - from;
            switch (s->justify) {
            case FLEX_END: start = free_space; break;
            case FLEX_CENTER: start = free_space / 2; break;
            case FLEX_BETWEEN: if (k > 1) between += free_space / (k - 1); break;
            case FLEX_AROUND: start = free_space / (2 * k), between += free_space / k; break;
            case FLEX_EVENLY: start = free_space / (k + 1), between += free_space / (k + 1); break;
            default: break;
            }
        }
        int pos = start;
        for (int i = from; i < to; i++) {
            it[i].pos = pos;
            pos += it[i].size + (row ? it[i].m[1] + it[i].m[3] : it[i].m[0] + it[i].m[2]) + between;
        }
        int end = pos - between;
        if (end > main_end)
            main_end = end;
    }
    if (main_room >= 0 && main_room > main_end)
        main_end = main_room;
    /* Lay each item out in its place. */
    bool reverse = s->direction == FLEX_ROW_REVERSE || s->direction == FLEX_COLUMN_REVERSE;
    int bottom = y0;
    int first_line = c->line_number, last_line = first_line, line_of = -1;
    for (int i = 0; i < n; i++) {
        struct flex_item *f = &it[i];
        if (row) {                              /* (27i: a flex line's items read side by side) */
            if (f->line != line_of)
                first_line = last_line, line_of = f->line;
            c->line_number = first_line;
        }
        int ltop = line_start;
        for (int l = 0; l < f->line; l++)
            ltop += line_cross[l] + line_gap;
        int lc = line_cross[f->line];
        if (s->wrap == FLEX_WRAP_REVERSE && row)
            ltop = total_cross - ltop - lc;
        int offset = 0, min_h = 0;
        int a1 = row ? 0 : 3, a2 = row ? 2 : 1;            /* (the cross axis' margins) */
        int free_cross = lc - f->outer_cross;
        if (f->auto_m[a1] || f->auto_m[a2]) {
            offset = f->auto_m[a1] && f->auto_m[a2] ? free_cross / 2 : f->auto_m[a1] ? free_cross : 0;
            if (offset < 0)
                offset = 0;
        } else if (f->align == FLEX_END)
            offset = free_cross;
        else if (f->align == FLEX_CENTER)
            offset = free_cross / 2;
        else if (f->align == FLEX_STRETCH && row && f->s && f->s->height <= 0)
            min_h = lc - f->m[0] - f->m[2];
        int main_pos = f->pos;
        int main_outer = f->size + (row ? f->m[1] + f->m[3] : f->m[0] + f->m[2]);
        if (reverse)
            main_pos = main_end - main_pos - main_outer;
        int saved = c->link;
        if (f->s && !strcmp(f->n->tag, "a") && node_attr(f->n, "href") && !c->measuring)
            c->link = add_link(c->page, node_attr(f->n, "href"), c->base);
        int box_bottom;
        c->exact = true;
        if (row) {
            c->min_h = min_h;
            box_bottom = lay_box(c, f->n, x0 + main_pos + f->m[3], f->size, y0 + ltop + offset, true);
        } else {
            c->min_h = f->size;
            box_bottom = lay_box(c, f->n, x0 + ltop + offset + f->m[3], f->cross, y0 + main_pos, true);
        }
        c->exact = false, c->min_h = 0;
        c->link = saved;
        if (box_bottom > bottom)
            bottom = box_bottom;
        if (c->line_number > last_line)
            last_line = c->line_number;
    }
    c->line_number = last_line;
    int end = row ? y0 + total_cross : y0 + main_end;
    c->y = bottom > end ? bottom : end;
    start_line(c);
    free(line_cross);
    free(it);
}

/* --- tables (27i) --- */

#define TABLE_COLUMNS 1000
#define TABLE_WIDEST 4096               /* the room a cell's widest is measured in (unbroken lines) */

struct table_cell {
    struct node *n;
    int row, col, cols, rows;
};

struct table {
    struct table_cell *cells;
    int ncells, cell_room;
    struct node **rows;                 /* each row's element (0: an anonymous one) */
    int nrows, row_room;
    int ncols;
    int *width;                         /* each column's (border box of a one-column cell) */
    int sx, sy;                         /* border-spacing (0 when collapsed) */
    bool collapse;
    int need;                           /* the content width it takes */
};

static bool is_kind(const struct node *n, const char *tag, enum display d)
{
    return n->kind == NODE_ELEMENT && n->style && n->style->display != DISPLAY_NONE &&
           (!strcmp(n->tag, tag) || (n->style->display == d && strcmp(n->tag, "tr") && strcmp(n->tag, "td") &&
                                      strcmp(n->tag, "th")));
}

static bool is_cell(const struct node *n)
{
    return n->kind == NODE_ELEMENT && n->style && n->style->display != DISPLAY_NONE &&
           (!strcmp(n->tag, "td") || !strcmp(n->tag, "th") || n->style->display == DISPLAY_TABLE_CELL);
}

static bool is_row(const struct node *n)
{
    return is_kind(n, "tr", DISPLAY_TABLE_ROW);
}

static bool is_group(const struct node *n)
{
    return n->kind == NODE_ELEMENT && n->style && n->style->display != DISPLAY_NONE &&
           (!strcmp(n->tag, "thead") || !strcmp(n->tag, "tbody") || !strcmp(n->tag, "tfoot"));
}

static int attr_count(const struct node *e, const char *name)
{
    const char *v = node_attr(e, name);
    int n = v ? atoi(v) : 1;
    return n < 1 ? 1 : n > TABLE_COLUMNS ? TABLE_COLUMNS : n;
}

static void add_row(struct table *t, struct node *row)
{
    if (t->nrows == t->row_room) {
        int room = t->row_room ? 2 * t->row_room : 16;
        struct node **r = realloc(t->rows, sizeof(*r) * (size_t)room);
        if (!r)
            return;
        t->rows = r, t->row_room = room;
    }
    t->rows[t->nrows++] = row;
}

static void add_cell(struct table *t, struct node *n, int row)
{
    if (t->ncells == t->cell_room) {
        int room = t->cell_room ? 2 * t->cell_room : 32;
        struct table_cell *cs = realloc(t->cells, sizeof(*cs) * (size_t)room);
        if (!cs)
            return;
        t->cells = cs, t->cell_room = room;
    }
    t->cells[t->ncells++] = (struct table_cell){ n, row, 0, attr_count(n, "colspan"), attr_count(n, "rowspan") };
}

/* A row's cells into t (the row's element, or 0 and its parent's cells). */
static void collect_row(struct table *t, struct node *row, struct node *from)
{
    add_row(t, row);
    for (struct node *k = from; k; k = k->next) {
        if (!row && !is_cell(k))
            break;                              /* (an anonymous row: the cells in a run) */
        if (is_cell(k))
            add_cell(t, k, t->nrows - 1);
    }
}

static void table_free(struct table *t)
{
    if (!t)
        return;
    free(t->cells);
    free(t->rows);
    free(t->width);
    free(t);
}

/* e's rows and cells, and its columns' widths in `avail` (or `given`, its
 * own width, when it has one): CSS's automatic table layout, as browsers
 * do it: each column between its cells' narrowest (their longest words)
 * and widest (their lines unbroken), cells of several columns shared out
 * over them, then the room given out in proportion. */
static struct table *table_build(struct ctx *c, struct node *e, int avail, int given)
{
    struct table *t = calloc(1, sizeof(*t));
    if (!t)
        return 0;
    const struct style *s = e->style;
    t->collapse = s->collapse;
    t->sx = t->collapse ? 0 : s->spacing_x, t->sy = t->collapse ? 0 : s->spacing_y;
    /* Rows: the head's first, then the bodies' and loose ones, the foot's last. */
    for (int pass = 0; pass < 3; pass++)
        for (struct node *k = e->first; k; k = k->next) {
            bool head = is_group(k) && !strcmp(k->tag, "thead"), foot = is_group(k) && !strcmp(k->tag, "tfoot");
            if ((pass == 0) != head || (pass == 2) != foot)
                continue;
            if (is_group(k)) {
                for (struct node *r = k->first; r; r = r->next)
                    if (is_row(r))
                        collect_row(t, r, r->first);
            } else if (is_row(k)) {
                collect_row(t, k, k->first);
            } else if (is_cell(k) && (!k->prev || !is_cell(k->prev))) {
                collect_row(t, 0, k);
            }
        }
    /* The grid: each cell at the first column its row has free. */
    int *busy = calloc(TABLE_COLUMNS, sizeof(int));     /* (rows a column is still taken for) */
    if (!busy) {
        table_free(t);
        return 0;
    }
    int at = 0;
    for (int r = 0; r < t->nrows; r++) {
        int col = 0;
        for (; at < t->ncells && t->cells[at].row == r; at++) {
            struct table_cell *cl = &t->cells[at];
            while (col < TABLE_COLUMNS && busy[col])
                col++;
            if (col + cl->cols > TABLE_COLUMNS)
                cl->cols = TABLE_COLUMNS - col;
            cl->col = col;
            if (cl->rows > t->nrows - r)
                cl->rows = t->nrows - r;
            for (int k = col; k < col + cl->cols; k++)
                busy[k] = cl->rows;
            col += cl->cols;
            if (col > t->ncols)
                t->ncols = col;
        }
        for (int k = 0; k < TABLE_COLUMNS; k++)
            if (busy[k])
                busy[k]--;
    }
    free(busy);
    int n = t->ncols;
    t->width = calloc((size_t)(n ? n : 1), sizeof(int));
    int *lo = calloc((size_t)(n ? n : 1), sizeof(int)), *hi = calloc((size_t)(n ? n : 1), sizeof(int));
    bool *fixed = calloc((size_t)(n ? n : 1), sizeof(bool));
    if (!t->width || !lo || !hi || !fixed) {
        free(lo), free(hi), free(fixed);
        table_free(t);
        return 0;
    }
    int room = given >= 0 ? given : avail;
    /* The cells' narrowest and widest; one column's first, then the wider. */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < t->ncells; i++) {
            struct table_cell *cl = &t->cells[i];
            if ((cl->cols == 1) != (pass == 0) || cl->cols == 0)
                continue;
            const struct style *cs = cl->n->style;
            int narrow, wide;
            measure(c, cl->n, 1, false, 1, &narrow);
            measure(c, cl->n, TABLE_WIDEST, false, 0, &wide);    /* (one room for every table:
                                                                    nested tables measure once) */
            int w = resolved_width(cs->width, cs->width_add, room);
            bool fix = w >= 0;
            if (fix) {                          /* (a width given: that, unless it cannot) */
                w = outer_of(cs, w);
                wide = w > narrow ? w : narrow;
                narrow = wide;
            }
            if (wide < narrow)
                wide = narrow;
            if (cl->cols == 1) {
                if (narrow > lo[cl->col]) lo[cl->col] = narrow;
                if (wide > hi[cl->col]) hi[cl->col] = wide;
                fixed[cl->col] = fixed[cl->col] || fix;
                continue;
            }
            int have_lo = t->sx * (cl->cols - 1), have_hi = have_lo;
            for (int k = cl->col; k < cl->col + cl->cols; k++)
                have_lo += lo[k], have_hi += hi[k];
            for (int k = cl->col; k < cl->col + cl->cols; k++) {  /* (the more evenly) */
                if (narrow > have_lo)
                    lo[k] += (narrow - have_lo) / cl->cols + (k - cl->col < (narrow - have_lo) % cl->cols);
                if (wide > have_hi)
                    hi[k] += (wide - have_hi) / cl->cols + (k - cl->col < (wide - have_hi) % cl->cols);
                if (hi[k] < lo[k])
                    hi[k] = lo[k];
            }
        }
    long sum_lo = 0, sum_hi = 0, sum_free = 0;
    for (int k = 0; k < n; k++) {
        sum_lo += lo[k], sum_hi += hi[k];
        if (!fixed[k])
            sum_free += hi[k];
    }
    int gaps = n ? t->sx * (n + 1) : 0;
    long want = given >= 0 ? given : (sum_hi + gaps < avail ? sum_hi + gaps : avail);
    if (want < sum_lo + gaps)
        want = sum_lo + gaps;                   /* (no narrower than its words) */
    long inside = want - gaps;
    float sum = 0;                              /* (rounded as a running sum) */
    for (int k = 0; k < n; k++) {
        float w;
        if (inside >= sum_hi) {                 /* more room than wanted: to the columns not fixed */
            long extra = inside - sum_hi;
            w = (float)hi[k] + (sum_free > 0 ? (fixed[k] ? 0 : (float)extra * hi[k] / sum_free)
                                             : (float)extra / n);
        } else if (sum_hi > sum_lo) {           /* between: each its share of what is between */
            w = (float)lo[k] + (float)(hi[k] - lo[k]) * (inside - sum_lo) / (sum_hi - sum_lo);
        } else {
            w = (float)lo[k];
        }
        int before = (int)(sum + 0.5f);
        sum += w;
        t->width[k] = (int)(sum + 0.5f) - before;
    }
    t->need = (int)want;
    free(lo), free(hi), free(fixed);
    return t;
}

/* The cells laid out in their columns, a row as high as its tallest cell,
 * each cell's box that high (its contents placed by vertical-align). */
static void table_lay(struct ctx *c, struct node *e, struct table *t)
{
    for (struct node *k = e->first; k; k = k->next)       /* its captions, above it */
        if (k->kind == NODE_ELEMENT && k->style && k->style->display != DISPLAY_NONE &&
            !strcmp(k->tag, "caption"))
            block(c, k);
    end_line(c, false, 0);
    int x0 = c->left, y0 = c->y + c->margin;
    c->margin = 0;
    int *col_x = calloc((size_t)t->ncols + 1, sizeof(int));
    int *row_h = calloc((size_t)t->nrows + 1, sizeof(int));
    int *cell_h = calloc((size_t)t->ncells + 1, sizeof(int));
    if (!col_x || !row_h || !cell_h) {
        free(col_x), free(row_h), free(cell_h);
        return;
    }
    col_x[0] = x0 + t->sx;
    for (int k = 0; k < t->ncols; k++)
        col_x[k + 1] = col_x[k] + t->width[k] + t->sx;
    /* The rows' heights: the cells measured at their widths. */
    for (int pass = 0; pass < 2; pass++)        /* (one-row cells first, then the taller) */
        for (int i = 0; i < t->ncells; i++) {
            struct table_cell *cl = &t->cells[i];
            if ((cl->rows == 1) != (pass == 0) || cl->cols == 0)
                continue;
            int w = col_x[cl->col + cl->cols] - col_x[cl->col] - t->sx;
            cell_h[i] = measure(c, cl->n, w, true, 2, 0);
            const struct style *cs = cl->n->style;
            if (cs->height > 0) {
                int fixed = cs->border_box ? cs->height
                                           : cs->height + cs->border[0] + cs->border[2] +
                                             (cs->padding[0] > 0 ? cs->padding[0] : 0) +
                                             (cs->padding[2] > 0 ? cs->padding[2] : 0);
                if (fixed > cell_h[i])
                    cell_h[i] = fixed;
            }
            int last = cl->row + cl->rows - 1, have = t->sy * (cl->rows - 1);
            for (int r = cl->row; r <= last; r++)
                have += row_h[r];
            if (cell_h[i] > have)
                row_h[last] += cell_h[i] - have;
        }
    for (int r = 0; r < t->nrows; r++) {        /* (a row's own height) */
        struct node *row = t->rows[r];
        if (row && row->style && row->style->height > row_h[r])
            row_h[r] = row->style->height;
    }
    int *row_y = calloc((size_t)t->nrows + 1, sizeof(int));
    if (!row_y) {
        free(col_x), free(row_h), free(cell_h);
        return;
    }
    row_y[0] = y0 + t->sy;
    for (int r = 0; r < t->nrows; r++)
        row_y[r + 1] = row_y[r] + row_h[r] + t->sy;
    int at = 0;
    for (int r = 0; r < t->nrows; r++) {
        struct node *row = t->rows[r];
        int first_line = c->line_number, last_line = first_line;   /* (a row's cells read side by side) */
        if (row) {
            add_anchor(c, row, row_y[r]);
            const struct style *rs = row->style;
            if (rs && rs->background != COLOR_NONE && !rs->hidden)     /* (a row's background) */
                fill(c, col_x[0], row_y[r], col_x[t->ncols] - col_x[0] - t->sx, row_h[r],
                     mix(rs->background, rs->background_alpha, c->under));
        }
        for (; at < t->ncells && t->cells[at].row == r; at++) {
            struct table_cell *cl = &t->cells[at];
            if (cl->cols == 0)
                continue;
            int x = col_x[cl->col], w = col_x[cl->col + cl->cols] - x - t->sx;
            int y = row_y[r], h = row_y[r + cl->rows] - y - t->sy;
            const struct style *cs = cl->n->style;
            if (t->collapse) {                  /* (borders shared: each over the one before) */
                if (cl->col > 0)
                    x -= cs->border[3], w += cs->border[3];
                if (r > 0)
                    y -= cs->border[0], h += cs->border[0];
            }
            int free_h = h - cell_h[at];
            c->exact = true;
            c->min_h = h;
            c->push = free_h <= 0 ? 0 : cs->valign == VALIGN_MIDDLE ? free_h / 2
                    : cs->valign == VALIGN_BOTTOM ? free_h : 0;
            c->line_number = first_line;
            lay_box(c, cl->n, x, w, y, true);
            c->exact = false, c->min_h = 0, c->push = 0;
            if (c->line_number > last_line)
                last_line = c->line_number;
        }
        c->line_number = last_line;
    }
    c->y = row_y[t->nrows];
    start_line(c);
    free(col_x), free(row_h), free(cell_h), free(row_y);
}

/* --- grids (27n) --- */

#define GRID_TRACKS 1000
#define GRID_ROWS   10000

enum { TRACK_PX, TRACK_MIN, TRACK_MAX, TRACK_FR };   /* a track's minimum or maximum sizing */

struct track {
    int min_kind, max_kind;             /* SIZE_*: the minimum never TRACK_FR */
    int min_px, max_px;                 /* (TRACK_PX; a TRACK_MAX maximum's cap from fit-content, or -1) */
    float fr;
    int size;                           /* as laid out */
};

struct tracks {
    struct track *t;
    int n, room;
};

static void add_track(struct tracks *ts, struct track t)
{
    if (ts->n >= GRID_TRACKS)
        return;
    if (ts->n == ts->room) {
        int room = ts->room ? 2 * ts->room : 16;
        struct track *g = realloc(ts->t, sizeof(*g) * (size_t)room);
        if (!g)
            return;
        ts->t = g, ts->room = room;
    }
    ts->t[ts->n++] = t;
}

/* One sizing word: a length, a percentage of room, fr, auto, min-content,
 * max-content. As a minimum (fr: auto) or a maximum. */
static void sizing(const char *w, int em, int room, bool as_max, int *kind, int *px, float *fr)
{
    *px = 0, *fr = 0;
    size_t n = strlen(w);
    if (n > 2 && !strcmp(w + n - 2, "fr")) {
        float v = (float)atof(w);
        if (as_max && v > 0)
            *kind = TRACK_FR, *fr = v > 1000 ? 1000 : v;
        else
            *kind = TRACK_MIN;
    } else if (same_word(w, "min-content")) {
        *kind = TRACK_MIN;
    } else if (same_word(w, "max-content")) {
        *kind = TRACK_MAX;
    } else if (same_word(w, "auto")) {
        *kind = as_max ? TRACK_MAX : TRACK_MIN;
    } else if (css_length_px(w, em, room, px) && *px >= 0) {
        *kind = TRACK_PX;
        if (*px > 100000)
            *px = 100000;
    } else {
        *kind = as_max ? TRACK_MAX : TRACK_MIN;
    }
}

/* A track list ("100px 1fr", "repeat(3, 1fr)", "repeat(auto-fill,
 * minmax(200px, 1fr))", "[name] auto"): its tracks, for `room` pixels
 * with `gap` between (auto repetitions: as many as fit; auto-fit: no more
 * than there are items). */
static void parse_tracks(const char *text, int em, int room, int gap, int items, struct tracks *out, int depth)
{
    for (const char *p = text; p && *p && depth < 4;) {
        while (is_space(*p))
            p++;
        if (!*p)
            break;
        if (*p == '[') {                        /* (line names) */
            const char *end = strchr(p, ']');
            p = end ? end + 1 : p + strlen(p);
            continue;
        }
        const char *start = p;
        int paren = 0;
        while (*p && (paren || !is_space(*p))) {
            paren += *p == '(' ? 1 : *p == ')' ? -1 : 0;
            p++;
        }
        char word[512];
        snprintf(word, sizeof(word), "%.*s", (int)((size_t)(p - start) < 511 ? p - start : 511), start);
        if (starts_word(word, "repeat(")) {
            char *inner = word + 7, *comma = strchr(inner, ',');
            size_t len = strlen(inner);
            if (!comma || len < 1)
                continue;
            *comma = 0;
            if (inner[len - 1] == ')')
                inner[len - 1] = 0;
            char *what = comma + 1;
            if (strrchr(what, ')') && strrchr(what, ')') == what + strlen(what) - 1 && !strchr(what, '('))
                *strrchr(what, ')') = 0;
            int count;
            bool fill = starts_word(inner, "auto-fill") || starts_word(inner, "auto-fit");
            if (fill) {                         /* as many as fit at their fixed sizes */
                struct tracks one = { 0 };
                parse_tracks(what, em, room, gap, items, &one, depth + 1);
                int each = 0;
                for (int i = 0; i < one.n; i++) {
                    int size = one.t[i].max_kind == TRACK_PX ? one.t[i].max_px
                             : one.t[i].min_kind == TRACK_PX ? one.t[i].min_px : 0;
                    each += size + gap;
                }
                count = each > 0 && room > 0 ? (room + gap) / each : 1;
                if (count < 1)
                    count = 1;
                if (starts_word(inner, "auto-fit") && items > 0 && count * (one.n ? one.n : 1) > items)
                    count = (items + (one.n ? one.n : 1) - 1) / (one.n ? one.n : 1);
                free(one.t);
            } else {
                count = atoi(inner);
            }
            if (count < 1)
                count = 1;
            for (int k = 0; k < count && out->n < GRID_TRACKS; k++)
                parse_tracks(what, em, room, gap, items, out, depth + 1);
            continue;
        }
        struct track t = { TRACK_MIN, TRACK_MAX, 0, -1, 0, 0 };
        if (starts_word(word, "minmax(")) {
            char *inner = word + 7, *comma = strchr(inner, ',');
            if (!comma)
                continue;
            *comma = 0;
            char *b = comma + 1;
            if (strrchr(b, ')'))
                *strrchr(b, ')') = 0;
            char a1[64], b1[64];
            snprintf(a1, sizeof(a1), "%s", inner);
            snprintf(b1, sizeof(b1), "%s", b);
            for (char *q = b1; *q == ' '; q++)
                memmove(q, q + 1, strlen(q));
            sizing(a1, em, room, false, &t.min_kind, &t.min_px, &t.fr);
            sizing(b1, em, room, true, &t.max_kind, &t.max_px, &t.fr);
        } else if (starts_word(word, "fit-content(")) {
            int cap;
            t.min_kind = TRACK_MIN, t.max_kind = TRACK_MAX;
            if (css_length_px(word + 12, em, room, &cap))
                t.max_px = cap;                 /* (a TRACK_MAX maximum, capped) */
        } else {
            sizing(word, em, room, false, &t.min_kind, &t.min_px, &t.fr);
            float fr;
            sizing(word, em, room, true, &t.max_kind, &t.max_px, &fr);
            t.fr = fr;
        }
        if (t.max_kind == TRACK_PX && t.min_kind == TRACK_PX && t.max_px < t.min_px)
            t.max_px = t.min_px;
        add_track(out, t);
    }
}

/* grid-template-areas: the named areas' rows and columns (zero-based,
 * end exclusive) into names/area; how many it has. */
#define GRID_AREAS 32
struct area_box {
    char name[32];
    int r0, c0, r1, c1;
};

static int parse_areas(const char *text, struct area_box *areas, int *rows, int *cols)
{
    int count = 0, r = 0;
    *rows = *cols = 0;
    for (const char *p = text; p && *p && r < 100;) {
        while (*p && *p != '"' && *p != '\'')
            p++;
        if (!*p)
            break;
        char q = *p++;
        int col = 0;
        while (*p && *p != q) {
            while (*p == ' ' || *p == '\t')
                p++;
            if (!*p || *p == q)
                break;
            char name[32];
            size_t n = 0;
            while (*p && *p != q && *p != ' ' && *p != '\t') {
                if (n + 1 < sizeof(name))
                    name[n++] = *p;
                p++;
            }
            name[n] = 0;
            if (strcmp(name, ".") && name[0] != '.') {
                int k = 0;
                while (k < count && strcmp(areas[k].name, name))
                    k++;
                if (k == count && count < GRID_AREAS) {
                    snprintf(areas[k].name, sizeof(areas[k].name), "%s", name);
                    areas[k].r0 = r, areas[k].c0 = col, areas[k].r1 = r + 1, areas[k].c1 = col + 1;
                    count++;
                } else if (k < count) {
                    if (r + 1 > areas[k].r1) areas[k].r1 = r + 1;
                    if (col + 1 > areas[k].c1) areas[k].c1 = col + 1;
                    if (col < areas[k].c0) areas[k].c0 = col;
                }
            }
            col++;
        }
        if (*p)
            p++;
        if (col > *cols)
            *cols = col;
        r++;
    }
    *rows = r;
    return count;
}

struct grid_item {
    struct node *n;
    int r0, c0, rs, cs;                 /* row and column, their spans */
    int narrow, wide, height;
};

/* A line number (1 the first, negative from the end of n tracks) as a
 * zero-based position, or -1 for auto. */
static int grid_line(int line, int n)
{
    if (line > 0)
        return line - 1;
    if (line < 0)
        return n + 1 + line >= 0 ? n + 1 + line : 0;
    return -1;
}

/* The tracks' sizes in `room` (CSS Grid's track sizing, its simple core):
 * fixed ones their size, content ones between their items' narrowest and
 * widest, then the room left to the flexible (fr) ones, or else to the
 * auto ones when nothing is flexible. */
static void size_tracks(struct track *t, int n, int room, int gap, const int *lo, const int *hi, bool stretch_auto)
{
    long used = n > 1 ? (long)gap * (n - 1) : 0;
    for (int k = 0; k < n; k++) {
        int base = t[k].min_kind == TRACK_PX ? t[k].min_px : t[k].min_kind == TRACK_MAX ? hi[k] : lo[k];
        int limit = t[k].max_kind == TRACK_PX ? t[k].max_px : t[k].max_kind == TRACK_MIN ? lo[k]
                  : t[k].max_kind == TRACK_MAX ? (t[k].max_px >= 0 && t[k].max_px < hi[k] ? t[k].max_px : hi[k]) : base;
        if (limit < base)
            limit = base;
        t[k].size = base;
        t[k].max_px = t[k].max_kind == TRACK_FR ? -1 : limit;
        used += base;
    }
    long free_space = room - used;
    /* the content tracks toward their limits, equally */
    for (int round = 0; round < 32 && free_space > 0; round++) {
        int growing = 0;
        for (int k = 0; k < n; k++)
            if (t[k].max_kind != TRACK_FR && t[k].size < t[k].max_px)
                growing++;
        if (!growing)
            break;
        long share = free_space / growing;
        if (share == 0)
            share = 1;
        for (int k = 0; k < n && free_space > 0; k++)
            if (t[k].max_kind != TRACK_FR && t[k].size < t[k].max_px) {
                long add = t[k].max_px - t[k].size < share ? t[k].max_px - t[k].size : share;
                t[k].size += (int)add;
                free_space -= add;
            }
    }
    /* the flexible ones share what is left (none below its base) */
    float sum_fr = 0;
    long flex_room = free_space;
    for (int k = 0; k < n; k++)
        if (t[k].max_kind == TRACK_FR) {
            sum_fr += t[k].fr;
            flex_room += t[k].size;
        }
    if (sum_fr > 0) {
        bool *fixed = calloc((size_t)n, sizeof(bool));
        for (int round = 0; fixed && round < 8; round++) {
            float unit = sum_fr > 0 ? (float)flex_room / (sum_fr < 1 ? 1 : sum_fr) : 0;
            bool again = false;
            for (int k = 0; k < n; k++)
                if (t[k].max_kind == TRACK_FR && !fixed[k] && unit * t[k].fr < (float)t[k].size) {
                    fixed[k] = true;        /* (its content needs more than its share) */
                    sum_fr -= t[k].fr;
                    flex_room -= t[k].size;
                    again = true;
                }
            if (!again) {
                float sum = 0;
                for (int k = 0; k < n; k++)
                    if (t[k].max_kind == TRACK_FR && !fixed[k]) {
                        int before = (int)(sum + 0.5f);
                        sum += unit * t[k].fr;
                        t[k].size = (int)(sum + 0.5f) - before;
                    }
                break;
            }
        }
        free(fixed);
    } else if (stretch_auto && free_space > 0) {   /* (nothing flexible: the auto tracks take it) */
        int autos = 0;
        for (int k = 0; k < n; k++)
            autos += t[k].max_kind == TRACK_MAX;
        for (int k = 0, given = 0; k < n && autos; k++)
            if (t[k].max_kind == TRACK_MAX) {
                int add = (int)(free_space * (given + 1) / autos - free_space * given / autos);
                t[k].size += add;
                given++;
            }
    }
}

/* The grid items of e (CSS Grid Layout: placed by their lines, areas or
 * in order, the columns sized to the room, the rows to their items). */
static void grid_contents(struct ctx *c, struct node *e, int definite)
{
    const struct style *s = e->style;
    int W = c->right - c->left, x0 = c->left, y0 = c->y + c->margin;
    c->margin = 0;
    int em = s->font_size;
    int cgap = s->column_gap, rgap = s->row_gap;
    /* The items. */
    int count = 0;
    for (struct node *k = e->first; k; k = contents_next(k, e))
        count++;
    struct grid_item *it = calloc((size_t)(count ? count : 1), sizeof(*it));
    if (!it)
        return;
    int n = 0;
    for (struct node *k = e->first; k && n < count; k = contents_next(k, e)) {
        if (k->kind == NODE_TEXT) {
            bool blank = true;
            for (const char *p = k->text; p && *p && blank; p++)
                blank = is_space(*p);
            if (!blank)
                it[n++].n = k;
            continue;
        }
        const struct style *ks = k->style;
        if (!ks || ks->display == DISPLAY_NONE || ks->display == DISPLAY_CONTENTS || !strcmp(k->tag, "br"))
            continue;
        if (ks->position == POSITION_ABSOLUTE || ks->position == POSITION_FIXED) {
            c->line.x = x0, c->y = y0;
            defer(c, k);
            continue;
        }
        it[n++].n = k;
    }
    for (int i = 1; i < n; i++)                 /* by `order` (stable) */
        for (int k = i; k > 0; k--) {
            int a = it[k - 1].n->style ? it[k - 1].n->style->order : 0, b = it[k].n->style ? it[k].n->style->order : 0;
            if (a <= b)
                break;
            struct grid_item t = it[k];
            it[k] = it[k - 1], it[k - 1] = t;
        }
    /* The explicit grid. */
    struct tracks cols = { 0 }, rows = { 0 };
    parse_tracks(s->grid_cols, em, W, cgap, n, &cols, 0);
    parse_tracks(s->grid_rows, em, definite, rgap, n, &rows, 0);
    struct area_box areas[GRID_AREAS];
    int area_rows = 0, area_cols = 0;
    int nareas = s->grid_areas ? parse_areas(s->grid_areas, areas, &area_rows, &area_cols) : 0;
    int ncols = cols.n > area_cols ? cols.n : area_cols, nrows = rows.n > area_rows ? rows.n : area_rows;
    /* Lines given, areas named: placed first. */
    for (int i = 0; i < n; i++) {
        struct grid_item *g = &it[i];
        const struct style *gs = g->n->kind == NODE_ELEMENT ? g->n->style : 0;
        g->r0 = g->c0 = -1, g->rs = g->cs = 1;
        if (!gs)
            continue;
        if (gs->grid_area) {
            for (int k = 0; k < nareas; k++)
                if (!strcmp(areas[k].name, gs->grid_area)) {
                    g->r0 = areas[k].r0, g->c0 = areas[k].c0;
                    g->rs = areas[k].r1 - areas[k].r0, g->cs = areas[k].c1 - areas[k].c0;
                }
            continue;
        }
        int axis_n[2] = { ncols, nrows };
        for (int axis = 0; axis < 2; axis++) {
            int start = axis ? gs->row_start : gs->col_start, end = axis ? gs->row_end : gs->col_end;
            int span = axis ? gs->row_span : gs->col_span;
            if (span < 1)
                span = 1;
            int a = grid_line(start, axis_n[axis]), b = grid_line(end, axis_n[axis]);
            if (a >= 0 && b >= 0) {
                if (b < a) { int t = a; a = b; b = t; }
                span = b > a ? b - a : 1;
            } else if (a < 0 && b >= 0) {
                a = b - span >= 0 ? b - span : 0;
            }
            if (axis)
                g->r0 = a, g->rs = span;
            else
                g->c0 = a, g->cs = span;
        }
    }
    for (int i = 0; i < n; i++) {               /* (the grid grows to hold every item given a place) */
        if (it[i].c0 >= 0 && it[i].c0 + it[i].cs > ncols)
            ncols = it[i].c0 + it[i].cs;
        if (it[i].r0 >= 0 && it[i].r0 + it[i].rs > nrows)
            nrows = it[i].r0 + it[i].rs;
    }
    bool by_col = s->grid_by_column;
    if (!by_col && ncols == 0)
        ncols = 1;
    if (by_col && nrows == 0)
        nrows = 1;
    if (ncols > GRID_TRACKS)
        ncols = GRID_TRACKS;
    /* The others in order: by rows (or columns), the first free place. */
    int minor_n = by_col ? nrows : ncols;
    int major_room = GRID_ROWS;
    unsigned char *taken = calloc((size_t)major_room * (size_t)(minor_n ? minor_n : 1), 1);
    if (!taken) {
        free(it), free(cols.t), free(rows.t);
        return;
    }
    #define TAKEN(mj, mn) taken[(size_t)(mj) * (size_t)minor_n + (size_t)(mn)]
    int used_major = by_col ? ncols : nrows;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n; i++) {
            struct grid_item *g = &it[i];
            int *mj = by_col ? &g->c0 : &g->r0, *mn = by_col ? &g->r0 : &g->c0;
            int mjs = by_col ? g->cs : g->rs, mns = by_col ? g->rs : g->cs;
            if (mns > minor_n)
                mns = minor_n, *(by_col ? &g->rs : &g->cs) = mns;
            bool placed = *mj >= 0 && *mn >= 0;
            if ((pass == 0) != placed)
                continue;
            if (!placed) {
                int a = *mj >= 0 ? *mj : 0, b_fixed = *mn;
                for (;; a++) {
                    if (a + mjs > major_room) {
                        a = major_room - mjs;
                        break;
                    }
                    bool fits = false;
                    for (int b = b_fixed >= 0 ? b_fixed : 0; b + mns <= minor_n; b++) {
                        bool free_here = true;
                        for (int x = 0; x < mjs && free_here; x++)
                            for (int y = 0; y < mns && free_here; y++)
                                free_here = !TAKEN(a + x, b + y);
                        if (free_here) {
                            *mn = b, fits = true;
                            break;
                        }
                        if (b_fixed >= 0)
                            break;
                    }
                    if (fits || *mj >= 0)
                        break;
                }
                *mj = a;
                if (*mn < 0)
                    *mn = 0;
            }
            for (int x = 0; x < mjs; x++)
                for (int y = 0; y < mns; y++)
                    if (*mj + x < major_room && *mn + y < minor_n)
                        TAKEN(*mj + x, *mn + y) = 1;
            if (*mj + mjs > used_major)
                used_major = *mj + mjs;
        }
    #undef TAKEN
    free(taken);
    if (by_col)
        ncols = used_major;
    else
        nrows = used_major;
    if (nrows > GRID_ROWS)
        nrows = GRID_ROWS;
    /* Implicit tracks: grid-auto-columns / -rows (cycled), else auto. */
    struct tracks auto_c = { 0 }, auto_r = { 0 };
    parse_tracks(s->grid_auto_cols, em, W, cgap, n, &auto_c, 0);
    parse_tracks(s->grid_auto_rows, em, definite, rgap, n, &auto_r, 0);
    struct track plain = { TRACK_MIN, TRACK_MAX, 0, -1, 0, 0 };
    while (cols.n < ncols && cols.n < GRID_TRACKS)
        add_track(&cols, auto_c.n ? auto_c.t[cols.n % auto_c.n] : plain);
    while (rows.n < nrows && rows.n < GRID_ROWS) {
        struct track t = auto_r.n ? auto_r.t[rows.n % auto_r.n] : plain;
        if (rows.n == rows.room) {
            int room = rows.room ? 2 * rows.room : 16;
            struct track *g = realloc(rows.t, sizeof(*g) * (size_t)room);
            if (!g)
                break;
            rows.t = g, rows.room = room;
        }
        rows.t[rows.n++] = t;
    }
    free(auto_c.t), free(auto_r.t);
    ncols = cols.n, nrows = rows.n;
    if (!ncols || !nrows) {
        free(it), free(cols.t), free(rows.t);
        c->y = y0;
        start_line(c);
        return;
    }
    /* The columns: by their items' narrowest and widest (one column's). */
    int *lo = calloc((size_t)(ncols > nrows ? ncols : nrows), sizeof(int));
    int *hi = calloc((size_t)(ncols > nrows ? ncols : nrows), sizeof(int));
    if (!lo || !hi) {
        free(lo), free(hi), free(it), free(cols.t), free(rows.t);
        return;
    }
    for (int i = 0; i < n; i++) {
        struct grid_item *g = &it[i];
        if (g->c0 < 0 || g->c0 >= ncols)
            continue;
        measure(c, g->n, 1, false, 1, &g->narrow);
        measure(c, g->n, W > 1 ? W : 1, false, 0, &g->wide);
        if (g->cs == 1) {
            if (g->narrow > lo[g->c0]) lo[g->c0] = g->narrow;
            if (g->wide > hi[g->c0]) hi[g->c0] = g->wide;
        }
    }
    for (int k = 0; k < ncols; k++)
        if (hi[k] < lo[k])
            hi[k] = lo[k];
    bool stretch_cols = s->justify == FLEX_NORMAL || s->justify == FLEX_STRETCH;
    size_tracks(cols.t, ncols, W, cgap, lo, hi, stretch_cols);
    /* The rows: by their items' heights at their widths. */
    memset(lo, 0, sizeof(int) * (size_t)nrows);
    for (int i = 0; i < n; i++) {
        struct grid_item *g = &it[i];
        if (g->c0 < 0 || g->c0 >= ncols || g->r0 < 0 || g->r0 >= nrows)
            continue;
        int w = 0;
        for (int k = g->c0; k < g->c0 + g->cs && k < ncols; k++)
            w += cols.t[k].size + (k > g->c0 ? cgap : 0);
        g->height = measure(c, g->n, w > 0 ? w : 1, true, 2, 0);
        if (g->rs == 1 && g->height > lo[g->r0])
            lo[g->r0] = g->height;
    }
    for (int i = 0; i < n; i++) {               /* (taller items over several rows: the last grows) */
        struct grid_item *g = &it[i];
        if (g->rs <= 1 || g->r0 < 0 || g->r0 >= nrows)
            continue;
        int last = g->r0 + g->rs - 1 < nrows ? g->r0 + g->rs - 1 : nrows - 1;
        int have = rgap * (last - g->r0);
        for (int r = g->r0; r <= last; r++)
            have += lo[r];
        if (g->height > have)
            lo[last] += g->height - have;
    }
    for (int r = 0; r < nrows; r++) {
        hi[r] = lo[r];
        if (definite < 0 && rows.t[r].max_kind == TRACK_FR)
            rows.t[r].max_kind = TRACK_MAX;      /* (no height to share: as tall as its items) */
    }
    size_tracks(rows.t, nrows, definite >= 0 ? definite : 0, rgap, lo, hi,
                definite >= 0 && (s->align_content == FLEX_NORMAL || s->align_content == FLEX_STRETCH));
    /* Where the tracks begin (justify-content and align-content). */
    int *cx = calloc((size_t)ncols + 1, sizeof(int)), *ry = calloc((size_t)nrows + 1, sizeof(int));
    if (!cx || !ry) {
        free(cx), free(ry), free(lo), free(hi), free(it), free(cols.t), free(rows.t);
        return;
    }
    for (int axis = 0; axis < 2; axis++) {
        struct track *t = axis ? rows.t : cols.t;
        int count_t = axis ? nrows : ncols, gap = axis ? rgap : cgap, *at = axis ? ry : cx;
        int room = axis ? definite : W;
        long total = (long)gap * (count_t - 1);
        for (int k = 0; k < count_t; k++)
            total += t[k].size;
        long extra = room > total ? room - total : 0;
        enum flex_align a = axis ? s->align_content : s->justify;
        long start = 0, between = gap;
        if (extra > 0) {
            if (a == FLEX_CENTER) start = extra / 2;
            else if (a == FLEX_END) start = extra;
            else if (a == FLEX_BETWEEN && count_t > 1) between += extra / (count_t - 1);
            else if (a == FLEX_AROUND) start = extra / (2 * count_t), between += extra / count_t;
            else if (a == FLEX_EVENLY) start = extra / (count_t + 1), between += extra / (count_t + 1);
        }
        long pos = start;
        for (int k = 0; k < count_t; k++) {
            at[k] = (int)pos;
            pos += t[k].size + between;
        }
        at[count_t] = (int)(pos - between);
    }
    /* Each item in its area, by justify-self and align-self. */
    int bottom = y0 + ry[nrows];
    int first_line = c->line_number, last_line = first_line;
    for (int r = 0; r < nrows; r++) {
        for (int i = 0; i < n; i++) {
            struct grid_item *g = &it[i];
            if (g->r0 != r || g->c0 < 0 || g->c0 >= ncols)
                continue;
            int c1 = g->c0 + g->cs < ncols ? g->c0 + g->cs : ncols, r1 = g->r0 + g->rs < nrows ? g->r0 + g->rs : nrows;
            int ax = x0 + cx[g->c0];             /* (its area: from its first track to its last's end) */
            int aw = (c1 > g->c0 ? cx[c1 - 1] + cols.t[c1 - 1].size : 0) - cx[g->c0];
            int ay = y0 + ry[g->r0], ah = (r1 > g->r0 ? ry[r1 - 1] + rows.t[r1 - 1].size : 0) - ry[g->r0];
            const struct style *gs = g->n->kind == NODE_ELEMENT ? g->n->style : 0;
            enum flex_align js = gs && gs->justify_self != FLEX_AUTO && gs->justify_self != FLEX_NORMAL
                               ? gs->justify_self : s->justify_items;
            enum flex_align as = gs && gs->align_self != FLEX_AUTO ? gs->align_self : s->align_items;
            int w = aw, x = ax;
            bool own_width = gs && (gs->width >= 0 || gs->width < -1);
            if ((js != FLEX_NORMAL && js != FLEX_STRETCH) || own_width) {
                int ow = own_width ? resolved_width(gs->width, gs->width_add, aw) : -1;
                if (ow >= 0 && gs)
                    ow = gs->border_box ? ow : ow + gs->border[1] + gs->border[3] +
                         (gs->padding[1] > 0 ? gs->padding[1] : 0) + (gs->padding[3] > 0 ? gs->padding[3] : 0);
                w = ow >= 0 ? ow : g->wide < aw ? g->wide : aw;
                if (w > aw && !own_width)
                    w = aw;
                x = js == FLEX_CENTER ? ax + (aw - w) / 2 : js == FLEX_END ? ax + aw - w : ax;
            }
            int h = g->height, y = ay, min_h = 0;
            if (as == FLEX_NORMAL || as == FLEX_STRETCH) {
                if (!gs || gs->height <= 0)
                    min_h = ah;
            } else if (as == FLEX_CENTER) {
                y = ay + (ah - h) / 2;
            } else if (as == FLEX_END) {
                y = ay + ah - h;
            }
            if (w != aw)                        /* (narrower: its height again) */
                h = measure(c, g->n, w > 0 ? w : 1, true, 2, 0);
            int saved = c->link;
            if (g->n->kind == NODE_ELEMENT && !strcmp(g->n->tag, "a") && node_attr(g->n, "href") && !c->measuring)
                c->link = add_link(c->page, node_attr(g->n, "href"), c->base);
            c->line_number = first_line;
            c->exact = true;
            c->min_h = min_h;
            int b = lay_box(c, g->n, x, w > 0 ? w : 1, y, true);
            c->exact = false, c->min_h = 0;
            c->link = saved;
            if (b > bottom)
                bottom = b;
            if (c->line_number > last_line)
                last_line = c->line_number;
        }
        first_line = last_line;
    }
    c->line_number = last_line;
    c->y = bottom;
    start_line(c);
    free(cx), free(ry), free(lo), free(hi), free(it), free(cols.t), free(rows.t);
}

/* A table row: its cells side by side, equal columns. */
static void table_row(struct ctx *c, struct node *row)
{
    int cells = 0;
    for (struct node *n = row->first; n; n = n->next)
        if (n->kind == NODE_ELEMENT && n->style && n->style->display != DISPLAY_NONE)
            cells++;
    if (!cells)
        return;
    int left = c->left, right = c->right, top = c->y;
    int width = (right - left) / cells, bottom = top;
    int first_line = c->line_number, last_line = first_line;
    int k = 0;
    for (struct node *n = row->first; n; n = n->next) {
        if (n->kind != NODE_ELEMENT || !n->style || n->style->display == DISPLAY_NONE)
            continue;
        c->left = left + k * width;
        c->right = k == cells - 1 ? right : left + (k + 1) * width;
        c->y = top;
        c->margin = 0;
        c->line_number = first_line;            /* (the cells' lines side by side) */
        start_line(c);
        block(c, n);
        end_line(c, false, 0);
        if (c->y > bottom)
            bottom = c->y;
        if (c->line_number > last_line)
            last_line = c->line_number;
        k++;
    }
    c->line_number = last_line;
    c->left = left, c->right = right, c->y = bottom;
    start_line(c);
}

static void block_box(struct ctx *c, struct node *e);

/* (27q) What block_box adds is e's (item.owner), wherever it is laid out
 * from (flex items, table cells, positioned boxes). */
static void block(struct ctx *c, struct node *e)
{
    struct node *outer = c->owner;
    if (e->tag[0] != ':')
        c->owner = e;
    block_box(c, e);
    c->owner = outer;
}

static void block_box(struct ctx *c, struct node *e)
{
    const struct style *s = e->style;
    bool contain = c->contain || s->holds_floats || s->flex || s->grid;
    bool exact = c->exact;                      /* (27g: a flex item, its box given) */
    int min_h = c->min_h, push = c->push;
    bool has_given = c->has_given;              /* (27m) a height given by a positioned box's container */
    int given_h = c->given_h, saved_cb_h = c->cb_h;
    c->contain = false, c->exact = false, c->min_h = 0, c->push = 0, c->has_given = false;
    end_line(c, false, 0);                      /* what was inline before it */
    if (c->depth >= 200)
        return;
    c->depth++;
    if (s->clear) {                             /* (27d) below the floats of those sides */
        for (int below; (below = next_float_bottom(c, c->y + c->margin, s->clear)) >= 0;) {
            c->y = below;
            c->margin = 0;
        }
    }
    int floats_before = c->nfloats, first_item = c->page->count, saved_band = c->band;
    if (s->position == POSITION_RELATIVE)
        c->band = positioned_band(s);
    int saved_left = c->left, saved_right = c->right;
    enum align saved_align = c->align;
    int room = c->right - c->left;
    int m[4], pad[4];
    for (int i = 0; i < 4; i++) {
        m[i] = s->margin[i] == LENGTH_AUTO ? 0 : s->margin[i];
        pad[i] = s->padding[i] < 0 ? 0 : s->padding[i];
    }
    const int *bd = s->border;                  /* (27h) top, right, bottom, left */
    int width = exact ? -1 : resolved_width(s->width, s->width_add, room);
    int max = exact ? -1 : resolved_width(s->max_width, s->max_width_add, room);
    int min = exact ? -1 : resolved_width(s->min_width, s->min_width_add, room);
    int around = bd[1] + bd[3] + pad[1] + pad[3];
    if (s->border_box) {                        /* (27g) the sizes include padding and border */
        if (width >= 0)
            width = width > around ? width - around : 0;
        if (max >= 0)
            max = max > around ? max - around : 0;
        if (min >= 0)
            min = min > around ? min - around : 0;
    }
    if (exact)
        m[1] = m[3] = 0;
    int outer = room - m[1] - m[3];
    int content = width >= 0 ? width : outer - around;
    if (max >= 0 && content > max)
        content = max;
    if (min >= 0 && content < min)
        content = min;
    struct table *table = 0;
    if (s->table && c->depth < 100) {           /* (27i) as wide as its columns need */
        table = table_build(c, e, content, width >= 0 ? content : -1);
        if (table && width < 0)
            content = table->need;
    }
    if (content < (exact ? 0 : 8))
        content = exact ? 0 : 8;
    int x = c->left + m[3];
    if (exact)
        ;                                       /* (its place is the flex container's) */
    else if (s->margin[1] == LENGTH_AUTO && s->margin[3] == LENGTH_AUTO)      /* centred */
        x = c->left + (room - content - around) / 2;
    else if (s->margin[3] == LENGTH_AUTO)
        x = c->right - m[1] - content - around;
    /* The top margin collapses with the one waiting above. */
    int top_margin = m[0] > c->margin ? m[0] : c->margin;
    c->y += top_margin;
    c->margin = 0;
    int top = c->y;
    add_anchor(c, e, top);
    /* Its decorations' places, under what follows (27h: a shadow, the
     * background colour, the background picture or gradient). */
    uint32_t under = c->under;
    bool root = !strcmp(e->tag, "body") || !strcmp(e->tag, "html");
    int shadow = -1, background = -1, picture_at = -1;
    if (s->shadow && !s->hidden && !root) {
        struct item *it = add_item(c, ITEM_FILL);
        if (it)
            it->decoration = true, shadow = c->page->count - 1;
    }
    if (s->background != COLOR_NONE && !root) {
        struct item *bg = add_item(c, ITEM_FILL);
        if (bg) {
            bg->decoration = true;
            background = c->page->count - 1;
            c->under = mix(s->background, s->background_alpha, under);
        }
    }
    if (s->background_image && !root && !s->hidden) {
        bool gradient = strstr(s->background_image, "gradient(") != 0;
        if (gradient || e->background) {
            struct item *it = add_item(c, gradient ? ITEM_FILL : ITEM_PICTURE);
            if (it)
                it->decoration = true, picture_at = c->page->count - 1;
        }
    }
    c->y += bd[0] + pad[0] + push;
    c->left = x + bd[3] + pad[3];
    c->right = c->left + content;
    c->align = s->align;
    start_line(c);
    if (s->display == DISPLAY_LIST_ITEM)
        list_marker(c, e);
    int height = s->height;                     /* (content box; -1: auto) */
    int vertical = pad[0] + pad[2] + bd[0] + bd[2];
    if (height <= 0 && s->height_pct > 0 && saved_cb_h >= 0)   /* (27m) a percentage of the known */
        height = saved_cb_h * s->height_pct / 100 + s->height_add;
    if (height > 0 && s->border_box)
        height = height > vertical ? height - vertical : 0;
    if (has_given && height <= 0)               /* (27m: top and bottom both given) */
        height = given_h > vertical ? given_h - vertical : 0;
    bool fixed_h = height > 0;
    c->cb_h = fixed_h ? height : min_h > 0 ? min_h - vertical : -1;  /* (what its children's % are of) */
    int content_from = c->page->count;          /* (what is inside: clipped by overflow) */
    if (s->display == DISPLAY_TABLE_ROW) {
        table_row(c, e);
    } else if (!strcmp(e->tag, "hr")) {
        /* (only its border) */
    } else if (table) {
        table_lay(c, e, table);
    } else if (s->grid) {                       /* (27n) */
        grid_contents(c, e, fixed_h ? height : min_h > 0 ? min_h - vertical : -1);
    } else if (s->flex) {                       /* (27g) its height is definite when given or
                                                   stretched by its own container */
        int definite = fixed_h ? height : min_h > 0 ? min_h - vertical : -1;
        flex_contents(c, e, definite);
    } else {
        contents(c, e);
        end_line(c, false, 0);
    }
    if (s->display == DISPLAY_LIST_ITEM && c->marker >= 0) {   /* (an item without a line) */
        struct item *mk = &c->page->items[c->marker];
        mk->y = c->y;
        c->y += mk->h;
        c->marker = -1;
    }
    if (c->margin) {                            /* a child's bottom margin stays inside */
        if (pad[2] || bd[2])
            c->y += c->margin;
        c->margin = pad[2] || bd[2] ? 0 : c->margin;
    }
    if (contain && !(s->clips && fixed_h))      /* (27d) its floats inside it */
        for (int i = floats_before; i < c->nfloats; i++)
            if (c->floats[i].y + c->floats[i].h > c->y + c->margin) {
                c->y = c->floats[i].y + c->floats[i].h;
                c->margin = 0;
            }
    int inner_bottom = c->y + pad[2];
    if (fixed_h && (inner_bottom < top + bd[0] + pad[0] + height + pad[2] || s->clips))
        inner_bottom = top + bd[0] + pad[0] + height + pad[2];  /* (clipped: just that high) */
    /* (27m) min-height (and a clipped box's max-height), of the content box
     * unless box-sizing says the border box */
    int lowest = s->min_height_pct && saved_cb_h >= 0 ? saved_cb_h * s->min_height_pct / 100 + s->min_height
               : s->min_height_pct ? 0 : s->min_height;
    int highest = s->max_height_pct && saved_cb_h >= 0 ? saved_cb_h * s->max_height_pct / 100 + s->max_height
                : s->max_height_pct ? -1 : s->max_height;
    if (s->border_box) {
        lowest -= vertical;
        if (highest >= 0)
            highest = highest > vertical ? highest - vertical : 0;
    }
    if (lowest > 0 && inner_bottom < top + bd[0] + pad[0] + lowest + pad[2])
        inner_bottom = top + bd[0] + pad[0] + lowest + pad[2];
    if (highest >= 0 && s->clips && inner_bottom > top + bd[0] + pad[0] + highest + pad[2])
        inner_bottom = top + bd[0] + pad[0] + highest + pad[2];
    int bottom = inner_bottom + bd[2];
    if (min_h > 0 && bottom - top < min_h)      /* (27g: stretched in its flex line) */
        bottom = top + min_h;
    int box_w = content + pad[1] + pad[3] + bd[1] + bd[3];
    c->under = under;
    decorate(c, e, x, top, box_w, bottom - top, shadow, background, picture_at);
    if (s->clips)                               /* (27h) its contents seen only inside it */
        clip_items(c, content_from, x + bd[3], top + bd[0], x + box_w - bd[1], bottom - bd[2]);
    if (s->opacity < 255)
        fade_items(c, first_item, s->opacity, under);
    c->y = bottom;
    if (c->margin < m[2])
        c->margin = m[2];
    e->box[0] = x, e->box[1] = top, e->box[2] = box_w, e->box[3] = bottom - top;
    if (s->position == POSITION_RELATIVE)
        add_shift(c, s, first_item, saved_right - saved_left);
    add_translate(c, s, first_item, box_w, bottom - top);
    c->cb_h = saved_cb_h;
    c->band = saved_band;
    c->left = saved_left, c->right = saved_right;
    c->align = saved_align;
    start_line(c);
    c->depth--;
    table_free(table);
}

void layout(struct node *root, int width, int view_h, const char *base, struct page *out)
{
    memset(out, 0, sizeof(*out));
    out->width = width;
    out->background = 0xFFFFFF;
    struct node *body = node_find(root, "body");
    const struct style *bs = body ? body->style : 0;
    const struct style *hs = root->style;
    if (hs && hs->background != COLOR_NONE)
        out->background = hs->background;
    else if (bs && bs->background != COLOR_NONE)
        out->background = bs->background;
    struct node *title = node_find(root, "title");
    if (title && title->first && title->first->kind == NODE_TEXT) {
        size_t o = 0;
        bool space = false;
        for (const char *p = title->first->text; *p && o + 2 < sizeof(out->title); p++) {
            if (is_space(*p)) {
                space = o > 0;
                continue;
            }
            if (space)
                out->title[o++] = ' ';
            space = false;
            out->title[o++] = *p;
        }
        out->title[o] = 0;
    }
    for (struct node *n = root; n; n = node_next(n, root)) {
        memset(n->box, 0, sizeof(n->box));      /* (boxes of an earlier layout) */
        memset(n->memo, 0, sizeof(n->memo));    /* (and what was measured for it) */
    }
    struct ctx *c = calloc(1, sizeof(*c));
    if (!c)
        return;
    c->page = out, c->base = base, c->left = 0, c->right = width, c->link = -1, c->marker = -1;
    c->view_h = view_h;
    c->under = out->background;
    c->cb_h = view_h;                           /* (27m: the page's own box: the window's height) */
    start_line(c);
    if (body && body->style)
        block(c, body);
    end_line(c, false, 0);
    int bottom = c->y + c->margin;
    for (int i = 0; i < c->nfloats; i++)
        if (c->floats[i].y + c->floats[i].h > bottom)
            bottom = c->floats[i].y + c->floats[i].h;
    for (int i = 0; i < c->nlater; i++) {      /* (27d) absolute and fixed boxes; theirs too */
        struct later d = c->later[i];           /* (a copy: laying it out may grow the list) */
        int b = lay_positioned(c, &d);
        if (b > bottom)
            bottom = b;
    }
    for (int i = 0; i < c->nshifts; i++)        /* position: relative */
        for (int k = c->shifts[i].from; k < c->shifts[i].to && k < out->count; k++) {
            struct item *it = &out->items[k];
            it->x += c->shifts[i].dx, it->y += c->shifts[i].dy;
            it->tile_x += c->shifts[i].dx, it->tile_y += c->shifts[i].dy;
            if (it->clipped)
                it->clip[0] += c->shifts[i].dx, it->clip[2] += c->shifts[i].dx,
                it->clip[1] += c->shifts[i].dy, it->clip[3] += c->shifts[i].dy;
        }
    out->height = bottom;
    free(c->shifts);
    free(c->later);
    free(c);
    /* The painting order: a stable sort by layer (in the flow: backgrounds
     * and borders, then content; floats between them). */
    out->order = malloc(sizeof(int) * (size_t)(out->count ? out->count : 1));
    int *keys = malloc(sizeof(int) * (size_t)(out->count ? out->count : 1));
    int *spare = malloc(sizeof(int) * (size_t)(out->count ? out->count : 1));
    if (!out->order || !keys || !spare) {
        free(out->order);
        out->order = 0;                         /* (then the items' own order) */
    } else {
        for (int i = 0; i < out->count; i++) {
            const struct item *it = &out->items[i];
            keys[i] = it->band ? it->band : it->decoration ? 100000 : 300000;
            out->order[i] = i;
        }
        for (int run = 1; run < out->count; run *= 2) {        /* (merge sort: stable) */
            for (int lo = 0; lo < out->count; lo += 2 * run) {
                int mid = lo + run < out->count ? lo + run : out->count;
                int hi = lo + 2 * run < out->count ? lo + 2 * run : out->count;
                int a = lo, b = mid, o = lo;
                while (a < mid || b < hi)
                    spare[o++] = b >= hi || (a < mid && keys[out->order[a]] <= keys[out->order[b]])
                               ? out->order[a++] : out->order[b++];
            }
            memcpy(out->order, spare, sizeof(int) * (size_t)out->count);
        }
    }
    free(keys);
    free(spare);
}

int item_top(const struct item *it, int scroll)
{
    return it->fixed ? it->y : it->y - scroll;
}

void page_free(struct page *p)
{
    for (int i = 0; i < p->count; i++) {
        free(p->items[i].text);
        free(p->items[i].gradient);
    }
    free(p->items);
    for (int i = 0; i < p->link_count; i++)
        free(p->links[i].href);
    free(p->links);
    for (int i = 0; i < p->anchor_count; i++)
        free(p->anchors[i].name);
    free(p->anchors);
    free(p->order);
    memset(p, 0, sizeof(*p));
}

int page_anchor(const struct page *p, const char *name)
{
    for (int pass = 0; pass < 2; pass++)        /* as written, then %-decoded ("#a%20b") */
        for (int i = 0; i < p->anchor_count; i++) {
            char decoded[256];
            const char *want = name;
            if (pass) {
                size_t o = 0;
                for (const char *s = name; *s && o + 1 < sizeof(decoded); s++) {
                    int hi = s[0] == '%' ? hex_digit((unsigned char)s[1]) : -1;
                    int lo = hi >= 0 ? hex_digit((unsigned char)s[2]) : -1;
                    if (lo >= 0) {
                        decoded[o++] = (char)(hi * 16 + lo);
                        s += 2;
                    } else
                        decoded[o++] = *s;
                }
                decoded[o] = 0;
                want = decoded;
            }
            if (!strcmp(p->anchors[i].name, want))
                return p->anchors[i].y;
        }
    return -1;
}

int page_link_at(const struct page *p, int x, int y, int scroll)
{
    for (int k = p->count - 1; k >= 0; k--) {   /* (the topmost first: painting order) */
        const struct item *it = &p->items[p->order ? p->order[k] : k];
        int top = item_top(it, scroll) + scroll;
        if (it->kind != ITEM_BOX && x >= it->x && x < it->x + it->w && y >= top && y < top + it->h &&
            (it->kind != ITEM_FILL || !it->decoration || it->band))
            return it->link;                    /* (a box painted over a link hides it) */
    }
    return -1;
}

/* (27q) The topmost item there with an element names it; a block whose
 * box holds the point and lies inside that element (or anywhere, with no
 * item there) is deeper: the deepest such one. */
static struct node *deepest_box(struct node *n, int x, int y, int depth)
{
    struct node *found = 0;
    for (struct node *k = n->first; k && depth < 200; k = k->next) {
        if (k->kind != NODE_ELEMENT || k->tag[0] == ':' || !k->style || k->style->display == DISPLAY_NONE)
            continue;
        struct node *in = deepest_box(k, x, y, depth + 1);
        if (in)
            found = in;                         /* (a later sibling is painted over an earlier one) */
        else if ((k->box[2] || k->box[3]) && x >= k->box[0] && x < k->box[0] + k->box[2] && y >= k->box[1] &&
                 y < k->box[1] + k->box[3])
            found = k;
    }
    return found;
}

struct node *page_node_at(const struct page *p, struct node *root, int x, int y, int scroll)
{
    struct node *top = 0;
    for (int k = p->count - 1; k >= 0 && !top; k--) {
        const struct item *it = &p->items[p->order ? p->order[k] : k];
        int t = item_top(it, scroll) + scroll;
        if (it->owner && !it->hidden && x >= it->x && x < it->x + it->w && y >= t && y < t + it->h &&
            (!it->clipped || (x >= it->clip[0] && x < it->clip[2] && y >= it->clip[1] && y < it->clip[3])))
            top = it->owner;
    }
    struct node *deep = deepest_box(top ? top : root, x, y, 0);
    return deep ? deep : top;
}

void page_print_text(const struct page *p, const char *prefix)
{
    int line = -1, end = 0;                     /* (the x where the last piece ended) */
    char out[4096];
    size_t o = 0;
    for (int i = 0; i <= p->count; i++) {
        const struct item *it = i < p->count ? &p->items[i] : 0;
        if (it && it->decoration)
            continue;                           /* (backgrounds and borders) */
        if (!it || it->line != line) {
            if (o > 0) {
                out[o] = 0;
                printf("%s%s\n", prefix, out);
            }
            o = 0;
            if (!it)
                break;
            line = it->line;
        }
        char piece[600];
        if (it->kind == ITEM_TEXT)
            snprintf(piece, sizeof(piece), "%s", it->text);
        else if (it->kind == ITEM_CONTROL)
            control_describe(it->node, piece, sizeof(piece));
        else
            snprintf(piece, sizeof(piece), "[picture %dx%d]", it->w, it->h);
        if (o > 0 && it->x > end && o + 1 < sizeof(out))
            out[o++] = ' ';                     /* (a gap: "a link." has none) */
        end = it->x + it->w;
        size_t n = strlen(piece);
        if (o + n >= sizeof(out))
            n = sizeof(out) - o - 1;
        memcpy(out + o, piece, n);
        o += n;
    }
}

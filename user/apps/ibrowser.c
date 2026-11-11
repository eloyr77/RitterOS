/*
 * ibrowser [-t] [URL]: iBrowser, a basic web browser in a window. It shows
 * web pages from http:// and https:// addresses and HTML files on the disk
 * (a path): text with headings, lists, links and tables, PNG and JPEG
 * pictures, styled by the page's basic CSS (colours, font size and weight,
 * margins and padding, borders, alignment, display). Scripts since 27p
 * (dom.c, on RitterOS's own JavaScript engine): run in document order
 * once the page is parsed, before it is styled.
 *
 * At the top: Back and the address line (libwindow's text field: click it or
 * press Ctrl-L, edit with the mouse and the keys, copy and paste, Enter);
 * at the bottom: where a link under the pointer goes, or the page's
 * title. A click on a link follows it; the wheel, the scroll bar, the
 * arrow keys, Page Up/Down, Home/End and space scroll; Backspace (outside the
 * address line) goes back. File > Open File... shows a page from the disk.
 * A drag across the page selects its text (a double click a word); Ctrl+C
 * copies it to the clipboard (15y). An address with "#part" scrolls to the
 * element of that id (or <a name>); a link to another part of the page shown
 * only scrolls, and Back returns to the place before (27b).
 * Forms (27c): a click or Tab focuses a control. Text fields are edited by
 * the window library's text field, put over the control's box while it
 * has the keyboard (the page's controls are drawn by the program: a field
 * that draws itself would not stay under the tool bar when the page
 * scrolls); passwords (shown as *) and textareas by a small editor of
 * their own (typing at the end, Backspace, Enter a new line). Space or a
 * click checks a box or a radio button; a select opens its list (or Up and
 * Down change it); a submit button, or Enter in a text field, sends the
 * form (GET or POST), a reset button resets it.
 * Pictures load in the background (27e): a loader thread fetches and
 * decodes them while the window's thread goes on reading its events (so the
 * page scrolls, links and fields work at once), and each is laid into the
 * page as it comes. One lock (`lock`) is the page's: the window's thread
 * holds it except while it waits for an event; the loader takes it to look
 * at the page, never while it fetches. A page left meanwhile is a new
 * `generation`: what still comes for the old one is dropped. A link is followed when the button is
 * let go without the pointer having moved (a drag from it selects).
 * -t prints each page's text and links on standard output (for tests).
 * The parts are in user/apps/ibrowser/ (browser.h tells which does what).
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>
#include "ibrowser/browser.h"
#include "image.h"
#include "ritteros.h"
#include "wfont.h"
#include "window.h"

PROGRAM_HELP("ibrowser [-t] [URL]", "iBrowser: show web pages (http, https, HTML files) in a window");

#define TOOLBAR_H   28
#define STATUS_H    18
#define BACK_W      28
#define STOP_W      24          /* (27q) the Stop button's place, after Back (drawn while loading) */
#define ADDRESS_X   (BACK_W + STOP_W + 6)
#define LINE_STEP   16          /* pixels an arrow key scrolls */
#define WHEEL_STEP  48          /* pixels a wheel step scrolls */
#define HISTORY     64
#define PICTURES    64          /* pictures a page may show */
#define SHEETS      8           /* style sheets a page may link */

#define M_OPEN      1
#define M_RELOAD    2
#define M_BACK      3
#define M_QUIT      4

static struct window *win;
static bool print_text;

/* The page shown. */
static struct node *tree;
static struct sheet *sheet;
static struct page page;
static char page_url[URL_MAX];          /* (without its "#part") */
static char page_part[URL_MAX];         /* the "#part" shown with it, or "" (27b) */
static int scroll;                      /* the page's y at the content area's top */

/* The address line: the window library's text field (15w). */
static struct window_field *address;

static char status[300];
static int hover = -1;                  /* the link under the pointer */

/* Forms (27c). */
static struct node *focus;              /* the control with the keyboard, or 0 */
static struct window_field *editor;     /* edits focus when it is a text field */
static bool editing;                    /* editor is over focus and has the keyboard */
static struct node *list_of;            /* a select whose option list is open, or 0 */
static int list_at;                     /* the option under the pointer (or the keys) */
static char *post_body;                 /* what the next open_page posts (malloc'ed), or 0 */
static size_t post_n;

/* (27e) Loading pictures in the background. */
static mtx_t lock;                      /* the page's (see the top) */
static unsigned generation;             /* the page shown: one more for each new one */
static bool page_failed;                /* (its status line is the reason) */
static int load_at;                     /* the place open_page was asked for, or -1 */
static bool scrolled_by_hand;           /* since the page came: then pictures move nothing */
/* (27q) the page's events for scripts */
static struct node *hovered;            /* the element under the pointer (mouseover, mouseout) */
static struct node *pressed_on;         /* where the left button went down: its click comes at the release */
static struct node *press_control;      /* a button or check box pressed: used at the release */
static bool mouse_suppressed;           /* pointerdown canceled: no mouse events until the release */
static bool double_pending;             /* a double click: dblclick after the second click */
static int mods_held;                   /* DOM_CTRL, ... as the physical keys say */
static int swallowed_key = -1;          /* a key whose keydown was canceled: its bytes are dropped */
static int key_esc;                     /* bytes of an escape sequence (no keypress for them) */
static bool scroll_event;               /* the page scrolled: its scroll event is due */
static char *focus_value;               /* the focused field's text when it got the keyboard (change) */
static int pointer_x = -1, pointer_y = -1;
static cnd_t timer_wake;
static unsigned load_id;                /* one more for each page load begun or stopped */
static struct {                         /* (27q) <meta http-equiv="refresh">: where and when */
    bool on;
    unsigned generation;                /* (the page it is for) */
    double due;                         /* CLOCK_MONOTONIC, ms */
    char url[URL_MAX];
} refresh;
static bool loading;                    /* a page load is on (the Stop button shows) */

static struct { char url[URL_MAX]; int scroll; } history[HISTORY];
static int history_count;

static const char HOME_PAGE[] =
    "<html><head><title>iBrowser</title></head><body>"
    "<h1>iBrowser</h1>"
    "<p>A basic web browser for RitterOS: type an address in the line above "
    "(<b>Ctrl-L</b>, then <b>Enter</b>), e.g. <code>http://10.0.2.2:8080/</code>, "
    "<code>https://example.org/</code> or a file like <code>/home/root/page.html</code>.</p>"
    "<ul><li>Links: a click follows them; <b>Back</b> or Backspace returns.</li>"
    "<li>Scroll with the wheel, the bar, the arrow keys, Page Up/Down or space.</li>"
    "<li>Pages from the disk: File &gt; Open File...</li></ul>"
    "<p>HTML and basic CSS, PNG and JPEG pictures; no scripts.</p></body></html>";

/* --- drawing --- */

static uint32_t native(uint32_t rgb)
{
    return window_rgb(win, (int)(rgb >> 16) & 255, (int)(rgb >> 8) & 255, (int)rgb & 255);
}

static int content_w(void)
{
    return window_width(win) - WINDOW_SCROLLBAR_W;
}

static int content_h(void)
{
    int h = window_height(win) - TOOLBAR_H - STATUS_H;
    return h > 1 ? h : 1;
}

static int max_scroll(void)
{
    int m = page.height - content_h();
    return m > 0 ? m : 0;
}

/* Pictures scaled to their item's size, in the window's pixels, kept. */
static struct { const struct image *src; int w, h; uint32_t *pixels; } scaled[PICTURES];
static int scaled_count;

static void forget_scaled(void)
{
    for (int i = 0; i < scaled_count; i++)
        free(scaled[i].pixels);
    scaled_count = 0;
}

static const uint32_t *picture_pixels(const struct image *src, int w, int h)
{
    for (int i = 0; i < scaled_count; i++)
        if (scaled[i].src == src && scaled[i].w == w && scaled[i].h == h)
            return scaled[i].pixels;
    struct image s;
    if (image_scale(src, w, h, &s) < 0)
        return 0;
    uint32_t *px = malloc((size_t)w * h * 4);
    if (px) {
        int rs, gs, bs;
        window_shifts(win, &rs, &gs, &bs);
        const uint8_t *p = s.pixels;
        uint32_t bg = page.background;
        for (long i = 0; i < (long)w * h; i++, p += 4) {
            int a = p[3];                   /* (transparent parts: on the page's background) */
            int r = (p[0] * a + (int)((bg >> 16) & 255) * (255 - a)) / 255;
            int g = (p[1] * a + (int)((bg >> 8) & 255) * (255 - a)) / 255;
            int b = (p[2] * a + (int)(bg & 255) * (255 - a)) / 255;
            px[i] = (uint32_t)r << rs | (uint32_t)g << gs | (uint32_t)b << bs;
        }
    }
    image_free(&s);
    if (!px)
        return 0;
    if (scaled_count == PICTURES) {
        free(scaled[0].pixels);
        memmove(scaled, scaled + 1, sizeof(scaled[0]) * (PICTURES - 1));
        scaled_count--;
    }
    scaled[scaled_count].src = src, scaled[scaled_count].w = w, scaled[scaled_count].h = h;
    scaled[scaled_count].pixels = px;
    scaled_count++;
    return px;
}

/* (27h) What of the page the window shows of an item: the content area,
 * cut by the item's clip (overflow) in window coordinates. */
struct area { int x0, y0, x1, y1; };

static struct area seen(const struct item *it, int top)
{
    struct area a = { 0, top, content_w(), top + content_h() };
    if (it->clipped && !it->fixed) {
        int dy = top - scroll;
        if (it->clip[0] > a.x0) a.x0 = it->clip[0];
        if (it->clip[2] < a.x1) a.x1 = it->clip[2];
        if (it->clip[1] + dy > a.y0) a.y0 = it->clip[1] + dy;
        if (it->clip[3] + dy < a.y1) a.y1 = it->clip[3] + dy;
    }
    return a;
}

static void fill_in(const struct area *a, int x, int y, int w, int h, uint32_t color)
{
    int x1 = x + w, y1 = y + h;
    if (x < a->x0) x = a->x0;
    if (y < a->y0) y = a->y0;
    if (x1 > a->x1) x1 = a->x1;
    if (y1 > a->y1) y1 = a->y1;
    if (x1 > x && y1 > y)
        window_fill(win, x, y, x1 - x, y1 - y, color);
}

/* Row `row` of a w x h box with corner radii r (top-left, top-right,
 * bottom-right, bottom-left): how far its left and right edges are in. */
static void corner_insets(int w, int h, const int r[4], int row, int *left, int *right)
{
    *left = *right = 0;
    for (int k = 0; k < 4; k++) {
        int rad = r[k];
        if (rad <= 0)
            continue;
        if (rad > w / 2) rad = w / 2;
        if (rad > h / 2) rad = h / 2;
        bool top_side = k < 2;
        int d = top_side ? row : h - 1 - row;           /* (rows from that corner's edge) */
        if (d >= rad)
            continue;
        double dy = rad - d - 0.5, dx = (double)rad * rad - dy * dy;
        int in = rad - (int)(dx > 0 ? sqrt(dx) + 0.5 : 0);
        if (k == 0 || k == 3)
            *left = in > *left ? in : *left;
        else
            *right = in > *right ? in : *right;
    }
}

static bool any_corner(const int r[4])
{
    return r[0] > 0 || r[1] > 0 || r[2] > 0 || r[3] > 0;
}

/* A box filled, its corners rounded, or only a ring `ring` thick. */
static void rounded(const struct area *a, int x, int y, int w, int h, const int r[4], int ring, uint32_t color)
{
    if (w <= 0 || h <= 0)
        return;
    if (!any_corner(r) && !ring) {
        fill_in(a, x, y, w, h, color);
        return;
    }
    int inner[4];
    for (int k = 0; k < 4; k++)
        inner[k] = r[k] > ring ? r[k] - ring : 0;
    int y0 = y < a->y0 ? a->y0 - y : 0, y1 = y + h > a->y1 ? a->y1 - y : h;
    for (int row = y0; row < y1; row++) {
        int l, rr;
        corner_insets(w, h, r, row, &l, &rr);
        if (!ring || row < ring || row >= h - ring) {
            fill_in(a, x + l, y + row, w - l - rr, 1, color);
            continue;
        }
        int il, ir;
        corner_insets(w - 2 * ring, h - 2 * ring, inner, row - ring, &il, &ir);
        fill_in(a, x + l, y + row, ring + il - l, 1, color);
        fill_in(a, x + w - ring - ir, y + row, ring + ir - rr, 1, color);
    }
}

static uint32_t gradient_at(const struct gradient *g, int t)
{
    if (t <= g->at[0] || g->count == 1)
        return g->colors[0];
    for (int i = 1; i < g->count; i++)
        if (t <= g->at[i]) {
            int span = g->at[i] - g->at[i - 1];
            int f = span > 0 ? (t - g->at[i - 1]) * 256 / span : 256;
            uint32_t a = g->colors[i - 1], b = g->colors[i], out = 0;
            for (int shift = 0; shift <= 16; shift += 8) {
                int ca = (int)(a >> shift & 255), cb = (int)(b >> shift & 255);
                out |= (uint32_t)((ca * (256 - f) + cb * f) >> 8) << shift;
            }
            return out;
        }
    return g->colors[g->count - 1];
}

static void draw_gradient(const struct area *a, const struct item *it, int x, int y)
{
    const struct gradient *g = it->gradient;
    int along = g->across ? it->w : it->h;
    if (along <= 0)
        return;
    int from = g->across ? a->x0 - x : a->y0 - y, to = g->across ? a->x1 - x : a->y1 - y;
    if (from < 0)
        from = 0;
    if (to > along)
        to = along;
    for (int i = from; i < to; i++) {           /* (only what is in view) */
        int t = (int)((2L * i + 1) * 1000 / (2L * along));
        uint32_t c = native(gradient_at(g, g->reverse ? 1000 - t : t));
        if (g->across) {
            fill_in(a, x + i, y, 1, it->h, c);
        } else {
            int l, r;
            corner_insets(it->w, it->h, it->radius, i, &l, &r);
            fill_in(a, x + l, y + i, it->w - l - r, 1, c);
        }
    }
}

/* Pixels of a w-wide picture, their rows from y and columns from x, drawn
 * at (dx, dy) as far as `a` (and rounded corners of a box w x h) allow. */
static void blit_in(const struct area *a, const uint32_t *px, int pw, int ph, int dx, int dy,
                    const int *r)
{
    int y0 = dy < a->y0 ? a->y0 - dy : 0, y1 = dy + ph > a->y1 ? a->y1 - dy : ph;
    int x0 = dx < a->x0 ? a->x0 - dx : 0, x1 = dx + pw > a->x1 ? a->x1 - dx : pw;
    if (x1 <= x0 || y1 <= y0)
        return;
    if (!r && x0 == 0 && x1 == pw) {            /* (whole rows: in one go) */
        window_blit(win, dx, dy + y0, pw, y1 - y0, px + (long)y0 * pw);
        return;
    }
    for (int row = y0; row < y1; row++) {
        int l = 0, rr = 0;
        if (r)
            corner_insets(pw, ph, r, row, &l, &rr);
        int from = x0 > l ? x0 : l, to = x1 < pw - rr ? x1 : pw - rr;
        if (to > from)
            window_blit(win, dx + from, dy + row, to - from, 1, px + (long)row * pw + from);
    }
}

/* A background picture: its tile repeated over the item's box. */
static void draw_tiles(const struct area *box_area, const struct item *it, int x, int y)
{
    const uint32_t *px = picture_pixels(it->picture, it->tile_w, it->tile_h);
    if (!px)
        return;
    int tw = it->tile_w, th = it->tile_h;
    /* A small tile that repeats: put together into one of about 128 x 128
     * first, or a 1-pixel strip would take a blit per pixel. */
    int kx = (it->repeat == REPEAT_BOTH || it->repeat == REPEAT_X) && tw < 128 ? 128 / tw : 1;
    int ky = (it->repeat == REPEAT_BOTH || it->repeat == REPEAT_Y) && th < 128 ? 128 / th : 1;
    uint32_t *big = 0;
    if (kx > 1 || ky > 1) {
        big = malloc((size_t)tw * kx * th * ky * 4);
        if (big) {
            for (int r = 0; r < th * ky; r++)
                for (int k = 0; k < kx; k++)
                    memcpy(big + ((size_t)r * tw * kx + (size_t)k * tw), px + (size_t)(r % th) * tw,
                           (size_t)tw * 4);
            px = big, tw *= kx, th *= ky;
        }
    }
    struct area a = *box_area;                  /* (inside its box) */
    if (x > a.x0) a.x0 = x;
    if (y > a.y0) a.y0 = y;
    if (x + it->w < a.x1) a.x1 = x + it->w;
    if (y + it->h < a.y1) a.y1 = y + it->h;
    int ox = it->tile_x, oy = it->tile_y + (y - it->y);     /* (the tile's place in the window) */
    int fx = ox, fy = oy, lx = ox, ly = oy;
    if (it->repeat == REPEAT_BOTH || it->repeat == REPEAT_X) {
        fx = a.x0 - (((a.x0 - ox) % tw) + tw) % tw;     /* (the tile at or left of the view's edge) */
        lx = a.x1;
    }
    if (it->repeat == REPEAT_BOTH || it->repeat == REPEAT_Y) {
        fy = a.y0 - (((a.y0 - oy) % th) + th) % th;
        ly = a.y1;
    }
    if (a.x1 > a.x0 && a.y1 > a.y0)
        for (int ty = fy; ty <= ly && ty < a.y1; ty += th)
            for (int tx = fx; tx <= lx && tx < a.x1; tx += tw)
                blit_in(&a, px, tw, th, tx, ty, 0);
    free(big);
}

/* Text at its scale (1 or 2) through a scratch buffer: the glyphs' runs
 * of set pixels as blocks, only inside `a` (big text, and text cut by an
 * overflow's edge). */
static void scaled_text(const struct item *it, int x, int y, uint32_t color, const struct area *a)
{
    int scale = it->scale, cw = wfont_width(it->font), ch = wfont_height(it->font);
    int w = it->w / scale + cw, h = ch;
    uint32_t *buf = calloc((size_t)w * h, 4);
    if (!buf)
        return;
    wfont_text(buf, (uint32_t)w, (uint32_t)w, (uint32_t)h, it->font, 0, 0, it->text, 1, 0);
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w;) {
            if (buf[r * w + c] != 1) {
                c++;
                continue;
            }
            int s = c;
            while (c < w && buf[r * w + c] == 1)
                c++;
            fill_in(a, x + scale * s, y + scale * r, scale * (c - s), scale, color);
            if (it->fake_bold)
                fill_in(a, x + scale * s + 1, y + scale * r, scale * (c - s), scale, color);
        }
    free(buf);
}

/* --- the selection on the page (15y) --- */

struct spot { int item, ch; };          /* a text item and a character in it */
static struct spot sel_a, sel_b;        /* as the mouse made them (either order) */
static bool selected, selecting, moved_since_press;
static int press_x, press_y, press_link = -1;

static int item_chars(const struct item *it)
{
    int n = 0;
    for (const char *p = it->text; *p; p++)
        n += ((unsigned char)*p & 0xC0) != 0x80;
    return n;
}

/* (27k) Where character k of a text item begins, in pixels from its left
 * (proportional faces: by the widths of the characters before it). */
static int char_x(const struct item *it, int k)
{
    char prefix[600];
    size_t n = 0;
    const char *p = it->text;
    for (int i = 0; i < k && *p; i++) {
        const char *q = p;
        utf8_next(&q);
        if (n + (size_t)(q - p) >= sizeof(prefix))
            break;
        memcpy(prefix + n, p, (size_t)(q - p));
        n += (size_t)(q - p);
        p = q;
    }
    prefix[n] = 0;
    return wfont_text_width(it->font, prefix) * it->scale;
}

/* The character at pixel dx from a text item's left. */
static int char_at(const struct item *it, int dx)
{
    int n = item_chars(it), k = 0;
    while (k + 1 < n && char_x(it, k + 1) <= dx)
        k++;
    return k;
}

/* The byte offset of character ch in s. */
static size_t char_offset(const char *s, int ch)
{
    size_t i = 0;
    for (int k = 0; k < ch && s[i]; k++) {
        i++;
        while (((unsigned char)s[i] & 0xC0) == 0x80)
            i++;
    }
    return i;
}

static bool before(struct spot a, struct spot b)
{
    return a.item < b.item || (a.item == b.item && a.ch < b.ch);
}

static void ordered(struct spot *a, struct spot *b)
{
    *a = before(sel_b, sel_a) ? sel_b : sel_a;
    *b = before(sel_b, sel_a) ? sel_a : sel_b;
}

/* The character at page position (px, py): in the text item there, or the
 * nearest one on that line; above all lines, the end of the last above.
 * item -1 if the page has no text. */
static struct spot spot_at(int px, int py)
{
    struct spot s = { -1, 0 }, above = { -1, 0 };
    int best = 1 << 30;
    for (int i = 0; i < page.count; i++) {
        const struct item *it = &page.items[i];
        if (it->kind != ITEM_TEXT)
            continue;
        if (it->y + it->h <= py) {              /* a line above: its end if nothing better */
            above.item = i, above.ch = item_chars(it) - 1;
            continue;
        }
        if (py < it->y)
            continue;
        int d = px < it->x ? it->x - px : px >= it->x + it->w ? px - (it->x + it->w) + 1 : 0;
        if (d < best) {
            best = d;
            int n = item_chars(it), c = char_at(it, px - it->x);
            s.item = i, s.ch = c < 0 ? 0 : c >= n ? n - 1 : c;
        }
    }
    return s.item >= 0 ? s : above;
}

/* --- form controls (27c): drawing --- */

static void frame(int x, int y, int w, int h, uint32_t edge, int line)
{
    window_fill(win, x, y, w, line, edge);
    window_fill(win, x, y + h - line, w, line, edge);
    window_fill(win, x, y, line, h, edge);
    window_fill(win, x + w - line, y, line, h, edge);
}

/* As much of text as fits in w pixels at the font's width (UTF-8). */
static void fitted(const char *text, const struct font *f, int w, char *out, size_t size, bool tail)
{
    int room = w / wfont_width(f), n = 0;
    for (const char *p = text; *p; p++)
        n += ((unsigned char)*p & 0xC0) != 0x80;
    const char *start = text;
    if (tail && n > room)
        start = text + char_offset(text, n - room);     /* (the end: where typing goes) */
    size_t bytes = char_offset(start, room);
    if (bytes >= size)
        bytes = size - 1;
    while (bytes > 0 && ((unsigned char)start[bytes] & 0xC0) == 0x80)
        bytes--;
    memcpy(out, start, bytes);
    out[bytes] = 0;
}

static void round_box(int x, int y, bool checked, uint32_t edge, uint32_t dot)
{
    for (int py = 0; py < 13; py++)             /* a circle of radius 6 (in half pixels) */
        for (int px = 0; px < 13; px++) {
            int d = (2 * px - 12) * (2 * px - 12) + (2 * py - 12) * (2 * py - 12);
            if (d <= 144 && d >= 100)
                window_fill(win, x + px, y + py, 1, 1, edge);
            else if (d < 100)
                window_fill(win, x + px, y + py, 1, 1, checked && d <= 30 ? dot : native(0xFFFFFF));
        }
}

static void draw_control(const struct item *it, int x, int y)
{
    struct node *e = it->node;
    struct control *k = control_of(e);
    if (!k)
        return;
    const struct font *f = it->font;
    int w = it->w, h = it->h, ch = wfont_height(f);
    bool off = node_attr(e, "disabled") != 0, mine = e == focus;
    uint32_t edge = native(mine ? 0x3D6FD0 : 0x9AA3B0), ink = native(off ? 0x9AA3B0 : 0x2E3440);
    uint32_t paper = native(off ? 0xEEF0F4 : 0xFFFFFF);
    char shown[600];
    switch (k->kind) {
    case CONTROL_CHECKBOX:
        window_fill(win, x, y, w, h, paper);
        frame(x, y, w, h, edge, mine ? 2 : 1);
        if (k->checked)
            window_fill(win, x + 3, y + 3, w - 6, h - 6, native(off ? 0x9AA3B0 : 0x3D6FD0));
        return;
    case CONTROL_RADIO:
        round_box(x, y, k->checked, edge, native(off ? 0x9AA3B0 : 0x3D6FD0));
        return;
    case CONTROL_SUBMIT:
    case CONTROL_RESET:
    case CONTROL_BUTTON: {
        window_fill(win, x, y, w, h, native(off ? 0xEEF0F4 : 0xE5E9F0));
        frame(x, y, w, h, edge, mine ? 2 : 1);
        char *label = control_label(e);
        fitted(label ? label : "", f, w - 8, shown, sizeof(shown), false);
        free(label);
        int tw = wfont_text_width(f, shown);
        window_text_font(win, f, x + (w - tw) / 2, y + (h - ch) / 2, shown, ink, WINDOW_TRANSPARENT);
        return;
    }
    case CONTROL_SELECT: {
        window_fill(win, x, y, w, h, paper);
        frame(x, y, w, h, edge, mine ? 2 : 1);
        struct node *options[256];
        int count = select_options(e, options, 256);
        char *label = k->selected < count && k->selected < 256 ? node_text(options[k->selected]) : 0;
        fitted(label ? label : "", f, w - 24, shown, sizeof(shown), false);
        free(label);
        window_text_font(win, f, x + 4, y + 3, shown, ink, WINDOW_TRANSPARENT);
        for (int i = 0; i < 4; i++)             /* the arrow: a small triangle */
            window_fill(win, x + w - 14 + i, y + h / 2 - 2 + i, 8 - 2 * i, 1, ink);
        return;
    }
    case CONTROL_TEXTAREA: {
        window_fill(win, x, y, w, h, paper);
        frame(x, y, w, h, edge, mine ? 2 : 1);
        int rows = (h - 8) / ch, cols = (w - 8) / wfont_width(f);
        if (rows < 1 || cols < 1)
            return;
        /* Its lines as they wrap at the box's width; the last ones while typing. */
        const char *text = k->value ? k->value : "";
        int total = 0;
        for (int pass = 0; pass < 2; pass++) {
            int line = 0, first = pass && mine && total > rows ? total - rows : 0;
            const char *p = text;
            do {
                size_t n = strcspn(p, "\n");
                char part[600];
                do {                            /* (an empty line is a line too) */
                    size_t bytes = char_offset(p, cols);
                    if (bytes > n)
                        bytes = n;
                    if (pass && line >= first && line - first < rows) {
                        size_t take = bytes < sizeof(part) - 1 ? bytes : sizeof(part) - 1;
                        memcpy(part, p, take);
                        part[take] = 0;
                        window_text_font(win, f, x + 4, y + 4 + (line - first) * ch, part, ink,
                                         WINDOW_TRANSPARENT);
                        if (mine && !p[bytes] && line - first < rows)   /* the cursor: at the end */
                            window_fill(win, x + 4 + wfont_text_width(f, part), y + 4 + (line - first) * ch,
                                        1, ch, ink);
                    }
                    line++;
                    p += bytes, n -= bytes;
                } while (n > 0);
                if (*p == '\n') {
                    p++;
                    if (!*p) {                  /* (a line break at the end: an empty last line) */
                        if (pass && mine && line >= first && line - first < rows)
                            window_fill(win, x + 4, y + 4 + (line - first) * ch, 1, ch, ink);
                        line++;
                    }
                }
            } while (*p);
            total = line;
        }
        return;
    }
    default: {                                  /* text fields and passwords */
        window_fill(win, x, y, w, h, paper);
        frame(x, y, w, h, edge, mine ? 2 : 1);
        const char *text = k->value ? k->value : "";
        char stars[600];
        if (k->kind == CONTROL_PASSWORD) {
            size_t n = 0;
            for (const char *p = text; *p && n + 1 < sizeof(stars); p++)
                if (((unsigned char)*p & 0xC0) != 0x80)
                    stars[n++] = '*';
            stars[n] = 0;
            text = stars;
        }
        fitted(text, f, w - 8, shown, sizeof(shown), mine);
        window_text_font(win, f, x + 4, y + 3, shown, ink, WINDOW_TRANSPARENT);
        if (mine && k->kind == CONTROL_PASSWORD)
            window_fill(win, x + 4 + wfont_text_width(f, shown), y + 3, 1, h - 6, ink);
        return;
    }
    }
}

static void draw_item(const struct item *it, int top)
{
    int x = it->x, y = item_top(it, scroll) + top;      /* (fixed boxes do not scroll) */
    if (it->kind == ITEM_BOX || it->hidden)
        return;
    int grow = it->soft;                        /* (a shadow's blur reaches beyond its box) */
    if (y + it->h + grow < top || y - grow >= top + content_h())
        return;
    struct area view = seen(it, top);
    if (view.x1 <= view.x0 || view.y1 <= view.y0)
        return;
    int idx = (int)(it - page.items), c0 = 0, c1 = 0;
    if (selected && it->kind == ITEM_TEXT) {    /* (15y) its selected characters */
        struct spot a, b;
        ordered(&a, &b);
        if (idx >= a.item && idx <= b.item) {
            c0 = idx == a.item ? a.ch : 0;
            c1 = idx == b.item ? b.ch + 1 : item_chars(it);
            if (c1 > c0)
                fill_in(&view, x + char_x(it, c0), y, char_x(it, c1) - char_x(it, c0), it->h, native(0x3D6FD0));
        }
    }
    if (it->kind == ITEM_CONTROL) {
        if (!(editing && it->node == focus))    /* (the editor draws that one) */
            draw_control(it, x, y);
    } else if (it->kind == ITEM_FILL) {
        if (it->gradient) {
            draw_gradient(&view, it, x, y);
        } else if (it->soft) {                  /* a shadow: its blurred edge, then itself */
            int g = it->soft / 2, r[4];
            for (int k = 0; k < 4; k++)
                r[k] = it->radius[k] + g;
            rounded(&view, x - g, y - g, it->w + 2 * g, it->h + 2 * g, r, 0, native(it->color2));
            rounded(&view, x, y, it->w, it->h, it->radius, 0, native(it->color));
        } else {
            rounded(&view, x, y, it->w, it->h, it->radius, it->ring, native(it->color));
        }
    } else if (it->kind == ITEM_PICTURE) {
        if (it->tiled) {
            draw_tiles(&view, it, x, y);
        } else {
            const uint32_t *px = picture_pixels(it->picture, it->w, it->h);
            if (px)
                blit_in(&view, px, it->w, it->h, x, y, any_corner(it->radius) ? it->radius : 0);
            else
                rounded(&view, x, y, it->w, it->h, it->radius, 0, native(0xC8C8C8));
        }
    } else {
        uint32_t color = native(it->link >= 0 && it->link == hover ? 0xCC0000 : it->color);
        bool cut = x < view.x0 || x + it->w > view.x1 || y < view.y0 || y + it->h > view.y1;
        if (it->scale == 2 || (cut && it->clipped)) {
            scaled_text(it, x, y + (it->scale == 2 ? 2 : 1), color, &view);
        } else {
            window_text_font(win, it->font, x, y + 1, it->text, color, WINDOW_TRANSPARENT);
            if (it->fake_bold)
                window_text_font(win, it->font, x + 1, y + 1, it->text, color, WINDOW_TRANSPARENT);
            if (c1 > c0) {                      /* the selected part again, white on blue */
                char part[600];
                size_t o0 = char_offset(it->text, c0), o1 = char_offset(it->text, c1);
                size_t n = o1 - o0 < sizeof(part) - 1 ? o1 - o0 : sizeof(part) - 1;
                memcpy(part, it->text + o0, n);
                part[n] = 0;
                window_text_font(win, it->font, x + char_x(it, c0), y + 1, part,
                                 native(0xFFFFFF), WINDOW_TRANSPARENT);
            }
        }
        int line = it->scale;
        if (it->underline)
            fill_in(&view, x, y + it->h - 1 - line, it->w, line, color);
        if (it->strike)
            fill_in(&view, x, y + it->h / 2, it->w, line, color);
    }
}

static void draw_bars(void)
{
    int w = window_width(win), h = window_height(win);
    uint32_t bar = native(0xE5E9F0), edge = native(0x9AA3B0), text = native(0x2E3440);
    window_fill(win, 0, 0, w, TOOLBAR_H, bar);
    window_fill(win, 0, TOOLBAR_H - 1, w, 1, edge);
    /* Back */
    bool can_back = history_count > 0;
    window_fill(win, 4, 4, BACK_W - 4, TOOLBAR_H - 9, native(can_back ? 0xD8DEE9 : 0xEEF0F4));
    window_text(win, 4 + (BACK_W - 4 - 9) / 2, 6, "<", can_back ? text : native(0xA0A8B4),
                WINDOW_TRANSPARENT);
    if (loading) {                              /* (27q) Stop, while a page loads */
        window_fill(win, BACK_W + 2, 4, STOP_W - 2, TOOLBAR_H - 9, native(0xD8DEE9));
        window_text(win, BACK_W + 2 + (STOP_W - 2 - 9) / 2, 6, "x", text, WINDOW_TRANSPARENT);
    }
    /* The address line. */
    int ax = ADDRESS_X;
    window_field_place(address, ax, (TOOLBAR_H - window_field_height(address)) / 2, w - ax - 6);
    window_field_draw(address);
    /* The status line. */
    window_fill(win, 0, h - STATUS_H, w, STATUS_H, bar);
    window_fill(win, 0, h - STATUS_H, w, 1, edge);
    const char *s = hover >= 0 && hover < page.link_count ? page.links[hover].href : status;
    window_text(win, 4, h - STATUS_H + 2, s, text, WINDOW_TRANSPARENT);
}

/* The focused control's item, or -1. */
static int item_of(const struct node *e)
{
    for (int i = 0; e && i < page.count; i++)
        if (page.items[i].kind == ITEM_CONTROL && page.items[i].node == e)
            return i;
    return -1;
}

/* An open select's list: its rows below the control (above it if there is
 * no room below), each an option's text. */
#define LIST_ROW 18
static int list_rect(int top, int *lx, int *ly, int *lw, int *lh)
{
    int i = item_of(list_of);
    if (i < 0)
        return 0;
    const struct item *it = &page.items[i];
    struct node *options[256];
    int count = select_options(list_of, options, 256);
    count = count > 256 ? 256 : count;
    int rows = count, room = content_h() / LIST_ROW - 1;
    rows = rows > room ? room : rows;
    *lw = it->w, *lh = rows * LIST_ROW + 2;
    *lx = it->x, *ly = item_top(it, scroll) + top + it->h;
    if (*ly + *lh > top + content_h())
        *ly = item_top(it, scroll) + top - *lh;
    if (*ly < top)
        *ly = top;
    return rows;
}

static void commit_editor(void)
{
    if (!editing)
        return;
    struct control *k = control_of(focus);
    if (k)
        control_set_text(k, window_field_text(editor));
    window_field_focus(editor, 0);
    editing = false;
}

static void draw_forms(int top)
{
    if (editing) {                              /* the editor over its control, if all shown */
        int i = item_of(focus);
        const struct item *it = i >= 0 ? &page.items[i] : 0;
        if (!it || item_top(it, scroll) < 0 || item_top(it, scroll) + it->h > content_h()) {
            commit_editor();                    /* (scrolled away: the control keeps the text) */
            focus = 0;
            dom_focused = 0;
            if (it)
                draw_control(it, it->x, item_top(it, scroll) + top);
        } else {
            window_field_place(editor, it->x, item_top(it, scroll) + top, it->w);
            window_field_draw(editor);
        }
    }
    int lx, ly, lw, lh, rows = list_rect(top, &lx, &ly, &lw, &lh);
    if (rows > 0) {
        struct node *options[256];
        select_options(list_of, options, 256);
        const struct font *f = wfont_regular();
        window_fill(win, lx, ly, lw, lh, native(0xFFFFFF));
        frame(lx, ly, lw, lh, native(0x3D6FD0), 1);
        int first = list_at >= rows ? list_at - rows + 1 : 0;
        for (int r = 0; r < rows; r++) {
            char *label = node_text(options[first + r]), shown[600];
            fitted(label ? label : "", f, lw - 8, shown, sizeof(shown), false);
            free(label);
            bool lit = first + r == list_at;
            if (lit)
                window_fill(win, lx + 1, ly + 1 + r * LIST_ROW, lw - 2, LIST_ROW, native(0x3D6FD0));
            window_text_font(win, f, lx + 4, ly + 2 + r * LIST_ROW, shown,
                             native(lit ? 0xFFFFFF : 0x2E3440), WINDOW_TRANSPARENT);
        }
    }
}

static void draw(void)
{
    int top = TOOLBAR_H;
    dom_scroll_y = scroll;
    window_fill(win, 0, top, content_w(), content_h(), native(page.background));
    for (int i = 0; i < page.count; i++)
        draw_item(&page.items[page.order ? page.order[i] : i], top);   /* (27d) CSS's order */
    draw_forms(top);
    window_fill(win, content_w(), top, WINDOW_SCROLLBAR_W, content_h(), native(0xE5E9F0));
    window_scrollbar(win, content_w(), top, content_h(), scroll, content_h(),
                     page.height > content_h() ? page.height : content_h());
    draw_bars();
    window_flush(win);
}

/* --- pages --- */

static void set_status(const char *text)
{
    snprintf(status, sizeof(status), "%s", text);
}

static void relayout(void)
{
    selected = selecting = false;               /* (the items change: no selection) */
    page_free(&page);
    forget_scaled();
    if (tree)
        layout(tree, content_w(), content_h(), page_url, &page);
    if (scroll > max_scroll())
        scroll = max_scroll();
    if (scroll < 0)
        scroll = 0;
    dom_scroll_y = scroll;
}

/* Text as HTML that shows it as it is (a .txt page). */
static char *plain_page(const char *text, long n)
{
    char *out = malloc((size_t)n * 5 + 32);
    if (!out)
        return 0;
    memcpy(out, "<pre>", 5);
    size_t o = 5;
    for (long i = 0; i < n; i++) {
        const char *e = text[i] == '<' ? "&lt;" : text[i] == '&' ? "&amp;" : 0;
        if (e) {
            memcpy(out + o, e, strlen(e));
            o += strlen(e);
        } else {
            out[o++] = text[i];
        }
    }
    strcpy(out + o, "</pre>");
    return out;
}

static void escape(const char *in, char *out, size_t size)
{
    size_t o = 0;
    for (; *in && o + 6 < size; in++) {
        const char *e = *in == '<' ? "&lt;" : *in == '&' ? "&amp;" : *in == '"' ? "&quot;" : 0;
        if (e) {
            memcpy(out + o, e, strlen(e));
            o += strlen(e);
        } else {
            out[o++] = *in;
        }
    }
    out[o] = 0;
}

/* --- the page's files (27q): style sheets and script files, fetched off
 * the page's lock (ahead by the page's loader, or later by a thread each),
 * so the window never waits for the network. The store is the page shown's
 * (under the lock); a file that comes for a page left is dropped. --- */

#define PREFETCH 16                     /* files fetched ahead with a page, at once */

struct resource {
    char url[URL_MAX];
    struct response r;                  /* (its body ours) */
    bool got;                           /* r is the answer, else r.error says why */
    bool fetching;                      /* on its way */
    bool told;                          /* (-t: a failure printed once) */
};

static struct resource *resources;
static int resource_n, resource_room;
static bool sheets_ready;                       /* (27p) style_sheets done for the page */
static bool after_dom(void);

static struct resource *resource_of(const char *url)
{
    for (int i = 0; i < resource_n; i++)
        if (!strcmp(resources[i].url, url))
            return &resources[i];
    return 0;
}

static struct resource *resource_add(const char *url)
{
    if (resource_n == resource_room) {
        int room = resource_room ? resource_room * 2 : 16;
        struct resource *r = realloc(resources, sizeof(*r) * (size_t)room);
        if (!r)
            return 0;
        resources = r, resource_room = room;
    }
    struct resource *res = &resources[resource_n++];
    memset(res, 0, sizeof(*res));
    snprintf(res->url, sizeof(res->url), "%s", url);
    return res;
}

static void resources_free(void)
{
    for (int i = 0; i < resource_n; i++)
        response_free(&resources[i].r);
    resource_n = 0;
}

static bool usable(const struct resource *res)
{
    return res->got && (res->r.status == 0 || (res->r.status >= 200 && res->r.status < 300));
}

struct resource_job {
    unsigned generation;
    char url[URL_MAX];
};

static int resource_loader(void *arg)
{
    struct resource_job *j = arg;
    struct response r;
    dom_set_stack_base(&r);                     /* (a script that came runs here) */
    bool got = http_get(j->url, &r);
    mtx_lock(&lock);
    struct resource *res = j->generation == generation ? resource_of(j->url) : 0;
    if (res) {
        res->r = r, res->got = got, res->fetching = false;
        r.body = 0, r.headers = 0;              /* (the store's now) */
        sheets_ready = false;                   /* (a style sheet: the page restyled) */
        dom_changed = true;
        dom_run_waiting();                      /* (a script file: its script runs) */
        after_dom();
    }
    mtx_unlock(&lock);
    response_free(&r);
    free(j);
    return 0;
}

/* (27r) A script's request (fetch, XMLHttpRequest): made on a thread of
 * its own, its answer given to the script with the page's lock. */
static int request_thread(void *arg)
{
    struct dom_request *q = arg;
    dom_set_stack_base(&q);                     /* (the script's callback runs here) */
    dom_perform(q);
    mtx_lock(&lock);
    dom_request_done(q);
    after_dom();
    mtx_unlock(&lock);
    return 0;
}

static int start_request(struct dom_request *q)
{
    thrd_t t;
    if (thrd_create(&t, request_thread, q) != thrd_success)
        return 0;
    thrd_detach(t);
    return 1;
}

/* The page's file at url: the store's, or -1 while it comes (its fetch
 * started now if it was not asked for yet). Under the lock. */
static int page_file(const char *url, struct resource **out)
{
    struct resource *res = resource_of(url);
    if (res) {
        *out = res;
        return res->fetching ? -1 : 1;
    }
    res = resource_add(url);
    struct resource_job *j = res ? calloc(1, sizeof(*j)) : 0;
    if (!j) {
        if (res)
            resource_n--;
        return 0;
    }
    res->fetching = true;
    j->generation = generation;
    snprintf(j->url, sizeof(j->url), "%s", url);
    thrd_t t;
    if (thrd_create(&t, resource_loader, j) != thrd_success) {
        free(j);
        res->fetching = false;                  /* (no thread: it fails, the window never waits) */
        snprintf(res->r.error, sizeof(res->r.error), "%s: no thread to fetch it", url);
        *out = res;
        return 1;
    }
    thrd_detach(t);
    return -1;
}

/* dom.c's hook for script files (under the lock) */
static int script_file(const char *url, struct response *r)
{
    struct resource *res;
    int k = page_file(url, &res);
    if (k <= 0)
        return k ? -1 : 0;
    *r = res->r;
    r->body = 0, r->headers = 0;                /* (the copy owns nothing of the store's) */
    if (res->got && res->r.body) {
        r->body = malloc((size_t)res->r.length + 1);
        if (!r->body)
            return 0;
        memcpy(r->body, res->r.body, (size_t)res->r.length + 1);
    }
    return res->got;
}

static int navigations;                         /* (27p) pages a script sent us on to, in a row */

/* The style sheets: <style> texts and <link rel=stylesheet> files, in order. */
static void style_sheets(void)
{
    sheets_ready = true;
    css_free(sheet);
    sheet = 0;
    int linked = 0;
    for (struct node *n = tree; n; n = node_next(n, tree)) {
        if (n->kind != NODE_ELEMENT)
            continue;
        if (!strcmp(n->tag, "style")) {
            for (struct node *t = n->first; t; t = t->next)
                if (t->kind == NODE_TEXT)
                    sheet = css_parse(t->text, sheet);
        } else if (!strcmp(n->tag, "link") && linked < SHEETS) {
            const char *rel = node_attr(n, "rel"), *href = node_attr(n, "href");
            char url[URL_MAX];
            if (!rel || !href || !strstr(rel, "stylesheet") ||
                !url_resolve(page_url, href, url, sizeof(url)))
                continue;
            linked++;
            struct resource *res;
            if (page_file(url, &res) < 0)
                continue;                       /* (27q: on its way: the page is styled again then) */
            if (usable(res)) {
                char *text = css_absolute_urls(res->r.body, url);   /* (27h: its url()s are its own) */
                sheet = css_parse(text ? text : res->r.body, sheet);
                free(text);
            } else if (print_text && !res->told) {
                res->told = true;
                printf("ibrowser: style sheet %s: %s\n", url, res->r.error[0] ? res->r.error : "not found");
            }
        }
    }
}

/* The page's pictures: each <img>'s file got and decoded. */
/* The pictures still to come for the page shown: fetched and decoded by a
 * thread of their own (27e), each laid into the page as it comes. */
struct picture_job {
    struct node *n;                     /* (valid while the page is the same generation) */
    char url[URL_MAX];
    bool background;                    /* (27h) its background-image, not an <img> */
    int want_w, want_h;                 /* (27j) an <img>'s width and height attributes, or 0 */
};

struct loader {
    unsigned generation;
    int count;
    struct picture_job jobs[PICTURES];
};

static void finish_page(int pictures);
static bool after_dom(void);

static int picture_loader(void *arg)
{
    struct loader *l = arg;
    int shown = 0;
    dom_set_stack_base(&shown);                 /* (27q: an image's load event runs scripts here) */
    bool current = true;
    for (int i = 0; i < l->count && current; i++) {
        mtx_lock(&lock);
        current = l->generation == generation;
        if (current) {
            snprintf(status, sizeof(status), "Loading %s", l->jobs[i].url);
            draw();
        }
        mtx_unlock(&lock);
        if (!current)
            break;
        struct response r;
        memset(&r, 0, sizeof(r));
        struct image *img = calloc(1, sizeof(*img));
        bool ok = img && http_get(l->jobs[i].url, &r) && (r.status == 0 || (r.status >= 200 && r.status < 300));
        if (ok && svg_looks_like(r.body, (size_t)r.length, r.type)) {
            /* (27j) an SVG file: drawn at the size asked for, under the page's
             * lock (the style code keeps the window's size in statics) */
            mtx_lock(&lock);
            struct image *drawn = svg_load(r.body, (size_t)r.length, l->jobs[i].want_w, l->jobs[i].want_h);
            mtx_unlock(&lock);
            free(img);
            img = drawn;
            ok = img != 0;
        } else if (ok) {
            ok = image_load_memory(r.body, r.length, img) == 0;
        }
        mtx_lock(&lock);
        current = l->generation == generation;
        if (current && ok) {
            if (l->jobs[i].background)
                l->jobs[i].n->background = img;
            else
                l->jobs[i].n->picture = img;    /* (into the page as it comes) */
            shown++;
            relayout();
            draw();
            if (!l->jobs[i].background) {
                dom_fire(l->jobs[i].n, "load", 0);      /* (27q) */
                after_dom();
            }
        } else {
            if (current) {
                if (l->jobs[i].background)
                    l->jobs[i].n->background_failed = 1;
                else
                    l->jobs[i].n->picture_failed = 1;
                if (print_text)
                    printf("ibrowser: picture %s: %s\n", l->jobs[i].url, r.error[0] ? r.error : "not shown");
                if (!l->jobs[i].background) {
                    dom_fire(l->jobs[i].n, "error", 0);
                    after_dom();
                }
            }
            if (ok)
                image_free(img);                /* (for a page no longer shown) */
            free(img);
        }
        mtx_unlock(&lock);
        response_free(&r);
    }
    mtx_lock(&lock);
    if (l->generation == generation) {
        finish_page(shown);
        after_dom();
    }
    mtx_unlock(&lock);
    free(l);
    return 0;
}

/* Start loading the page's pictures; false if it has none to load. */
static bool start_pictures(void)
{
    struct loader *l = calloc(1, sizeof(*l));
    if (!l)
        return false;
    l->generation = generation;
    for (struct node *n = tree; n && l->count < PICTURES; n = node_next(n, tree)) {
        if (n->kind == NODE_ELEMENT && n->style && n->style->background_image && !n->background &&
            !n->background_failed && n->style->display != DISPLAY_NONE &&
            !strstr(n->style->background_image, "gradient(")) {     /* (27h) a background picture */
            char *ref = css_url(n->style->background_image);
            struct picture_job *j = &l->jobs[l->count];
            if (ref && !starts_word(ref, "data:") && url_resolve(page_url, ref, j->url, sizeof(j->url))) {
                j->n = n, j->background = true;
                l->count++;
            } else {
                n->background_failed = 1;
            }
            free(ref);
            if (l->count == PICTURES)
                break;
        }
        if (n->kind != NODE_ELEMENT || strcmp(n->tag, "img") || n->picture || n->picture_failed)
            continue;
        if (n->style && n->style->display == DISPLAY_NONE)
            continue;
        const char *src = node_attr(n, "src");
        struct picture_job *j = &l->jobs[l->count];
        if (!src || !url_resolve(page_url, src, j->url, sizeof(j->url))) {
            n->picture_failed = 1;
            continue;
        }
        j->n = n;
        const char *aw = node_attr(n, "width"), *ah = node_attr(n, "height");
        j->want_w = aw && !strchr(aw, '%') ? atoi(aw) : 0;
        j->want_h = ah && !strchr(ah, '%') ? atoi(ah) : 0;
        if (j->want_w > 4096 || j->want_w < 0)
            j->want_w = 0;
        if (j->want_h > 4096 || j->want_h < 0)
            j->want_h = 0;
        l->count++;
    }
    thrd_t thread;
    if (!l->count || thrd_create(&thread, picture_loader, l) != thrd_success) {
        free(l);
        return false;                           /* (none, or no thread: shown without them) */
    }
    thrd_detach(thread);
    return true;
}

static void report(void)
{
    if (!print_text)
        return;
    printf("ibrowser: page %s: %d lines, %d links, %d pixels high\n", page_url,
           page.count ? page.items[page.count - 1].line + 1 : 0, page.link_count, page.height);
    page_print_text(&page, "ibrowser| ");
    for (int i = 0; i < page.link_count; i++) {
        for (int k = 0; k < page.count; k++)
            if (page.items[k].link == i) {
                printf("ibrowser: link %d at %d,%d: %s\n", i, page.items[k].x,
                       page.items[k].y, page.links[i].href);
                break;
            }
    }
    for (int k = 0, n = 0; k < page.count; k++)      /* (27c) the form controls */
        if (page.items[k].kind == ITEM_CONTROL) {
            char what[600];
            control_describe(page.items[k].node, what, sizeof(what));
            printf("ibrowser: control %d at %d,%d %dx%d: %s\n", n++, page.items[k].x, page.items[k].y,
                   page.items[k].w, page.items[k].h, what);
        }
}

/* The place of "#part" on the page: its anchor's y (as far as the page
 * scrolls), or 0 if the page has none of that name. */
static int part_y(const char *part)
{
    int y = part[0] ? page_anchor(&page, part + 1) : 0;
    if (part[0] && y < 0 && print_text)
        printf("ibrowser: %s is not on the page\n", part);
    if (y < 0)
        y = 0;
    return y > max_scroll() ? max_scroll() : y;
}

static void report_scroll(void)
{
    if (print_text)
        printf("ibrowser: scrolled to y %d%s%s\n", scroll, page_part[0] ? " for " : "", page_part);
}

static void finish_page(int pictures);

/* --- loading a page (27q: on a thread of its own, so the window stays
 * alive: scrolling, Back, Stop, closing; the page shown stays until the new
 * one is there) --- */

struct load {
    unsigned id;                        /* load_id when it began (a later load or Stop drops it) */
    char url[URL_MAX];
    bool record;                        /* the page shown goes into the history */
    bool back;                          /* it comes from the history (taken out when shown) */
    int at;
    char *body;                         /* a form's POST (malloc'ed), or 0 */
    size_t body_n;
    struct response r;
    bool got;                           /* r is the answer (else r.error says why) */
    struct prefetch *pre;               /* (27q) its style sheets and script files, fetched ahead */
    int pre_n;
};

struct prefetch {
    char url[URL_MAX];
    struct response r;
    bool got;
};

static int prefetcher(void *arg)
{
    struct prefetch *p = arg;
    p->got = http_get(p->url, &p->r);
    return 0;
}

static bool shown_as_html(const struct response *r)
{
    return !starts_word(r->type, "image/") && strcmp(r->type, "text/plain") && strcmp(r->type, "text/css");
}

/* The page's style sheets and script files (as its HTML names them), all
 * fetched at once by threads of their own, before the page is shown: off
 * the page's lock, so the window goes on meanwhile (27q). */
static void prefetch(struct load *l)
{
    struct node *t = html_parse(l->r.body, (size_t)l->r.length);
    l->pre = calloc(PREFETCH, sizeof(*l->pre));
    for (struct node *n = t; n && l->pre && l->pre_n < PREFETCH; n = node_next(n, t)) {
        if (n->kind != NODE_ELEMENT)
            continue;
        const char *ref = 0, *rel = node_attr(n, "rel");
        if (!strcmp(n->tag, "script") && dom_script_runnable(n))
            ref = node_attr(n, "src");
        else if (!strcmp(n->tag, "link") && rel && strstr(rel, "stylesheet"))
            ref = node_attr(n, "href");
        char url[URL_MAX];
        if (!ref || !url_resolve(l->r.url, ref, url, sizeof(url)))
            continue;
        bool twice = false;
        for (int i = 0; i < l->pre_n; i++)
            twice |= !strcmp(l->pre[i].url, url);
        if (!twice)
            snprintf(l->pre[l->pre_n++].url, sizeof(l->pre[0].url), "%s", url);
    }
    html_free(t);
    thrd_t threads[PREFETCH];
    bool started[PREFETCH] = { false };
    for (int i = 0; i < l->pre_n; i++)
        started[i] = thrd_create(&threads[i], prefetcher, &l->pre[i]) == thrd_success;
    for (int i = 0; i < l->pre_n; i++)
        if (started[i])
            thrd_join(threads[i], 0);
        else
            snprintf(l->pre[i].r.error, sizeof(l->pre[i].r.error), "%s: no thread to fetch it", l->pre[i].url);
}

static void load_free(struct load *l)
{
    response_free(&l->r);
    for (int i = 0; i < l->pre_n; i++)
        response_free(&l->pre[i].r);
    free(l->pre);
    free(l->body);
    free(l);
}


static void show_page(struct load *l);

static int page_loader(void *arg)
{
    struct load *l = arg;
    dom_set_stack_base(&l);                     /* (the page's scripts run here) */
    l->got = l->body ? http_post(l->url, l->body, l->body_n, &l->r) : http_get(l->url, &l->r);
    if (l->got && shown_as_html(&l->r) && l->id == __atomic_load_n(&load_id, __ATOMIC_RELAXED))
        prefetch(l);                            /* (27q) */
    mtx_lock(&lock);
    if (l->id == load_id) {                     /* (not stopped, not overtaken) */
        loading = false;
        show_page(l);
    }
    mtx_unlock(&lock);
    load_free(l);
    return 0;
}

/* Show the page at url (record: put the one shown now into the history;
 * back: it is the history's last, taken out once shown); at: the y to
 * show, or -1 for the top or url's "#part". The page comes on a thread of
 * its own (27q); a "#part" of the page shown and about:home at once. */
static bool reloading;                  /* the next load is Reload's: never only a scroll */

static void begin_load(const char *url, bool record, int at, bool back)
{
    size_t whole = strcspn(url, "#");
    bool again = reloading;
    reloading = false;
    if (!again && url[whole] && tree && strlen(page_url) == whole && !strncmp(url, page_url, whole)) {
        if (record && page_url[0]) {
            if (history_count == HISTORY) {
                memmove(history, history + 1, sizeof(history[0]) * (HISTORY - 1));
                history_count--;
            }
            snprintf(history[history_count].url, sizeof(history[0].url), "%s%s", page_url, page_part);
            history[history_count].scroll = scroll;
            history_count++;
        }
        if (back)
            history_count--;
        load_id++;                              /* (a load going on is overtaken) */
        loading = false;
        snprintf(page_part, sizeof(page_part), "%s", url + whole);     /* (the page shown: only a scroll) */
        char shown[URL_MAX * 2];
        snprintf(shown, sizeof(shown), "%s%s", page_url, page_part);
        window_field_focus(address, 0);
        window_field_set_text(address, shown);
        scroll = at >= 0 ? (at > max_scroll() ? max_scroll() : at) : part_y(page_part);
        report_scroll();
        draw();
        dom_set_url(shown);                     /* (27q) location.hash, and the page's hashchange */
        dom_fire(0, "hashchange", 0);
        after_dom();
        return;
    }
    struct load *l = calloc(1, sizeof(*l));
    if (!l)
        return;
    l->id = ++load_id;
    snprintf(l->url, sizeof(l->url), "%s", url);
    l->record = record, l->back = back, l->at = at;
    l->body = post_body, l->body_n = post_n;
    post_body = 0, post_n = 0;
    window_field_focus(address, 0);
    window_field_set_text(address, url);
    if (!strcmp(url, "about:home") || !strcmp(url, "about:")) {
        show_page(l);                           /* (nothing to fetch) */
        load_free(l);
        return;
    }
    loading = true;
    snprintf(status, sizeof(status), "Loading %s", url);
    draw();
    thrd_t thread;
    if (thrd_create(&thread, page_loader, l) == thrd_success) {
        thrd_detach(thread);
        return;
    }
    loading = false;                            /* (no thread: the window never waits, so none) */
    snprintf(l->r.error, sizeof(l->r.error), "no thread to load the page");
    show_page(l);
    load_free(l);
}

static void open_page(const char *url, bool record, int at)
{
    begin_load(url, record, at, false);
}

/* Stop: the load going on is dropped (its thread ends by itself, within
 * http.c's limit); the page shown stays. */
static void stop_load(void)
{
    if (!loading)
        return;
    load_id++;
    loading = false;
    if (print_text)
        printf("ibrowser: stopped\n");
    char shown[URL_MAX * 2];
    snprintf(shown, sizeof(shown), "%s%s", page_url, page_part);
    window_field_set_text(address, shown);
    set_status(page.title[0] ? page.title : page_url[0] ? page_url : "iBrowser");
    draw();
}

/* The page a load brought (or why there is none), in place of the one
 * shown: under the page's lock. */
static void show_page(struct load *l)
{
    const char *url = l->url;
    if (l->record && page_url[0]) {
        if (history_count == HISTORY) {
            memmove(history, history + 1, sizeof(history[0]) * (HISTORY - 1));
            history_count--;
        }
        snprintf(history[history_count].url, sizeof(history[0].url), "%s%s", page_url, page_part);
        history[history_count].scroll = scroll;
        history_count++;
    }
    if (l->back && history_count > 0)
        history_count--;
    int at = l->at;
    size_t whole = strcspn(url, "#");
    snprintf(page_part, sizeof(page_part), "%s", url + whole);
    generation++;                               /* (27e) its pictures still coming are dropped */
    resources_free();                           /* (27q) the old page's files; the new one's fetched ahead */
    for (int i = 0; i < l->pre_n; i++) {
        struct resource *res = resource_add(l->pre[i].url);
        if (res) {
            res->r = l->pre[i].r, res->got = l->pre[i].got;
            l->pre[i].r.body = 0, l->pre[i].r.headers = 0;
        }
    }
    commit_editor();                            /* (27c) the old page's controls go with it */
    focus = list_of = 0;
    hovered = pressed_on = press_control = 0;   /* (27q) */
    selecting = false;
    free(focus_value);
    focus_value = 0;
    hover = -1;
    char *html = 0;
    long n = 0;
    struct response *rp = &l->r, r = *rp;       /* (r's body is ours from here: rp's is cleared) */
    rp->body = 0, rp->headers = 0;
    bool picture = false, failed = false;
    if (!strcmp(url, "about:home") || !strcmp(url, "about:")) {
        memset(&r, 0, sizeof(r));
        snprintf(r.url, sizeof(r.url), "about:home");
        html = copy_text(HOME_PAGE, sizeof(HOME_PAGE) - 1);
        n = (long)sizeof(HOME_PAGE) - 1;
        set_status("iBrowser");
    } else if (!l->got) {
        char shown[URL_MAX * 2], why[400];
        escape(url, shown, sizeof(shown));
        escape(r.error, why, sizeof(why));
        size_t size = strlen(shown) + strlen(why) + 200;
        html = malloc(size);
        if (html)
            n = snprintf(html, size, "<title>Cannot show the page</title><h2>Cannot show the "
                         "page</h2><p>%s</p><p>%s</p>", shown, why);
        printf("ibrowser: %s: %s\n", url, r.error);
        set_status(r.error);
        failed = true;
        snprintf(r.url, sizeof(r.url), "%s", url);
        free(r.body);
        r.body = 0;
    } else {
        if (starts_word(r.type, "image/")) {
            picture = true;
            char shown[URL_MAX * 2];
            escape(r.url, shown, sizeof(shown));
            size_t size = strlen(shown) + 64;
            html = malloc(size);
            if (html)
                n = snprintf(html, size, "<body style=\"margin:0\"><img src=\"%s\"></body>", shown);
        } else if (!strcmp(r.type, "text/plain") || !strcmp(r.type, "text/css")) {
            html = plain_page(r.body, r.length);
            n = html ? (long)strlen(html) : 0;
        } else {
            html = r.body;                      /* (HTML, or what is read as HTML) */
            n = r.length;
            r.body = 0;
        }
        if (r.encoding[0])
            printf("ibrowser: loaded %s (%d, %s, %ld bytes, %s from %ld)\n", r.url, r.status, r.type,
                   r.length, r.encoding, r.packed);
        else
            printf("ibrowser: loaded %s (%d, %s, %ld bytes)\n", r.url, r.status, r.type,
                   r.length);
        char shown[URL_MAX * 2];
        snprintf(shown, sizeof(shown), "%s%s", r.url, page_part);
        window_field_set_text(address, shown);
    }
    snprintf(page_url, sizeof(page_url), "%s", r.url);
    dom_end();                                  /* (27p) the old page's scripts first */
    html_free(tree);
    tree = html ? html_parse(html, (size_t)n) : 0;
    free(html);
    sheets_ready = false;
    if (tree) {
        char shown[URL_MAX * 2];
        snprintf(shown, sizeof(shown), "%s%s", page_url, page_part);
        css_scripting = !picture && !failed && strcmp(r.url, "about:home");     /* (27q: <noscript>) */
        dom_start(tree, shown, content_w(), content_h(), css_scripting);
        dom_run_scripts();                      /* (27p) before the page is styled */
        dom_loaded(1);                          /* (27q) DOMContentLoaded */
    }
    if (picture && tree) {                      /* the picture itself: already here */
        struct node *img = node_find(tree, "img");
        struct image *decoded = calloc(1, sizeof(*decoded));
        if (img && decoded && image_load_memory(r.body, r.length, decoded) == 0)
            img->picture = decoded;
        else
            free(decoded);
    }
    response_free(&r);
    if (tree) {
        if (!sheets_ready)                      /* (a script's geometry may have read them) */
            style_sheets();
        css_apply(tree, sheet, content_w(), content_h());
    }
    scroll = 0;
    page_failed = failed;
    load_at = at;
    scrolled_by_hand = false;
    relayout();
    if (at >= 0)                                /* (the place asked for, as far as it goes yet) */
        scroll = at > max_scroll() ? max_scroll() : at;
    else if (page_part[0] && page_anchor(&page, page_part + 1) >= 0)
        scroll = part_y(page_part);
    if (!failed)
        set_status(page.title[0] ? page.title : page_url);
    dom_changed = false;                        /* (styled and laid out above) */
    draw();
    unsigned shown_generation = generation;
    if (!tree || !start_pictures())
        finish_page(0);
    if (generation == shown_generation)
        after_dom();                            /* (27p) a script sent the page elsewhere, ... */
}

static double monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void wake_timers(void);

/* (27q) The page's first <meta http-equiv="refresh" content="SECONDS[; url=URL]">
 * (one in a <noscript> counts only when scripts do not run): the timer
 * thread opens URL (or the page again) once the seconds are over, in the
 * page's place in the history. */
static void schedule_refresh(void)
{
    refresh.on = false;
    for (struct node *n = tree; n; n = node_next(n, tree)) {
        const char *how = n->kind == NODE_ELEMENT && !strcmp(n->tag, "meta") ? node_attr(n, "http-equiv") : 0;
        const char *c = how && same_word(how, "refresh") ? node_attr(n, "content") : 0;
        if (!c)
            continue;
        bool hidden = false;
        for (struct node *a = n->parent; a && css_scripting; a = a->parent)
            hidden |= a->kind == NODE_ELEMENT && !strcmp(a->tag, "noscript");
        if (hidden)
            continue;
        while (is_space(*c))
            c++;
        if (!(*c >= '0' && *c <= '9') && *c != '.')
            continue;
        double seconds = strtod(c, (char **)&c);
        if (seconds != seconds || seconds < 0 || seconds > 86400)
            continue;
        while (*c && *c != ';' && *c != ',')
            c++;
        while (*c == ';' || *c == ',' || is_space(*c))
            c++;
        if (starts_word(c, "url")) {            /* (url= is optional) */
            const char *e = c + 3;
            while (is_space(*e))
                e++;
            if (*e == '=') {
                c = e + 1;
                while (is_space(*c))
                    c++;
            }
        }
        char target[URL_MAX];
        size_t n_url = strlen(c);
        if (n_url && (*c == '"' || *c == '\'')) {
            const char *end = strchr(c + 1, *c);
            n_url = end ? (size_t)(end - c - 1) : strlen(c + 1);
            c++;
        }
        snprintf(target, sizeof(target), "%.*s", (int)(n_url < sizeof(target) ? n_url : sizeof(target) - 1), c);
        char here[URL_MAX * 2];
        snprintf(here, sizeof(here), "%s%s", page_url, page_part);
        if (!target[0])
            snprintf(target, sizeof(target), "%s", here);
        if (!url_resolve(page_url, target, refresh.url, sizeof(refresh.url)))
            continue;
        refresh.generation = generation;
        refresh.due = monotonic_ms() + seconds * 1000;
        refresh.on = true;
        wake_timers();
        return;
    }
}

/* The page is complete (its pictures in, or none): the place it was asked
 * for again (pictures move things) unless the reader has scrolled, and the
 * report for -t. */
static void finish_page(int pictures)
{
    if (pictures || (tree && node_find(tree, "img"))) {
        css_apply(tree, sheet, content_w(), content_h());
        relayout();
    }
    dom_loaded(2);                              /* (27q) the window's load: before the report */
    if (dom_changed && tree) {
        dom_changed = false;
        css_apply(tree, sheet, content_w(), content_h());
        relayout();
    }
    if (!scrolled_by_hand)
        scroll = load_at >= 0 ? (load_at > max_scroll() ? max_scroll() : load_at) : part_y(page_part);
    if (!page_failed)
        set_status(page.title[0] ? page.title : page_url);
    if (print_text && pictures)
        printf("ibrowser: %d picture(s) shown\n", pictures);
    schedule_refresh();                         /* (27q) */
    report();
    if (page_part[0] || scroll)
        report_scroll();
    draw();
}

static void back(void)
{
    if (!history_count)
        return;
    char url[URL_MAX];                          /* (taken out of the history once it is shown, 27q) */
    snprintf(url, sizeof(url), "%s", history[history_count - 1].url);
    begin_load(url, false, history[history_count - 1].scroll, true);
}

static void scroll_to(int y)
{
    if (y > max_scroll())
        y = max_scroll();
    if (y < 0)
        y = 0;
    if (y != scroll) {
        scroll = y;
        dom_scroll_y = scroll;
        scroll_event = true;                    /* (27q: fired when the scroll is over) */
        scrolled_by_hand = true;                /* (27e) pictures coming now move nothing */
        if (print_text)
            printf("ibrowser: scrolled by hand to y %d\n", scroll);
        draw();
    }
}

static void follow(int link)
{
    if (link < 0 || link >= page.link_count)
        return;
    char url[URL_MAX];
    snprintf(url, sizeof(url), "%s", page.links[link].href);
    printf("ibrowser: following %s\n", url);
    open_page(url, true, -1);
}

/* --- what scripts asked for (27q) --- */

static void send_form(struct node *form, struct node *submitter);

/* After the page's scripts ran (an event, a timer): where they sent the
 * page, a form to send, the page styled and laid out again if they changed
 * it, a scroll asked for. True if another page is shown now. */
static bool after_dom(void)
{
    if (dom_navigate_to[0]) {
        char to[URL_MAX];
        snprintf(to, sizeof(to), "%s", dom_navigate_to);
        dom_navigate_to[0] = 0;
        bool link = dom_navigate_link;
        dom_navigate_link = false;
        dom_submit_pending = false;
        dom_scroll_request = -1;
        if (link)
            printf("ibrowser: following %s\n", to);
        if (navigations < 8) {                  /* (pages sending us on: 8 in a row at most) */
            navigations++;
            open_page(to, true, -1);
            navigations--;
        }
        return true;
    }
    if (dom_submit_pending) {
        dom_submit_pending = false;
        dom_scroll_request = -1;
        send_form(dom_submit_form, dom_submitter);
        return true;
    }
    bool redraw = false;
    if (dom_changed && tree) {
        dom_changed = false;
        if (!sheets_ready)
            style_sheets();
        css_apply(tree, sheet, content_w(), content_h());
        relayout();
        if (!page_failed)
            set_status(page.title[0] ? page.title : page_url);
        redraw = true;
    }
    if (editing) {                              /* (a script set the field's value) */
        struct control *k = control_of(focus);
        const char *v = k && k->value ? k->value : "";
        if (k && strcmp(v, window_field_text(editor))) {
            window_field_set_text(editor, v);
            redraw = true;
        }
    }
    if (dom_scroll_request >= 0) {
        int y = dom_scroll_request;
        dom_scroll_request = -1;
        int was = scroll;
        scroll_to(y);
        redraw = redraw && scroll == was;       /* (scroll_to drew it) */
    }
    if (redraw)
        draw();
    return false;
}

/* The page's scroll event, once a scroll is over (not inside it). */
static void scroll_events(void)
{
    if (!scroll_event)
        return;
    scroll_event = false;
    dom_fire(dom_document(), "scroll", 0);
    after_dom();
}

/* The keyboard to e (or none): blur and focusout for the control that had
 * it (and change if its text changed meanwhile), focus and focusin for e.
 * Scripts may run: no item pointers across it. */
static void focus_to(struct node *e)
{
    if (e == focus)
        return;
    struct node *old = focus;
    commit_editor();
    focus = e;
    dom_focused = e;
    struct dom_event d;
    memset(&d, 0, sizeof(d));
    if (old) {
        struct control *k = control_of(old);
        if (k && focus_value && (k->kind == CONTROL_TEXT || k->kind == CONTROL_PASSWORD ||
                                 k->kind == CONTROL_TEXTAREA) &&
            strcmp(focus_value, k->value ? k->value : ""))
            dom_fire(old, "change", 0);
        d.related = e;
        dom_fire(old, "blur", &d);
        dom_fire(old, "focusout", &d);
    }
    free(focus_value);
    focus_value = 0;
    if (e && focus == e) {                      /* (unless a listener moved it on) */
        struct control *k = control_of(e);
        focus_value = strdup(k && k->value ? k->value : "");
        d.related = old;
        dom_fire(e, "focus", &d);
        dom_fire(e, "focusin", &d);
    }
}

/* --- form controls (27c): input --- */

static bool sendable(const struct node *e)
{
    struct control *k = control_of((struct node *)e);
    return k && k->kind != CONTROL_HIDDEN && !node_attr(e, "disabled");
}

/* Scroll so item i is wholly shown. */
static void show_item(int i)
{
    const struct item *it = &page.items[i];
    if (it->fixed)
        return;                                 /* (always in view) */
    int y = scroll;
    if (it->y + it->h > y + content_h())
        y = it->y + it->h - content_h() + 8;
    if (it->y < y)
        y = it->y - 8;
    scroll_to(y);
}

static void set_focus(struct node *e, bool select_all)
{
    commit_editor();
    focus_to(e);                                /* (27q: its events first: they may change the page) */
    if (focus != e)
        return;
    list_of = 0;
    struct control *k = control_of(e);
    int i = item_of(e);
    if (k && i >= 0) {
        show_item(i);
        if (k->kind == CONTROL_TEXT) {
            const struct item *it = &page.items[i];
            window_field_place(editor, it->x, item_top(it, scroll) + TOOLBAR_H, it->w);
            window_field_set_text(editor, k->value ? k->value : "");
            window_field_focus(editor, select_all);
            editing = true;
        }
    }
    draw();
}

/* Enter in a field: a click on its form's first submit button, or its
 * form's submit event (27q) */
static void submit(struct node *control, struct node *submitter)
{
    struct control *edited = editing ? control_of(focus) : 0;
    if (edited)                                 /* (the field keeps the keyboard: a listener may cancel) */
        control_set_text(edited, window_field_text(editor));
    struct node *form = form_of(control, tree);
    if (!submitter)                             /* (Enter in a field: the form's first button) */
        for (struct node *n = form ? form : tree; n; n = node_next(n, form ? form : tree)) {
            struct control *k = is_control(n) ? control_of(n) : 0;
            if (k && k->kind == CONTROL_SUBMIT && form_of(n, tree) == form) {
                submitter = node_attr(n, "disabled") ? 0 : n;
                break;
            }
        }
    struct dom_event d;
    memset(&d, 0, sizeof(d));
    d.mods = mods_held;
    if (submitter)
        dom_click(submitter, &d);
    else
        dom_submit(form, 0);
    after_dom();
}

/* Send form (its submit event is over) */
static void send_form(struct node *form, struct node *submitter)
{
    commit_editor();
    char url[URL_MAX], *body;
    if (!form_submission(form, tree, submitter, page_url, url, sizeof(url), &body)) {
        set_status("The form cannot be sent (its address is too long)");
        draw();
        return;
    }
    if (print_text) {
        if (body)
            printf("ibrowser: sending POST %s (%zu bytes): %s\n", url, strlen(body), body);
        else
            printf("ibrowser: sending GET %s\n", url);
    }
    free(post_body);
    post_body = body, post_n = body ? strlen(body) : 0;
    open_page(url, true, -1);
}

static void describe_now(struct node *e)
{
    if (print_text && is_control(e)) {
        char what[600];
        control_describe(e, what, sizeof(what));
        printf("ibrowser: now %s\n", what);
    }
}

/* (27q) a click changed a control (dom_click): told, drawn */
static void control_changed(struct node *e)
{
    describe_now(e);
    draw();
}

/* (27q) a select's choice changed: its input and change events */
static void select_changed(struct node *e)
{
    dom_fire(e, "input", 0);
    dom_fire(e, "change", 0);
    after_dom();
}

/* (27q) a script's focus() or blur() */
static void script_focus(struct node *e)
{
    if (e)
        set_focus(e, false);
    else {
        focus_to(0);
        draw();
    }
}

/* A control used (a click, Space or Enter on it): a select opens its list,
 * the others are clicked (the click event, then what they do: 27q). */
static void use_control(struct node *e)
{
    struct control *k = control_of(e);
    if (!k || !sendable(e))
        return;
    if (k->kind != CONTROL_SELECT) {
        struct dom_event d;
        memset(&d, 0, sizeof(d));
        d.mods = mods_held;
        dom_click(e, &d);                       /* (control_changed tells what changed) */
        after_dom();
        return;
    }
    list_of = list_of ? 0 : e;
    list_at = k->selected;
    if (print_text) {
        char what[600];
        control_describe(e, what, sizeof(what));
        printf("ibrowser: now %s\n", what);
    }
    draw();
}

/* Tab: the next control that can be used (after the last: the first). */
static void tab(void)
{
    int at = item_of(focus), n = page.count;
    for (int step = 1; step <= n; step++) {
        int i = ((at < 0 ? -1 : at) + step) % n;
        if (page.items[i].kind == ITEM_CONTROL && sendable(page.items[i].node)) {
            set_focus(page.items[i].node, true);
            return;
        }
    }
}

/* A typed byte for the focused control that is not in the editor:
 * passwords and textareas type at their end; Space uses the rest; Up and
 * Down change a select. */
static int form_esc;

static void control_key(int byte)
{
    struct control *k = control_of(focus);
    if (!k)
        return;
    if (form_esc == 1) {
        form_esc = byte == '[' || byte == 'O' ? 2 : 0;
        return;
    }
    if (form_esc == 2) {
        if (byte >= '0' && byte <= '9')
            return;
        form_esc = 0;
        if (k->kind == CONTROL_SELECT && (byte == 'A' || byte == 'B')) {
            struct node *options[256];
            int count = select_options(focus, options, 256), *at = list_of ? &list_at : &k->selected;
            count = count > 256 ? 256 : count;
            int was = *at;
            *at += byte == 'A' ? -1 : 1;
            *at = *at < 0 ? 0 : *at >= count ? count - 1 : *at;
            if (!list_of && print_text) {
                char what[600];
                control_describe(focus, what, sizeof(what));
                printf("ibrowser: now %s\n", what);
            }
            draw();
            if (!list_of && *at != was)
                select_changed(focus);          /* (27q) */
        }
        return;
    }
    if (byte == 27) {
        form_esc = 1;
        return;
    }
    if (k->kind == CONTROL_PASSWORD || k->kind == CONTROL_TEXTAREA) {
        char *v = k->value ? k->value : copy_text("", 0);
        if (!v)
            return;
        size_t n = strlen(v);
        if (byte == 8 || byte == 127) {
            while (n > 0 && ((unsigned char)v[n - 1] & 0xC0) == 0x80)
                n--;
            if (n > 0)
                n--;
            v[n] = 0;
        } else if (byte == '\r' || byte == '\n') {
            if (k->kind == CONTROL_PASSWORD) {
                k->value = v;
                submit(focus, 0);
                return;
            }
            v = join_text(v, "\n");
        } else if (byte >= 32) {
            char one[2] = { (char)byte, 0 };
            if (n < 65536)
                v = join_text(v, one);
        }
        k->value = v;
        draw();
        return;
    }
    if (k->kind == CONTROL_SELECT && list_of && (byte == '\r' || byte == ' ')) {
        bool changed = k->selected != list_at;
        k->selected = list_at;
        list_of = 0;
        if (print_text) {
            char what[600];
            control_describe(focus, what, sizeof(what));
            printf("ibrowser: now %s\n", what);
        }
        draw();
        if (changed)
            select_changed(focus);
        return;
    }
    if (byte == ' ' || (byte == '\r' && k->kind != CONTROL_CHECKBOX && k->kind != CONTROL_RADIO &&
                        k->kind != CONTROL_TEXT))
        use_control(focus);
    else if (byte == '\r')
        submit(focus, 0);                       /* (a text field's too: its form's implicit submission) */
}

/* A press on the page at (px, py) (page coordinates): true if a control
 * (or an open list) took it. */
static bool form_press(const struct window_event *ev, int px, int py)
{
    struct node *was_open = list_of;
    int lx, ly, lw, lh, rows = list_rect(TOOLBAR_H, &lx, &ly, &lw, &lh);
    if (rows > 0) {
        if (ev->x >= lx && ev->x < lx + lw && ev->y >= ly && ev->y < ly + lh) {
            int first = list_at >= rows ? list_at - rows + 1 : 0;
            int r = (ev->y - ly - 1) / LIST_ROW;
            struct control *k = control_of(list_of);
            bool changed = false;
            if (k && r >= 0 && r < rows) {
                changed = k->selected != first + r;
                k->selected = first + r;
            }
            struct node *select = list_of;
            focus = dom_focused = list_of;
            list_of = 0;
            if (print_text && k) {
                char what[600];
                control_describe(focus, what, sizeof(what));
                printf("ibrowser: now %s\n", what);
            }
            draw();
            if (changed)
                select_changed(select);         /* (27q) */
            return true;
        }
        list_of = 0;                            /* a press elsewhere closes it */
    }
    for (int k = page.count - 1; k >= 0; k--) {         /* (the topmost first) */
        const struct item *it = &page.items[page.order ? page.order[k] : k];
        int top = item_top(it, scroll) + scroll;
        if (it->kind != ITEM_CONTROL || px < it->x || px >= it->x + it->w || py < top ||
            py >= top + it->h)
            continue;
        struct node *e = it->node;
        if (!sendable(e))
            return true;
        struct control *k = control_of(e);
        if (k->kind == CONTROL_TEXT) {
            set_focus(e, false);
            if (focus == e && editing)
                window_field_event(editor, ev); /* (the cursor where the press was) */
            return true;
        }
        commit_editor();
        focus_to(e);                            /* (27q: its focus events; then it may be gone) */
        if (focus != e)
            return true;
        if (e == was_open) {                    /* (its own box again: the list closes) */
            draw();
            return true;
        }
        if (k->kind == CONTROL_SELECT)
            use_control(e);                     /* (its list opens at the press) */
        else if (k->kind != CONTROL_PASSWORD && k->kind != CONTROL_TEXTAREA)
            press_control = e;                  /* (27q: used at the release, by its click) */
        draw();
        return true;
    }
    if (focus) {                                /* a press elsewhere: no control has the keys */
        commit_editor();
        focus_to(0);
        draw();
    }
    return false;
}

/* --- input --- */

static int esc_state;                   /* 0, 1 after ESC, 2 after ESC [ */
static char esc_number;

static void key(int byte)
{
    if (byte == 9) {                            /* Tab: the next control (27c) */
        tab();
        return;
    }
    if (focus && !editing && byte != 12) {
        control_key(byte);
        return;
    }
    if (esc_state == 1) {
        esc_state = byte == '[' || byte == 'O' ? 2 : 0;
        esc_number = 0;
        return;
    }
    if (esc_state == 2) {
        if (byte >= '0' && byte <= '9') {
            esc_number = (char)byte;
            return;
        }
        esc_state = 0;
        int page_step = content_h() - LINE_STEP;
        if (byte == 'A')
            scroll_to(scroll - LINE_STEP);
        else if (byte == 'B')
            scroll_to(scroll + LINE_STEP);
        else if (byte == 'H' || (byte == '~' && esc_number == '1'))
            scroll_to(0);
        else if (byte == 'F' || (byte == '~' && esc_number == '4'))
            scroll_to(max_scroll());
        else if (byte == '~' && esc_number == '5')
            scroll_to(scroll - page_step);
        else if (byte == '~' && esc_number == '6')
            scroll_to(scroll + page_step);
        return;
    }
    if (byte == 27) {
        esc_state = 1;
        return;
    }
    if (byte == 12) {                           /* Ctrl-L: the address line, all selected */
        window_field_focus(address, 1);
        draw();
        return;
    }
    if (byte == ' ')
        scroll_to(scroll + content_h() - LINE_STEP);
    else if (byte == 8 || byte == 127)
        back();
    else if (byte == 'q')
        exit(0);
}

/* --- the page's mouse events (27q) --- */

/* The element at (x, y) of the window (the body if there is none). */
static struct node *element_at(int wx, int wy)
{
    if (!tree)
        return 0;
    struct node *n = page_node_at(&page, tree, wx, wy - TOOLBAR_H + scroll, scroll);
    if (!n)
        n = node_find(tree, "body");
    return n ? n : tree;
}

static bool inside(const struct node *a, const struct node *n)      /* n is a or in it */
{
    for (; n; n = n->parent)
        if (n == a)
            return true;
    return false;
}

static struct node *common_ancestor(struct node *a, struct node *b)
{
    for (; a; a = a->parent)
        if (inside(a, b))
            return a;
    return 0;
}

static void mouse_event(struct dom_event *d, const struct window_event *ev, int button, int detail)
{
    memset(d, 0, sizeof(*d));
    d->x = ev->x;
    d->y = ev->y - TOOLBAR_H;
    d->button = button;
    d->buttons = (int)(ev->buttons & 7);        /* (left 1, right 2, middle 4: as the web's) */
    d->mods = mods_held;
    d->detail = detail;
}

/* A pointer event and its mouse one (none while a pointerdown said so):
 * false if one was canceled. */
static bool mouse_pair(struct node *t, const char *pointer_type, const char *mouse_type, struct dom_event *d)
{
    bool ok = dom_fire(t, pointer_type, d);
    if (mouse_type && !mouse_suppressed)
        ok = dom_fire(t, mouse_type, d) && ok;
    return ok;
}

static bool watching_moves(void)
{
    static const char *const types[] = { "pointermove", "mousemove", "pointerover", "mouseover", "pointerout",
                                         "mouseout", "pointerenter", "mouseenter", "pointerleave", "mouseleave" };
    for (int i = 0; i < 10; i++)
        if (dom_wants(types[i]))
            return true;
    return false;
}

/* The pointer onto n: out and leave for what it left, over and enter for
 * what it came into. */
static void hover_to(struct node *n, struct dom_event *d)
{
    struct node *old = hovered;
    if (n == old)
        return;
    hovered = n;
    if (old) {
        d->related = n;
        mouse_pair(old, "pointerout", "mouseout", d);
        for (struct node *a = old; a && !inside(a, n); a = a->parent)
            mouse_pair(a, "pointerleave", "mouseleave", d);
    }
    if (n) {
        d->related = old;
        mouse_pair(n, "pointerover", "mouseover", d);
        struct node *chain[256];
        int k = 0;
        for (struct node *a = n; a && !inside(a, old) && k < 256; a = a->parent)
            chain[k++] = a;
        while (k-- > 0)                         /* (from the outermost in) */
            mouse_pair(chain[k], "pointerenter", "mouseenter", d);
    }
    d->related = 0;
}

static bool selectable;                 /* the press may select text (not one on a control) */

static void pointer(const struct window_event *ev, unsigned *held, bool *dragging)
{
    unsigned was = *held;
    bool pressed = (ev->buttons & 1) && !(was & 1);
    bool right_pressed = (ev->buttons & 2) && !(was & 2), right_released = !(ev->buttons & 2) && (was & 2);
    *held = ev->buttons;
    int top = TOOLBAR_H;
    if (*dragging) {
        if (!(ev->buttons & 1)) {
            *dragging = false;
            return;
        }
        scroll_to(window_scrollbar_hit(ev->y - top, content_h(), ev->y - top, content_h(),
                                       page.height > content_h() ? page.height : content_h()));
        return;
    }
    bool on_page = ev->y >= top && ev->y < top + content_h() && ev->x < content_w();
    bool moved = ev->x != pointer_x || ev->y != pointer_y;
    pointer_x = ev->x, pointer_y = ev->y;
    struct dom_event d;
    struct node *t = 0;
    bool releasing = selecting && !(ev->buttons & 1);
    if (on_page && tree && (pressed || releasing || right_pressed || right_released || watching_moves())) {
        t = element_at(ev->x, ev->y);           /* (27q) the page's mouse events */
        mouse_event(&d, ev, 0, 0);
        hover_to(t, &d);
        if (moved)
            mouse_pair(t, "pointermove", "mousemove", &d);
        if (right_pressed || right_released) {
            d.button = 2;
            mouse_pair(t, right_pressed ? "pointerdown" : "pointerup", right_pressed ? "mousedown" : "mouseup", &d);
            if (right_released)
                dom_fire(t, "contextmenu", &d);
        }
        if (after_dom())
            return;                             /* (another page) */
    }
    int link = -1;
    if (on_page)
        link = page_link_at(&page, ev->x, ev->y - top + scroll, scroll);
    if (link != hover) {
        hover = link;
        draw();
    }
    if (selecting) {                            /* (15y) a drag selecting */
        if (ev->buttons & 1) {
            int dx = ev->x - press_x, dy = ev->y - press_y;
            if (dx * dx + dy * dy > 16)
                moved_since_press = true;
            if (moved_since_press && selectable) {
                struct spot s = spot_at(ev->x, ev->y - top + scroll);
                if (s.item >= 0 && sel_a.item >= 0 && (!selected || s.item != sel_b.item ||
                                                       s.ch != sel_b.ch)) {
                    sel_b = s;
                    selected = true;
                    draw();
                }
            }
            return;
        }
        selecting = false;                      /* the release: a click if nothing moved */
        struct node *on = pressed_on, *control = press_control;
        pressed_on = press_control = 0;
        if (!tree) {
            if (!moved_since_press && press_link >= 0 && link == press_link)
                follow(link);
            return;
        }
        if (t) {
            mouse_event(&d, ev, 0, 1);
            mouse_pair(t, "pointerup", "mouseup", &d);
        }
        mouse_suppressed = false;
        struct node *c = !t || moved_since_press ? 0 : control && inside(control, t) ? control
                         : control ? 0 : on ? common_ancestor(on, t) : 0;
        if (c && dom_click(c, &d) && double_pending) {
            d.detail = 2;
            dom_fire(c, "dblclick", &d);        /* (after the second click) */
        }
        double_pending = false;
        after_dom();
        return;
    }
    if (!pressed)
        return;
    if (ev->y < TOOLBAR_H) {
        if (ev->x < BACK_W + 2)
            back();
        else if (ev->x < BACK_W + STOP_W)
            stop_load();                        /* (27q; nothing when no page loads) */
        return;
    }
    if (ev->x >= content_w() && ev->y >= top && ev->y < top + content_h()) {
        *dragging = true;
        scroll_to(window_scrollbar_hit(ev->y - top, content_h(), ev->y - top, content_h(),
                                       page.height > content_h() ? page.height : content_h()));
        return;
    }
    if (ev->y >= top && ev->y < top + content_h()) {      /* a press on the page */
        bool go_on = true;
        if (t) {
            mouse_event(&d, ev, 0, 1);
            go_on = dom_fire(t, "pointerdown", &d);
            mouse_suppressed = !go_on;
            if (go_on)
                go_on = dom_fire(t, "mousedown", &d);   /* (canceled: no focus, no selection) */
            if (after_dom())
                return;
        }
        pressed_on = t;
        press_control = 0;
        selecting = true;
        selectable = false;
        moved_since_press = false;
        press_x = ev->x, press_y = ev->y, press_link = link;
        if (!go_on)
            return;
        if (form_press(ev, ev->x, ev->y - top + scroll)) {
            selecting = press_control != 0;     /* (a field's editor takes the release) */
            return;
        }
        selectable = true;
        sel_a = sel_b = spot_at(ev->x, ev->y - top + scroll);
        if (selected) {
            selected = false;
            draw();
        }
    }
}

/* A double click on the page: the word under it. */
static void select_word(const struct window_event *ev)
{
    int top = TOOLBAR_H;
    if (ev->y < top || ev->y >= top + content_h() || ev->x >= content_w())
        return;
    struct spot s = spot_at(ev->x, ev->y - top + scroll);
    if (s.item < 0)
        return;
    const struct item *it = &page.items[s.item];
    const char *t = it->text;
    int n = item_chars(it), c0 = s.ch, c1 = s.ch;
    #define WORDCHAR(k) (t[char_offset(t, k)] != ' ')
    if (!WORDCHAR(s.ch))
        return;
    while (c0 > 0 && WORDCHAR(c0 - 1))
        c0--;
    while (c1 + 1 < n && WORDCHAR(c1 + 1))
        c1++;
    #undef WORDCHAR
    sel_a.item = sel_b.item = s.item;
    sel_a.ch = c0, sel_b.ch = c1;
    selected = true;
    selecting = false;
    draw();
}

/* Copy the selection: its pieces in reading order, a line break between
 * lines and a space where pieces of one line stand apart. */
static void copy_selection(void)
{
    static char text[65536];
    size_t n = 0;
    struct spot a, b;
    ordered(&a, &b);
    const struct item *prev = 0;
    for (int i = a.item; selected && i <= b.item && i < page.count; i++) {
        const struct item *it = &page.items[i];
        if (it->kind != ITEM_TEXT || it->decoration)
            continue;
        int c0 = i == a.item ? a.ch : 0, c1 = i == b.item ? b.ch + 1 : item_chars(it);
        size_t o0 = char_offset(it->text, c0), o1 = char_offset(it->text, c1);
        if (n + (o1 - o0) + 2 >= sizeof(text))
            break;
        if (prev && prev->line != it->line)
            text[n++] = '\n';
        else if (prev && prev->x + prev->w < it->x)
            text[n++] = ' ';
        memcpy(text + n, it->text + o0, o1 - o0);
        n += o1 - o0;
        prev = it;
    }
    if (n) {
        window_clipboard_set(win, text, n);
        if (print_text)
            printf("ibrowser: copied %zu bytes\n", n);
    }
}

static void choose_file(void)
{
    char path[URL_MAX];
    char folder[URL_MAX] = "/";
    if (!strncmp(page_url, "/", 1)) {
        snprintf(folder, sizeof(folder), "%s", page_url);
        char *slash = strrchr(folder, '/');
        if (slash)
            slash[slash == folder ? 1 : 0] = 0;
    }
    if (window_choose_file(win, "Open a page", folder, path, sizeof(path)) == 1)
        open_page(path, true, -1);
    else
        draw();
}

/* --- the page's key events (27q) --- */

/* A physical key's KeyboardEvent names: key (as the US layout types it,
 * shift held or not), code, keyCode. */
static void key_names(int usage, bool shift, char *key, size_t size, char *code, size_t csize, int *key_code)
{
    static const struct { int usage; const char *key, *code; int key_code; } NAMED[] = {
        { WKEY_ENTER, "Enter", "Enter", 13 }, { WKEY_ESCAPE, "Escape", "Escape", 27 },
        { WKEY_BACKSPACE, "Backspace", "Backspace", 8 }, { WKEY_TAB, "Tab", "Tab", 9 },
        { WKEY_SPACE, " ", "Space", 32 }, { WKEY_CAPSLOCK, "CapsLock", "CapsLock", 20 },
        { WKEY_INSERT, "Insert", "Insert", 45 }, { WKEY_HOME, "Home", "Home", 36 },
        { WKEY_PAGEUP, "PageUp", "PageUp", 33 }, { WKEY_DELETE, "Delete", "Delete", 46 },
        { WKEY_END, "End", "End", 35 }, { WKEY_PAGEDOWN, "PageDown", "PageDown", 34 },
        { WKEY_RIGHT, "ArrowRight", "ArrowRight", 39 }, { WKEY_LEFT, "ArrowLeft", "ArrowLeft", 37 },
        { WKEY_DOWN, "ArrowDown", "ArrowDown", 40 }, { WKEY_UP, "ArrowUp", "ArrowUp", 38 },
        { WKEY_LCTRL, "Control", "ControlLeft", 17 }, { WKEY_RCTRL, "Control", "ControlRight", 17 },
        { WKEY_LSHIFT, "Shift", "ShiftLeft", 16 }, { WKEY_RSHIFT, "Shift", "ShiftRight", 16 },
        { WKEY_LALT, "Alt", "AltLeft", 18 }, { WKEY_RALT, "AltGraph", "AltRight", 18 },
        { WKEY_LGUI, "Meta", "MetaLeft", 91 }, { WKEY_RGUI, "Meta", "MetaRight", 92 },
        { WKEY_PRINTSCREEN, "PrintScreen", "PrintScreen", 44 }, { WKEY_SCROLLLOCK, "ScrollLock", "ScrollLock", 145 },
        { WKEY_PAUSE, "Pause", "Pause", 19 },
    };
    static const struct { int usage; char plain, shifted; const char *code; int key_code; } SIGNS[] = {
        { WKEY_MINUS, '-', '_', "Minus", 189 }, { WKEY_EQUAL, '=', '+', "Equal", 187 },
        { WKEY_LBRACKET, '[', '{', "BracketLeft", 219 }, { WKEY_RBRACKET, ']', '}', "BracketRight", 221 },
        { WKEY_BACKSLASH, '\\', '|', "Backslash", 220 }, { WKEY_SEMICOLON, ';', ':', "Semicolon", 186 },
        { WKEY_APOSTROPHE, '\'', '"', "Quote", 222 }, { WKEY_GRAVE, '`', '~', "Backquote", 192 },
        { WKEY_COMMA, ',', '<', "Comma", 188 }, { WKEY_PERIOD, '.', '>', "Period", 190 },
        { WKEY_SLASH, '/', '?', "Slash", 191 },
    };
    snprintf(key, size, "Unidentified");
    snprintf(code, csize, "Unidentified");
    *key_code = 0;
    if (usage >= WKEY_A && usage <= WKEY_Z) {
        int c = 'a' + usage - WKEY_A;
        snprintf(key, size, "%c", shift ? c - 32 : c);
        snprintf(code, csize, "Key%c", c - 32);
        *key_code = c - 32;
        return;
    }
    if (usage >= WKEY_1 && usage <= WKEY_0) {
        int digit = usage == WKEY_0 ? 0 : usage - WKEY_1 + 1;
        snprintf(key, size, "%c", shift ? ")!@#$%^&*("[digit] : '0' + digit);
        snprintf(code, csize, "Digit%d", digit);
        *key_code = '0' + digit;
        return;
    }
    if (usage >= WKEY_F1 && usage <= WKEY_F12) {
        snprintf(key, size, "F%d", usage - WKEY_F1 + 1);
        snprintf(code, csize, "F%d", usage - WKEY_F1 + 1);
        *key_code = 112 + usage - WKEY_F1;
        return;
    }
    for (size_t i = 0; i < sizeof(NAMED) / sizeof(NAMED[0]); i++)
        if (NAMED[i].usage == usage) {
            snprintf(key, size, "%s", NAMED[i].key);
            snprintf(code, csize, "%s", NAMED[i].code);
            *key_code = NAMED[i].key_code;
            return;
        }
    for (size_t i = 0; i < sizeof(SIGNS) / sizeof(SIGNS[0]); i++)
        if (SIGNS[i].usage == usage) {
            snprintf(key, size, "%c", shift ? SIGNS[i].shifted : SIGNS[i].plain);
            snprintf(code, csize, "%s", SIGNS[i].code);
            *key_code = SIGNS[i].key_code;
            return;
        }
}

static struct node *key_target(void)
{
    struct node *body = tree ? node_find(tree, "body") : 0;
    return focus ? focus : body ? body : tree;
}

/* A physical key (WINDOW_KEY_DOWN/UP): keydown or keyup on the focused
 * control (or the body); a canceled keydown drops what the key types. */
static void page_key(const struct window_event *ev)
{
    static const int MODS[8] = { DOM_CTRL, DOM_SHIFT, DOM_ALT, DOM_META, DOM_CTRL, DOM_SHIFT, DOM_ALT, DOM_META };
    bool down = ev->type == WINDOW_KEY_DOWN;
    if (down && ev->key == WKEY_ESCAPE && loading) {
        stop_load();                            /* (27q) Escape stops a load (its bytes go nowhere) */
        swallowed_key = WKEY_ESCAPE;
        return;
    }
    if (ev->key >= WKEY_LCTRL && ev->key <= WKEY_RGUI) {
        int bit = MODS[ev->key - WKEY_LCTRL];
        mods_held = down ? mods_held | bit : mods_held & ~bit;
    }
    if (!down && swallowed_key == ev->key)
        swallowed_key = -1;
    if (!tree || window_field_focused(address))
        return;
    struct dom_event d;
    memset(&d, 0, sizeof(d));
    char key[32], code[32];
    key_names(ev->key, mods_held & DOM_SHIFT, key, sizeof(key), code, sizeof(code), &d.key_code);
    d.key = key, d.code = code, d.mods = mods_held;
    if (!dom_fire(key_target(), down ? "keydown" : "keyup", &d) && down)
        swallowed_key = ev->key;
    after_dom();
}

/* A typed byte: false if the page's keypress (printable ones and Enter)
 * was canceled, or its key's keydown was. */
static bool page_keypress(int byte)
{
    if (swallowed_key >= 0)
        return false;
    if (key_esc == 1)                           /* (an escape sequence's bytes are no keys of their own) */
        key_esc = byte == '[' || byte == 'O' ? 2 : 0;
    else if (key_esc == 2)
        key_esc = (byte >= '0' && byte <= '9') || byte == ';' ? 2 : 0;
    else if (byte == 27)
        key_esc = 1;
    else if (tree && (byte == '\r' || (byte >= 32 && byte < 127)) && !window_field_focused(address) &&
             dom_wants("keypress")) {
        char key[2] = { (char)byte, 0 };
        struct dom_event d;
        memset(&d, 0, sizeof(d));
        d.key = byte == '\r' ? "Enter" : key;
        d.code = "";
        d.key_code = d.char_code = byte;
        d.mods = mods_held;
        bool ok = dom_fire(key_target(), "keypress", &d);
        after_dom();
        return ok;
    }
    return true;
}

/* --- timers (27q): a thread of their own, as the pictures --- */

static int timer_thread(void *arg)
{
    (void)arg;
    int base;
    dom_set_stack_base(&base);
    mtx_lock(&lock);
    for (;;) {
        long wait = dom_next_timer();
        if (refresh.on) {                       /* (27q) a <meta> refresh: due, or sooner than the timers */
            double left = refresh.due - monotonic_ms();
            if (refresh.generation != generation)
                refresh.on = false;
            else if (left <= 0) {
                refresh.on = false;
                reloading = true;
                begin_load(refresh.url, false, -1, false);
                wait = 1;
            } else if (wait < 0 || left < wait)
                wait = (long)left + 1;
        }
        if (wait == 0) {
            dom_run_timers();
            scroll_events();
            after_dom();
            wait = 1;                           /* (the window's thread gets the lock in between) */
        }
        if (wait < 0) {
            cnd_wait(&timer_wake, &lock);
            continue;
        }
        struct timespec until;
        timespec_get(&until, TIME_UTC);
        until.tv_sec += wait / 1000;
        until.tv_nsec += (wait % 1000) * 1000000L;
        if (until.tv_nsec >= 1000000000L)
            until.tv_sec++, until.tv_nsec -= 1000000000L;
        cnd_timedwait(&timer_wake, &lock, &until);
    }
    return 0;
}

static void wake_timers(void)
{
    cnd_signal(&timer_wake);
}

/* (27p) A script asks for geometry: the page styled and laid out now. */
static void layout_for_scripts(void)
{
    if (!tree)
        return;
    if (!sheets_ready)
        style_sheets();
    css_apply(tree, sheet, content_w(), content_h());
    relayout();
}

int main(int argc, char **argv)
{
    dom_set_stack_base(&argc);
    dom_layout_hook = layout_for_scripts;
    dom_focus_hook = script_focus;              /* (27q) */
    dom_control_hook = control_changed;
    dom_timers_hook = wake_timers;
    dom_fetch_hook = script_file;               /* (27q: script files never fetched under the lock) */
    dom_request_hook = start_request;           /* (27r) fetch, XMLHttpRequest */
    const char *start = "about:home";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t"))
            print_text = dom_print = true;      /* (27q: script errors with the rest of -t's report) */
        else if (argv[i][0] == '-' && argv[i][1]) {
            dprintf(STDERR_FILENO, "usage: ibrowser [-t] [URL]\n");
            return 2;
        } else
            start = argv[i];
    }
    int w = 900, h = 640;                       /* (smaller on a small screen) */
    struct sys_display d;
    if (ritteros_display_info(&d) == 0) {
        if (w > (int)d.width - 140)
            w = (int)d.width - 140;
        if (h > (int)d.height - 180)
            h = (int)d.height - 180;
    }
    win = window_open(w < 300 ? 300 : w, h < 200 ? 200 : h, "iBrowser");
    if (!win) {
        dprintf(STDERR_FILENO, "ibrowser: cannot open a window: %s\n",
                errno == EACCES ? "the screen is another user's" : "is windowd running?");
        return 1;
    }
    static const struct window_menu_item FILE_ITEMS[] = {
        { M_OPEN, "Open File..." }, { M_RELOAD, "Reload" }, { M_BACK, "Back" }, { M_QUIT, "Quit" }
    };
    static const struct window_menu MENUS[] = { { "File", FILE_ITEMS, 4 } };
    window_set_menu(win, MENUS, 1);
    address = window_field_new(win, ADDRESS_X, 3, window_width(win) - ADDRESS_X - 6);
    editor = window_field_new(win, 0, 0, 100);
    if (!address || !editor)
        return 1;
    char first[URL_MAX];
    snprintf(first, sizeof(first), "%s", start);
    mtx_init(&lock, mtx_plain);
    cnd_init(&timer_wake);
    mtx_lock(&lock);                            /* (27e) held except while waiting */
    thrd_t timers;
    if (thrd_create(&timers, timer_thread, 0) == thrd_success)    /* (27q) */
        thrd_detach(timers);
    open_page(first, false, -1);
    struct window_event ev;
    unsigned held = 0;
    bool dragging = false;
    for (;;) {
        scroll_events();                        /* (27q) the last event's scroll */
        mtx_unlock(&lock);
        bool got = window_wait(win, &ev);
        mtx_lock(&lock);
        if (!got)
            break;
        if (ev.type == WINDOW_KEY_DOWN || ev.type == WINDOW_KEY_UP) {
            page_key(&ev);                      /* (27q) keydown, keyup */
            continue;
        }
        if (ev.type == WINDOW_KEY && !page_keypress(ev.key))
            continue;                           /* (a listener took the key) */
        int field = window_field_event(address, &ev);   /* the address line first */
        if (field == 1 && window_field_text(address)[0]) {
            char url[URL_MAX];
            snprintf(url, sizeof(url), "%s", window_field_text(address));
            open_page(url, true, -1);
            continue;
        }
        if (field >= 0) {
            if (ev.type == WINDOW_POINTER)
                held = ev.buttons;
            continue;
        }
        if (editing) {                          /* (27c) a page's text field has the keys */
            if (ev.type == WINDOW_KEY && (ev.key == 9 || ev.key == 12)) {
                key(ev.key);                    /* (Tab, Ctrl-L: the browser's) */
                continue;
            }
            char *before = ev.type == WINDOW_KEY ? strdup(window_field_text(editor)) : 0;
            int got = window_field_event(editor, &ev);
            if (got == 0 && before && strcmp(before, window_field_text(editor))) {
                struct control *k = control_of(focus);  /* (27q) the field's text now, and its input event */
                if (k)
                    control_set_text(k, window_field_text(editor));
                struct dom_event d;
                memset(&d, 0, sizeof(d));
                char typed[2] = { (char)ev.key, 0 };
                bool longer = strlen(window_field_text(editor)) > strlen(before);
                d.input_type = longer ? "insertText" : "deleteContentBackward";
                d.data = longer && ev.key >= 32 ? typed : 0;
                dom_fire(focus, "input", &d);
                after_dom();
            }
            free(before);
            if (got == 1) {
                submit(focus, 0);
                continue;
            }
            if (got == 0) {
                if (ev.type == WINDOW_POINTER)
                    held = ev.buttons;
                continue;
            }
            if (!window_field_focused(editor))  /* (a press elsewhere: it let go) */
                commit_editor();
        }
        switch (ev.type) {
        case WINDOW_KEY:
            if (ev.key == 3)                    /* Ctrl+C: the page's selection (15y) */
                copy_selection();
            else
                key(ev.key);
            break;
        case WINDOW_DOUBLE_CLICK:
            double_pending = true;              /* (27q: dblclick after the release) */
            select_word(&ev);
            break;
        case WINDOW_POINTER:
            pointer(&ev, &held, &dragging);
            break;
        case WINDOW_SCROLL: {
            struct dom_event d;                 /* (27q) the wheel event first: it may be canceled */
            memset(&d, 0, sizeof(d));
            d.x = pointer_x, d.y = pointer_y - TOOLBAR_H, d.mods = mods_held;
            d.delta_y = ev.y * WHEEL_STEP;
            if (dom_fire(hovered ? hovered : key_target(), "wheel", &d))
                scroll_to(scroll + ev.y * WHEEL_STEP);
            after_dom();
            break;
        }
        case WINDOW_RESIZE:
            if (window_resize(win, ev.x, ev.y) == 0) {
                if (tree)                       /* (27f: @media and vw follow the window) */
                    css_apply(tree, sheet, content_w(), content_h());
                relayout();
                draw();
                dom_resize(content_w(), content_h());   /* (27q) */
                after_dom();
            }
            break;
        case WINDOW_MENU:
            if (ev.menu == M_OPEN)
                choose_file();
            else if (ev.menu == M_RELOAD && page_url[0]) {
                char url[URL_MAX];
                snprintf(url, sizeof(url), "%s%s", page_url, page_part);
                reloading = true;               /* (so its "#part" does not make it a scroll only) */
                open_page(url, false, scroll);
            } else if (ev.menu == M_BACK)
                back();
            else if (ev.menu == M_QUIT)
                ev.type = WINDOW_CLOSED;
            break;
        }
        if (ev.type == WINDOW_CLOSED)
            break;
    }
    window_close(win);
    return 0;
}

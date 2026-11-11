/*
 * libwidget (see widget.h). A tree of struct widget: containers (row,
 * column, grid) hold children in a list; leaves draw themselves. Layout is
 * two passes: measure (each widget's natural size, children first) and
 * place (a rectangle each, spare length to the stretching children).
 * widget_run keeps the run's state (the window, the font, the pressed and
 * focused widgets, whether the window has the keyboard) in the root; a
 * change marks a widget to draw again (or the whole tree to lay out
 * again), done after each event, then the window is flushed once.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "draw.h"
#include "wfont.h"
#include "widget.h"
#include "window.h"

#define GAP 8                           /* between children, and the root's margin */
#define LABEL_WIDTH 400                 /* where a label wraps */
#define BUTTON_PAD_X 14
#define BUTTON_PAD_Y 6
#define BUTTON_MIN 80                   /* a button's width at least */
#define RING 3                          /* the focus ring, outside a control */
#define BAR_WIDTH 160                   /* a slider's and a progress bar's natural width */
#define TAB_PAD 12                      /* a tab title's room either side */
#define ROWS_SHOWN 8                    /* (41e-3) a list's natural height in rows */
#define ROW_PAD 6                       /* a row's text from its left edge, a cell's from the next */
#define BAR 10                          /* a scroll bar's width */
#define INDENT 16                       /* a tree's level */
#define COLUMNS_MAX 16
#define EDIT_PAD 4                      /* (41e-4) an editor's text from its edges */
#define BLINK_MS 530                    /* the cursor's blink */
#define BLINK_TIMER 0x7E41              /* (widget_run's timer's id) */
#define PASTE_MAX 65536

/* The colours: the user's theme and accent (41g-5, window_color), each
 * as the three components window_rgb and draw_rgba take. */
#define PARTS(k) (int)(window_color(k) >> 16 & 0xFF), (int)(window_color(k) >> 8 & 0xFF), \
                 (int)(window_color(k) & 0xFF)
#define BACKGROUND PARTS(WINDOW_COLOR_BACKGROUND)
#define FACE PARTS(WINDOW_COLOR_FACE)
#define FACE_PRESSED PARTS(WINDOW_COLOR_FACE_PRESSED)
#define TEXT PARTS(WINDOW_COLOR_TEXT)
#define EDGE PARTS(WINDOW_COLOR_EDGE)
#define ACCENT PARTS(WINDOW_COLOR_ACCENT)
#define PANEL PARTS(WINDOW_COLOR_PANEL)
#define HOVER PARTS(WINDOW_COLOR_HOVER)

enum kind { ROW, COLUMN, GRID, TABS, LABEL, BUTTON, CHECK, RADIO, SLIDER, PROGRESS, LIST, TABLE, TREE, EDITOR };

struct run {
    struct run *under;                  /* (41e-5) the run a dialog's run is inside, or 0 */
    struct widget *root;
    struct window *win;
    const struct font *font;
    struct widget *pressed, *focused;
    struct widget *hovered;             /* (41e-3) the control under the pointer */
    int px, py;                         /* the pointer's last place (the wheel scrolls what is there) */
    bool dragging_bar;                  /* a list's scroll bar held */
    bool ctrl, blink;                   /* (41e-4) Ctrl held; the cursor shown in its blink */
    int cursor_shape;                   /* the pointer's shape asked for (window_set_cursor) */
    bool has_keyboard, shift, quit, closed, relayout, drawn;
};

struct widget {
    enum kind kind;
    struct widget *parent, *first, *last, *next;
    struct widget *group;               /* (radio) the next of its group, round */
    char *text;
    int value, columns, stretch;
    int min, max;                       /* (41e-2) a slider's range; a progress bar's max */
    int header;                         /* (41e-2) tabs: the titles' row's height */
    /* (41e-3) a list's, table's or tree's rows from its text (a table's
     * cells split at tabs, its first row the header; a tree's level its
     * leading tabs, its branches open or not) */
    char **items;
    int nitems, top, hover_row, ncols;
    unsigned char *level, *open;
    int colw[COLUMNS_MAX];
    /* (41e-4) an editor: its text is `text`; the cursor and the other end
     * of the selection (bytes), the first line shown (top), its lines'
     * count, a one-line editor's sideways scroll, Up/Down's column */
    int cur, anchor, lines, scroll_x, prefer_x;
    /* (41e-5) a dialog's root: the button Escape presses, the one with the
     * keyboard at the start */
    struct widget *cancel, *start;
    void (*fn)(struct widget *w, void *data);
    void *data;
    int x, y, w, h;                     /* where it was placed */
    int nw, nh;                         /* its natural size */
    bool dirty;
    struct run *run;                    /* (the root, while it runs) */
};

static struct widget *make(enum kind kind, const char *text)
{
    struct widget *w = calloc(1, sizeof(*w));
    if (!w)
        return 0;
    w->kind = kind;
    if (text && !(w->text = strdup(text))) {
        free(w);
        return 0;
    }
    w->group = w;
    return w;
}

struct widget *widget_row(void) { return make(ROW, 0); }
struct widget *widget_column(void) { return make(COLUMN, 0); }

struct widget *widget_grid(int columns)
{
    struct widget *w = make(GRID, 0);
    if (w)
        w->columns = columns > 0 ? columns : 1;
    return w;
}

struct widget *widget_tabs(void) { return make(TABS, 0); }

struct widget *widget_slider(int min, int max, int value)
{
    struct widget *w = make(SLIDER, "");
    if (w) {
        w->min = min;
        w->max = max > min ? max : min;
        w->value = value < w->min ? w->min : value > w->max ? w->max : value;
    }
    return w;
}

struct widget *widget_progress(int max, int value)
{
    struct widget *w = make(PROGRESS, "");
    if (w) {
        w->max = max > 0 ? max : 1;
        w->value = value < 0 ? 0 : value > w->max ? w->max : value;
    }
    return w;
}

static void parse_rows(struct widget *w);

static struct widget *rows_widget(enum kind kind, const char *text, int columns)
{
    struct widget *w = make(kind, text ? text : "");
    if (w) {
        w->ncols = columns < 1 ? 1 : columns > COLUMNS_MAX ? COLUMNS_MAX : columns;
        w->value = -1;
        parse_rows(w);
    }
    return w;
}

struct widget *widget_list(const char *lines) { return rows_widget(LIST, lines, 1); }
struct widget *widget_table(int columns, const char *rows) { return rows_widget(TABLE, rows, columns); }
struct widget *widget_tree(const char *lines) { return rows_widget(TREE, lines, 1); }

struct widget *widget_editor(const char *utf8, int lines)
{
    struct widget *w = make(EDITOR, utf8 ? utf8 : "");
    if (w) {
        w->lines = lines < 1 ? 1 : lines > 200 ? 200 : lines;
        if (w->lines == 1)                      /* (one line: newlines as spaces) */
            for (char *c = w->text; *c; c++)
                if (*c == '\n')
                    *c = ' ';
        w->cur = w->anchor = (int)strlen(w->text);
        w->prefer_x = -1;
    }
    return w;
}

struct widget *widget_label(const char *utf8) { return make(LABEL, utf8 ? utf8 : ""); }
struct widget *widget_button(const char *utf8) { return make(BUTTON, utf8 ? utf8 : ""); }

struct widget *widget_check(const char *utf8, int on)
{
    struct widget *w = make(CHECK, utf8 ? utf8 : "");
    if (w)
        w->value = on != 0;
    return w;
}

static void choose(struct widget *w);

struct widget *widget_radio(const char *utf8, struct widget *group, int on)
{
    struct widget *w = make(RADIO, utf8 ? utf8 : "");
    if (!w)
        return 0;
    if (group && group->kind == RADIO) {        /* into the ring after group */
        w->group = group->group;
        group->group = w;
    }
    if (on)
        choose(w);
    return w;
}

static bool container(const struct widget *w)
{
    return w->kind == ROW || w->kind == COLUMN || w->kind == GRID || w->kind == TABS;
}

static bool rows_kind(const struct widget *w)
{
    return w->kind == LIST || w->kind == TABLE || w->kind == TREE;
}

static bool focusable(const struct widget *w)
{
    return w->kind == BUTTON || w->kind == CHECK || w->kind == RADIO || w->kind == SLIDER || w->kind == TABS ||
           rows_kind(w) || w->kind == EDITOR;
}

/* Is a child shown? (a tabs' page only while it is the open one) */
static bool shown(const struct widget *child)
{
    const struct widget *p = child->parent;
    if (!p || p->kind != TABS)
        return true;
    int i = 0;
    for (const struct widget *c = p->first; c && c != child; c = c->next)
        i++;
    return i == p->value;
}

static int children(const struct widget *w)
{
    int n = 0;
    for (const struct widget *c = w->first; c; c = c->next)
        n++;
    return n;
}

static struct widget *root_of(struct widget *w)
{
    while (w->parent)
        w = w->parent;
    return w;
}

static struct run *run_of(struct widget *w)
{
    return root_of(w)->run;
}

/* Draw it again after this event (or lay out everything again). */
static void changed(struct widget *w, bool size)
{
    struct run *r = run_of(w);
    w->dirty = true;
    if (r && size)
        r->relayout = true;
}

void widget_add(struct widget *parent, struct widget *child)
{
    if (!parent || !child || !container(parent) || child->parent || child == parent)
        return;
    child->parent = parent;
    if (parent->last)
        parent->last->next = child;
    else
        parent->first = child;
    parent->last = child;
    changed(parent, true);
}

void widget_stretch(struct widget *w, int weight)
{
    if (w) {
        w->stretch = weight > 0 ? weight : 0;
        changed(w, true);
    }
}

void widget_on(struct widget *w, void (*fn)(struct widget *w, void *data), void *data)
{
    if (w) {
        w->fn = fn;
        w->data = data;
    }
}

int widget_value(const struct widget *w)
{
    return w ? (w->kind == EDITOR ? w->cur : w->value) : 0;
}

/* A radio button chosen: the rest of its group cleared. */
static void choose(struct widget *w)
{
    for (struct widget *o = w->group; o != w; o = o->group)
        if (o->value) {
            o->value = 0;
            changed(o, false);
        }
    w->value = 1;
    changed(w, false);
}

static int data_rows(const struct widget *w);

/* A value kept within what the widget takes. */
static int clamp_value(const struct widget *w, int value)
{
    switch (w->kind) {
    case SLIDER:
        return value < w->min ? w->min : value > w->max ? w->max : value;
    case PROGRESS:
        return value < 0 ? 0 : value > w->max ? w->max : value;
    case TABS: {
        int n = children(w);
        return value < 0 ? 0 : value >= n ? (n ? n - 1 : 0) : value;
    }
    case EDITOR: {                                      /* (41e-4) the cursor, a byte of its text */
        int n = (int)strlen(w->text);
        return value < 0 ? 0 : value > n ? n : value;
    }
    case LIST:
    case TABLE:
    case TREE: {                                        /* (41e-3) a row, or -1 */
        int n = w->kind == TREE ? w->nitems : data_rows(w);
        return value < -1 ? -1 : value >= n ? n - 1 : value;
    }
    default:
        return value != 0;
    }
}

void widget_set_value(struct widget *w, int value)
{
    if (!w || (w->kind != CHECK && w->kind != RADIO && w->kind != SLIDER && w->kind != PROGRESS &&
               w->kind != TABS && !rows_kind(w) && w->kind != EDITOR))
        return;
    if (w->kind == EDITOR) {
        w->cur = w->anchor = clamp_value(w, value);
        changed(w, false);
        return;
    }
    if (w->kind == RADIO && value)
        choose(w);
    else {
        w->value = clamp_value(w, value);
        changed(w, w->kind == TABS);            /* (another page: everything drawn again) */
    }
}


const char *widget_text(const struct widget *w)
{
    if (w && rows_kind(w))                      /* (41e-3) the chosen row's text, or "" */
        return w->value >= 0 && w->value < data_rows(w) ? w->items[w->value + (w->kind == TABLE)] : "";
    return w && w->text ? w->text : "";
}

void widget_set_text(struct widget *w, const char *utf8)
{
    if (!w)
        return;                                 /* (any widget: a page's text is its tab's title) */
    char *t = strdup(utf8 ? utf8 : "");
    if (!t)
        return;
    free(w->text);
    w->text = t;
    if (rows_kind(w))
        parse_rows(w);
    if (w->kind == EDITOR) {                    /* (the cursor at its end) */
        if (w->lines == 1)
            for (char *c = w->text; *c; c++)
                if (*c == '\n')
                    *c = ' ';
        w->cur = w->anchor = (int)strlen(w->text);
        w->top = w->scroll_x = 0;
        changed(w, false);
        return;
    }
    changed(w, true);
}

void widget_quit(struct widget *root)
{
    if (root && root_of(root)->run)
        root_of(root)->run->quit = true;
}

void widget_free(struct widget *root)
{
    if (!root)
        return;
    for (struct widget *c = root->first, *next; c; c = next) {
        next = c->next;
        c->parent = 0;
        widget_free(c);
    }
    if (root->kind == RADIO && root->group != root) {   /* out of its group's ring */
        struct widget *o = root->group;
        while (o->group != root)
            o = o->group;
        o->group = root->group;
    }
    for (int i = 0; i < root->nitems; i++)
        free(root->items[i]);
    free(root->items);
    free(root->level);
    free(root->open);
    free(root->text);
    free(root);
}

/* --- (41e-3) rows: lists, tables, trees ----------------------------------- */

/* The text's lines as items (a tree's leading tabs as its level, taken
 * off); the chosen row and the first shown start again. */
static void parse_rows(struct widget *w)
{
    for (int i = 0; i < w->nitems; i++)
        free(w->items[i]);
    free(w->items), free(w->level), free(w->open);
    w->items = 0, w->level = w->open = 0, w->nitems = 0;
    int n = 1;
    for (const char *c = w->text; *c; c++)
        n += *c == '\n';
    if (!w->text[0])
        n = 0;
    w->items = calloc((size_t)n + 1, sizeof(char *));
    w->level = calloc((size_t)n + 1, 1);
    w->open = calloc((size_t)n + 1, 1);
    if (!w->items || !w->level || !w->open)
        return;
    const char *p = w->text;
    for (int i = 0; i < n; i++) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        int lv = 0;
        while (w->kind == TREE && lv < (int)len && p[lv] == '\t')
            lv++;
        w->level[i] = (unsigned char)(lv > 255 ? 255 : lv);
        w->items[i] = strndup(p + lv, len - (size_t)lv);
        if (!w->items[i])
            break;
        w->nitems = i + 1;
        p = end ? end + 1 : p + len;
    }
    w->value = -1;
    w->top = 0;
    w->hover_row = -1;
}

/* Rows a program chooses among: a table's are those under its header. */
static int data_rows(const struct widget *w)
{
    return w->kind == TABLE ? (w->nitems > 0 ? w->nitems - 1 : 0) : w->nitems;
}

/* The item shown at place k (a tree hides what is under a closed branch),
 * or -1; with k -1, how many are shown. */
static int shown_item(const struct widget *w, int k)
{
    int seen = 0;
    if (w->kind != TREE)
        return k < 0 ? data_rows(w) : k < data_rows(w) ? k : -1;
    int hide_below = 256;                       /* (a closed branch's level: what is deeper is hidden) */
    for (int i = 0; i < w->nitems; i++) {
        if (w->level[i] > hide_below)
            continue;
        hide_below = 256;
        if (seen == k)
            return i;
        seen++;
        if (i + 1 < w->nitems && w->level[i + 1] > w->level[i] && !w->open[i])
            hide_below = w->level[i];
    }
    return k < 0 ? seen : -1;
}

static int place_of(const struct widget *w, int item)
{
    for (int k = 0, n = shown_item(w, -1); k < n; k++)
        if (shown_item(w, k) == item)
            return k;
    return -1;
}

static bool branch(const struct widget *w, int i)
{
    return w->kind == TREE && i >= 0 && i + 1 < w->nitems && w->level[i + 1] > w->level[i];
}

/* One cell of a table's row: its text up to the next tab, copied. */
static void cell_text(const char *row, int col, char *out, size_t size)
{
    for (int c = 0; c < col && row; c++) {
        row = strchr(row, '\t');
        row = row ? row + 1 : 0;
    }
    size_t n = 0;
    if (row)
        while (row[n] && row[n] != '\t' && n < size - 1)
            n++;
    memcpy(out, row ? row : "", n);
    out[n] = 0;
}

/* --- layout --------------------------------------------------------------- */

static int box_size(const struct font *f)
{
    int b = wfont_height(f) - 4;
    return b < 12 ? 12 : b;
}

static void measure(struct widget *w, const struct font *f)
{
    int line = wfont_height(f);
    switch (w->kind) {
    case LABEL: {
        int tw = wfont_text_width(f, w->text);
        w->nw = tw < LABEL_WIDTH ? tw : LABEL_WIDTH;
        for (const char *c = w->text; *c; c++)          /* (a newline: its lines' widest) */
            if (*c == '\n') {
                w->nw = LABEL_WIDTH;
                break;
            }
        w->nh = wfont_layout(0, 0, 0, 0, f, 0, 0, w->nw, w->text, 0, 0);
        if (w->nh < line)
            w->nh = line;
        break;
    }
    case BUTTON:
        w->nw = wfont_text_width(f, w->text) + 2 * BUTTON_PAD_X;
        if (w->nw < BUTTON_MIN)
            w->nw = BUTTON_MIN;
        w->nh = line + 2 * BUTTON_PAD_Y;
        break;
    case CHECK:
    case RADIO:
        w->nw = box_size(f) + 6 + wfont_text_width(f, w->text);
        w->nh = line;
        break;
    case SLIDER:
        w->nw = BAR_WIDTH;
        w->nh = line;
        break;
    case PROGRESS:
        w->nw = BAR_WIDTH;
        w->nh = line / 2 > 6 ? line / 2 : 6;
        break;
    case EDITOR:                                        /* (41e-4) its lines high */
        w->nw = w->lines == 1 ? 200 : 300;
        w->nh = w->lines * line + 2 * EDIT_PAD;
        break;
    case LIST:
    case TABLE:
    case TREE: {                                        /* (41e-3) the widest row; 8 rows high */
        int widest = 0;
        for (int c = 0; c < w->ncols; c++)
            w->colw[c] = 2 * ROW_PAD;
        for (int i = 0; i < w->nitems; i++) {
            if (w->kind == TABLE) {                     /* (each column its widest cell's width) */
                for (int c = 0; c < w->ncols; c++) {
                    char cell[256];
                    cell_text(w->items[i], c, cell, sizeof(cell));
                    int cw = wfont_text_width(f, cell) + 2 * ROW_PAD;
                    if (cw > w->colw[c])
                        w->colw[c] = cw;
                }
            } else {
                int tw = wfont_text_width(f, w->items[i]) + 2 * ROW_PAD + (w->kind == TREE ? (w->level[i] + 1) * INDENT : 0);
                widest = tw > widest ? tw : widest;
            }
        }
        if (w->kind == TABLE)
            for (int c = 0; c < w->ncols; c++)
                widest += w->colw[c];
        int rows = data_rows(w) < 3 ? 3 : data_rows(w) > ROWS_SHOWN ? ROWS_SHOWN : data_rows(w);
        w->nw = (widest < 120 ? 120 : widest) + BAR + 2;
        w->nh = (rows + (w->kind == TABLE)) * (line + 4) + 2;
        break;
    }
    case TABS: {                                        /* the titles over the largest page */
        int titles = 0, pw = 0, ph = 0;
        for (struct widget *c = w->first; c; c = c->next) {
            measure(c, f);
            titles += wfont_text_width(f, widget_text(c)) + 2 * TAB_PAD;
            pw = c->nw > pw ? c->nw : pw;
            ph = c->nh > ph ? c->nh : ph;
        }
        w->header = line + 2 * BUTTON_PAD_Y;
        w->nw = (titles > pw + 2 * GAP ? titles : pw + 2 * GAP);
        w->nh = w->header + ph + 2 * GAP;
        break;
    }
    case ROW:
    case COLUMN: {
        int along = 0, across = 0, n = 0;
        for (struct widget *c = w->first; c; c = c->next, n++) {
            measure(c, f);
            along += w->kind == ROW ? c->nw : c->nh;
            int a = w->kind == ROW ? c->nh : c->nw;
            across = a > across ? a : across;
        }
        along += n > 1 ? GAP * (n - 1) : 0;
        w->nw = w->kind == ROW ? along : across;
        w->nh = w->kind == ROW ? across : along;
        break;
    }
    case GRID: {
        int cols = w->columns, colw[64] = { 0 }, total_h = 0, rowh = 0, i = 0;
        if (cols > 64)
            cols = 64;
        for (struct widget *c = w->first; c; c = c->next, i++) {
            measure(c, f);
            if (c->nw > colw[i % cols])
                colw[i % cols] = c->nw;
            if (c->nh > rowh)
                rowh = c->nh;
            if (i % cols == cols - 1 || !c->next) {
                total_h += rowh + (total_h ? GAP : 0);
                rowh = 0;
            }
        }
        int used = i < cols ? i : cols;
        w->nw = 0;
        for (int k = 0; k < used; k++)
            w->nw += colw[k] + (k ? GAP : 0);
        w->nh = total_h;
        break;
    }
    }
    if (!w->parent) {                                   /* (the root: a margin round it) */
        w->nw += 2 * GAP;
        w->nh += 2 * GAP;
    }
}

static void place(struct widget *w, int x, int y, int wd, int ht)
{
    w->x = x, w->y = y, w->w = wd, w->h = ht;
    w->dirty = true;
    if (!w->parent && container(w)) {
        x += GAP, y += GAP, wd -= 2 * GAP, ht -= 2 * GAP;
    }
    if (w->kind == ROW || w->kind == COLUMN) {
        bool row = w->kind == ROW;
        int natural = 0, weights = 0, n = 0;
        for (struct widget *c = w->first; c; c = c->next, n++) {
            natural += row ? c->nw : c->nh;
            weights += c->stretch;
        }
        natural += n > 1 ? GAP * (n - 1) : 0;
        int spare = (row ? wd : ht) - natural, at = row ? x : y, given = 0, seen = 0;
        for (struct widget *c = w->first; c; c = c->next) {
            int extra = 0;
            if (spare > 0 && c->stretch) {              /* (by weight; the last takes the rest) */
                seen += c->stretch;
                extra = seen == weights ? spare - given : spare * c->stretch / weights;
                given += extra;
            }
            int len = (row ? c->nw : c->nh) + extra, across = row ? ht : wd;
            int cross = container(c) ? across : (row ? c->nh : c->nw);
            int off = container(c) || !row ? 0 : (across - cross) / 2;  /* (in a row: centred) */
            if (row)
                place(c, at, y + off, len, cross);
            else
                place(c, x + off, at, cross, len);
            at += len + GAP;
        }
    } else if (w->kind == TABS) {                       /* every page in the same place, below the titles */
        for (struct widget *c = w->first; c; c = c->next)
            place(c, x + GAP, y + w->header + GAP, wd - 2 * GAP, ht - w->header - 2 * GAP);
    } else if (w->kind == GRID) {
        int cols = w->columns > 64 ? 64 : w->columns, colw[64] = { 0 }, i = 0;
        for (struct widget *c = w->first; c; c = c->next, i++)
            if (c->nw > colw[i % cols])
                colw[i % cols] = c->nw;
        int cy = y, rowh = 0, cx = x;
        i = 0;
        for (struct widget *c = w->first; c; c = c->next, i++) {
            if (i % cols == 0) {
                rowh = 0;                               /* (this row's tallest) */
                int k = 0;
                for (struct widget *d = c; d && k < cols; d = d->next, k++)
                    rowh = d->nh > rowh ? d->nh : rowh;
                cx = x;
            }
            place(c, cx, cy + (rowh - c->nh) / 2, container(c) ? colw[i % cols] : c->nw, c->nh);
            cx += colw[i % cols] + GAP;
            if (i % cols == cols - 1)
                cy += rowh + GAP;
        }
    }
}

/* --- drawing -------------------------------------------------------------- */

static uint32_t rgb(int r, int g, int b) { return draw_rgba(r, g, b, 255); }
#define RGB(c) rgb(c)

static struct draw_canvas canvas_of(struct window *win)
{
    struct draw_canvas c = { window_pixels(win), window_width(win), window_height(win), window_width(win),
                             0, 0, 0, 0, 0, 0, 0, 0, 0 };
    window_shifts(win, &c.red_shift, &c.green_shift, &c.blue_shift);
    return c;
}

static void text_at(struct run *r, int x, int y, const char *text)
{
    struct window *win = r->win;
    wfont_text(window_pixels(win), (uint32_t)window_width(win), (uint32_t)window_width(win),
               (uint32_t)window_height(win), r->font, x, y, text, window_rgb(win, TEXT), WFONT_TRANSPARENT);
}

/* (41e-3) Text clipped to a rectangle (drawn into that part of the window). */
static void text_in(struct run *r, int cx, int cy, int cw, int ch, int x, int y, const char *text, uint32_t color)
{
    int W = window_width(r->win), H = window_height(r->win);
    if (cx < 0) cw += cx, cx = 0;
    if (cy < 0) ch += cy, cy = 0;
    if (cx + cw > W) cw = W - cx;
    if (cy + ch > H) ch = H - cy;
    if (cw <= 0 || ch <= 0)
        return;
    wfont_text(window_pixels(r->win) + (size_t)cy * (size_t)W + (size_t)cx, (uint32_t)W, (uint32_t)cw,
               (uint32_t)ch, r->font, x - cx, y - cy, text, color, WFONT_TRANSPARENT);
}

static int row_h(const struct run *r)
{
    return wfont_height(r->font) + 4;
}

/* Rows that fit (under a table's header). */
static int rows_fit(const struct run *r, const struct widget *w)
{
    int n = (w->h - 2 - (w->kind == TABLE ? row_h(r) : 0)) / row_h(r);
    return n > 0 ? n : 1;
}

static bool bar_shown(const struct run *r, const struct widget *w)
{
    return shown_item(w, -1) > rows_fit(r, w);
}

/* The first row shown kept within the rows (and the chosen one in view
 * when asked). */
static void keep_rows(const struct run *r, struct widget *w, bool show_chosen)
{
    int n = shown_item(w, -1), fit = rows_fit(r, w);
    if (show_chosen && w->value >= 0) {
        int k = place_of(w, w->value);
        if (k >= 0 && k < w->top)
            w->top = k;
        if (k >= 0 && k >= w->top + fit)
            w->top = k - fit + 1;
    }
    if (w->top > n - fit)
        w->top = n - fit;
    if (w->top < 0)
        w->top = 0;
}

static void draw_rows(struct run *r, struct draw_canvas *c, struct widget *w, bool ring)
{
    int line = wfont_height(r->font), rh = row_h(r), fit = rows_fit(r, w), n = shown_item(w, -1);
    bool bar = n > fit;
    int x0 = w->x + 1, y0 = w->y + 1, iw = w->w - 2 - (bar ? BAR : 0);
    draw_rect(c, (float)w->x, (float)w->y, (float)w->w, (float)w->h, 0, RGB(PANEL));
    if (w->kind == TABLE) {                             /* the header */
        draw_rect(c, (float)x0, (float)y0, (float)iw, (float)rh, 0, RGB(FACE));
        int cx = x0;
        for (int col = 0; col < w->ncols && w->nitems; col++) {
            char cell[256];
            cell_text(w->items[0], col, cell, sizeof(cell));
            text_in(r, cx, y0, w->colw[col] < x0 + iw - cx ? w->colw[col] : x0 + iw - cx, rh, cx + ROW_PAD,
                    y0 + (rh - line) / 2, cell, window_rgb(r->win, TEXT));
            cx += w->colw[col];
        }
        y0 += rh;
    }
    keep_rows(r, w, false);
    for (int k = w->top; k < w->top + fit; k++) {
        int item = shown_item(w, k);
        if (item < 0)
            break;
        int ry = y0 + (k - w->top) * rh;
        bool chosen = item == w->value;
        if (chosen || item == w->hover_row)
            draw_rect(c, (float)x0, (float)ry, (float)iw, (float)rh, 0, chosen ? RGB(ACCENT) : RGB(HOVER));
        uint32_t ink = chosen ? window_rgb(r->win, BACKGROUND) : window_rgb(r->win, TEXT);
        int ty = ry + (rh - line) / 2;
        if (w->kind == TABLE) {
            int cx = x0;
            for (int col = 0; col < w->ncols; col++) {
                char cell[256];
                cell_text(w->items[item + 1], col, cell, sizeof(cell));
                text_in(r, cx, ry, w->colw[col] < x0 + iw - cx ? w->colw[col] : x0 + iw - cx, rh, cx + ROW_PAD, ty,
                        cell, ink);
                cx += w->colw[col];
            }
        } else {
            int tx = x0 + ROW_PAD;
            if (w->kind == TREE) {                      /* its level, and a branch's triangle */
                tx += w->level[item] * INDENT;
                if (branch(w, item)) {
                    float cx = (float)tx + 5, cy = (float)ry + (float)rh / 2;
                    float closed[] = { cx - 3, cy - 5, cx + 3, cy, cx - 3, cy + 5 };
                    float opened[] = { cx - 5, cy - 3, cx + 5, cy - 3, cx, cy + 3 };
                    draw_polygon(c, w->open[item] ? opened : closed, 3, 0, chosen ? RGB(BACKGROUND) : RGB(EDGE));
                }
                tx += INDENT;
            }
            text_in(r, x0, ry, iw, rh, tx, ty, w->items[item], ink);
        }
    }
    if (bar) {                                          /* the scroll bar: a thumb as long as what shows */
        int bx = w->x + w->w - 1 - BAR, bh = w->h - 2, len = bh * fit / n;
        len = len < 12 ? 12 : len;
        int at = (bh - len) * w->top / (n - fit);
        draw_rect(c, (float)bx, (float)(w->y + 1), (float)BAR, (float)bh, 0, RGB(FACE));
        draw_round_rect(c, (float)(bx + 2), (float)(w->y + 1 + at), (float)(BAR - 4), (float)len, 3, 0, RGB(EDGE));
    }
    if (ring)
        draw_round_rect(c, (float)(w->x - RING), (float)(w->y - RING), (float)(w->w + 2 * RING),
                        (float)(w->h + 2 * RING), 5, 2, RGB(ACCENT));
}

/* --- (41e-4) editors --------------------------------------------------- */

/* Width of the first n bytes of text. */
static int width_of(const struct run *r, const char *text, int n)
{
    char small[256], *t = n < (int)sizeof(small) ? small : malloc((size_t)n + 1);
    if (!t)
        return 0;
    memcpy(t, text, (size_t)n);
    t[n] = 0;
    int width = wfont_text_width(r->font, t);
    if (t != small)
        free(t);
    return width;
}

static int edit_width(const struct widget *w)
{
    return w->w - 2 * EDIT_PAD - (w->lines > 1 ? BAR : 0);
}

/* Where each shown line starts (bytes; wrapped at the editor's width, and
 * after newlines); how many there are (at least one). *starts is malloc'd. */
static int edit_lines(const struct run *r, const struct widget *w, int **starts)
{
    int cap = 64, n = 0, len = (int)strlen(w->text);
    int *s = malloc(sizeof(int) * (size_t)cap);
    if (!s) {
        *starts = 0;
        return 0;
    }
    s[n++] = 0;
    if (w->lines > 1)
        for (int at = 0; at < len;) {
            int k = wfont_wrap(r->font, w->text + at, edit_width(w));
            at += k > 0 ? k : 1;
            if (at > len || (at == len && w->text[len - 1] != '\n'))
                break;
            if (n == cap) {
                int *more = realloc(s, sizeof(int) * (size_t)(cap *= 2));
                if (!more)
                    break;
                s = more;
            }
            s[n++] = at;
        }
    *starts = s;
    return n;
}

/* The line of byte `at` (the last that starts at or before it). */
static int line_of(const int *starts, int n, int at)
{
    int i = 0;
    while (i + 1 < n && starts[i + 1] <= at)
        i++;
    return i;
}

/* A line's end: the next one's start, less its newline. */
static int line_end(const struct widget *w, const int *starts, int n, int i)
{
    int end = i + 1 < n ? starts[i + 1] : (int)strlen(w->text);
    if (end > starts[i] && w->text[end - 1] == '\n')
        end--;
    return end;
}

/* The byte of line i nearest to x pixels into it. */
static int byte_at_x(const struct run *r, const struct widget *w, const int *starts, int n, int i, int x)
{
    int at = starts[i], end = line_end(w, starts, n, i), best = at, bestd = x < 0 ? -x : x;
    for (const char *p = w->text + at; p < w->text + end;) {
        utf8_next(&p);
        int d = width_of(r, w->text + at, (int)(p - w->text) - at) - x;
        d = d < 0 ? -d : d;
        if (d < bestd)
            bestd = d, best = (int)(p - w->text);
    }
    return best;
}

static int edit_fit(const struct run *r, const struct widget *w)
{
    int n = (w->h - 2 * EDIT_PAD) / wfont_height(r->font);
    return n > 0 ? n : 1;
}

/* The cursor kept in view: its line among the shown ones, a one-line
 * editor's column within its width. */
static void edit_show_cursor(const struct run *r, struct widget *w)
{
    int *st, n = edit_lines(r, w, &st);
    if (!st)
        return;
    int i = line_of(st, n, w->cur), fit = edit_fit(r, w);
    if (i < w->top)
        w->top = i;
    if (i >= w->top + fit)
        w->top = i - fit + 1;
    if (w->top > n - fit)
        w->top = n - fit > 0 ? n - fit : 0;
    if (w->lines == 1) {
        int x = width_of(r, w->text, w->cur), iw = edit_width(w);
        if (x < w->scroll_x)
            w->scroll_x = x;
        if (x > w->scroll_x + iw - 2)
            w->scroll_x = x - iw + 2;
    }
    free(st);
}

static void draw_editor(struct run *r, struct draw_canvas *c, struct widget *w, bool ring)
{
    int line = wfont_height(r->font), *st, n = edit_lines(r, w, &st), fit = edit_fit(r, w);
    if (!st)
        return;
    int x0 = w->x + EDIT_PAD, y0 = w->y + EDIT_PAD, iw = edit_width(w);
    int lo = w->cur < w->anchor ? w->cur : w->anchor, hi = w->cur < w->anchor ? w->anchor : w->cur;
    draw_rect(c, (float)w->x, (float)w->y, (float)w->w, (float)w->h, 0, RGB(PANEL));
    for (int i = w->top; i < n && i < w->top + fit; i++) {
        int ly = y0 + (i - w->top) * line, start = st[i], end = line_end(w, st, n, i);
        int shift = w->lines == 1 ? w->scroll_x : 0;
        if (hi > lo && lo <= end && hi >= start) {     /* the selection's part of this line */
            int a = lo > start ? lo : start, b = hi < end ? hi : end;
            int xa = width_of(r, w->text + start, a - start), xb = width_of(r, w->text + start, b - start);
            if (hi > end && i + 1 < n)
                xb += 4;                                /* (its newline) */
            draw_clip(c, x0, w->y, iw, w->h);
            draw_rect(c, (float)(x0 + xa - shift), (float)ly, (float)(xb - xa), (float)line, 0,
                      draw_rgba(ACCENT, 110));
            draw_clip(c, 0, 0, c->w, c->h);
        }
        char small[512], *t = end - start < (int)sizeof(small) ? small : malloc((size_t)(end - start) + 1);
        if (t) {
            memcpy(t, w->text + start, (size_t)(end - start));
            t[end - start] = 0;
            text_in(r, x0, w->y, iw, w->h, x0 - shift, ly, t, window_rgb(r->win, TEXT));
            if (t != small)
                free(t);
        }
    }
    int ci = line_of(st, n, w->cur);
    if (r->focused == w && r->has_keyboard && r->blink && ci >= w->top && ci < w->top + fit) {
        int cx = x0 + width_of(r, w->text + st[ci], w->cur - st[ci]) - (w->lines == 1 ? w->scroll_x : 0);
        if (cx >= x0 - 1 && cx <= x0 + iw)
            draw_rect(c, (float)cx, (float)(y0 + (ci - w->top) * line), 1.5f, (float)line, 0, RGB(TEXT));
    }
    if (w->lines > 1 && n > fit) {                      /* a scroll bar, as a list's */
        int bx = w->x + w->w - 1 - BAR, bh = w->h - 2, len = bh * fit / n;
        len = len < 12 ? 12 : len;
        int at = (bh - len) * w->top / (n - fit);
        draw_rect(c, (float)bx, (float)(w->y + 1), (float)BAR, (float)bh, 0, RGB(FACE));
        draw_round_rect(c, (float)(bx + 2), (float)(w->y + 1 + at), (float)(BAR - 4), (float)len, 3, 0, RGB(EDGE));
    }
    if (ring)
        draw_round_rect(c, (float)(w->x - RING), (float)(w->y - RING), (float)(w->w + 2 * RING),
                        (float)(w->h + 2 * RING), 5, 2, RGB(ACCENT));
    free(st);
}

/* The editor's text changed: drawn, the cursor shown, told. */
static void edited(struct run *r, struct widget *w)
{
    r->blink = true;
    edit_show_cursor(r, w);
    changed(w, false);
    if (w->fn)
        w->fn(w, w->data);
}

/* Replace the selection (or nothing, at the cursor) with n bytes of s. */
static void edit_insert(struct run *r, struct widget *w, const char *s, int n)
{
    int lo = w->cur < w->anchor ? w->cur : w->anchor, hi = w->cur < w->anchor ? w->anchor : w->cur;
    int len = (int)strlen(w->text);
    char *t = malloc((size_t)(len - (hi - lo) + n) + 1);
    if (!t)
        return;
    memcpy(t, w->text, (size_t)lo);
    memcpy(t + lo, s, (size_t)n);
    for (int i = 0; w->lines == 1 && i < n; i++)
        if (t[lo + i] == '\n' || t[lo + i] == '\r')
            t[lo + i] = ' ';                    /* (one line: newlines as spaces) */
    memcpy(t + lo + n, w->text + hi, (size_t)(len - hi) + 1);
    free(w->text);
    w->text = t;
    w->cur = w->anchor = lo + n;
    w->prefer_x = -1;
    edited(r, w);
}

/* The previous or next character's byte from at. */
static int char_before(const struct widget *w, int at)
{
    while (at > 0 && (w->text[--at] & 0xC0) == 0x80)
        ;
    return at;
}

static int char_after(const struct widget *w, int at)
{
    int len = (int)strlen(w->text);
    if (at < len)
        at++;
    while (at < len && (w->text[at] & 0xC0) == 0x80)
        at++;
    return at;
}

/* The cursor moved (Shift: the selection grows). */
static void edit_move(struct run *r, struct widget *w, int to, bool keep_column)
{
    w->cur = to;
    if (!r->shift)
        w->anchor = to;
    if (!keep_column)
        w->prefer_x = -1;
    r->blink = true;
    edit_show_cursor(r, w);
    changed(w, false);
}

/* The byte under (x, y) in an editor. */
static int edit_byte_at(const struct run *r, const struct widget *w, int x, int y)
{
    int *st, n = edit_lines(r, w, &st);
    if (!st)
        return w->cur;
    int i = w->top + (y - w->y - EDIT_PAD) / wfont_height(r->font);
    i = i < 0 ? 0 : i >= n ? n - 1 : i;
    int at = byte_at_x(r, w, st, n, i, x - w->x - EDIT_PAD + (w->lines == 1 ? w->scroll_x : 0));
    free(st);
    return at;
}

static bool word_char(char c)
{
    return (c & 0x80) || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/* A key in an editor: true when it took it. */
static bool edit_key(struct run *r, struct widget *w, int key)
{
    int *st, n = edit_lines(r, w, &st);
    if (!st)
        return false;
    int i = line_of(st, n, w->cur), len = (int)strlen(w->text), fit = edit_fit(r, w);
    int lo = w->cur < w->anchor ? w->cur : w->anchor, hi = w->cur < w->anchor ? w->anchor : w->cur;
    bool took = true;
    if (r->ctrl && key == WKEY_A) {             /* (all of it) */
        w->anchor = 0;
        w->cur = len;
        changed(w, false);
    } else if (r->ctrl && (key == WKEY_C || key == WKEY_X)) {
        if (hi > lo)
            window_clipboard_set(r->win, w->text + lo, (size_t)(hi - lo));
        if (key == WKEY_X && hi > lo)
            edit_insert(r, w, "", 0);
    } else if (r->ctrl && key == WKEY_V) {
        char *paste = malloc(PASTE_MAX);
        int got = paste ? window_clipboard_get(r->win, paste, PASTE_MAX) : -1;
        if (got > 0)
            edit_insert(r, w, paste, got);
        free(paste);
    } else if (key == WKEY_LEFT || key == WKEY_RIGHT) {
        int to = !r->shift && hi > lo ? (key == WKEY_LEFT ? lo : hi)     /* (a selection: to its end) */
               : key == WKEY_LEFT ? char_before(w, w->cur) : char_after(w, w->cur);
        edit_move(r, w, to, false);
    } else if (key == WKEY_UP || key == WKEY_DOWN || key == WKEY_PAGEUP || key == WKEY_PAGEDOWN) {
        int step = key == WKEY_UP ? -1 : key == WKEY_DOWN ? 1 : key == WKEY_PAGEUP ? -fit : fit;
        int to_line = i + step;
        if (w->prefer_x < 0)
            w->prefer_x = width_of(r, w->text + st[i], w->cur - st[i]);
        if (to_line < 0)
            edit_move(r, w, 0, true);
        else if (to_line >= n)
            edit_move(r, w, len, true);
        else
            edit_move(r, w, byte_at_x(r, w, st, n, to_line, w->prefer_x), true);
    } else if (key == WKEY_HOME || key == WKEY_END) {
        int to = r->ctrl ? (key == WKEY_HOME ? 0 : len) : key == WKEY_HOME ? st[i] : line_end(w, st, n, i);
        edit_move(r, w, to, false);
    } else if (key == WKEY_BACKSPACE || key == WKEY_DELETE) {
        if (hi == lo) {
            if (key == WKEY_BACKSPACE)
                w->anchor = char_before(w, w->cur);
            else
                w->anchor = char_after(w, w->cur);
        }
        if (w->anchor != w->cur)
            edit_insert(r, w, "", 0);
    } else if (key == WKEY_ENTER && w->lines > 1) {
        edit_insert(r, w, "\n", 1);
    } else
        took = false;
    free(st);
    return took;
}

/* A typed character into an editor (Ctrl held: a command, not text). */
static void edit_text(struct run *r, struct widget *w, uint32_t c)
{
    if (r->ctrl || c < 0x20)
        return;
    char u[4];
    int n = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    if (n == 1)
        u[0] = (char)c;
    else {
        for (int k = n - 1; k > 0; k--, c >>= 6)
            u[k] = (char)(0x80 | (c & 0x3F));
        u[0] = (char)((n == 2 ? 0xC0 : n == 3 ? 0xE0 : 0xF0) | c);
    }
    edit_insert(r, w, u, n);
}

/* A double click: the word there chosen. */
static void edit_word(struct run *r, struct widget *w, int at)
{
    int a = at, b = at, len = (int)strlen(w->text);
    while (a > 0 && word_char(w->text[a - 1]))
        a--;
    while (b < len && word_char(w->text[b]))
        b++;
    w->anchor = a;
    w->cur = b;
    r->blink = true;
    changed(w, false);
}

static void draw_one(struct run *r, struct draw_canvas *c, struct widget *w)
{
    int line = wfont_height(r->font), box = box_size(r->font);
    bool ring = r->has_keyboard && r->focused == w;
    switch (w->kind) {
    case LABEL:
        wfont_layout(window_pixels(r->win), (uint32_t)window_width(r->win), (uint32_t)window_width(r->win),
                     (uint32_t)window_height(r->win), r->font, w->x, w->y, w->w, w->text,
                     window_rgb(r->win, TEXT), WFONT_TRANSPARENT);
        break;
    case BUTTON: {
        bool down = r->pressed == w;
        draw_round_rect(c, (float)w->x, (float)w->y, (float)w->w, (float)w->h, 5, 0,
                        down ? RGB(FACE_PRESSED) : r->hovered == w ? RGB(HOVER) : RGB(FACE));
        int tw = wfont_text_width(r->font, w->text);
        text_at(r, w->x + (w->w - tw) / 2, w->y + (w->h - line) / 2 + (down ? 1 : 0), w->text);
        if (ring)
            draw_round_rect(c, (float)(w->x - RING), (float)(w->y - RING), (float)(w->w + 2 * RING),
                            (float)(w->h + 2 * RING), 8, 2, RGB(ACCENT));
        break;
    }
    case CHECK:
    case RADIO: {
        float bx = (float)w->x, by = (float)(w->y + (w->h - box) / 2), s = (float)box;
        if (w->kind == CHECK) {
            if (w->value) {
                draw_round_rect(c, bx, by, s, s, 3, 0, RGB(ACCENT));
                float mark[] = { bx + s * 0.22f, by + s * 0.52f, bx + s * 0.42f, by + s * 0.72f,
                                 bx + s * 0.78f, by + s * 0.30f };
                draw_line(c, mark[0], mark[1], mark[2], mark[3], s * 0.14f, RGB(BACKGROUND));
                draw_line(c, mark[2], mark[3], mark[4], mark[5], s * 0.14f, RGB(BACKGROUND));
            } else
                draw_round_rect(c, bx, by, s, s, 3, 1.5f, RGB(EDGE));
        } else {
            draw_circle(c, bx + s / 2, by + s / 2, s / 2, w->value ? 0 : 1.5f, w->value ? RGB(ACCENT) : RGB(EDGE));
            if (w->value)
                draw_circle(c, bx + s / 2, by + s / 2, s * 0.2f, 0, RGB(BACKGROUND));
        }
        if (r->pressed == w)
            draw_round_rect(c, bx, by, s, s, 3, 1, RGB(TEXT));
        text_at(r, w->x + box + 6, w->y + (w->h - line) / 2, w->text);
        if (ring)
            draw_round_rect(c, (float)(w->x - RING), (float)(w->y - RING), (float)(w->w + 2 * RING),
                            (float)(w->h + 2 * RING), 5, 2, RGB(ACCENT));
        break;
    }
    case SLIDER: {                                      /* a track, filled to the knob */
        float span = (float)(w->max - w->min), t = span > 0 ? (float)(w->value - w->min) / span : 0;
        float knob = (float)line * 0.35f, x0 = (float)w->x + knob, x1 = (float)(w->x + w->w) - knob;
        float cy = (float)w->y + (float)w->h / 2, kx = x0 + (x1 - x0) * t;
        draw_round_rect(c, x0, cy - 2, x1 - x0, 4, 2, 0, RGB(FACE));
        draw_round_rect(c, x0, cy - 2, kx - x0, 4, 2, 0, RGB(ACCENT));
        draw_circle(c, kx, cy, knob, 0, r->pressed == w ? RGB(ACCENT) : RGB(TEXT));
        if (ring)
            draw_round_rect(c, (float)(w->x - RING), (float)(w->y - RING), (float)(w->w + 2 * RING),
                            (float)(w->h + 2 * RING), 5, 2, RGB(ACCENT));
        break;
    }
    case PROGRESS: {
        float t = (float)w->value / (float)w->max, rr = (float)w->h / 2;
        draw_round_rect(c, (float)w->x, (float)w->y, (float)w->w, (float)w->h, rr, 0, RGB(FACE));
        if (w->value > 0)
            draw_round_rect(c, (float)w->x, (float)w->y, (float)w->w * t, (float)w->h, rr, 0, RGB(ACCENT));
        break;
    }
    case LIST:
    case TABLE:
    case TREE:
        draw_rows(r, c, w, ring);
        break;
    case EDITOR:
        draw_editor(r, c, w, ring);
        break;
    case TABS: {                                        /* the titles; the open one on a face, underlined */
        int tx = w->x, i = 0;
        draw_rect(c, (float)w->x, (float)(w->y + w->header - 1), (float)w->w, 1, 0, RGB(FACE));
        for (struct widget *p = w->first; p; p = p->next, i++) {
            int tw = wfont_text_width(r->font, widget_text(p)) + 2 * TAB_PAD;
            if (i == w->value) {
                draw_round_rect(c, (float)tx, (float)w->y, (float)tw, (float)w->header, 5, 0, RGB(FACE));
                draw_rect(c, (float)tx, (float)(w->y + w->header - 3), (float)tw, 3, 0, RGB(ACCENT));
            }
            text_at(r, tx + TAB_PAD, w->y + (w->header - line) / 2, widget_text(p));
            tx += tw;
        }
        if (ring)
            draw_round_rect(c, (float)(w->x - RING), (float)(w->y - RING), (float)(tx - w->x + 2 * RING),
                            (float)(w->header + 2 * RING), 7, 2, RGB(ACCENT));
        break;
    }
    default:
        break;
    }
}

/* Every widget marked: its rectangle (with its ring's room) cleared and
 * drawn again. */
static void draw_marked(struct run *r, struct draw_canvas *c, struct widget *w)
{
    if (w->dirty) {
        w->dirty = false;
        if (!container(w) || w->kind == TABS) {        /* (tabs: its titles' row) */
            int h = w->kind == TABS ? w->header : w->h;
            draw_clip(c, w->x - RING, w->y - RING, w->w + 2 * RING, h + 2 * RING);
            draw_rect(c, (float)(w->x - RING), (float)(w->y - RING), (float)(w->w + 2 * RING),
                      (float)(h + 2 * RING), 0, RGB(BACKGROUND));
            draw_one(r, c, w);
            draw_clip(c, 0, 0, c->w, c->h);
            r->drawn = true;
        }
    }
    for (struct widget *k = w->first; k; k = k->next)
        if (shown(k))
            draw_marked(r, c, k);
}

static void draw_all(struct run *r, struct widget *root)
{
    struct draw_canvas c = canvas_of(r->win);
    if (r->relayout) {
        r->relayout = false;
        measure(root, r->font);
        /* (41f-2) never resized below what the layout needs; a window
         * smaller than that grows to it */
        window_set_limits(r->win, root->nw, root->nh, 0, 0);
        if (c.w < root->nw || c.h < root->nh) {
            window_resize(r->win, c.w > root->nw ? c.w : root->nw, c.h > root->nh ? c.h : root->nh);
            c = canvas_of(r->win);
        }
        place(root, 0, 0, c.w, c.h);
        draw_rect(&c, 0, 0, (float)c.w, (float)c.h, 0, RGB(BACKGROUND));
        r->drawn = true;
    }
    draw_marked(r, &c, root);
    if (r->drawn)
        window_flush(r->win);
    r->drawn = false;
}

/* --- events --------------------------------------------------------------- */

/* The control under (x, y), or 0. */
static struct widget *control_at(struct widget *w, int x, int y)
{
    for (struct widget *c = w->first; c; c = c->next) {
        struct widget *hit = shown(c) ? control_at(c, x, y) : 0;
        if (hit)
            return hit;
    }
    int h = w->kind == TABS ? w->header : w->h;
    if (focusable(w) && x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + h)
        return w;
    return 0;
}

/* The controls in order: the one after (or before) `from` (0: the first or
 * last), round. */
static void controls(struct widget *w, struct widget **list, int *n, int max)
{
    if (focusable(w) && *n < max)
        list[(*n)++] = w;
    for (struct widget *c = w->first; c; c = c->next)
        if (shown(c))
            controls(c, list, n, max);
}

static struct widget *next_control(struct widget *root, struct widget *from, bool back)
{
    struct widget *list[512];
    int n = 0, at = -1;
    controls(root, list, &n, 512);
    if (!n)
        return 0;
    for (int i = 0; i < n; i++)
        if (list[i] == from)
            at = i;
    if (at < 0)
        return list[back ? n - 1 : 0];
    return list[(at + (back ? n - 1 : 1)) % n];
}

static void focus(struct run *r, struct widget *w)
{
    r->blink = true;
    if (r->focused == w)
        return;
    if (r->focused)
        changed(r->focused, false);
    r->focused = w;
    if (w)
        changed(w, false);
}

/* (41e-2) A slider's or tabs' value changed by the user: drawn, and told. */
static void move_to(struct widget *w, int value)
{
    value = clamp_value(w, value);
    if (value == w->value)
        return;
    w->value = value;
    changed(w, w->kind == TABS);
    if (w->fn)
        w->fn(w, w->data);
}

/* A slider's value for a pointer at x. */
static int slider_at(struct run *r, struct widget *w, int x)
{
    float knob = (float)wfont_height(r->font) * 0.35f, x0 = (float)w->x + knob, x1 = (float)(w->x + w->w) - knob;
    float t = x1 > x0 ? ((float)x - x0) / (x1 - x0) : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return w->min + (int)(t * (float)(w->max - w->min) + 0.5f);
}

/* The tab whose title is at x, or -1. */
static int tab_at(struct run *r, struct widget *w, int x)
{
    int tx = w->x, i = 0;
    for (struct widget *p = w->first; p; p = p->next, i++) {
        int tw = wfont_text_width(r->font, widget_text(p)) + 2 * TAB_PAD;
        if (x >= tx && x < tx + tw)
            return i;
        tx += tw;
    }
    return -1;
}

/* (41e-3) A row chosen (an item; -1 none): drawn, kept in view, told. */
static void choose_row(struct run *r, struct widget *w, int item)
{
    if (item == w->value)
        return;
    w->value = item;
    keep_rows(r, w, true);
    changed(w, false);
    if (w->fn)
        w->fn(w, w->data);
}

/* The item at y in a rows widget (-1: none). */
static int row_at(const struct run *r, const struct widget *w, int y)
{
    int y0 = w->y + 1 + (w->kind == TABLE ? row_h(r) : 0);
    if (y < y0)
        return -1;
    int k = (y - y0) / row_h(r);
    return k < rows_fit(r, w) ? shown_item(w, w->top + k) : -1;
}

/* A tree's branch opened or closed (a chosen row it hides: the branch
 * chosen instead). */
static void open_branch(struct run *r, struct widget *w, int item, bool open)
{
    if (!branch(w, item) || w->open[item] == open)
        return;
    w->open[item] = open;
    if (!open && w->value > item && place_of(w, w->value) < 0)
        choose_row(r, w, item);
    keep_rows(r, w, false);
    changed(w, false);
}

/* The scroll bar dragged to y. */
static void bar_to(struct run *r, struct widget *w, int y)
{
    int n = shown_item(w, -1), fit = rows_fit(r, w), bh = w->h - 2;
    int len = bh * fit / n < 12 ? 12 : bh * fit / n;
    if (bh - len > 0)
        w->top = (int)((long)(y - w->y - 1 - len / 2) * (n - fit) / (bh - len));
    keep_rows(r, w, false);
    changed(w, false);
}

/* The pointer over a control (hover highlights), or over none. */
static void hover_to(struct run *r, struct widget *root, int x, int y, bool inside)
{
    struct widget *h = inside ? control_at(root, x, y) : 0;
    int row = h && rows_kind(h) ? row_at(r, h, y) : -1;
    if (h != r->hovered) {
        if (r->hovered) {
            if (rows_kind(r->hovered))
                r->hovered->hover_row = -1;
            changed(r->hovered, false);
        }
        r->hovered = h;
        if (h)
            changed(h, false);
    }
    if (h && rows_kind(h) && row != h->hover_row) {
        h->hover_row = row;
        changed(h, false);
    }
    int shape = h && h->kind == EDITOR ? WINDOW_CURSOR_TEXT : WINDOW_CURSOR_ARROW;   /* (41e-4) an I-beam */
    if (shape != r->cursor_shape) {
        r->cursor_shape = shape;
        window_set_cursor(r->win, shape);
    }
}

/* A control used: a button pressed, a check box turned, a radio chosen. */
static void use(struct widget *w)
{
    if (w->kind == SLIDER || w->kind == TABS || w->kind == LIST || w->kind == TABLE || w->kind == EDITOR)
        return;                                 /* (they change by moving: move_to, choose_row) */
    if (w->kind == TREE) {                      /* (Space/Enter: the chosen branch opened or closed) */
        if (branch(w, w->value))
            open_branch(run_of(w), w, w->value, !w->open[w->value]);
        return;
    }
    if (w->kind == CHECK) {
        w->value = !w->value;
        changed(w, false);
    } else if (w->kind == RADIO) {
        if (w->value)
            return;                             /* (already the chosen one) */
        choose(w);
    }
    if (w->fn)
        w->fn(w, w->data);
}

static void event(struct run *r, struct widget *root, const struct window_event *ev)
{
    switch (ev->type) {
    case WINDOW_POINTER: {
        bool down = ev->buttons & 1;
        r->px = ev->x, r->py = ev->y;
        if (!down)
            hover_to(r, root, ev->x, ev->y, true);
        if (down && !r->pressed) {
            struct widget *hit = control_at(root, ev->x, ev->y);
            if (hit) {
                r->pressed = hit;
                focus(r, hit);
                changed(hit, false);
                if (hit->kind == SLIDER)
                    move_to(hit, slider_at(r, hit, ev->x));
                else if (hit->kind == TABS && tab_at(r, hit, ev->x) >= 0)
                    move_to(hit, tab_at(r, hit, ev->x));
                else if (hit->kind == EDITOR)
                    edit_move(r, hit, edit_byte_at(r, hit, ev->x, ev->y), false);
                else if (rows_kind(hit)) {
                    if (bar_shown(r, hit) && ev->x >= hit->x + hit->w - 1 - BAR) {
                        r->dragging_bar = true;
                        bar_to(r, hit, ev->y);
                    } else {
                        int item = row_at(r, hit, ev->y);
                        int tri = hit->x + 1 + ROW_PAD + (item >= 0 && hit->kind == TREE ? hit->level[item] * INDENT : 0);
                        if (branch(hit, item) && ev->x < tri + INDENT)
                            open_branch(r, hit, item, !hit->open[item]);   /* (its triangle) */
                        else if (item >= 0)
                            choose_row(r, hit, item);
                    }
                }
            }
        } else if (down && r->pressed && r->pressed->kind == SLIDER) {
            move_to(r->pressed, slider_at(r, r->pressed, ev->x));      /* (dragged) */
        } else if (down && r->pressed && r->dragging_bar) {
            bar_to(r, r->pressed, ev->y);
        } else if (down && r->pressed && r->pressed->kind == EDITOR) {   /* (dragged: a selection) */
            bool shift = r->shift;
            r->shift = true;
            edit_move(r, r->pressed, edit_byte_at(r, r->pressed, ev->x, ev->y), false);
            r->shift = shift;
        } else if (!down && r->pressed) {
            r->dragging_bar = false;
            struct widget *was = r->pressed;
            r->pressed = 0;
            changed(was, false);
            if (control_at(root, ev->x, ev->y) == was)
                use(was);
        }
        break;
    }
    case WINDOW_KEY_DOWN:
        if (ev->key == WKEY_LSHIFT || ev->key == WKEY_RSHIFT)
            r->shift = true;
        else if (ev->key == WKEY_LCTRL || ev->key == WKEY_RCTRL)
            r->ctrl = true;
        else if (r->focused && r->focused->kind == EDITOR && ev->key != WKEY_TAB &&
                 edit_key(r, r->focused, ev->key))
            ;                                   /* (41e-4) taken by the editor */
        else if (ev->key == WKEY_TAB)
            focus(r, next_control(root, r->focused, r->shift));
        else if ((ev->key == WKEY_SPACE || ev->key == WKEY_ENTER) && r->focused)
            use(r->focused);
        else if (ev->key == WKEY_ESCAPE && root->cancel)   /* (41e-5) a dialog: its cancel */
            use(root->cancel);
        else if (r->focused && r->focused->kind == SLIDER) {     /* (arrows a step, pages a tenth) */
            struct widget *w = r->focused;
            int tenth = (w->max - w->min) / 10 > 0 ? (w->max - w->min) / 10 : 1;
            int k = ev->key, v = w->value;
            v = k == WKEY_RIGHT || k == WKEY_UP ? v + 1 : k == WKEY_LEFT || k == WKEY_DOWN ? v - 1
              : k == WKEY_PAGEUP ? v + tenth : k == WKEY_PAGEDOWN ? v - tenth
              : k == WKEY_HOME ? w->min : k == WKEY_END ? w->max : v;
            move_to(w, v);
        } else if (r->focused && r->focused->kind == TABS) {
            if (ev->key == WKEY_RIGHT)
                move_to(r->focused, r->focused->value + 1);
            else if (ev->key == WKEY_LEFT)
                move_to(r->focused, r->focused->value - 1);
        } else if (r->focused && rows_kind(r->focused)) {  /* (41e-3) rows: up, down, pages, ends */
            struct widget *w = r->focused;
            int n = shown_item(w, -1), k = w->value >= 0 ? place_of(w, w->value) : -1, fit = rows_fit(r, w);
            int key = ev->key, to = k;
            to = key == WKEY_DOWN ? k + 1 : key == WKEY_UP ? (k < 0 ? 0 : k - 1)
               : key == WKEY_PAGEDOWN ? k + fit : key == WKEY_PAGEUP ? k - fit
               : key == WKEY_HOME ? 0 : key == WKEY_END ? n - 1 : to;
            if (w->kind == TREE && w->value >= 0 && key == WKEY_RIGHT)
                open_branch(r, w, w->value, true);
            else if (w->kind == TREE && w->value >= 0 && key == WKEY_LEFT) {
                if (branch(w, w->value) && w->open[w->value])
                    open_branch(r, w, w->value, false);
                else                                    /* (a closed one: its parent) */
                    for (int i = w->value - 1; i >= 0; i--)
                        if (w->level[i] < w->level[w->value]) {
                            choose_row(r, w, i);
                            break;
                        }
            } else if (to != k && n > 0) {
                to = to < 0 ? 0 : to >= n ? n - 1 : to;
                choose_row(r, w, shown_item(w, to));
            }
        }
        break;
    case WINDOW_KEY_UP:
        if (ev->key == WKEY_LSHIFT || ev->key == WKEY_RSHIFT)
            r->shift = false;
        if (ev->key == WKEY_LCTRL || ev->key == WKEY_RCTRL)
            r->ctrl = false;
        break;
    case WINDOW_TEXT:                                   /* (41f-4) a typed character */
        if (r->focused && r->focused->kind == EDITOR)
            edit_text(r, r->focused, (uint32_t)ev->key);
        break;
    case WINDOW_DOUBLE_CLICK: {
        struct widget *w = control_at(root, ev->x, ev->y);
        if (w && w->kind == EDITOR)
            edit_word(r, w, edit_byte_at(r, w, ev->x, ev->y));
        break;
    }
    case WINDOW_TIMER:                                  /* (41e-4) the cursor blinks */
        if (ev->key == BLINK_TIMER) {
            r->blink = !r->blink;
            if (r->focused && r->focused->kind == EDITOR)
                changed(r->focused, false);
        }
        break;
    case WINDOW_FOCUS:
        r->has_keyboard = ev->key != 0;
        if (!r->has_keyboard)
            r->shift = r->ctrl = false;
        if (r->focused)
            changed(r->focused, false);
        break;
    case WINDOW_RESIZE:
        window_resize(r->win, ev->x, ev->y);
        r->relayout = true;
        break;
    case WINDOW_SCROLL: {                               /* (41e-3) the rows under the pointer, 3 a step */
        struct widget *w = control_at(root, r->px, r->py);
        if (w && rows_kind(w)) {
            w->top += ev->y * 3;
            keep_rows(r, w, false);
            changed(w, false);
        } else if (w && w->kind == EDITOR && w->lines > 1) {   /* (41e-4) its lines */
            int *st, n = edit_lines(r, w, &st), fit = edit_fit(r, w);
            free(st);
            w->top += ev->y * 3;
            w->top = w->top > n - fit ? n - fit : w->top;
            w->top = w->top < 0 ? 0 : w->top;
            changed(w, false);
        }
        break;
    }
    case WINDOW_LEAVE:                                  /* (41f-3) hover highlights end */
        hover_to(r, root, 0, 0, false);
        break;
    default:
        break;
    }
}

/* (41e-5) The innermost widget_run (a dialog's runs inside its window's). */
static struct run *innermost;

int widget_run(struct window *win, struct widget *root)
{
    if (!win || !root)
        return 0;
    struct run r = { .under = innermost, .root = root, .win = win, .has_keyboard = true, .relayout = true };
    r.font = wfont_open("FreeSans.ttf", 0);
    if (!r.font)
        r.font = wfont_regular();
    root->run = &r;
    innermost = &r;
    window_timer(win, BLINK_MS, BLINK_TIMER);  /* (41e-4) an editor's cursor blinks */
    r.blink = true;
    r.focused = root->start;                    /* (41e-5) a dialog's first button */
    draw_all(&r, root);
    struct window_event ev;
    while (!r.quit) {
        if (window_wait(win, &ev) == 0) {
            r.closed = true;
            break;
        }
        event(&r, root, &ev);
        if (!r.quit)
            draw_all(&r, root);
    }
    window_timer(win, 0, BLINK_TIMER);
    window_set_cursor(win, WINDOW_CURSOR_ARROW);
    root->run = 0;
    innermost = r.under;
    return r.quit ? 1 : 0;
}

/* --- (41e-5) dialogs --------------------------------------------------- */

/* The widget_run of a window (running now, a dialog's run being inside
 * it), or 0. */
static struct run *parent_run(struct window *win)
{
    for (struct run *r = innermost; r; r = r->under)
        if (r->win == win)
            return r;
    return 0;
}

static void answer(struct widget *w, void *root)
{
    struct widget *r = root;
    r->value = w == r->start ? 1 : 0;           /* (the root keeps the answer) */
    widget_quit(r);
}

/* A dialog over parent: the text, then a row of buttons on the right (yes
 * first, the one with the keyboard; no, Escape's); sized to its layout.
 * 1 for yes, 0 for no, Escape or the window closed. */
static int dialog(struct window *parent, const char *title, const char *text, const char *yes, const char *no)
{
    struct widget *root = widget_column(), *buttons = widget_row(), *space = widget_row();
    struct widget *y = widget_button(yes), *n = no ? widget_button(no) : 0;
    if (!root || !buttons || !space || !y || (no && !n)) {
        widget_free(root), widget_free(buttons), widget_free(space), widget_free(y), widget_free(n);
        return 0;
    }
    widget_add(root, widget_label(text ? text : ""));
    widget_add(buttons, space);
    widget_stretch(space, 1);                   /* (the buttons pushed to the right) */
    widget_add(buttons, y);
    if (n)
        widget_add(buttons, n);
    widget_add(root, buttons);
    widget_on(y, answer, root);
    if (n)
        widget_on(n, answer, root);
    root->start = y;
    root->cancel = n ? n : y;
    const struct font *f = wfont_open("FreeSans.ttf", 0);
    measure(root, f ? f : wfont_regular());
    int w = root->nw < 240 ? 240 : root->nw;    /* (not narrower than a small dialog) */
    struct window *win = window_dialog_open(parent, w, root->nh, title ? title : "");
    int result = 0;
    if (win) {
        root->value = 0;
        struct run *under = parent_run(parent);  /* (its window waits: drawn without the keyboard) */
        if (under && under->has_keyboard) {
            under->has_keyboard = false;
            if (under->focused)
                changed(under->focused, false);
            draw_all(under, under->root);
        }
        widget_run(win, root);
        result = root->value;
        window_close(win);
    }
    widget_free(root);
    return result;
}

void widget_message(struct window *parent, const char *title, const char *text)
{
    dialog(parent, title, text, "OK", 0);
}

int widget_question(struct window *parent, const char *title, const char *text, const char *yes, const char *no)
{
    return dialog(parent, title, text, yes ? yes : "Yes", no ? no : "No");
}

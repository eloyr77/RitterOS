/*
 * A one-line text field (15w), the same in every app: the app places it in
 * its window and hands it every event; the field keeps its text, the cursor
 * and a selection, draws itself and shows the change.
 *
 * - The mouse: a press places the cursor (with Shift: extends the
 *   selection), a drag selects, a double click selects a word.
 * - Keys: typing replaces the selection; Backspace and Delete remove it (or
 *   a character); the arrows, Home and End move (with Shift: select);
 *   Ctrl+A selects all; Ctrl+C copies, Ctrl+X cuts and Ctrl+V pastes,
 *   through windowd's clipboard (window_clipboard_*); Enter is reported.
 * Shift is known from the physical keys (WINDOW_KEY_DOWN/UP); the rest
 * from the bytes the keys type (WINDOW_KEY), as a terminal gets them.
 * Text is UTF-8; the cursor never stands inside a character.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "wfont.h"
#include "window.h"

#define FIELD_MAX       1024            /* bytes of text */
#define PAD             4               /* pixels between the border and the text */

struct window_field {
    struct window *win;
    int x, y, w, h;
    char text[FIELD_MAX + 1];
    size_t len, cursor, anchor;         /* the selection lies between anchor and cursor */
    size_t first;                       /* the first character shown (a byte index) */
    int focused, dragging, shift, esc;  /* esc: 0, 1 after ESC, 2 after ESC [ */
    char esc_digit;
};

static int cell(void)
{
    return wfont_width(wfont_regular());
}

static int continuation(char c)
{
    return ((unsigned char)c & 0xC0) == 0x80;
}

static size_t prev_char(const struct window_field *f, size_t i)
{
    if (i == 0)
        return 0;
    i--;
    while (i > 0 && continuation(f->text[i]))
        i--;
    return i;
}

static size_t next_char(const struct window_field *f, size_t i)
{
    if (i >= f->len)
        return f->len;
    i++;
    while (i < f->len && continuation(f->text[i]))
        i++;
    return i;
}

/* Characters between byte indexes a and b. */
static int chars_between(const struct window_field *f, size_t a, size_t b)
{
    int n = 0;
    for (size_t i = a; i < b; i = next_char(f, i))
        n++;
    return n;
}

static int visible_chars(const struct window_field *f)
{
    int n = (f->w - 2 * PAD) / cell();
    return n > 1 ? n : 1;
}

/* Keep the cursor in view. */
static void scroll_to_cursor(struct window_field *f)
{
    if (f->cursor < f->first)
        f->first = f->cursor;
    while (chars_between(f, f->first, f->cursor) >= visible_chars(f))
        f->first = next_char(f, f->first);
}

/* The byte index under the pixel x (in the window). */
static size_t index_at(const struct window_field *f, int x)
{
    int col = (x - f->x - PAD + cell() / 2) / cell();
    size_t i = f->first;
    if (col <= 0) {
        for (int k = col; k < 0 && i > 0; k++)
            i = prev_char(f, i);
        return i;
    }
    for (int k = 0; k < col && i < f->len; k++)
        i = next_char(f, i);
    return i;
}

static int is_word(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || continuation(c) || (unsigned char)c >= 0xC0;
}

static void selection(const struct window_field *f, size_t *a, size_t *b)
{
    *a = f->anchor < f->cursor ? f->anchor : f->cursor;
    *b = f->anchor < f->cursor ? f->cursor : f->anchor;
}

static void delete_selection(struct window_field *f)
{
    size_t a, b;
    selection(f, &a, &b);
    memmove(f->text + a, f->text + b, f->len - b + 1);
    f->len -= b - a;
    f->cursor = f->anchor = a;
}

static void insert(struct window_field *f, const char *s, size_t n)
{
    delete_selection(f);
    if (f->len + n > FIELD_MAX) {
        n = FIELD_MAX - f->len;
        while (n > 0 && continuation(s[n]))     /* (cut: whole characters only) */
            n--;
    }
    memmove(f->text + f->cursor + n, f->text + f->cursor, f->len - f->cursor + 1);
    for (size_t i = 0; i < n; i++)              /* (a line break pasted becomes a space) */
        f->text[f->cursor + i] = s[i] == '\n' || s[i] == '\r' || s[i] == '\t' ? ' ' : s[i];
    f->len += n;
    f->cursor = f->anchor = f->cursor + n;
}

static void copy(struct window_field *f)
{
    size_t a, b;
    selection(f, &a, &b);
    if (b > a)
        window_clipboard_set(f->win, f->text + a, b - a);
}

static void paste(struct window_field *f)
{
    static char buf[FIELD_MAX + 1];
    int n = window_clipboard_get(f->win, buf, sizeof(buf));
    if (n > 0)
        insert(f, buf, (size_t)n);
}

static void move(struct window_field *f, size_t to)
{
    f->cursor = to;
    if (!f->shift)
        f->anchor = to;
}

struct window_field *window_field_new(struct window *win, int x, int y, int w)
{
    struct window_field *f = calloc(1, sizeof(*f));
    if (!f)
        return 0;
    f->win = win;
    f->h = wfont_height(wfont_regular()) + 2 * PAD - 2;
    window_field_place(f, x, y, w);
    return f;
}

void window_field_free(struct window_field *f)
{
    free(f);
}

void window_field_place(struct window_field *f, int x, int y, int w)
{
    f->x = x, f->y = y, f->w = w > 3 * PAD + cell() ? w : 3 * PAD + cell();
    scroll_to_cursor(f);
}

int window_field_height(const struct window_field *f)
{
    return f->h;
}

void window_field_draw(struct window_field *f)
{
    struct window *win = f->win;
    uint32_t edge = window_rgb(win, 0x9A, 0xA3, 0xB0);
    uint32_t bg = f->focused ? window_rgb(win, 0xFF, 0xFF, 0xFF) : window_rgb(win, 0xF8, 0xF9, 0xFB);
    uint32_t fg = window_rgb(win, 0x2E, 0x34, 0x40);
    uint32_t sel_bg = window_rgb(win, 0x3D, 0x6F, 0xD0), sel_fg = window_rgb(win, 0xFF, 0xFF, 0xFF);
    window_fill(win, f->x, f->y, f->w, f->h, bg);
    window_fill(win, f->x, f->y, f->w, 1, edge);
    window_fill(win, f->x, f->y + f->h - 1, f->w, 1, edge);
    window_fill(win, f->x, f->y, 1, f->h, edge);
    window_fill(win, f->x + f->w - 1, f->y, 1, f->h, edge);
    size_t a, b;
    selection(f, &a, &b);
    int show = visible_chars(f), col = 0, ty = f->y + PAD - 1;
    char one[8];
    for (size_t i = f->first; i < f->len && col < show; i = next_char(f, i), col++) {
        size_t n = next_char(f, i) - i;
        memcpy(one, f->text + i, n);
        one[n] = 0;
        bool selected = f->focused && i >= a && i < b;
        window_text(win, f->x + PAD + col * cell(), ty, one, selected ? sel_fg : fg,
                    selected ? sel_bg : WINDOW_TRANSPARENT);
    }
    if (f->focused) {
        int cx = f->x + PAD + chars_between(f, f->first, f->cursor) * cell();
        window_fill(win, cx, f->y + 3, 1, f->h - 6, fg);
    }
}

static int changed(struct window_field *f)
{
    scroll_to_cursor(f);
    window_field_draw(f);
    window_flush(f->win);
    return 0;
}

static int inside(const struct window_field *f, int x, int y)
{
    return x >= f->x && x < f->x + f->w && y >= f->y && y < f->y + f->h;
}

/* A byte typed while focused. */
static int key(struct window_field *f, int byte)
{
    if (f->esc == 1) {
        f->esc = byte == '[' || byte == 'O' ? 2 : 0;
        f->esc_digit = 0;
        return 0;
    }
    if (f->esc == 2) {
        if (byte >= '0' && byte <= '9') {
            f->esc_digit = (char)byte;
            return 0;
        }
        if (byte == ';')                        /* (ESC [ 1 ; 2 D: a modifier) */
            return 0;
        f->esc = 0;
        if (byte == 'D')
            move(f, prev_char(f, f->cursor));
        else if (byte == 'C')
            move(f, next_char(f, f->cursor));
        else if (byte == 'H' || (byte == '~' && f->esc_digit == '1'))
            move(f, 0);
        else if (byte == 'F' || (byte == '~' && f->esc_digit == '4'))
            move(f, f->len);
        else if (byte == '~' && f->esc_digit == '3') {          /* Delete */
            if (f->anchor == f->cursor)
                f->anchor = next_char(f, f->cursor);
            delete_selection(f);
        } else {
            return 0;
        }
        return changed(f);
    }
    switch (byte) {
    case 27:
        f->esc = 1;
        return 0;
    case '\r':
    case '\n':
        return 1;
    case 1:                                     /* Ctrl+A */
        f->anchor = 0, f->cursor = f->len;
        return changed(f);
    case 3:                                     /* Ctrl+C */
        copy(f);
        return 0;
    case 0x18:                                  /* Ctrl+X */
        copy(f);
        delete_selection(f);
        return changed(f);
    case 0x16:                                  /* Ctrl+V */
        paste(f);
        return changed(f);
    case 8:
    case 127:                                   /* Backspace */
        if (f->anchor == f->cursor)
            f->anchor = prev_char(f, f->cursor);
        delete_selection(f);
        return changed(f);
    }
    if (byte < 32)
        return 0;
    char c = (char)byte;
    insert(f, &c, 1);
    /* (a UTF-8 character arrives a byte at a time: shown when whole) */
    return continuation(c) || ((unsigned char)c >= 0xC0) ? 0 : changed(f);
}

int window_field_event(struct window_field *f, const struct window_event *ev)
{
    switch (ev->type) {
    case WINDOW_KEY_DOWN:
    case WINDOW_KEY_UP:
        if (ev->key == WKEY_LSHIFT || ev->key == WKEY_RSHIFT)
            f->shift = ev->type == WINDOW_KEY_DOWN;
        return f->focused ? 0 : -1;
    case WINDOW_KEY:
        return f->focused ? key(f, ev->key) : -1;
    case WINDOW_DOUBLE_CLICK:
        if (!inside(f, ev->x, ev->y))
            return -1;
        {
            size_t i = index_at(f, ev->x), a = i, b = i;
            while (a > 0 && is_word(f->text[prev_char(f, a)]))
                a = prev_char(f, a);
            while (b < f->len && is_word(f->text[b]))
                b = next_char(f, b);
            f->anchor = a, f->cursor = b;
            f->dragging = 0;
        }
        return changed(f);
    case WINDOW_POINTER:
        if (f->dragging) {
            if (!(ev->buttons & 1)) {
                f->dragging = 0;
                return 0;
            }
            size_t i = index_at(f, ev->x);
            if (i == f->cursor)
                return 0;
            f->cursor = i;                      /* (the anchor stays: selecting) */
            return changed(f);
        }
        if (!(ev->buttons & 1))
            return inside(f, ev->x, ev->y) ? 0 : -1;
        if (!inside(f, ev->x, ev->y)) {
            if (f->focused) {                   /* a press elsewhere: the field lets go */
                f->focused = 0;
                changed(f);
            }
            return -1;
        }
        f->focused = 1;
        f->dragging = 1;
        f->cursor = index_at(f, ev->x);
        if (!f->shift)
            f->anchor = f->cursor;
        return changed(f);
    }
    return -1;
}

const char *window_field_text(const struct window_field *f)
{
    return f->text;
}

void window_field_set_text(struct window_field *f, const char *utf8)
{
    size_t n = strlen(utf8);
    if (n > FIELD_MAX)
        n = FIELD_MAX;
    while (n > 0 && continuation(utf8[n]))
        n--;
    memcpy(f->text, utf8, n);
    f->text[n] = 0;
    f->len = n;
    f->cursor = f->anchor = n;
    f->first = 0;
    scroll_to_cursor(f);
}

void window_field_focus(struct window_field *f, int on)
{
    f->focused = on;
    if (on) {                                   /* (as a browser's address line: all selected) */
        f->anchor = 0;
        f->cursor = f->len;
    }
    f->first = 0;
    scroll_to_cursor(f);
}

int window_field_focused(const struct window_field *f)
{
    return f->focused;
}

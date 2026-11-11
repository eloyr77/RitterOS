/*
 * The "Open file" dialog every app shares (window_choose_file, window.h): a
 * small window of its own listing one folder -- ".." first, then folders
 * (marked with a slash), then files, each group by name. A click selects a
 * row and a double click opens it (windowd decides what is one, the same
 * for every program; a folder: go in; a file: the choice); the arrow keys
 * move the selection and Enter opens it. A name typed
 * goes into the line at the bottom, the window library's text field (15w:
 * select, edit, copy and paste there): Enter then opens that (a path from
 * "/", or a name in the folder shown). Backspace with nothing typed goes up; the Cancel button or the close box cancel (not
 * Escape: keys come as bytes, and Escape starts an arrow key's ESC [ A). It
 * runs on the caller's thread until the user is done (modal). (42c)
 * window_choose is the same with a caller's `accept`: what it refuses is
 * greyed and cannot be chosen, and an app bundle (NAME.program) is one
 * item, NAME, among the files (chosen as a whole, never entered).
 */
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "window.h"

#define CW          9                   /* the default font's cell */
#define CH          15
#define COLS        56
#define ROWS        22                  /* the folder line, 19 rows of entries, the name, buttons */
#define LIST_ROWS   (ROWS - 3)
#define MAX_NAMES   512
#define BTN_W       (10 * CW)
#define FIELD_EXTRA 8                   /* the name field is taller than a text row */

struct chooser {
    struct window *win;
    char folder[512];
    char names[MAX_NAMES][64];
    unsigned char is_dir[MAX_NAMES];
    unsigned char bundle[MAX_NAMES], ok[MAX_NAMES];   /* (42c) an app's bundle; may be chosen */
    int (*accept)(const char *path, void *data);      /* (42c; 0: anything) */
    void *data;
    int count, sel, top;
    bool follow, dragging;          /* the view keeps the selection in it; the bar is held (15o) */
    struct window_field *name;          /* the name line */
    uint32_t bg, fg, dir, hi, path, button, grey;
};

/* (42c) Is name an app's bundle's, NAME.program? */
static bool bundle_name(const char *name)
{
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    size_t n = strlen(base);
    return n > 8 && !strcmp(base + n - 8, ".program");
}

static void full_path(const struct chooser *c, const char *name, char *out, size_t size)
{
    snprintf(out, size, "%s/%s", strcmp(c->folder, "/") ? c->folder : "", name);
}

static int by_kind_then_name(const void *a, const void *b, const struct chooser *c)
{
    int i = *(const int *)a, j = *(const int *)b;
    if (strcmp(c->names[i], "..") == 0)
        return -1;
    if (strcmp(c->names[j], "..") == 0)
        return 1;
    if (c->is_dir[i] != c->is_dir[j])
        return c->is_dir[i] ? -1 : 1;
    return strcmp(c->names[i], c->names[j]);
}

/* Read c->folder into the list. */
static void list_folder(struct chooser *c)
{
    static char names[MAX_NAMES][64];
    static unsigned char dirs[MAX_NAMES], bundles[MAX_NAMES], oks[MAX_NAMES];
    int n = 0;
    if (strcmp(c->folder, "/") != 0) {
        snprintf(names[n], sizeof(names[n]), "..");
        bundles[n] = 0, oks[n] = 1;
        dirs[n++] = 1;
    }
    DIR *d = opendir(c->folder);
    struct dirent *e;
    while (d && n < MAX_NAMES && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char full[1024];
        struct stat st;
        snprintf(full, sizeof(full), "%s/%s", strcmp(c->folder, "/") ? c->folder : "", e->d_name);
        snprintf(names[n], sizeof(names[n]), "%s", e->d_name);
        dirs[n] = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        bundles[n] = c->accept && dirs[n] && bundle_name(e->d_name);   /* (42c) an item */
        if (bundles[n])
            dirs[n] = 0;
        oks[n] = !c->accept || dirs[n] || c->accept(full, c->data);
        n++;
    }
    if (d)
        closedir(d);
    /* An index sort (insertion: a folder is small), then copied in order. */
    int order[MAX_NAMES];
    for (int i = 0; i < n; i++)
        order[i] = i;
    memcpy(c->names, names, sizeof(names));
    memcpy(c->is_dir, dirs, sizeof(dirs));          /* (what the order reads) */
    for (int i = 1; i < n; i++) {
        int k = order[i], j = i - 1;
        while (j >= 0 && by_kind_then_name(&order[j], &k, c) > 0) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = k;
    }
    for (int i = 0; i < n; i++) {
        snprintf(c->names[i], sizeof(c->names[i]), "%s", names[order[i]]);
        c->is_dir[i] = dirs[order[i]];
        c->bundle[i] = bundles[order[i]];
        c->ok[i] = oks[order[i]];
    }
    c->count = n;
    c->sel = n > 0 ? 0 : -1;
    c->top = 0;
    c->follow = true;
}

static void draw(struct chooser *c)
{
    struct window *w = c->win;
    int width = window_width(w), height = window_height(w);
    window_fill(w, 0, 0, width, height, c->bg);
    char line[COLS + 8];
    snprintf(line, sizeof(line), "%.*s", COLS, c->folder);
    window_text(w, 0, 0, line, c->path, WINDOW_TRANSPARENT);
    if (c->follow && c->sel >= 0 && c->sel < c->top)
        c->top = c->sel;
    if (c->follow && c->sel >= c->top + LIST_ROWS)
        c->top = c->sel - LIST_ROWS + 1;
    if (c->top > c->count - LIST_ROWS)
        c->top = c->count - LIST_ROWS;
    if (c->top < 0)
        c->top = 0;
    int lw = width - WINDOW_SCROLLBAR_W;
    for (int r = 0; r < LIST_ROWS && c->top + r < c->count; r++) {
        int i = c->top + r, y = (r + 1) * CH;
        if (i == c->sel)
            window_fill(w, 0, y, lw, CH, c->hi);
        int shown = (int)strlen(c->names[i]) - (c->bundle[i] ? 8 : 0);   /* (a bundle: NAME) */
        snprintf(line, sizeof(line), " %.*s%s", shown < COLS - 2 ? shown : COLS - 2, c->names[i],
                 c->is_dir[i] ? "/" : "");
        window_text(w, 0, y, line, !c->ok[i] ? c->grey : c->is_dir[i] ? c->dir : c->fg,
                    WINDOW_TRANSPARENT);
    }
    window_scrollbar(w, lw, CH, LIST_ROWS * CH, c->top, LIST_ROWS, c->count);
    int ny = (ROWS - 2) * CH;
    window_text(w, 0, ny + 2, "Name:", c->fg, WINDOW_TRANSPARENT);
    window_field_draw(c->name);
    int by = (ROWS - 1) * CH + FIELD_EXTRA;
    window_fill(w, width - 2 * BTN_W - 12, by, BTN_W, CH, c->button);
    window_text(w, width - 2 * BTN_W - 12 + 2 * CW, by, "Open", c->fg, WINDOW_TRANSPARENT);
    window_fill(w, width - BTN_W - 4, by, BTN_W, CH, c->button);
    window_text(w, width - BTN_W - 4 + CW, by, "Cancel", c->fg, WINDOW_TRANSPARENT);
    window_flush(w);
}

static bool in_field(const struct chooser *c, int x, int y)
{
    int fy = (ROWS - 2) * CH - 2;
    return x >= 6 * CW && x < COLS * CW - 4 && y >= fy && y < fy + window_field_height(c->name);
}

/* Go into folder `name` of the current one ("..": up). */
static void enter(struct chooser *c, const char *name)
{
    if (!strcmp(name, "..")) {
        char *slash = strrchr(c->folder, '/');
        if (slash && slash != c->folder)
            *slash = 0;
        else
            snprintf(c->folder, sizeof(c->folder), "/");
    } else if (name[0] == '/') {
        snprintf(c->folder, sizeof(c->folder), "%s", name);
    } else {
        char next[512];
        snprintf(next, sizeof(next), "%s/%s", strcmp(c->folder, "/") ? c->folder : "", name);
        snprintf(c->folder, sizeof(c->folder), "%s", next);
    }
    list_folder(c);
}

/* Open the typed name (or the selection): 1 with a file in path, else 0. */
static int open_choice(struct chooser *c, char *path, int size)
{
    char full[1024];
    const char *typed = window_field_text(c->name);
    if (typed[0]) {
        if (typed[0] == '/')
            snprintf(full, sizeof(full), "%s", typed);
        else
            snprintf(full, sizeof(full), "%s/%s", strcmp(c->folder, "/") ? c->folder : "", typed);
        window_field_set_text(c->name, "");
    } else if (c->sel >= 0 && c->sel < c->count) {
        if (!c->ok[c->sel])
            return 0;                           /* (42c) greyed: not this one */
        if (c->is_dir[c->sel]) {
            enter(c, c->names[c->sel]);
            return 0;
        }
        full_path(c, c->names[c->sel], full, sizeof(full));
    } else {
        return 0;
    }
    size_t len = strlen(full);
    if (len > 1 && full[len - 1] == '/')
        full[len - 1] = 0;                      /* (a bundle typed as a folder) */
    struct stat st;
    if (stat(full, &st) != 0 && c->accept && len < sizeof(full) - 9) {
        strcat(full, ".program");               /* (42c) a bundle typed as it is shown */
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode))
            return 0;
    } else if (stat(full, &st) != 0) {
        return 0;                               /* nothing there: stay */
    }
    bool item = c->accept && S_ISDIR(st.st_mode) && bundle_name(full);
    if (S_ISDIR(st.st_mode) && !item) {
        enter(c, full);
        return 0;
    }
    if (c->accept && !c->accept(full, c->data))
        return 0;                               /* (42c) typed, but refused: stay */
    snprintf(path, (size_t)size, "%s", full);
    return 1;
}

int window_choose_file(struct window *parent, const char *title, const char *folder,
                       char *path, int size)
{
    return window_choose(parent, title, folder, 0, 0, path, size);
}

int window_choose(struct window *parent, const char *title, const char *folder,
                  int (*accept)(const char *path, void *data), void *data, char *path, int size)
{
    (void)parent;                               /* (windowd places the dialog itself) */
    if (!path || size <= 0)
        return -1;
    static struct chooser c;
    memset(&c, 0, sizeof(c));
    c.accept = accept;
    c.data = data;
    struct stat st;
    snprintf(c.folder, sizeof(c.folder), "%s",
             folder && folder[0] == '/' && stat(folder, &st) == 0 && S_ISDIR(st.st_mode) ? folder : "/");
    size_t len = strlen(c.folder);
    if (len > 1 && c.folder[len - 1] == '/')
        c.folder[len - 1] = 0;
    c.win = window_open(COLS * CW, ROWS * CH + FIELD_EXTRA, title && title[0] ? title : "Open");
    if (!c.win)
        return -1;
    c.name = window_field_new(c.win, 6 * CW, (ROWS - 2) * CH - 2, (COLS - 6) * CW - 4);
    if (!c.name) {
        window_close(c.win);
        return -1;
    }
    window_field_focus(c.name, 1);              /* (typing goes there from the start) */
    c.bg = window_rgb(c.win, 0x36, 0x3D, 0x4A);  /* owl's colours */
    c.fg = window_rgb(c.win, 0xD8, 0xDE, 0xE9);
    c.dir = window_rgb(c.win, 0xA3, 0xBE, 0x8C);
    c.hi = window_rgb(c.win, 0x5E, 0x81, 0xAC);
    c.path = window_rgb(c.win, 0xEB, 0xCB, 0x8B);
    c.button = window_rgb(c.win, 0x4C, 0x56, 0x6A);
    c.grey = window_rgb(c.win, 0x6C, 0x76, 0x86);     /* (42c) what cannot be chosen */
    list_folder(&c);
    draw(&c);
    int result = 0, esc = 0;
    unsigned prev = 0;
    bool field_drag = false;
    struct window_event ev;
    while (!result && window_wait(c.win, &ev)) {
        if (ev.type == WINDOW_KEY_DOWN || ev.type == WINDOW_KEY_UP) {
            window_field_event(c.name, &ev);    /* (Shift, for selecting) */
        } else if (ev.type == WINDOW_KEY) {
            /* Every byte goes to the name field (which keeps its own state of
             * an ESC sequence); Up and Down (ESC [ A, ESC [ B) also move the
             * list, and Backspace with nothing typed goes up a folder. */
            int b = ev.key;
            bool arrow = esc == 2 && (b == 'A' || b == 'B');
            esc = esc == 1 ? (b == '[' ? 2 : 0) : esc == 2 ? (b >= '0' && b <= '9' ? 2 : 0)
                : b == 0x1B ? 1 : 0;
            if ((b == 0x7F || b == 0x08) && !window_field_text(c.name)[0]) {
                enter(&c, "..");
                draw(&c);
                continue;
            }
            if (window_field_event(c.name, &ev) == 1)
                result = open_choice(&c, path, size);
            if (arrow) {
                if (b == 'A' && c.sel > 0)
                    c.sel--, c.follow = true;
                else if (b == 'B' && c.sel < c.count - 1)
                    c.sel++, c.follow = true;
            }
            if (!result && (arrow || b == '\r' || b == '\n'))
                draw(&c);
        } else if ((ev.type == WINDOW_POINTER || ev.type == WINDOW_DOUBLE_CLICK) &&
                   (field_drag || in_field(&c, ev.x, ev.y))) {   /* the name field's */
            window_field_event(c.name, &ev);
            field_drag = ev.type == WINDOW_POINTER && (ev.buttons & 1);
            if (ev.type == WINDOW_POINTER)
                prev = ev.buttons;
        } else if (ev.type == WINDOW_DOUBLE_CLICK) {      /* open the row under it */
            int i = c.top + ev.y / CH - 1;
            if (ev.y >= CH && ev.y < (LIST_ROWS + 1) * CH && i >= 0 && i < c.count &&
                ev.x < window_width(c.win) - WINDOW_SCROLLBAR_W) {
                c.sel = i;
                window_field_set_text(c.name, "");
                result = open_choice(&c, path, size);
                if (!result)
                    draw(&c);
            }
        } else if (ev.type == WINDOW_SCROLL) {          /* the wheel: 3 rows a step (15o) */
            c.top += 3 * ev.y;
            c.follow = false;
            draw(&c);
        } else if (ev.type == WINDOW_POINTER) {
            int down = (ev.buttons & 1) && !(prev & 1);
            prev = ev.buttons;
            int width = window_width(c.win), by = (ROWS - 1) * CH + FIELD_EXTRA;
            if (c.dragging || (down && ev.x >= width - WINDOW_SCROLLBAR_W && ev.y >= CH &&
                               ev.y < (LIST_ROWS + 1) * CH)) {   /* the scroll bar */
                c.dragging = (ev.buttons & 1) != 0;
                c.top = window_scrollbar_hit(CH, LIST_ROWS * CH, ev.y, LIST_ROWS, c.count);
                c.follow = false;
                draw(&c);
                continue;
            }
            if (!down)
                continue;
            if (ev.y >= by && ev.y < by + CH) {     /* the buttons */
                if (ev.x >= width - 2 * BTN_W - 12 && ev.x < width - BTN_W - 12)
                    result = open_choice(&c, path, size);
                else if (ev.x >= width - BTN_W - 4)
                    result = -2;
            } else if (ev.y >= CH && ev.y < (LIST_ROWS + 1) * CH) {
                int i = c.top + ev.y / CH - 1;
                if (i >= 0 && i < c.count) {
                    c.sel = i;                  /* a click selects; a double click opens */
                    c.follow = true;
                }
            }
            if (!result)
                draw(&c);
        }
    }
    window_field_free(c.name);
    window_close(c.win);
    return result == 1 ? 1 : 0;                 /* chosen, or cancelled (also by the close box) */
}

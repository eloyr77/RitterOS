/*
 * libwindow (see window.h): opens a window on windowd, draws into its shared
 * pixel buffer, and reads events. The window's pixels are a shared buffer
 * windowd composites; window_flush tells windowd they changed.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <poll.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "ritteros.h"
#include "window.h"
#include "wfont.h"
#include "windowproto.h"
#include "../../common/settings_defaults.h"

#define WINDOW_MAX 4            /* windows one program may open (no malloc) */

#define PENDING 32                      /* events kept while waiting for the clipboard */
#define TIMERS 8                        /* (41a) timers per window (window_timer) */

struct window {
    int used;
    int sock;
    int bufd;
    uint32_t *pix;
    int w, h;
    uint8_t red_shift, green_shift, blue_shift;
    struct wmsg pending[PENDING];       /* read early (15w): window_wait hands them out first */
    int npending;
    int id;                             /* (41f-5) windowd's number for it (a dialog names its parent) */
    char *drop;                         /* (41f-6) the last drop's text (heap, NUL-ended), or 0 */
    struct {                            /* (41a) every: 0 for a free place */
        int id;
        uint64_t every, next;           /* nanoseconds; next on the monotonic clock */
    } timers[TIMERS];
};

static struct window pool[WINDOW_MAX];

static int read_full(int fd, void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, (char *)buf + done, n - done);
        if (r <= 0)
            return -1;
        done += (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = write(fd, (const char *)buf + done, n - done);
        if (r < 0)
            return -1;
        done += (size_t)r;
    }
    return 0;
}

static struct window *open_window(int w, int h, const char *title, const struct window *parent);

struct window *window_open(int w, int h, const char *title)
{
    return open_window(w, h, title, 0);
}

struct window *window_dialog_open(struct window *parent, int w, int h, const char *title)
{
    return parent ? open_window(w, h, title, parent) : 0;
}

/* (41h-1) Where windowd listens: the variable WINDOW_SOCKET (a headless
 * windowd's), else /run/windowd. */
static const char *socket_path(void)
{
    const char *p = getenv("WINDOW_SOCKET");
    return p && *p ? p : WINDOW_SOCKET;
}

static struct window *open_window(int w, int h, const char *title, const struct window *parent)
{
    if (w <= 0 || h <= 0)
        return 0;
    struct sys_display d;
    if (ritteros_display_info(&d) < 0)
        return 0;
    struct window *win = 0;
    for (int i = 0; i < WINDOW_MAX; i++)
        if (!pool[i].used) {
            win = &pool[i];
            break;
        }
    if (!win)
        return 0;
    memset(win, 0, sizeof(*win));
    win->used = 1;
    win->w = w;
    win->h = h;
    win->red_shift = d.red_shift;
    win->green_shift = d.green_shift;
    win->blue_shift = d.blue_shift;
    win->bufd = ritteros_share_create((size_t)w * h * 4);
    win->pix = win->bufd < 0 ? 0 : ritteros_share_map(win->bufd);
    if (!win->pix) {
        win->used = 0;
        return 0;
    }
    win->sock = -1;
    for (int i = 0; i < 100 && win->sock < 0; i++) {        /* wait for windowd */
        win->sock = ritteros_local_connect(socket_path());
        if (win->sock < 0 && errno == ECONNREFUSED)
            usleep(50000);
        else
            break;
    }
    if (win->sock >= 0) {                                    /* (15k) its user's or root's */
        int uid = ritteros_peer_uid(win->sock);
        if (uid != (int)getuid() && uid != 0) {
            close(win->sock);
            win->sock = -1;
            errno = EACCES;                                   /* another user's screen */
        }
    }
    if (win->sock < 0) {
        win->used = 0;
        return 0;
    }
    char t[WINDOW_TITLE_MAX];
    size_t tl = 0;
    if (title) {
        while (title[tl] && tl < sizeof(t) - 1)
            tl++;
        memcpy(t, title, tl);
    }
    uint8_t app[64];                                        /* (41g-3) whose window: its icon */
    size_t al = strlen(getprogname());
    al = al > sizeof(app) - 1 ? sizeof(app) - 1 : al;
    app[0] = (uint8_t)al;
    memcpy(app + 1, getprogname(), al);
    struct wmsg m = { WMSG_CREATE, (uint32_t)w, (uint32_t)h,
                      (uint32_t)tl | (parent ? (uint32_t)(parent->id + 1) << 16 : 0) | WMSG_CREATE_APP };
    if (ritteros_share_send(win->sock, win->bufd) < 0 ||
        write_full(win->sock, &m, sizeof(m)) < 0 ||
        (tl && write_full(win->sock, t, tl) < 0) ||
        write_full(win->sock, app, al + 1) < 0 ||
        read_full(win->sock, &m, sizeof(m)) < 0 || m.type != WMSG_CREATED) {
        close(win->sock);
        win->used = 0;
        return 0;
    }
    win->id = (int)m.a;
    return win;
}

void window_close(struct window *win)
{
    if (!win)
        return;
    close(win->sock);
    close(win->bufd);
    free(win->drop);
    win->drop = 0;
    win->used = 0;
}

int window_width(struct window *win) { return win->w; }
int window_height(struct window *win) { return win->h; }

int window_resize(struct window *win, int w, int h)
{
    if (!win || w <= 0 || h <= 0)
        return -1;
    int nbufd = ritteros_share_create((size_t)w * h * 4);
    uint32_t *npix = nbufd < 0 ? 0 : ritteros_share_map(nbufd);
    if (!npix) {
        if (nbufd >= 0)
            close(nbufd);
        return -1;
    }
    struct wmsg m = { WMSG_BUFFER, (uint32_t)w, (uint32_t)h, 0 };
    if (ritteros_share_send(win->sock, nbufd) < 0 || write_full(win->sock, &m, sizeof(m)) < 0) {
        close(nbufd);
        return -1;
    }
    close(win->bufd);                   /* drop the old buffer (unmaps it) */
    win->bufd = nbufd;
    win->pix = npix;
    win->w = w;
    win->h = h;
    return 0;
}

uint32_t window_rgb(struct window *win, int r, int g, int b)
{
    return ((uint32_t)(r & 0xFF) << win->red_shift) | ((uint32_t)(g & 0xFF) << win->green_shift) |
           ((uint32_t)(b & 0xFF) << win->blue_shift);
}

/* Append one menu item (id, label) to a blob; returns the new length. */
static int put_item(uint8_t *buf, int n, int size, int id, const char *label)
{
    size_t ll = 0;
    while (label[ll] && ll < WINDOW_LABEL_MAX - 1)
        ll++;
    if (n + 3 + (int)ll > size)
        return n;
    buf[n++] = (uint8_t)id;
    buf[n++] = (uint8_t)(id >> 8);
    buf[n++] = (uint8_t)ll;
    memcpy(buf + n, label, ll);
    return n + (int)ll;
}

int window_set_menu(struct window *win, const struct window_menu *menus, int count)
{
    if (!win)
        return -1;
    uint8_t buf[1024];
    int n = 0;
    if (count > WINDOW_MENU_MAX)
        count = WINDOW_MENU_MAX;
    buf[n++] = (uint8_t)count;
    for (int i = 0; i < count; i++) {
        size_t tl = 0;
        while (menus[i].title[tl] && tl < WINDOW_LABEL_MAX - 1)
            tl++;
        buf[n++] = (uint8_t)tl;
        memcpy(buf + n, menus[i].title, tl);
        n += (int)tl;
        int ni = menus[i].count > WINDOW_MENU_ITEMS ? WINDOW_MENU_ITEMS : menus[i].count;
        buf[n++] = (uint8_t)ni;
        for (int j = 0; j < ni; j++)
            n = put_item(buf, n, sizeof(buf), menus[i].items[j].id, menus[i].items[j].label);
    }
    struct wmsg m = { WMSG_SET_MENU, 0, 0, (uint32_t)n };
    if (write_full(win->sock, &m, sizeof(m)) < 0)
        return -1;
    return write_full(win->sock, buf, (size_t)n);
}

int window_popup_menu(struct window *win, int x, int y,
                      const struct window_menu_item *items, int count)
{
    if (!win)
        return -1;
    uint8_t buf[1024];
    int n = 0;
    if (count > WINDOW_MENU_ITEMS)
        count = WINDOW_MENU_ITEMS;
    buf[n++] = (uint8_t)count;
    for (int i = 0; i < count; i++)
        n = put_item(buf, n, sizeof(buf), items[i].id, items[i].label);
    struct wmsg m = { WMSG_POPUP, (uint32_t)x, (uint32_t)y, (uint32_t)n };
    if (write_full(win->sock, &m, sizeof(m)) < 0)
        return -1;
    return write_full(win->sock, buf, (size_t)n);
}

void window_scrollbar(struct window *win, int x, int y, int h, int first, int shown, int total)
{
    uint32_t trough = window_color(WINDOW_COLOR_BACKGROUND);   /* (41g-5) the user's theme */
    window_fill(win, x, y, WINDOW_SCROLLBAR_W, h,
                window_rgb(win, trough >> 16 & 0xFF, trough >> 8 & 0xFF, trough & 0xFF));
    if (total <= shown || total <= 0 || h < 8)
        return;                                 /* nothing to scroll: the trough alone */
    int len = (int)((long)h * shown / total);
    if (len < 12)
        len = 12;
    int max_first = total - shown;
    int at = max_first ? (int)((long)(h - len) * first / max_first) : 0;
    window_fill(win, x + 2, y + at, WINDOW_SCROLLBAR_W - 4, len,
                window_rgb(win, 0x81, 0xA1, 0xC1));
}

int window_scrollbar_hit(int y, int h, int py, int shown, int total)
{
    if (total <= shown || h <= 0)
        return 0;
    int len = (int)((long)h * shown / total);
    if (len < 12)
        len = 12;
    int max_first = total - shown;
    if (h - len <= 0)
        return 0;
    int first = (int)((long)(py - y - len / 2) * max_first / (h - len));
    return first < 0 ? 0 : first > max_first ? max_first : first;
}

void window_fill(struct window *win, int x, int y, int w, int h, uint32_t color)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > win->w)
        x1 = win->w;
    if (y1 > win->h)
        y1 = win->h;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            win->pix[(int64_t)yy * win->w + xx] = color;
}

void window_blit(struct window *win, int x, int y, int w, int h, const uint32_t *src)
{
    for (int yy = 0; yy < h; yy++) {
        int dy = y + yy;
        if (dy < 0 || dy >= win->h)
            continue;
        for (int xx = 0; xx < w; xx++) {
            int dx = x + xx;
            if (dx >= 0 && dx < win->w)
                win->pix[(int64_t)dy * win->w + dx] = src[(int64_t)yy * w + xx];
        }
    }
}

uint32_t *window_pixels(struct window *win)
{
    return win ? win->pix : 0;
}

void window_shifts(struct window *win, int *red, int *green, int *blue)
{
    *red = win->red_shift;
    *green = win->green_shift;
    *blue = win->blue_shift;
}

void window_text(struct window *win, int x, int y, const char *utf8, uint32_t fg, uint32_t bg)
{
    window_text_font(win, wfont_regular(), x, y, utf8, fg, bg);
}

void window_text_font(struct window *win, const struct font *font, int x, int y, const char *utf8,
                      uint32_t fg, uint32_t bg)
{
    wfont_text(win->pix, (uint32_t)win->w, (uint32_t)win->w, (uint32_t)win->h,
               font ? font : wfont_regular(), x, y, utf8, fg, bg);
}

void window_flush(struct window *win)
{
    struct wmsg m = { WMSG_DAMAGE, 0, 0, 0 };
    write_full(win->sock, &m, sizeof(m));
}

/* (41f-6) The text that follows a WMSG_DROP, read as soon as the message is
 * (so a drop kept while the clipboard is awaited is whole), into
 * win->drop; what is past WINDOW_CLIPBOARD_MAX is read and dropped. */
static int drop_read(struct window *win, const struct wmsg *m)
{
    if (m->type != WMSG_DROP)
        return 0;
    uint32_t keep = m->c > WINDOW_CLIPBOARD_MAX ? WINDOW_CLIPBOARD_MAX : m->c;
    char *text = malloc(keep + 1);
    char past[256];
    uint32_t got = 0;
    while (got < m->c) {
        uint32_t k = m->c - got;
        if (!text || got >= keep) {
            k = k < sizeof(past) ? k : (uint32_t)sizeof(past);
            if (read_full(win->sock, past, k) < 0)
                break;
        } else {
            k = k < keep - got ? k : keep - got;
            if (read_full(win->sock, text + got, k) < 0)
                break;
        }
        got += k;
    }
    if (got < m->c) {
        free(text);
        return -1;
    }
    if (text)
        text[keep] = 0;
    free(win->drop);
    win->drop = text;
    return 0;
}

/* The next message: a kept one first, else from windowd; -1 when it left. */
static int next_message(struct window *win, struct wmsg *m)
{
    if (win->npending) {
        *m = win->pending[0];
        memmove(win->pending, win->pending + 1, sizeof(win->pending[0]) * (size_t)--win->npending);
        return 0;
    }
    return read_full(win->sock, m, sizeof(*m));
}

int window_clipboard_set(struct window *win, const char *utf8, size_t n)
{
    if (n > WINDOW_CLIPBOARD_MAX)
        n = WINDOW_CLIPBOARD_MAX;
    struct wmsg m = { WMSG_CLIP_SET, 0, 0, (uint32_t)n };
    if (write_full(win->sock, &m, sizeof(m)) < 0 || (n && write_full(win->sock, utf8, n) < 0))
        return -1;
    return 0;
}

int window_clipboard_get(struct window *win, char *buf, size_t size)
{
    struct wmsg m = { WMSG_CLIP_GET, 0, 0, 0 };
    if (!size || write_full(win->sock, &m, sizeof(m)) < 0)
        return -1;
    /* The answer comes among the window's events: those are kept. */
    for (;;) {
        if (read_full(win->sock, &m, sizeof(m)) < 0 || drop_read(win, &m) < 0)
            return -1;
        if (m.type != WMSG_CLIP_TEXT) {
            if (win->npending < PENDING)
                win->pending[win->npending++] = m;
            continue;
        }
        size_t keep = m.c < size - 1 ? m.c : size - 1;
        if (keep && read_full(win->sock, buf, keep) < 0)
            return -1;
        char drop[256];
        for (size_t left = m.c - keep; left > 0;) {     /* (more than fits: read past) */
            size_t k = left < sizeof(drop) ? left : sizeof(drop);
            if (read_full(win->sock, drop, k) < 0)
                return -1;
            left -= k;
        }
        buf[keep] = 0;
        return (int)keep;
    }
}

/* A message from windowd as an event: 1 with *ev filled, 0 for none (read
 * past), -1 when the window is done. */
static int to_event(struct window *win, const struct wmsg *mp, struct window_event *ev)
{
    const struct wmsg m = *mp;
    if (m.type == WMSG_POINTER) {
        ev->type = WINDOW_POINTER;
        ev->x = (int)m.a;
        ev->y = (int)m.b;
        ev->buttons = m.c;
        ev->key = 0;
        return 1;
    }
    if (m.type == WMSG_KEYCODE) {
        ev->type = m.b ? WINDOW_KEY_DOWN : WINDOW_KEY_UP;
        ev->key = (int)m.a;         /* the key's USB HID usage ID (WKEY_*) */
        ev->x = ev->y = 0;
        ev->buttons = 0;
        return 1;
    }
    if (m.type == WMSG_SCROLL) {
        ev->type = WINDOW_SCROLL;
        ev->y = (int)(int32_t)m.a;  /* steps, negative up */
        ev->x = 0;
        ev->buttons = 0;
        ev->key = 0;
        return 1;
    }
    if (m.type == WMSG_DOUBLE_CLICK) {
        ev->type = WINDOW_DOUBLE_CLICK;
        ev->x = (int)m.a;
        ev->y = (int)m.b;
        ev->buttons = 1;
        ev->key = 0;
        return 1;
    }
    if (m.type == WMSG_KEY) {
        ev->type = WINDOW_KEY;
        ev->key = (int)m.a;
        ev->x = ev->y = 0;
        ev->buttons = 0;
        return 1;
    }
    if (m.type == WMSG_RESIZE) {
        ev->type = WINDOW_RESIZE;
        ev->x = (int)m.a;           /* the new content size windowd asks for */
        ev->y = (int)m.b;
        ev->buttons = 0;
        ev->key = 0;
        return 1;
    }
    if (m.type == WMSG_MENU) {
        ev->type = WINDOW_MENU;
        ev->menu = (int)m.a;        /* the chosen item's id */
        ev->x = ev->y = ev->key = 0;
        ev->buttons = 0;
        return 1;
    }
    if (m.type == WMSG_TEXT) {
        ev->type = WINDOW_TEXT;
        ev->key = (int)m.a;
        ev->x = ev->y = ev->menu = 0;
        ev->buttons = 0;
        return 1;
    }
    if (m.type == WMSG_HOVER) {
        ev->type = m.a ? WINDOW_ENTER : WINDOW_LEAVE;
        ev->x = ev->y = ev->key = ev->menu = 0;
        ev->buttons = 0;
        return 1;
    }
    if (m.type == WMSG_DROP) {                  /* (41f-6) its text already in win->drop */
        ev->type = WINDOW_DROP;
        ev->x = (int)m.a;
        ev->y = (int)m.b;
        ev->key = ev->menu = 0;
        ev->buttons = 0;
        return 1;
    }
    if (m.type == WMSG_FOCUS) {
        ev->type = WINDOW_FOCUS;
        ev->key = m.a ? 1 : 0;
        ev->x = ev->y = ev->menu = 0;
        ev->buttons = 0;
        return 1;
    }
    if (m.type == WMSG_CLIP_TEXT) {             /* (an answer nobody waits for: read past) */
        char drop[256];
        for (size_t left = m.c; left > 0;) {
            size_t k = left < sizeof(drop) ? left : sizeof(drop);
            if (read_full(win->sock, drop, k) < 0)
                break;
            left -= k;
        }
        return 0;
    }
    if (m.type == WMSG_CLOSE)
        return -1;                  /* the user clicked the close box: the window is done */
    return 0;
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}

int window_timer(struct window *win, int ms, int id)
{
    if (!win || ms < 0)
        return -1;
    int free_at = -1;
    for (int i = 0; i < TIMERS; i++) {
        if (win->timers[i].every && win->timers[i].id == id)
            win->timers[i].every = 0;               /* (set again: from now on) */
        if (!win->timers[i].every && free_at < 0)
            free_at = i;
    }
    if (ms == 0)
        return 0;
    if (free_at < 0)
        return -1;
    win->timers[free_at].id = id;
    win->timers[free_at].every = (uint64_t)ms * 1000000;
    win->timers[free_at].next = now_ns() + win->timers[free_at].every;
    return 0;
}

void window_set_cursor(struct window *win, int shape)
{
    struct wmsg m = { WMSG_CURSOR, (uint32_t)(shape < 0 ? 0 : shape), 0, 0 };
    if (win)
        write_full(win->sock, &m, sizeof(m));
}

void window_set_limits(struct window *win, int min_w, int min_h, int max_w, int max_h)
{
#define SIZE(v) ((uint32_t)((v) < 0 ? 0 : (v) > 0xFFFF ? 0xFFFF : (v)))
    struct wmsg m = { WMSG_LIMITS, SIZE(min_w) | SIZE(min_h) << 16, SIZE(max_w) | SIZE(max_h) << 16, 0 };
#undef SIZE
    if (win)
        write_full(win->sock, &m, sizeof(m));
}

void window_fullscreen(struct window *win, int on)
{
    struct wmsg m = { WMSG_FULLSCREEN, on ? 1u : 0u, 0, 0 };
    if (win)
        write_full(win->sock, &m, sizeof(m));
}

void window_minimize(struct window *win)
{
    struct wmsg m = { WMSG_MINIMIZE, 0, 0, 0 };
    if (win)
        write_full(win->sock, &m, sizeof(m));
}

int window_drag(struct window *win, const char *text, size_t n)
{
    if (!win || !text)
        return -1;
    if (n > WINDOW_CLIPBOARD_MAX)
        n = WINDOW_CLIPBOARD_MAX;
    struct wmsg m = { WMSG_DRAG, 0, 0, (uint32_t)n };
    if (write_full(win->sock, &m, sizeof(m)) < 0 || (n && write_full(win->sock, text, n) < 0))
        return -1;
    return 0;
}

const char *window_drop_text(struct window *win)
{
    return win && win->drop ? win->drop : "";
}

int window_notify(struct window *win, const char *title, const char *text)
{
    size_t tl = title ? strlen(title) : 0, xl = text ? strlen(text) : 0;
    if (tl > WINDOW_TITLE_MAX - 1)
        tl = WINDOW_TITLE_MAX - 1;
    if (xl > WINDOW_NOTIFY_MAX - 1)
        xl = WINDOW_NOTIFY_MAX - 1;
    int sock = win ? win->sock : ritteros_local_connect(socket_path());   /* (no window: its own) */
    if (sock < 0)
        return -1;
    if (!win) {                                             /* (15k) its user's or root's */
        int uid = ritteros_peer_uid(sock);
        if (uid != (int)getuid() && uid != 0) {
            close(sock);
            errno = EACCES;
            return -1;
        }
    }
    struct wmsg m = { WMSG_NOTIFY, (uint32_t)tl, (uint32_t)xl, 0 };
    int r = write_full(sock, &m, sizeof(m)) < 0 || (tl && write_full(sock, title, tl) < 0) ||
                    (xl && write_full(sock, text, xl) < 0) ? -1 : 0;
    if (!win)
        close(sock);
    return r;
}

/* (41g-5) The themes' colours, in WINDOW_COLOR_* order: the dark one is
 * Nord's polar night (what every window had before 41g-5), the light one
 * its snow storm. */
static const uint32_t theme_dark[] = { 0x2E3440, 0x3B4252, 0x434C5E, 0x4C566A, 0x555F75,
                                       0xECEFF4, 0xD8DEE9, 0x88C0D0, 0x5E81AC };
static const uint32_t theme_light[] = { 0xECEFF4, 0xE5E9F0, 0xD8DEE9, 0xC2CAD6, 0xE2E7EF,
                                        0x2E3440, 0x4C566A, 0x5E81AC, 0x5E81AC };
#define THEME_COLORS (sizeof(theme_dark) / sizeof(theme_dark[0]))

/* A key of the user's window settings (~/.config/windows, else
 * /system/config/windows, as windowd reads them): > 0 when set. */
static int windows_key(const char *key, char *value, size_t size)
{
    char mine[300];
    int r = 0;
    struct passwd *pw = getpwuid(getuid());
    if (pw) {
        snprintf(mine, sizeof(mine), "%s/.config/windows", pw->pw_dir);
        r = config_get(mine, key, value, size);
    }
    if (r <= 0)
        r = config_get("/system/config/windows", key, value, size);
    return r;
}

uint32_t window_color(int which)
{
    static uint32_t colors[THEME_COLORS];
    static int ready;
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) {     /* (two threads at once make the same) */
        char value[32];
        const uint32_t *t = windows_key("theme", value, sizeof(value)) > 0 && !strcasecmp(value, "light")
                                    ? theme_light : theme_dark;
        uint32_t c[THEME_COLORS];
        memcpy(c, t, sizeof(c));
        char *end;
        if (windows_key("accent", value, sizeof(value)) > 0 && strlen(value) == 6) {
            unsigned long a = strtoul(value, &end, 16);
            if (*end == 0)
                c[WINDOW_COLOR_ACCENT] = c[WINDOW_COLOR_SELECTION] = (uint32_t)a;
        }
        memcpy(colors, c, sizeof(colors));
        __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
    }
    return which >= 0 && which < (int)THEME_COLORS ? colors[which] : colors[WINDOW_COLOR_TEXT];
}

/* (42a) The user's file types, ~/.config/filetypes: written with its
 * default text when missing (the settings rule: whoever reads it), never
 * over a file that is there. */
static void filetypes_ready(const char *home)
{
    char dir[300], path[320];
    snprintf(dir, sizeof(dir), "%s/.config", home);
    snprintf(path, sizeof(path), "%s/" SETTINGS_USER_FILETYPES, home);
    struct stat st;
    if (stat(path, &st) == 0 || errno != ENOENT || (stat(dir, &st) < 0 && mkdir(dir, 0700) < 0))
        return;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
        return;
    write(fd, SETTINGS_USER_FILETYPES_TEXT, sizeof(SETTINGS_USER_FILETYPES_TEXT) - 1);
    close(fd);
}

int window_app_for(const char *path, char *buf, size_t size)
{
#if defined(__x86_64__)
    static const char sys[] = "/system/x86_64";
#else
    static const char sys[] = "/system/aarch64";
#endif
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    if (!dot || !dot[1] || dot == base || !size)
        return -1;
    char ending[32], name[64] = "", file[320];
    size_t n = 0;
    for (const char *c = dot + 1; *c && n < sizeof(ending) - 1; c++)   /* (any case) */
        ending[n++] = (char)(*c >= 'A' && *c <= 'Z' ? *c - 'A' + 'a' : *c);
    ending[n] = 0;
    struct passwd *pw = getpwuid(getuid());
    int r = 0;
    if (pw) {
        filetypes_ready(pw->pw_dir);
        snprintf(file, sizeof(file), "%s/" SETTINGS_USER_FILETYPES, pw->pw_dir);
        r = config_get(file, ending, name, sizeof(name));
    }
    if (r <= 0)
        r = config_get("/system/config/filetypes", ending, name, sizeof(name));
    if (r <= 0 || !name[0] || strchr(name, '/'))
        return -1;
    /* Its place says how it runs: an app in its window, a program in bin in
     * a terminal; a name in neither opens nothing. */
    struct stat st;
    snprintf(file, sizeof(file), "%s/apps/%s.program/%s", sys, name, name);   /* (42b) a bundle */
    int how = stat(file, &st) == 0 ? 0 : -1;
    if (how < 0) {
        snprintf(file, sizeof(file), "%s/bin/%s", sys, name);
        how = stat(file, &st) == 0 ? 1 : -1;
    }
    if (how >= 0)
        snprintf(buf, size, "%s", name);
    return how;
}

int window_app_opens(const char *bundle, const char *file)
{
    if (!bundle || !file)
        return 0;
    char dir[512], name[64], program[640], types[512];
    snprintf(dir, sizeof(dir), "%s", bundle);
    size_t n = strlen(dir);
    while (n > 1 && dir[n - 1] == '/')
        dir[--n] = 0;
    const char *base = strrchr(dir, '/');
    base = base ? base + 1 : dir;
    size_t len = strlen(base);
    if (len <= 8 || strcmp(base + len - 8, ".program") || len - 8 >= sizeof(name))
        return 0;
    snprintf(name, sizeof(name), "%.*s", (int)(len - 8), base);
    struct stat st;
    snprintf(program, sizeof(program), "%s/%s", dir, name);   /* (an app: its program there) */
    if (stat(program, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    const char *slash = strrchr(file, '/'), *f = slash ? slash + 1 : file;
    const char *dot = strrchr(f, '.');
    if (!dot || dot == f || !dot[1])
        return 0;
    snprintf(program, sizeof(program), "%s/info", dir);
    if (config_get(program, "types", types, sizeof(types)) <= 0)
        return 0;
    /* `types` is endings apart by spaces (or commas), any case. */
    size_t want = strlen(dot + 1);
    for (char *t = types; *t;) {
        while (*t == ' ' || *t == '\t' || *t == ',')
            t++;
        size_t k = strcspn(t, " \t,");
        if (k && k == want && !strncasecmp(t, dot + 1, k))
            return 1;
        t += k;
    }
    return 0;
}

int window_fd(struct window *win)
{
    return win ? win->sock : -1;
}

/* The timer due first at `now` as an event (1), else 0; *next_due: the
 * earliest deadline of the timers (UINT64_MAX without one). */
static int timer_due(struct window *win, uint64_t now, struct window_event *ev, uint64_t *next_due)
{
    int first = -1;
    for (int i = 0; i < TIMERS; i++)
        if (win->timers[i].every && (first < 0 || win->timers[i].next < win->timers[first].next))
            first = i;
    *next_due = first < 0 ? UINT64_MAX : win->timers[first].next;
    if (first < 0 || win->timers[first].next > now)
        return 0;
    /* its next deadline a period on, or a period from now when it fell
     * further behind (a stalled program gets one event, not a burst) */
    win->timers[first].next += win->timers[first].every;
    if (win->timers[first].next <= now)
        win->timers[first].next = now + win->timers[first].every;
    ev->type = WINDOW_TIMER;
    ev->key = win->timers[first].id;
    ev->x = ev->y = ev->menu = 0;
    ev->buttons = 0;
    return 1;
}

int window_wait_timeout(struct window *win, struct window_event *ev, int ms)
{
    uint64_t end = ms < 0 ? UINT64_MAX : now_ns() + (uint64_t)ms * 1000000;
    bool over = false;                              /* (a look at the socket after the end found nothing) */
    struct wmsg m;
    int r;
    for (;;) {
        while (win->npending) {                     /* (kept events first: they came first) */
            next_message(win, &m);
            if ((r = to_event(win, &m, ev)) != 0)
                goto got;
        }
        uint64_t now = now_ns(), due;
        if (timer_due(win, now, ev, &due))
            return 1;
        if (over) {
            ev->type = WINDOW_NONE;
            return 0;
        }
        uint64_t until = due < end ? due : end;
        int wait = -1;
        if (until != UINT64_MAX) {
            uint64_t left = until > now ? (until - now + 999999) / 1000000 : 0;
            wait = left > INT_MAX ? INT_MAX : (int)left;
        }
        struct pollfd p = { win->sock, POLLIN, 0 };
        int n = poll(&p, 1, wait);
        if (n > 0) {
            if (read_full(win->sock, &m, sizeof(m)) < 0 || drop_read(win, &m) < 0)
                r = -1;                             /* windowd went away */
            else if ((r = to_event(win, &m, ev)) == 0)
                continue;
            goto got;
        }
        if (n < 0 && errno != EINTR) {
            r = -1;
            goto got;
        }
        /* (poll's wait counts ticks and may end early: the clock decides) */
        over = end != UINT64_MAX && now_ns() >= end;
    }
got:
    if (r > 0)
        return 1;
    ev->type = WINDOW_CLOSED;
    return -1;
}

int window_wait(struct window *win, struct window_event *ev)
{
    return window_wait_timeout(win, ev, -1) > 0;
}

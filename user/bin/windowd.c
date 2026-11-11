/*
 * windowd: the window server (step 15e). It takes the screen from the text
 * console, keeps a screen-sized back buffer, and listens on the local socket
 * WINDOW_SOCKET. Each client makes a window (a shared buffer of pixels passed
 * over the socket); windowd composites the desktop, every window with its
 * decorations (a title bar and a border), and the mouse pointer, and flushes
 * the result to the screen. Pointer and keyboard events reach windowd through
 * the display (DISPLAY_EVENT) and are forwarded to the topmost window.
 *
 * Step 15e draws and shows windows and the pointer; step 15e2 adds the
 * interaction: a click focuses and raises a window, and dragging its title
 * bar moves it.
 *
 * (41h-1) `windowd headless WxH` takes no screen and no input devices: it
 * composes into its back buffer alone (any user may run one, for testing
 * programs), starts no session program, and listens where the variable
 * WINDOW_SOCKET says (as libwindow connects), so it can run beside the
 * screen's windowd.
 */
#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread.h>
#include <time.h>
#include <unistd.h>
#include "image.h"
#include "ritteros.h"
#include "settings_file.h"
#include "wfont.h"
#include "windowproto.h"

PROGRAM_HELP("windowd [headless WxH]",
             "the window server: shows windows on the screen (windowd & starts it)");

#if defined(__x86_64__)
#define ARCH_DIR "x86_64"
#else
#define ARCH_DIR "aarch64"
#endif

#define MAX_WINDOWS 8
#define OUTQ        32          /* events waiting for one client (pointer moves merged) */
#define CLIENT_POLL_MS 10       /* a client's thread sends its queue at least this often */
#define TITLEBAR    24          /* title-bar height, pixels (thick, room for the boxes) */
#define BORDER      4           /* border around a window (thick, so it is easy to grab) */
#define CLOSEBOX    TITLEBAR    /* the close box is a square at the right of the title bar */
#define MINBOX      TITLEBAR    /* the minimize box, a square just left of the close box */
#define MAXBOX      TITLEBAR    /* the maximize box, a square just left of the minimize box */
#define TASKBAR_H   20          /* minimized windows show as buttons this tall, at the bottom */
#define TASKBUTTON_W 160        /* each such button's width (with a 2 px gap) */
#define RESIZE_MARGIN 8         /* how near an edge a press grabs it to resize */
#define MIN_CONTENT_W 60        /* a window will not be resized smaller than this */
#define WINDOW_SIZE_MAX 8192    /* the largest content a client may give (then made to fit) */
#define MIN_CONTENT_H 40
#define RZ_LEFT     1           /* which edges a resize is dragging */
#define RZ_RIGHT    2
#define RZ_TOP      4
#define RZ_BOTTOM   8

#define MENUBAR_H   16          /* a window's menu bar, below the title bar, if it has one */
#define MENUITEM_H  16          /* a dropdown item's height */
#define DROPDOWN_W  190         /* a dropdown's width (wide enough for "Close window server") */
#define MENU_DESKTOP MAX_WINDOWS /* menu_win sentinel: the desktop's own menu (no window owns it) */

/* (41g-5) The colours, by the user's `theme`: dark (Nord's polar night, as
 * before 41g-5) or light (its snow storm); the window in front's title bar
 * and a menu's chosen item are window_color's SELECTION (the user's accent
 * when set), with white on them. Chosen in main, before anything is drawn. */
struct palette {
    uint32_t desktop, frame, title_off, title_text_off, menubar, dropdown, menu_text,
             note_bg, note_title, note_text, outline;
};
static const struct palette palette_dark = {
    0x2E3440, 0x4C566A, 0x434C5E, 0xFFFFFF, 0x3B4252, 0x434C5E, 0xFFFFFF,
    0x3B4252, 0xFFFFFF, 0xD8DEE9, 0xECEFF4 };
static const struct palette palette_light = {
    0xD8DEE9, 0xA7B0BE, 0xE5E9F0, 0x3B4252, 0xE5E9F0, 0xECEFF4, 0x2E3440,
    0xE5E9F0, 0x2E3440, 0x3B4252, 0x4C566A };
static struct palette pal;
static uint32_t selection;              /* window_color(WINDOW_COLOR_SELECTION) */
#define DESKTOP_BG  (pal.desktop)
#define FRAME       (pal.frame)
#define TITLEBAR_FOCUSED   selection    /* the window in front */
#define TITLEBAR_UNFOCUSED (pal.title_off)      /* the others, dimmer */
#define MENUBAR_BG  (pal.menubar)       /* the menu bar */
#define DROPDOWN_BG (pal.dropdown)      /* a dropdown */
#define MENU_HILITE selection           /* the item under the pointer / open menu */

struct wmenu_item { int id; char label[WINDOW_LABEL_MAX]; };
struct wmenu {
    char title[WINDOW_LABEL_MAX];
    struct wmenu_item items[WINDOW_MENU_ITEMS];
    int n_items;
};

struct window {
    int used;
    int sock;                   /* the client's connection */
    int bufd;                   /* the shared buffer descriptor */
    uint32_t *pix;              /* its mapped pixels (w x h) */
    uint32_t w, h;
    int ox, oy;                 /* outer top-left on the screen (may be dragged) */
    uint64_t z;                 /* stacking order: higher is nearer the front */
    int minimized;              /* hidden, shown as a button at the bottom of the screen */
    int maximized;              /* filling the screen; the `+` box restores the saved geometry */
    int saved_ox, saved_oy;     /* the window's place before it was maximized */
    uint32_t saved_w, saved_h;  /* its content size before it was maximized */
    int pending_resize;         /* waiting for the client's new buffer after a resize */
    int pending_ox, pending_oy; /* where the window goes once that buffer arrives */
    char title[WINDOW_TITLE_MAX];
    struct wmenu menus[WINDOW_MENU_MAX];        /* the menu bar (n_menus > 0: shown) */
    int n_menus;
    struct wmsg outq[OUTQ];     /* events for the client, sent by its own thread */
    int outq_n;
    int focus_told;             /* (41f-1) what the client was last told: 1 focused, 0 not, -1 nothing yet */
    uint32_t min_w, min_h, max_w, max_h;  /* (41f-2) the program's limits on its content (max 0: none) */
    int cursor;                 /* (41f-4) the pointer's shape over its content (WINDOW_CURSOR_*) */
    int parent;                 /* (41f-5) a dialog's window (it takes no input meanwhile), else -1 */
    int fullscreen;             /* (41f-6) the whole screen, no frame (window_fullscreen) */
    int fs_ox, fs_oy;           /* its place and content size before that */
    uint32_t fs_w, fs_h;
    char *drop;                 /* (41f-6) a drop's text, sent after its WMSG_DROP (heap), or 0 */
    uint32_t drop_len;
    char app[64];               /* (41g-3) its program's name (getprogname): its app's icon */
};

/* A rectangle of the screen (used for the dirty region and the resize outline). */
struct rect { int x, y, w, h; };

static struct sys_display screen;
static uint32_t *back;          /* the back buffer, screen->width x height */
static int back_fd = -1;        /* its shared buffer (66b-5: replaced when the mode changes) */
static int refresh_fixed;       /* (66b-5) the `refresh` setting chose the rate, not the screen */
static int headless;            /* (41h-1) no screen, no input devices: the back buffer alone */
static struct window windows[MAX_WINDOWS];
static uint64_t ztop;           /* the highest z given out so far */
static int cursor_x, cursor_y;
static int dragging = -1;       /* the window being dragged, or -1 */
static int drag_dx, drag_dy;    /* cursor minus the window's origin when grabbed */
static int resizing = -1;       /* the window being resized, or -1 */
static int ctrl_click;          /* the left button is down as a right-click (Ctrl + click) */
static int resize_edges;        /* which edges are grabbed (RZ_* bits) */
static int rs_gx, rs_gy, rs_gw, rs_gh;  /* its outer frame when the resize began */
static struct rect resize_outline;      /* the outline drawn now (and last) */
static char *drag_text;         /* (41f-6) what a drag carries (window_drag), heap, or 0 */
static uint32_t drag_len;
static int menu_win = -1;       /* the window whose menu is open (-1: none) */
static int menu_idx = -1;       /* which top-level menu is open; -1 with menu_win set: a popup */
static struct wmenu popup;      /* a context menu (menu_win set, menu_idx == -1) */
static int popup_x, popup_y;    /* a popup's top-left on the screen */
static uint8_t prev_buttons;    /* to notice a button going down or up */
static unsigned next_ticket, serving;   /* the lock between windowd's threads (take, give) */
static int done_threads[16];    /* finished client threads, to reap (no thread_detach) */
static int done_count;

/* (65e) A ticket lock, as malloc's (64i): a thread takes the next ticket
 * and sleeps on the futex of `serving` until its turn, and giving it back
 * wakes the waiters (only when there are some). Until 65e a thread that
 * found it taken slept 200 us and tried again: at least a timer tick (10
 * ms) each time, and a client thread working through its window's frames
 * took it back at once, so the input thread (the pointer, the keys) waited
 * 25-60 ms on average and up to 0.6 s, clients up to 3 s, on the J1900 with
 * two large windows (12.5 frames a second each). The turns are fair now. */
static void take(void)
{
    unsigned mine = __atomic_fetch_add(&next_ticket, 1, __ATOMIC_ACQUIRE);
    for (;;) {
        unsigned now = __atomic_load_n(&serving, __ATOMIC_ACQUIRE);
        if (now == mine)
            return;
        syscall6(SYS_FUTEX, (int64_t)&serving, FUTEX_WAIT, now, 0, 0, 0);   /* (back at once if */
    }                                                                    /* it has moved on) */
}

static void give(void)
{
    unsigned next = __atomic_add_fetch(&serving, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&next_ticket, __ATOMIC_ACQUIRE) != next)
        syscall6(SYS_FUTEX, (int64_t)&serving, FUTEX_WAKE, 0x7FFFFFFF, 0, 0, 0);
}

static uint32_t native(uint32_t rgb)
{
    return ((rgb >> 16 & 0xFF) << screen.red_shift) | ((rgb >> 8 & 0xFF) << screen.green_shift) |
           ((rgb & 0xFF) << screen.blue_shift);
}

static int menubar_h(int i)             /* the window's menu bar height (0 if it has none) */
{
    return windows[i].n_menus > 0 && !windows[i].fullscreen ? MENUBAR_H : 0;
}

/* (41f-6) A full-screen window has no frame: no border, no title bar. */
static int side(int i)                  /* its border's width */
{
    return windows[i].fullscreen ? 0 : BORDER;
}

static int title_h(int i)               /* its title bar's height */
{
    return windows[i].fullscreen ? 0 : TITLEBAR;
}

static int content_left(int i)          /* x of the window's content */
{
    return windows[i].ox + side(i);
}

static int content_top(int i)           /* y of the window's content (below title + menu bars) */
{
    return windows[i].oy + title_h(i) + menubar_h(i);
}

/* Only the changed rectangle is flushed, not the whole screen every time:
 * that was most of windowd's cost, and the lag with two windows redrawing at
 * once. */
static struct rect rect_window(int i)   /* a window's outer rectangle (frame + decorations) */
{
    struct rect r = { windows[i].ox, windows[i].oy, (int)windows[i].w + 2 * side(i),
                      (int)windows[i].h + title_h(i) + menubar_h(i) + side(i) };
    return r;
}

static struct rect rect_cursor(int x, int y)    /* the pointer's box (covers both shapes) */
{
    struct rect r = { x - 8, y - 8, 24, 24 };    /* the arrow (top-left) and a centred resize cursor */
    return r;
}

/* The smallest rectangle covering both (an empty one, w or h <= 0, is ignored). */
static struct rect rect_union(struct rect a, struct rect b)
{
    if (a.w <= 0 || a.h <= 0)
        return b;
    if (b.w <= 0 || b.h <= 0)
        return a;
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    struct rect r = { x0, y0, x1 - x0, y1 - y0 };
    return r;
}

/* (64l) What a redraw may draw into: the rectangle about to be flushed.
 * Every drawing helper keeps to it, so a pointer move or one window's new
 * frame redraws only that part of the desktop (until 64l every change
 * redrew all of it: on the J1900's 1600x1200 screen each mouse event cost
 * a whole desktop of pixels, more with every window open). Half-open:
 * x0 <= x < x1, y0 <= y < y1. */
static int clip_x0, clip_y0, clip_x1, clip_y1;

static void set_clip(struct rect r)
{
    clip_x0 = r.x < 0 ? 0 : r.x;
    clip_y0 = r.y < 0 ? 0 : r.y;
    clip_x1 = r.x + r.w > (int)screen.width ? (int)screen.width : r.x + r.w;
    clip_y1 = r.y + r.h > (int)screen.height ? (int)screen.height : r.y + r.h;
    if (clip_x1 < clip_x0)
        clip_x1 = clip_x0;
    if (clip_y1 < clip_y0)
        clip_y1 = clip_y0;
}

static int clipped_out(struct rect r)          /* nothing of r inside the clip */
{
    return r.x >= clip_x1 || r.y >= clip_y1 || r.x + r.w <= clip_x0 || r.y + r.h <= clip_y0;
}

static void put_px(int x, int y, uint32_t color)
{
    if (x >= clip_x0 && x < clip_x1 && y >= clip_y0 && y < clip_y1)
        back[(int64_t)y * screen.width + x] = color;
}

/* Text at (x, y), drawn into the clip's part of the back buffer only. */
static void text_at(int x, int y, const char *s, uint32_t color)
{
    if (clip_x1 <= clip_x0 || clip_y1 <= clip_y0)
        return;
    wfont_text(back + (int64_t)clip_y0 * screen.width + clip_x0, screen.width,
               (uint32_t)(clip_x1 - clip_x0), (uint32_t)(clip_y1 - clip_y0), wfont_regular(),
               x - clip_x0, y - clip_y0, s, color, WFONT_TRANSPARENT);
}

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

/* Receive a shared buffer the client just sent (it rides the connection and
 * may arrive a moment after the message). */
static int recv_shared(int sock)
{
    int bufd = -1;
    for (int i = 0; i < 20 && bufd < 0; i++) {
        bufd = ritteros_share_recv(sock);
        if (bufd < 0 && errno == EAGAIN)
            usleep(5000);
        else
            break;
    }
    return bufd;
}

/* --- drawing into the back buffer (holds the lock) --------------------------- */

/* A rectangle of a solid color, clipped to the clip (x, y may be off it). */
static void fill(int x, int y, int w, int h, uint32_t color)
{
    int x0 = x < clip_x0 ? clip_x0 : x, y0 = y < clip_y0 ? clip_y0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > clip_x1)
        x1 = clip_x1;
    if (y1 > clip_y1)
        y1 = clip_y1;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = back + (int64_t)yy * screen.width;
        for (int xx = x0; xx < x1; xx++)
            row[xx] = color;
    }
}

/* (41g-3) Apps' icons: (42b) each bundle's icon.png (32x32),
 * shown at half size (each 2x2 block's mean, weighted by alpha) in the
 * desktop menu and on taskbar buttons. Loaded once per app (a miss too),
 * holding the lock. */
#define ICON 16
struct icon {
    char app[64];
    int ok;
    uint8_t rgba[ICON * ICON * 4];
};
static struct icon icons[24];
static int icons_n;

static const struct icon *icon_of(const char *app)
{
    if (!app[0])
        return 0;
    for (int i = 0; i < icons_n; i++)
        if (!strcmp(icons[i].app, app))
            return icons[i].ok ? &icons[i] : 0;
    if (icons_n == (int)(sizeof(icons) / sizeof(icons[0])))
        return 0;
    struct icon *ic = &icons[icons_n++];
    snprintf(ic->app, sizeof(ic->app), "%s", app);
    ic->ok = 0;
    char path[200];                                 /* (42b) its bundle's icon.png */
    snprintf(path, sizeof(path), "/system/%s/apps/%s.program/icon.png", ARCH_DIR, app);
    struct image img, half;
    if (image_load(path, &img) < 0)
        return 0;
    if (img.w != 2 * ICON || img.h != 2 * ICON) {   /* (another size: scaled, nearest) */
        if (image_scale(&img, 2 * ICON, 2 * ICON, &half) < 0) {
            image_free(&img);
            return 0;
        }
        image_free(&img);
        img = half;
    }
    for (int y = 0; y < ICON; y++)
        for (int x = 0; x < ICON; x++) {
            unsigned sum[4] = { 0, 0, 0, 0 };
            for (int k = 0; k < 4; k++) {
                const uint8_t *p = img.pixels + (2 * y + k / 2) * img.stride + (2 * x + k % 2) * 4;
                for (int c = 0; c < 3; c++)
                    sum[c] += p[c] * p[3];
                sum[3] += p[3];
            }
            uint8_t *o = ic->rgba + (y * ICON + x) * 4;
            for (int c = 0; c < 3; c++)
                o[c] = (uint8_t)(sum[3] ? sum[c] / sum[3] : 0);
            o[3] = (uint8_t)(sum[3] / 4);
        }
    image_free(&img);
    ic->ok = 1;
    return ic;
}

/* An icon at (x, y), blended over what is there, within the clip. */
static void draw_icon(int x, int y, const struct icon *ic)
{
    for (int r = 0; r < ICON; r++)
        for (int c = 0; c < ICON; c++) {
            const uint8_t *p = ic->rgba + (r * ICON + c) * 4;
            int px = x + c, py = y + r;
            if (!p[3] || px < clip_x0 || px >= clip_x1 || py < clip_y0 || py >= clip_y1)
                continue;
            uint32_t d = back[(int64_t)py * screen.width + px];
            uint32_t out = 0;
            const int shift[3] = { screen.red_shift, screen.green_shift, screen.blue_shift };
            for (int k = 0; k < 3; k++) {
                unsigned under = d >> shift[k] & 0xFF;
                out |= (uint32_t)((p[k] * p[3] + under * (255 - p[3]) + 127) / 255) << shift[k];
            }
            put_px(px, py, out);
        }
}

/* The desktop's menu (below): its item ids, and each app's program. */
enum { DESK_QUIT = 1, DESK_APP = 2 };           /* app k: DESK_APP + k */
static char desk_apps[WINDOW_MENU_ITEMS][64];   /* their programs (NAME) */

/* A dropdown's width: the desktop menu's has room for icons. */
static int dropdown_w(void)
{
    return menu_win == MENU_DESKTOP ? DROPDOWN_W + ICON + 4 : DROPDOWN_W;
}

/* The x range [*x0, *x1) of the idx-th top-level menu title in window wi's bar. */
static void menu_title_span(int wi, int idx, int *x0, int *x1)
{
    int x = windows[wi].ox + BORDER + 2;
    for (int k = 0; k <= idx && k < windows[wi].n_menus; k++) {
        int wdt = (int)strlen(windows[wi].menus[k].title) * 9 + 14;
        if (k == idx) { *x0 = x; *x1 = x + wdt; return; }
        x += wdt;
    }
}

/* Which top-level menu of window wi a point is over in its menu bar, or -1. */
static int menubar_title_at(int wi, int x, int y)
{
    if (menubar_h(wi) == 0)
        return -1;
    int my0 = windows[wi].oy + TITLEBAR;
    if (y < my0 || y >= my0 + MENUBAR_H)
        return -1;
    for (int k = 0; k < windows[wi].n_menus; k++) {
        int x0, x1;
        menu_title_span(wi, k, &x0, &x1);
        if (x >= x0 && x < x1)
            return k;
    }
    return -1;
}

/* The items and top-left of whatever menu is open now (a bar dropdown, or a
 * context pop-up when menu_idx < 0). Returns the item count, 0 if none open. */
static int open_menu_items(struct wmenu_item **items, int *x, int *y)
{
    if (menu_win < 0)
        return 0;
    if (menu_idx >= 0) {
        struct wmenu *mn = &windows[menu_win].menus[menu_idx];
        int x0, x1;
        menu_title_span(menu_win, menu_idx, &x0, &x1);
        *items = mn->items;
        *x = x0;
        *y = windows[menu_win].oy + TITLEBAR + MENUBAR_H;
        return mn->n_items;
    }
    *items = popup.items;
    *x = popup_x;
    *y = popup_y;
    return popup.n_items;
}

/* The item index at (px, py) in the open dropdown: >= 0 the index, -1 outside
 * it (while a menu is open), -2 no menu open. */
static int dropdown_hit(int px, int py)
{
    struct wmenu_item *items;
    int x, y, n = open_menu_items(&items, &x, &y);
    if (menu_win < 0)
        return -2;
    if (n == 0 || px < x || px >= x + dropdown_w() || py < y || py >= y + n * MENUITEM_H)
        return -1;
    return (py - y) / MENUITEM_H;
}

/* A window's frame: border, title bar with its boxes, menu bar. */
static void draw_frame(const struct window *win, int focused)
{
    int wi = (int)(win - windows);
    int mh = menubar_h(wi);
    int ow = (int)win->w + 2 * BORDER, oh = (int)win->h + TITLEBAR + mh + BORDER;
    fill(win->ox, win->oy, ow, oh, native(FRAME));              /* frame + border */
    fill(win->ox, win->oy, ow, TITLEBAR,
         native(focused ? TITLEBAR_FOCUSED : TITLEBAR_UNFOCUSED));
    if (win->title[0])                                          /* the title, left of the bar */
        text_at(win->ox + 6, win->oy + 2, win->title, native(focused ? 0xFFFFFF : pal.title_text_off));
    int bx = win->ox + ow - CLOSEBOX;                           /* the close box: an X at the right */
    uint32_t xc = native(focused ? 0xFFFFFF : pal.title_text_off);
    for (int i = 5; i < CLOSEBOX - 5; i++) {
        put_px(bx + i, win->oy + i, xc);                        /* '\' */
        put_px(bx + i + 1, win->oy + i, xc);                    /* two pixels wide, for weight */
        put_px(bx + i, win->oy + (CLOSEBOX - 1 - i), xc);       /* '/' */
        put_px(bx + i + 1, win->oy + (CLOSEBOX - 1 - i), xc);
    }
    int mnx = win->ox + ow - CLOSEBOX - MINBOX;                 /* the minimize box: a - to its left */
    int my = win->oy + MINBOX / 2;
    for (int i = 5; i < MINBOX - 5; i++) {
        put_px(mnx + i, my, xc);
        put_px(mnx + i, my + 1, xc);                            /* two pixels tall */
    }
    int mxx = win->ox + ow - CLOSEBOX - MINBOX - MAXBOX;        /* the maximize box: a + to its left */
    int mxcx = mxx + MAXBOX / 2, mxcy = win->oy + TITLEBAR / 2; /* the + 's centre */
    int lh = MAXBOX / 2 - 5, lv = TITLEBAR / 2 - 5;             /* its arm half-lengths */
    for (int d = -lh; d <= lh; d++) {                          /* horizontal stroke, two pixels tall */
        put_px(mxcx + d, mxcy, xc);
        put_px(mxcx + d, mxcy - 1, xc);
    }
    for (int d = -lv; d <= lv; d++) {                          /* vertical stroke, two pixels wide */
        put_px(mxcx, mxcy + d, xc);
        put_px(mxcx - 1, mxcy + d, xc);
    }
    if (mh > 0) {                                              /* the menu bar, below the title */
        int my0 = win->oy + TITLEBAR;
        fill(win->ox, my0, ow, MENUBAR_H, native(MENUBAR_BG));
        for (int k = 0; k < win->n_menus; k++) {
            int tx0, tx1;
            menu_title_span(wi, k, &tx0, &tx1);
            if (menu_win == wi && menu_idx == k)               /* the open one, highlighted */
                fill(tx0, my0, tx1 - tx0, MENUBAR_H, native(MENU_HILITE));
            text_at(tx0 + 5, my0 + 1, win->menus[k].title,
                    native(menu_win == wi && menu_idx == k ? 0xFFFFFF : pal.menu_text));
        }
    }
}

static void draw_window(const struct window *win, int focused)
{
    int wi = (int)(win - windows);
    if (!win->fullscreen)                                       /* (41f-6) else the content alone */
        draw_frame(win, focused);
    int cx = content_left(wi), cy = content_top(wi);            /* content top-left */
    int x0 = cx < clip_x0 ? clip_x0 : cx, x1 = cx + (int)win->w > clip_x1 ? clip_x1 : cx + (int)win->w;
    int y0 = cy < clip_y0 ? clip_y0 : cy, y1 = cy + (int)win->h > clip_y1 ? clip_y1 : cy + (int)win->h;
    for (int yy = y0; x0 < x1 && yy < y1; yy++)                 /* (64l) the clipped rows, copied whole */
        memcpy(back + (int64_t)yy * screen.width + x0,
               win->pix + (int64_t)(yy - cy) * win->w + (x0 - cx), (size_t)(x1 - x0) * 4);
}

/* The open dropdown (a menu-bar menu or a context pop-up), on top of all else. */
static void draw_dropdown(void)
{
    struct wmenu_item *items;
    int x, y, n = open_menu_items(&items, &x, &y);
    if (n == 0)
        return;
    int w = dropdown_w(), desk = menu_win == MENU_DESKTOP;
    fill(x, y, w, n * MENUITEM_H, native(DROPDOWN_BG));
    int hi = (cursor_x >= x && cursor_x < x + w &&
              cursor_y >= y && cursor_y < y + n * MENUITEM_H) ? (cursor_y - y) / MENUITEM_H : -1;
    for (int i = 0; i < n; i++) {
        if (i == hi)
            fill(x, y + i * MENUITEM_H, w, MENUITEM_H, native(MENU_HILITE));
        int k = items[i].id - DESK_APP;             /* (41g-3) an app's icon, left of its name */
        const struct icon *ic = desk && k >= 0 && k < WINDOW_MENU_ITEMS ? icon_of(desk_apps[k]) : 0;
        if (ic)
            draw_icon(x + 3, y + i * MENUITEM_H, ic);
        text_at(x + 6 + (desk ? ICON + 4 : 0), y + i * MENUITEM_H + 1, items[i].label,
                native(i == hi ? 0xFFFFFF : pal.menu_text));
    }
}

static int window_at(int x, int y);             /* (defined below; used by cursor_shape) */
static int hover_win;                           /* (41f-3, below: the window whose content the pointer is over) */
static int resize_edges_at(int i, int x, int y);
static int spawn_program(const char *name);     /* (defined below; used by the desktop menu) */

/* The pointer's shape: an arrow, or a double-headed resize arrow when it is
 * over an edge or corner that resizes a window. */
enum { CUR_ARROW, CUR_H, CUR_V, CUR_NWSE, CUR_NESW, CUR_TEXT, CUR_HAND };

static int shape_for_edges(int e)
{
    if ((e & (RZ_LEFT | RZ_TOP)) == (RZ_LEFT | RZ_TOP) ||
        (e & (RZ_RIGHT | RZ_BOTTOM)) == (RZ_RIGHT | RZ_BOTTOM))
        return CUR_NWSE;                        /* a top-left or bottom-right corner: '\' */
    if ((e & (RZ_RIGHT | RZ_TOP)) == (RZ_RIGHT | RZ_TOP) ||
        (e & (RZ_LEFT | RZ_BOTTOM)) == (RZ_LEFT | RZ_BOTTOM))
        return CUR_NESW;                        /* a top-right or bottom-left corner: '/' */
    if (e & (RZ_LEFT | RZ_RIGHT))
        return CUR_H;                           /* a left or right edge: horizontal */
    if (e & (RZ_TOP | RZ_BOTTOM))
        return CUR_V;                           /* a top or bottom edge: vertical */
    return CUR_ARROW;
}

static int cursor_shape(void)
{
    if (resizing >= 0)                          /* mid-resize: keep the grabbed edge's shape */
        return shape_for_edges(resize_edges);
    if (dragging >= 0 || menu_win >= 0)
        return CUR_ARROW;
    int hit = window_at(cursor_x, cursor_y);
    if (hit >= 0) {
        int e = resize_edges_at(hit, cursor_x, cursor_y);
        if (e)
            return shape_for_edges(e);
    }
    if (hover_win >= 0 && windows[hover_win].used)   /* (41f-4) over a content: the program's shape */
        return windows[hover_win].cursor == 1 ? CUR_TEXT              /* (window.h's WINDOW_CURSOR_TEXT) */
             : windows[hover_win].cursor == 2 ? CUR_HAND : CUR_ARROW; /* (WINDOW_CURSOR_HAND) */
    return CUR_ARROW;
}

/* A small arrow pointer: a white wedge with a one-pixel dark outline. */
static void draw_arrow(void)
{
    static const char shape[16][12] = {
        "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X",
        "X.......X", "X....XXXX", "X..X.X", "X.X.X", "XX .X", "X   .X", "     .X", "      X",
    };
    for (int r = 0; r < 16; r++)
        for (int c = 0; shape[r][c]; c++) {
            if (shape[r][c] == 'X')
                put_px(cursor_x + c, cursor_y + r, native(0x000000));
            else if (shape[r][c] == '.')
                put_px(cursor_x + c, cursor_y + r, native(0xFFFFFF));
        }
}

/* A double-headed resize arrow, centred on the pointer: a white body (built
 * from the shaft and two heads) with a one-pixel black outline computed from it. */
static void draw_resize_cursor(int shape)
{
    uint8_t ink[16][16] = {{0}};
    if (shape == CUR_TEXT) {                            /* (41f-4) an I-beam, its stem at the hotspot */
        for (int r = 1; r <= 14; r++)
            ink[r][7] = 1;
        for (int c = 5; c <= 9; c++)
            ink[1][c] = ink[14][c] = 1;
    } else if (shape == CUR_HAND) {                     /* (41f-4) a hand, its finger at the hotspot */
        for (int r = 1; r <= 8; r++)
            ink[r][7] = ink[r][8] = 1;
        for (int r = 7; r <= 14; r++)
            for (int c = 5; c <= 12; c++)
                ink[r][c] = 1;
    } else if (shape == CUR_H || shape == CUR_V) {
        for (int i = 1; i <= 14; i++)                   /* the shaft, two pixels thick */
            ink[7][i] = ink[8][i] = 1;
        for (int d = 0; d <= 4; d++)                    /* a triangular head at each end */
            for (int y = 7 - d; y <= 8 + d; y++) {
                ink[y][1 + d] = 1;
                ink[y][14 - d] = 1;
            }
        if (shape == CUR_V) {                           /* turn it on its side */
            uint8_t t[16][16];
            for (int r = 0; r < 16; r++)
                for (int c = 0; c < 16; c++)
                    t[r][c] = ink[c][r];
            memcpy(ink, t, sizeof ink);
        }
    } else {                                            /* a diagonal '\' (NWSE) or '/' (NESW) */
        for (int i = 3; i <= 12; i++) {                 /* the shaft along the diagonal */
            int c = (shape == CUR_NWSE) ? i : 15 - i;
            ink[i][c] = 1;
            if (c + 1 < 16)
                ink[i][c + 1] = 1;
        }
        for (int d = 0; d <= 3; d++) {                  /* an L-shaped head at each tip */
            if (shape == CUR_NWSE) {
                ink[3][3 + d] = ink[3 + d][3] = 1;      /* top-left */
                ink[12][12 - d] = ink[12 - d][12] = 1;  /* bottom-right */
            } else {
                ink[3][12 - d] = ink[3 + d][12] = 1;    /* top-right */
                ink[12][3 + d] = ink[12 - d][3] = 1;    /* bottom-left */
            }
        }
    }
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 16; c++) {
            int x = cursor_x - 8 + c, y = cursor_y - 8 + r;
            if (ink[r][c])
                put_px(x, y, native(0xFFFFFF));
            else if ((r > 0 && ink[r - 1][c]) || (r < 15 && ink[r + 1][c]) ||
                     (c > 0 && ink[r][c - 1]) || (c < 15 && ink[r][c + 1]))
                put_px(x, y, native(0x000000));
        }
}

static void draw_cursor(void)
{
    int shape = cursor_shape();
    if (shape == CUR_ARROW)
        draw_arrow();
    else
        draw_resize_cursor(shape);
    if (drag_text)                              /* (41f-6) a drag: a page beside the arrow */
        for (int r = 6; r <= 15; r++)
            for (int c = 9; c <= 15; c++)
                put_px(cursor_x + c, cursor_y + r,
                       native(r == 6 || r == 15 || c == 9 || c == 15 ? 0x000000 : 0xFFFFFF));
}

/* The window in front (the highest z among the used ones), or -1. */
static int focused_window(void)
{
    int best = -1;
    uint64_t bz = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].used && !windows[i].minimized && (best < 0 || windows[i].z > bz)) {
            best = i;
            bz = windows[i].z;
        }
    return best;
}

/* The topmost window whose frame contains (x, y), or -1. Minimized windows are
 * off the screen (they show as buttons), so they are skipped. */
static int window_at(int x, int y)
{
    int best = -1;
    uint64_t bz = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].used || windows[i].minimized)
            continue;
        struct rect r = rect_window(i);
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h && (best < 0 || windows[i].z > bz)) {
            best = i;
            bz = windows[i].z;
        }
    }
    return best;
}

/* (41f-3) The window whose content the pointer is over: none over a
 * frame, a title bar or the desktop, nor while a menu is open. Each change
 * is told to both windows (WMSG_HOVER), the lock held. */
static int hover_win = -1;                      /* (declared above) */
static void enqueue(int id, struct wmsg m);      /* (defined below) */
static void composite_rect(struct rect r);        /* (defined below) */

static void hover_follow(int x, int y)
{
    int w = window_at(x, y);
    if (w >= 0 && (menu_win >= 0 || x < content_left(w) || x >= content_left(w) + (int)windows[w].w ||
                   y < content_top(w) || y >= content_top(w) + (int)windows[w].h))
        w = -1;
    if (w == hover_win)
        return;
    if (hover_win >= 0 && windows[hover_win].used) {
        struct wmsg left = { WMSG_HOVER, 0, 0, 0 };
        enqueue(hover_win, left);
    }
    if (w >= 0) {
        struct wmsg came = { WMSG_HOVER, 1, 0, 0 };
        enqueue(w, came);
    }
    hover_win = w;
}

/* (41f-5) The dialog open over window i (the innermost, when a dialog has
 * its own), or -1. */
static int dialog_of(int i)
{
    int found = -1;
    for (int depth = 0; depth < MAX_WINDOWS; depth++) {
        int next = -1;
        for (int j = 0; j < MAX_WINDOWS; j++)
            if (windows[j].used && windows[j].parent == i && !windows[j].minimized)
                next = j;
        if (next < 0)
            break;
        found = i = next;
    }
    return found;
}

/* (41f-6) The drag let go at (x, y): its text to the window whose content
 * is there (hover_win, kept by hover_follow), unless a dialog is open over
 * it, or it still has a drop to send, or its queue is full; then the drag
 * is over. Lock held. */
static void drop_at(int x, int y)
{
    int t = hover_win;
    if (t >= 0 && windows[t].used && dialog_of(t) < 0 && !windows[t].drop && windows[t].outq_n < OUTQ) {
        struct wmsg m = { WMSG_DROP, (uint32_t)(x - content_left(t)), (uint32_t)(y - content_top(t)),
                          drag_len };
        windows[t].drop = drag_text;                /* its thread sends it after the message */
        windows[t].drop_len = drag_len;
        enqueue(t, m);
    } else {
        free(drag_text);
    }
    drag_text = 0;
    composite_rect(rect_cursor(x, y));              /* the page off the pointer */
}

static int on_titlebar(int i, int x, int y)
{
    if (windows[i].fullscreen)
        return 0;
    int ow = (int)windows[i].w + 2 * BORDER;
    return x >= windows[i].ox && x < windows[i].ox + ow &&
           y >= windows[i].oy && y < windows[i].oy + TITLEBAR;
}

static int on_closebox(int i, int x, int y)
{
    if (windows[i].fullscreen)
        return 0;
    int bx = windows[i].ox + (int)windows[i].w + 2 * BORDER - CLOSEBOX;
    return x >= bx && x < bx + CLOSEBOX && y >= windows[i].oy && y < windows[i].oy + TITLEBAR;
}

static int on_minbox(int i, int x, int y)
{
    if (windows[i].fullscreen)
        return 0;
    int bx = windows[i].ox + (int)windows[i].w + 2 * BORDER - CLOSEBOX - MINBOX;
    return x >= bx && x < bx + MINBOX && y >= windows[i].oy && y < windows[i].oy + TITLEBAR;
}

static int on_maxbox(int i, int x, int y)
{
    if (windows[i].fullscreen)
        return 0;
    int bx = windows[i].ox + (int)windows[i].w + 2 * BORDER - CLOSEBOX - MINBOX - MAXBOX;
    return x >= bx && x < bx + MAXBOX && y >= windows[i].oy && y < windows[i].oy + TITLEBAR;
}

/* The button of the `slot`-th minimized window, at the bottom of the screen. */
static struct rect taskbar_button(int slot)
{
    struct rect r = { slot * TASKBUTTON_W, (int)screen.height - TASKBAR_H,
                      TASKBUTTON_W - 2, TASKBAR_H };
    return r;
}

/* The minimized window whose taskbar button is at (x, y), or -1. */
static int taskbar_hit(int x, int y)
{
    if (y < (int)screen.height - TASKBAR_H)
        return -1;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].used && windows[i].fullscreen && !windows[i].minimized)
            return -1;                          /* (41f-6) covered by a full-screen window */
    int slot = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].used || !windows[i].minimized)
            continue;
        struct rect b = taskbar_button(slot);
        if (x >= b.x && x < b.x + b.w)
            return i;
        slot++;
    }
    return -1;
}

/* Which edges of window i a press at (x, y) grabs, for resizing (0 if none:
 * the press is in the interior, not within RESIZE_MARGIN of an edge). */
static int resize_edges_at(int i, int x, int y)
{
    struct rect r = rect_window(i);
    if (windows[i].fullscreen || x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h)
        return 0;
    int e = 0;
    if (x < r.x + RESIZE_MARGIN)
        e |= RZ_LEFT;
    if (x >= r.x + r.w - RESIZE_MARGIN)
        e |= RZ_RIGHT;
    if (y < r.y + BORDER)                   /* only the thin top strip, so the title bar still drags */
        e |= RZ_TOP;
    if (y >= r.y + r.h - RESIZE_MARGIN)
        e |= RZ_BOTTOM;
    return e;
}

/* A two-pixel-thick rectangle outline (the live resize preview). */
static void draw_outline(struct rect r, uint32_t color)
{
    for (int x = r.x; x < r.x + r.w; x++) {
        put_px(x, r.y, color);          put_px(x, r.y + 1, color);
        put_px(x, r.y + r.h - 1, color); put_px(x, r.y + r.h - 2, color);
    }
    for (int y = r.y; y < r.y + r.h; y++) {
        put_px(r.x, y, color);          put_px(r.x + 1, y, color);
        put_px(r.x + r.w - 1, y, color); put_px(r.x + r.w - 2, y, color);
    }
}

/* (41g-4) Notifications (WMSG_NOTIFY): boxes at the top right, over
 * everything, the newest first, each shown for NOTE_US; notes_thread takes
 * them away. Under the lock. */
#define NOTES     4             /* at most this many at once (a new one drops the oldest) */
#define NOTE_W    320
#define NOTE_PAD  8             /* inside a box, around its text */
#define NOTE_GAP  8             /* from the screen's edges, and between boxes */
#define NOTE_US   5000000u
#define NOTE_BG   (pal.note_bg)
#define NOTE_TEXT (pal.note_text)
struct note {
    char title[WINDOW_TITLE_MAX];
    char text[WINDOW_NOTIFY_MAX];
    int h;                      /* its box's height */
    uint64_t until;             /* (now_us) when it goes */
};
static struct note notes[NOTES];
static int notes_n;
static unsigned notes_seq;      /* moves when one is added: wakes notes_thread */

/* Where the boxes are (all of them; 0 wide when there are none). */
static struct rect rect_notes(void)
{
    int h = 0;
    for (int i = 0; i < notes_n; i++)
        h += notes[i].h + NOTE_GAP;
    return (struct rect){ (int)screen.width - NOTE_W - NOTE_GAP, NOTE_GAP, notes_n ? NOTE_W : 0, h };
}

/* utf8 wrapped to width at (x, y) in font, inside the clip; its height
 * (buf 0 of wfont_layout: only measured when draw is 0). */
static int layout_at(int x, int y, int width, const char *utf8, const struct font *font,
                     uint32_t color, int draw)
{
    if (!draw || clip_x1 <= clip_x0 || clip_y1 <= clip_y0)
        return wfont_layout(0, 0, 0, 0, font, 0, 0, width, utf8, color, WFONT_TRANSPARENT);
    return wfont_layout(back + (int64_t)clip_y0 * screen.width + clip_x0, screen.width,
                        (uint32_t)(clip_x1 - clip_x0), (uint32_t)(clip_y1 - clip_y0), font,
                        x - clip_x0, y - clip_y0, width, utf8, color, WFONT_TRANSPARENT);
}

/* A box's height for its title and text. */
static int note_height(const struct note *n)
{
    int h = 2 * NOTE_PAD;
    if (n->title[0])
        h += wfont_height(wfont_bold()) + 2;
    if (n->text[0])
        h += layout_at(0, 0, NOTE_W - 2 * NOTE_PAD, n->text, wfont_regular(), 0, 0);
    return h;
}

static void draw_notes(void)
{
    struct rect all = rect_notes();
    if (clipped_out(all))
        return;
    int y = all.y;
    for (int i = 0; i < notes_n; i++) {
        const struct note *n = &notes[i];
        fill(all.x, y, NOTE_W, n->h, native(NOTE_BG));
        draw_outline((struct rect){ all.x, y, NOTE_W, n->h }, native(FRAME));
        int ty = y + NOTE_PAD;
        if (n->title[0]) {
            layout_at(all.x + NOTE_PAD, ty, NOTE_W - 2 * NOTE_PAD, n->title, wfont_bold(),
                      native(pal.note_title), 1);
            ty += wfont_height(wfont_bold()) + 2;
        }
        if (n->text[0])
            layout_at(all.x + NOTE_PAD, ty, NOTE_W - 2 * NOTE_PAD, n->text, wfont_regular(),
                      native(NOTE_TEXT), 1);
        y += n->h + NOTE_GAP;
    }
}

/* Minimized windows: a button with the title at the bottom of the screen. */
static void draw_taskbar(void)
{
    int slot = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].used || !windows[i].minimized)
            continue;
        struct rect b = taskbar_button(slot++);
        fill(b.x, b.y, b.w, b.h, native(TITLEBAR_UNFOCUSED));
        fill(b.x, b.y, b.w, 1, native(FRAME));          /* a top edge */
        const struct icon *ic = icon_of(windows[i].app);    /* (41g-3) its app's icon first */
        if (ic)
            draw_icon(b.x + 3, b.y + 2, ic);
        if (windows[i].title[0])
            text_at(b.x + (ic ? ICON + 7 : 5), b.y + 3, windows[i].title, native(pal.title_text_off));
    }
}

/* Redraw the rectangle r of the desktop into the back buffer (holds the
 * lock): what is about to be flushed, and nothing else (64l). Windows are
 * drawn back to front by their stacking order; the front one is the focused
 * one; a window outside r is skipped. The clip stays r, so drawing that
 * follows (the resize outline) keeps to it too. */
static void redraw_back(struct rect r)
{
    set_clip(r);
    fill(0, 0, (int)screen.width, (int)screen.height, native(DESKTOP_BG));
    int order[MAX_WINDOWS], n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].used && !windows[i].minimized)   /* minimized ones show as buttons */
            order[n++] = i;
    for (int i = 1; i < n; i++) {                   /* insertion sort by z ascending */
        int k = order[i], j = i - 1;
        while (j >= 0 && windows[order[j]].z > windows[k].z) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = k;
    }
    int foc = focused_window(), bar = 0;
    for (int i = 0; i < n; i++) {
        if (windows[order[i]].fullscreen && !bar) {
            draw_taskbar();                         /* (41f-6) a full-screen window covers it */
            bar = 1;
        }
        if (!clipped_out(rect_window(order[i])))
            draw_window(&windows[order[i]], order[i] == foc);
    }
    if (!bar)
        draw_taskbar();
    if (resizing >= 0)                               /* (65e-34b) the live resize outline */
        draw_outline(resize_outline, native(pal.outline));
    draw_notes();                                    /* (41g-4) over every window */
    draw_dropdown();                                 /* an open menu, over everything */
    draw_cursor();
}

/* Copy one rectangle of the back buffer to the screen (clipped to it). */
/* Copy rectangle d of the back buffer to the screen; false when the
 * kernel could not (41e-2: the first full frame's copy failed now and
 * then with EFAULT, partway, leaving the console's rows on the screen: the
 * compositor marks it again and the next frame copies it). */
static bool flush_rect(struct rect d)
{
    if (d.x < 0) { d.w += d.x; d.x = 0; }
    if (d.y < 0) { d.h += d.y; d.y = 0; }
    if (d.x + d.w > (int)screen.width)
        d.w = (int)screen.width - d.x;
    if (d.y + d.h > (int)screen.height)
        d.h = (int)screen.height - d.y;
    if (d.w <= 0 || d.h <= 0)
        return true;
    return ritteros_display_flush(back + (int64_t)d.y * screen.width + d.x, screen.width * 4,
                                  (uint32_t)d.x, (uint32_t)d.y, (uint32_t)d.w, (uint32_t)d.h) == 0;
}

/* (65e-34b) Drawing at the screen's rate. A change only marks its
 * rectangle (composite_rect, holding the lock) and wakes the compositor
 * thread, which redraws and shows the union of what was marked, once per
 * frame: at once after a quiet moment, then at most `frame_us` apart.
 * Until 65e-34b every change drew at once: dragging a large window drew
 * once per pointer event (~12 ms for 1.8 Mpx on the J1900), more events
 * came than could be drawn, and the window trailed the pointer; frames a
 * client drew faster than the screen shows were drawn too. */
static struct rect pending;             /* the union of what changed since the last frame */
static int pending_any;
static unsigned dirty_seq;              /* moves with every change: the compositor waits on it */
static int compositor_waits;
static uint64_t flush_failures;          /* (41e-2) flushes the kernel refused, done again */
static uint64_t frame_us = 16667;       /* one refresh of the screen (60 Hz until main asks) */

/* (66c-1) Where frames' time goes, for `kill -USR1` (frame_report): the
 * frames drawn, the pixels in them, the time composing them into the back
 * buffer and the time in DISPLAY_FLUSH (the kernel's copy to the screen). */
static uint64_t frames, frame_pixels, compose_us, flush_us;
static volatile sig_atomic_t report_wanted;
static int use_vblank;                  /* (66c-3) frames at the screen's vertical blanks */
/* (37e) The screen off after a while without input (INPUT_BLANK): 1 to
 * show black once, 2 black (nothing drawn until it is on again). */
static int blank_state;

/* (66d-3) Page flipping, where a driver has screen buffers: each frame is
 * composed in `back` as before, then the union of its change and the last
 * frame's (the hidden buffer missed that one) is copied into the hidden
 * buffer, which is shown from the next vertical blank. Mapped write-
 * combining: written, never read. */
static int use_flip;
static struct sys_display_buffers screen_buffers;
static int flip_hidden = 1;             /* the buffer drawn next (0, the console's, shows first) */
static struct rect flip_last;           /* the last frame's change */
static int flip_last_any;

static void composite_all(void);

/* Copy rectangle d of the back buffer into screen buffer k (holding the lock). */
static void copy_to_buffer(struct rect d, int k)
{
    if (d.x < 0) { d.w += d.x; d.x = 0; }
    if (d.y < 0) { d.h += d.y; d.y = 0; }
    if (d.x + d.w > (int)screen_buffers.width)
        d.w = (int)screen_buffers.width - d.x;
    if (d.y + d.h > (int)screen_buffers.height)
        d.h = (int)screen_buffers.height - d.y;
    if (d.w <= 0 || d.h <= 0)
        return;
    uint8_t *to = (uint8_t *)(uintptr_t)screen_buffers.address[k];
    for (int y = d.y; y < d.y + d.h; y++)
        memcpy(to + (uint64_t)y * screen_buffers.stride + (uint64_t)d.x * 4,
               back + (int64_t)y * screen.width + d.x, (size_t)d.w * 4);
}

static uint64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

/* Show rectangle d from the next frame on (holding the lock). */
static void composite_rect(struct rect d)
{
    pending = pending_any ? rect_union(pending, d) : d;
    pending_any = 1;
    __atomic_add_fetch(&dirty_seq, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&compositor_waits, __ATOMIC_ACQUIRE))
        syscall6(SYS_FUTEX, (int64_t)&dirty_seq, FUTEX_WAKE, 1, 0, 0, 0);
}

/* (41g-4) A notification in front of the others (holding the lock). */
static void note_add(const char *title, const char *text)
{
    struct rect before = rect_notes();
    if (notes_n == NOTES)
        notes_n--;                                   /* the oldest goes */
    memmove(&notes[1], &notes[0], (size_t)notes_n * sizeof(notes[0]));
    notes_n++;
    struct note *n = &notes[0];
    snprintf(n->title, sizeof(n->title), "%s", title);
    snprintf(n->text, sizeof(n->text), "%s", text);
    n->h = note_height(n);
    n->until = now_us() + NOTE_US;
    struct rect after = rect_notes();
    composite_rect(before.w ? rect_union(before, after) : after);
    __atomic_add_fetch(&notes_seq, 1, __ATOMIC_RELEASE);
    syscall6(SYS_FUTEX, (int64_t)&notes_seq, FUTEX_WAKE, 1, 0, 0, 0);
}

/* Takes each notification away when its time is up. The oldest goes
 * first (each is shown as long), so the wait is always for the last one. */
static int notes_thread(void *arg)
{
    (void)arg;
    for (;;) {
        take();
        if (!notes_n) {
            unsigned seen = __atomic_load_n(&notes_seq, __ATOMIC_ACQUIRE);
            give();
            syscall6(SYS_FUTEX, (int64_t)&notes_seq, FUTEX_WAIT, seen, 0, 0, 0);
            continue;
        }
        uint64_t now = now_us(), until = notes[notes_n - 1].until;
        if (now >= until) {
            struct rect before = rect_notes();
            while (notes_n && notes[notes_n - 1].until <= now)
                notes_n--;
            composite_rect(before);
            give();
            continue;
        }
        give();
        uint64_t wait = until - now;
        usleep((useconds_t)(wait > 1000000 ? 1000000 : wait));
    }
    return 0;
}

/* (41h-2) A copy of the screen as composed (the pointer too) with every
 * change so far, taken under the lock so no frame is half in it, to the client in a shared buffer of
 * its own (the back buffer is never shared: the next frame would change
 * what the client reads). */
static void shot_send(int sock)
{
    size_t n = (size_t)screen.width * screen.height * 4;
    int fd = ritteros_share_create(n);
    uint32_t *pix = fd < 0 ? 0 : ritteros_share_map(fd);
    if (!pix) {
        if (fd >= 0)
            close(fd);
        return;                                      /* (the client sees the connection end) */
    }
    take();
    if (pending_any && blank_state != 2)
        redraw_back(pending);                        /* (what the next frame shows: kept
                                                  * pending, so it is still flushed) */
    memcpy(pix, back, n);
    give();
    struct wmsg r = { WMSG_SHOT, screen.width, screen.height,
                      (uint32_t)screen.red_shift | (uint32_t)screen.green_shift << 8 |
                          (uint32_t)screen.blue_shift << 16 };
    if (write_full(sock, &r, sizeof(r)) == 0)
        ritteros_share_send(sock, fd);
    munmap(pix, n);
    close(fd);
}

static int client_read(int id, int sock, void *buf, size_t n);   /* (defined below) */
static void input_event(struct sys_input_event e);              /* (defined below) */

/* (41h-6) The open windows' titles, each ending in a newline, after a
 * WMSG_WINDOWS saying how many and how long. */
static void titles_send(int sock)
{
    char text[MAX_WINDOWS * WINDOW_TITLE_MAX];
    size_t n = 0;
    uint32_t count = 0;
    take();
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].used) {
            n += (size_t)snprintf(text + n, sizeof(text) - n, "%s\n", windows[i].title);
            count++;
        }
    give();
    struct wmsg r = { WMSG_WINDOWS, count, 0, (uint32_t)n };
    if (write_full(sock, &r, sizeof(r)) == 0 && n)
        write_full(sock, text, n);
}

/* (41h-3) WMSG_INPUT events, the first in m, until the client stops
 * sending them: each handled as one from the devices and answered once
 * the programs' queues are sent.
 * Only headless: on the screen, the input is the machine's keyboard and
 * mouse alone (another program of the user's would otherwise type into
 * the user's windows). */
static void input_take(int sock, struct wmsg m)
{
    do {
        struct wmsg r = { WMSG_INPUT, headless ? 1u : 0u, 0, 0 };
        if (headless) {
            struct sys_input_event e = { (uint8_t)m.a, (uint8_t)(m.a >> 8), (uint8_t)(m.a >> 16),
                                         (uint8_t)(m.a >> 24), (uint16_t)m.b, (uint16_t)(m.b >> 16) };
            if (e.x >= screen.width)                 /* (on the screen, as a mouse's) */
                e.x = (uint16_t)(screen.width - 1);
            if (e.y >= screen.height)
                e.y = (uint16_t)(screen.height - 1);
            input_event(e);
            /* Answered once what it made went out to the programs: windowd
             * keeps OUTQ events a window and drops the rest, which only a
             * program slower than a person's typing meets, but wsend sends
             * as fast as it is answered (at most 2 s). */
            for (int t = 0; t < 2000; t++) {
                int queued = 0;
                take();
                for (int i = 0; i < MAX_WINDOWS; i++)
                    queued |= windows[i].used && windows[i].outq_n > 0;
                give();
                if (!queued)
                    break;
                usleep(1000);
            }
        }
        if (write_full(sock, &r, sizeof(r)) < 0 || !headless)
            return;
    } while (read_full(sock, &m, sizeof(m)) == 0 && m.type == WMSG_INPUT);
}


/* (41g-4) A WMSG_NOTIFY's title and text (m.a and m.b bytes) from sock
 * (read by client_read for a window's client, id >= 0), then shown. */
static int note_take(int id, int sock, const struct wmsg *m)
{
    char title[WINDOW_TITLE_MAX] = {0}, text[WINDOW_NOTIFY_MAX] = {0};
    if (m->a >= sizeof(title) || m->b >= sizeof(text))
        return -1;
    if (id >= 0 ? (m->a && client_read(id, sock, title, m->a) < 0) ||
                          (m->b && client_read(id, sock, text, m->b) < 0)
                : (m->a && read_full(sock, title, m->a) < 0) || (m->b && read_full(sock, text, m->b) < 0))
        return -1;
    take();
    note_add(title, text);
    give();
    return 0;
}

/* (66c-1) The frame report, asked for with SIGUSR1: on windowd's output
 * and, where it may (root), in the log "windowd" (/system/logs/windowd.log),
 * since its output is the console it hides. */
static void frame_report(void)
{
    report_wanted = 0;
    uint64_t n = frames ? frames : 1;
    char line[256];
    snprintf(line, sizeof(line), "windowd: frames: %llu, composing %llu ms, flush calls %llu ms "
             "(%llu Mpx; per frame %llu.%03llu ms and %llu.%03llu ms; %llu MB/s flushed; %llu again)\n",
             (unsigned long long)frames, (unsigned long long)(compose_us / 1000),
             (unsigned long long)(flush_us / 1000), (unsigned long long)(frame_pixels / 1000000),
             (unsigned long long)(compose_us / n / 1000), (unsigned long long)(compose_us / n % 1000),
             (unsigned long long)(flush_us / n / 1000), (unsigned long long)(flush_us / n % 1000),
             (unsigned long long)(flush_us ? frame_pixels * 4 / flush_us : 0),
             (unsigned long long)flush_failures);
    fputs(line, stdout);
    fflush(stdout);
    ritteros_log_write("windowd", line);
}

static void on_usr1(int sig)
{
    (void)sig;
    report_wanted = 1;
    __atomic_add_fetch(&dirty_seq, 1, __ATOMIC_RELEASE);  /* wake the compositor to print it */
    syscall6(SYS_FUTEX, (int64_t)&dirty_seq, FUTEX_WAKE, 1, 0, 0, 0);
}

/* The compositor: waits for a change, for the frame's time (a sleep lasts
 * whole 10 ms ticks, so the target moves one frame each time and a wake
 * within half a tick of it counts: 60 frames a second on average), then
 * draws everything marked meanwhile in one go. */
static int compositor_thread(void *arg)
{
    (void)arg;
    sigset_t usr1 = 1UL << SIGUSR1;                   /* (66c-1) the frame report comes here */
    sigprocmask(SIG_UNBLOCK, &usr1, 0);
    uint64_t next = 0;                  /* when the next frame is due */
    for (;;) {
        if (report_wanted)
            frame_report();
        take();
        if ((!pending_any || blank_state == 2) && !report_wanted) {
            unsigned seen = __atomic_load_n(&dirty_seq, __ATOMIC_ACQUIRE);
            __atomic_store_n(&compositor_waits, 1, __ATOMIC_RELEASE);
            give();
            syscall6(SYS_FUTEX, (int64_t)&dirty_seq, FUTEX_WAIT, seen, 0, 0, 0);   /* (back at
                                                 * once if a change came meanwhile) */
            __atomic_store_n(&compositor_waits, 0, __ATOMIC_RELEASE);
            continue;
        }
        give();
        if (use_flip) {
            /* (66d-3) The flip waits for the vertical blank itself. */
        } else if (use_vblank) {                    /* (66c-3) draw as the screen's frame ends */
            uint64_t frame;
            ritteros_display_vblank(&frame, 100);   /* (none in 100 ms: a mode change; draw) */
        } else {
            uint64_t now = now_us();
            if (now + 5000 < next)
                usleep((useconds_t)(next - now));
        }
        take();
        if (blank_state == 2) {                     /* (37e) off meanwhile */
            give();
            continue;
        }
        uint64_t start = now_us();
        struct rect d = pending;
        pending_any = 0;
        if (blank_state == 1) {                     /* (37e) black, the whole screen */
            memset(back, 0, (size_t)screen.width * screen.height * 4);
            d = (struct rect){ 0, 0, (int)screen.width, (int)screen.height };
            blank_state = 2;
        } else {
            redraw_back(d);
        }
        uint64_t composed = now_us();
        int show = -1;
        if (use_flip) {                             /* (66d-3) into the hidden buffer */
            copy_to_buffer(flip_last_any ? rect_union(d, flip_last) : d, flip_hidden);
            flip_last = d;
            flip_last_any = 1;
            show = flip_hidden;
        } else if (!headless && !flush_rect(d)) {
            composite_rect(d);                      /* (41e-2) again next frame */
            flush_failures++;
        }
        uint64_t flushed = now_us();
        frames++;
        frame_pixels += (uint64_t)(d.w > 0 ? d.w : 0) * (uint64_t)(d.h > 0 ? d.h : 0);
        compose_us += composed - start;
        flush_us += flushed - composed;
        give();
        if (show >= 0) {                            /* shown from the next vertical blank */
            uint64_t frame;
            if (ritteros_display_flip(show, &frame) > 0) {
                flip_hidden = !show;
            } else {                                /* (66d-3) back to copying to the screen */
                take();
                use_flip = 0;
                composite_all();
                give();
                printf("windowd: page flipping stopped (%s): copying to the screen\n",
                       strerror(errno));
            }
        }
        next = next + frame_us >= start ? next + frame_us : start + frame_us;
    }
    return 0;
}

/* Redraw and show the whole screen (the first frame, and window open/close). */
static void composite_all(void)
{
    struct rect all = { 0, 0, (int)screen.width, (int)screen.height };
    composite_rect(all);
}

/* The desktop's own context menu (a right-click on the background). windowd
 * acts on the choice itself -- launching a program or quitting -- so its item
 * ids are windowd's own, not a client's (menu_win == MENU_DESKTOP marks it).
 * (41g) The apps are (42b) the bundles /system/<arch>/apps/NAME.program,
 * read each time it opens (an app installed since is there): each by its
 * info's `name` (NAME without one), sorted, then "Close window server". */

static void open_desktop_menu(int x, int y)
{
    int n = 0;
    char dir[64];
    snprintf(dir, sizeof(dir), "/system/%s/apps", ARCH_DIR);
    DIR *d = opendir(dir);
    for (struct dirent *e; d && n < WINDOW_MENU_ITEMS - 1 && (e = readdir(d));) {
        size_t len = strlen(e->d_name);
        if (len <= 8 || len - 8 >= sizeof(desk_apps[0]) || strcmp(e->d_name + len - 8, ".program"))
            continue;
        char path[200];
        struct wmenu_item it;
        struct stat st;
        snprintf(desk_apps[n], sizeof(desk_apps[0]), "%.*s", (int)(len - 8), e->d_name);
        snprintf(path, sizeof(path), "%s/%s/%s", dir, e->d_name, desk_apps[n]);
        if (stat(path, &st) != 0)
            continue;                               /* (a bundle without its program) */
        snprintf(path, sizeof(path), "%s/%s/info", dir, e->d_name);
        if (config_get(path, "name", it.label, sizeof(it.label)) <= 0 || !it.label[0])
            snprintf(it.label, sizeof(it.label), "%s", desk_apps[n]);
        it.id = DESK_APP + n;
        int j = n++;                                /* in by name (insertion) */
        while (j > 0 && strcasecmp(popup.items[j - 1].label, it.label) > 0) {
            popup.items[j] = popup.items[j - 1];
            j--;
        }
        popup.items[j] = it;
    }
    if (d)
        closedir(d);
    popup.items[n].id = DESK_QUIT;
    snprintf(popup.items[n].label, sizeof(popup.items[n].label), "Close window server");
    popup.n_items = n + 1;
    menu_win = MENU_DESKTOP;                     /* a pop-up no window owns */
    menu_idx = -1;                               /* drawn from `popup` */
    popup_x = x;
    popup_y = y;
    if (popup_x + dropdown_w() > (int)screen.width)          /* keep it on the screen */
        popup_x = (int)screen.width - dropdown_w();
    if (popup_y + popup.n_items * MENUITEM_H > (int)screen.height)
        popup_y = (int)screen.height - popup.n_items * MENUITEM_H;
}

/* --- events to the clients -------------------------------------------------- */

/* Queue an event for window id's client (the lock held). Nothing here writes
 * to a client: a client that stops reading would stop the pointer and every
 * other window with it. Its own thread sends the queue (send_queued). A
 * pointer move replaces a queued one with the same buttons (the client gets
 * where the pointer is now, not every step); a full queue drops the event. */
static void enqueue(int id, struct wmsg m)
{
    struct window *w = &windows[id];
    if (m.type == WMSG_POINTER && w->outq_n > 0) {
        struct wmsg *last = &w->outq[w->outq_n - 1];
        if (last->type == WMSG_POINTER && last->c == m.c) {
            *last = m;
            return;
        }
    }
    if (w->outq_n < OUTQ)
        w->outq[w->outq_n++] = m;
}

/* Send what was queued for window id (its client's thread, not locked).
 * (41f-1) First, whether it has the keyboard, when that changed since it
 * was last told (a click, a window opened, closed or minimised: looked at
 * here, every CLIENT_POLL_MS at most, so every way focus moves is seen). */
static void send_queued(int id, int sock)
{
    struct wmsg out[OUTQ];
    char *drop;
    uint32_t drop_len;
    take();
    int has = focused_window() == id;
    if (windows[id].used && has != windows[id].focus_told) {
        struct wmsg f = { WMSG_FOCUS, (uint32_t)has, 0, 0 };
        windows[id].focus_told = has;
        enqueue(id, f);
    }
    int n = windows[id].outq_n;
    memcpy(out, windows[id].outq, (size_t)n * sizeof(out[0]));
    windows[id].outq_n = 0;
    drop = windows[id].drop;                        /* (41f-6) queued with its WMSG_DROP */
    drop_len = windows[id].drop_len;
    windows[id].drop = 0;
    give();
    int k = 0;
    while (drop && k < n && out[k].type != WMSG_DROP)
        k++;
    if (drop && k < n) {                            /* the text right after its message */
        write_full(sock, out, (size_t)(k + 1) * sizeof(out[0]));
        if (drop_len)
            write_full(sock, drop, drop_len);
        write_full(sock, out + k + 1, (size_t)(n - k - 1) * sizeof(out[0]));
    } else if (n > 0) {
        write_full(sock, out, (size_t)n * sizeof(out[0]));
    }
    free(drop);
}

/* read_full for a client's thread: waits CLIENT_POLL_MS at a time and sends
 * the window's queued events in between. */
static int client_read(int id, int sock, void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        send_queued(id, sock);
        ssize_t r = ritteros_recvfrom(sock, (char *)buf + done, n - done, 0, 0, CLIENT_POLL_MS);
        if (r > 0)
            done += (size_t)r;
        else if (r == 0 || errno != ETIMEDOUT)
            return -1;                              /* the client left (or the socket failed) */
    }
    return 0;
}

/* The clipboard (15w): one text for the screen's user (only their programs
 * and root's reach windowd), kept in memory until windowd ends. Clients'
 * threads set and read it, under its own lock. */
static char *clipboard;
static uint32_t clipboard_len;
static volatile int clipboard_lock;

static void clip_take(void)
{
    while (__atomic_exchange_n(&clipboard_lock, 1, __ATOMIC_ACQUIRE))
        usleep(1000);
}

static void clip_give(void)
{
    __atomic_store_n(&clipboard_lock, 0, __ATOMIC_RELEASE);
}

/* Read a text of len bytes that follows a message into the clipboard (what
 * is past WINDOW_CLIPBOARD_MAX read and dropped, so the stream stays in step). */
static int clip_set(int id, int sock, uint32_t len)
{
    uint32_t keep = len > WINDOW_CLIPBOARD_MAX ? WINDOW_CLIPBOARD_MAX : len;
    char *text = malloc(keep + 1);
    if (!text)
        return -1;
    if (keep && client_read(id, sock, text, keep) < 0) {
        free(text);
        return -1;
    }
    char drop[256];
    for (uint32_t left = len - keep; left > 0;) {
        uint32_t n = left > sizeof(drop) ? (uint32_t)sizeof(drop) : left;
        if (client_read(id, sock, drop, n) < 0) {
            free(text);
            return -1;
        }
        left -= n;
    }
    text[keep] = 0;
    clip_take();
    free(clipboard);
    clipboard = text;
    clipboard_len = keep;
    clip_give();
    return 0;
}

/* Send the clipboard to the client (its own thread: after what was queued). */
static void clip_send(int id, int sock)
{
    send_queued(id, sock);
    clip_take();
    struct wmsg m = { WMSG_CLIP_TEXT, 0, 0, clipboard_len };
    write_full(sock, &m, sizeof(m));
    if (clipboard_len)
        write_full(sock, clipboard, clipboard_len);
    clip_give();
}

/* --- threads ----------------------------------------------------------------- */

/* The input thread: pointer and keyboard events for the display owner. A
 * press focuses and raises the window under the pointer, and on a title bar
 * begins a drag; moving while held drags the window; a release ends it.
 * Pointer and key events are forwarded to the focused window. */
/* Physical keys (15p): the keys held now, and the window that got their
 * presses. When another window comes to the front, the one before gets the
 * releases of the keys still held -- a game would otherwise keep a key down
 * for ever -- and the new one gets nothing for them (it never saw them
 * pressed). Lock held. */
static uint8_t keys_held[32];
static int keys_win = -1;

static void keys_follow_focus(int foc)
{
    if (foc == keys_win)
        return;
    if (keys_win >= 0 && windows[keys_win].used)
        for (int k = 0; k < 256; k++)
            if (keys_held[k / 8] & (1u << (k % 8))) {
                struct wmsg up = { WMSG_KEYCODE, (uint32_t)k, 0, 0 };
                enqueue(keys_win, up);
            }
    memset(keys_held, 0, sizeof(keys_held));
    keys_win = foc;
}

/* (41f-4) Typed text from the key bytes (the terminal's): a whole UTF-8
 * character, not control bytes nor a key's escape sequence (ESC [ or ESC
 * O, then up to its final byte; a lone ESC is no text); 0 while nothing is
 * whole. */
static int text_esc;                    /* 1 after ESC, 2 inside a sequence */
static uint32_t text_cp;
static int text_more;                   /* UTF-8 bytes still to come */

static uint32_t text_of(uint8_t b)
{
    if (text_esc == 1) {
        text_esc = b == '[' || b == 'O' ? 2 : 0;
        if (text_esc)
            return 0;
    } else if (text_esc == 2) {
        if (b >= 0x40 && b <= 0x7E)
            text_esc = 0;
        return 0;
    }
    if (b == 0x1B) {
        text_esc = 1;
        text_more = 0;
        return 0;
    }
    if (text_more && (b & 0xC0) == 0x80) {
        text_cp = text_cp << 6 | (b & 0x3F);
        return --text_more ? 0 : text_cp;
    }
    text_more = 0;
    if (b < 0x20 || b == 0x7F)
        return 0;
    if (b < 0x80)
        return b;
    int more = (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : 0;
    if (!more)
        return 0;                       /* (a stray continuation byte) */
    text_cp = b & (0x3F >> more);
    text_more = more;
    return 0;
}

/* Double clicks (15n, the user's: the same rule for every program, decided
 * here): the left button's press in a window's content, within double_click
 * milliseconds (the user's ~/.config/windows, else /system/config/windows;
 * 500 if not set) and 4 pixels of the press before, in the same window. A
 * double click starts the count again (a third press is a first one). */
static uint64_t double_click_ns = 500000000ull;
static uint64_t press_ns;
static int press_win = -1, press_x, press_y;
static unsigned held_buttons;

static bool double_click(int win, const struct sys_input_event *e, const struct wmsg *m)
{
    bool press = (e->buttons & MOUSE_LEFT) && !(held_buttons & MOUSE_LEFT);
    held_buttons = e->buttons;
    if (!press)
        return false;
    if ((int)m->a < 0 || (int)m->b < 0 || m->a >= windows[win].w || m->b >= windows[win].h) {
        press_win = -1;                             /* (a press on a title bar, a border) */
        return false;
    }
    uint64_t now = ritteros_time();
    int dx = e->x - press_x, dy = e->y - press_y;
    if (press_win == win && now - press_ns < double_click_ns && dx >= -4 && dx <= 4 &&
        dy >= -4 && dy <= 4) {
        press_win = -1;
        return true;
    }
    press_win = win, press_ns = now, press_x = e->x, press_y = e->y;
    return false;
}

/* Steady clicks (the user's, from a touchpad on the laptop: `click_steady`
 * in the window settings, on unless "off"). A finger pressing a touchpad's
 * click rolls: the pointer crept 1 pixel at a time, 10-100 ms apart, up to
 * 8 px in the 0.3 s before the button closed, up to 13 px while it was
 * held and up to 15 px back while the finger lifted; a deliberate move
 * comes several pixels an event. So around a click, slow movement is held:
 * - a press is taken where a creep that ran into it began (a run of slow
 *   steps, each within RUN_GAP_MS of the next and the last just before the
 *   press, at most HOLD_PX in all); a nudge followed by a pause stays;
 * - while the button is down, slow steps are held until they add up to
 *   HOLD_PX (a drag) or a fast one comes, then given back;
 * - for AFTER_MS after the release, slow steps are dropped.
 * The pointer shown is the kernel's minus `steady_off` (what was held). */
#define SLOW_PX     2           /* a step of at most this many pixels (x + y) is slow */
#define HOLD_PX     12          /* slow movement held at most this far */
#define RUN_GAP_MS  120         /* steps of a creep come at most this far apart */
#define RUN_END_MS  60          /* and the last at most this long before the press */
#define AFTER_MS    300
#define TRAIL       32

static bool click_steady = true;
static int steady_off_x, steady_off_y;       /* the kernel's pointer minus the one shown */
static struct { uint64_t ns; int x, y, slow; } trail[TRAIL];   /* recent shown positions */
static int trail_n;
static int raw_x = -1, raw_y = -1;
static uint8_t raw_buttons;
static int held_x, held_y;                   /* slow movement held since the press */
static bool holding;
static uint64_t release_ns;

static void steady(struct sys_input_event *e)
{
    if (!click_steady)
        return;
    uint64_t now = ritteros_time();
    int dx = raw_x < 0 ? 0 : e->x - raw_x, dy = raw_y < 0 ? 0 : e->y - raw_y;
    bool slow = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) <= SLOW_PX;
    bool press = (e->buttons & MOUSE_LEFT) && !(raw_buttons & MOUSE_LEFT);
    bool release = !(e->buttons & MOUSE_LEFT) && (raw_buttons & MOUSE_LEFT);
    raw_x = e->x, raw_y = e->y, raw_buttons = e->buttons;
    if (press) {
        /* The creep that ran into the press: back to where it began. */
        int start = trail_n, path = 0;
        uint64_t later = now;
        while (start > 0 && trail[start - 1].slow &&
               later - trail[start - 1].ns <= (start == trail_n ? RUN_END_MS : RUN_GAP_MS) *
                                              1000000ull) {
            start--;
            later = trail[start].ns;
            path++;
        }
        if (path >= 2 && start > 0) {
            int bx = trail[start - 1].x, by = trail[start - 1].y;  /* before the creep */
            int sx = e->x - steady_off_x, sy = e->y - steady_off_y;
            int mx = sx - bx, my = sy - by;
            if ((mx < 0 ? -mx : mx) + (my < 0 ? -my : my) <= HOLD_PX) {
                steady_off_x += mx;
                steady_off_y += my;
            }
        }
        holding = true;
        held_x = held_y = 0;
    } else if (holding && (e->buttons & MOUSE_LEFT) && (dx || dy)) {
        if (slow && (held_x < 0 ? -held_x : held_x) + (held_y < 0 ? -held_y : held_y) < HOLD_PX) {
            held_x += dx, held_y += dy;          /* the finger flattening */
            steady_off_x += dx, steady_off_y += dy;
        } else {
            steady_off_x -= held_x, steady_off_y -= held_y;   /* a drag: give it back */
            holding = false;
        }
    } else if (release) {
        holding = false;
        release_ns = now;
    } else if (!(e->buttons & MOUSE_LEFT) && (dx || dy) && now - release_ns < AFTER_MS * 1000000ull) {
        if (slow)
            steady_off_x += dx, steady_off_y += dy;     /* the finger lifting */
        else
            release_ns = 0;
    }
    /* At a screen edge the kernel's pointer stops: what was held goes. */
    if (e->x == 0 || e->x >= (int)screen.width - 1)
        steady_off_x = 0;
    if (e->y == 0 || e->y >= (int)screen.height - 1)
        steady_off_y = 0;
    int sx = e->x - steady_off_x, sy = e->y - steady_off_y;
    sx = sx < 0 ? 0 : sx >= (int)screen.width ? (int)screen.width - 1 : sx;
    sy = sy < 0 ? 0 : sy >= (int)screen.height ? (int)screen.height - 1 : sy;
    e->x = (uint16_t)sx, e->y = (uint16_t)sy;
    if (!(e->buttons & MOUSE_LEFT) && (dx || dy)) {   /* the trail of free movement */
        if (trail_n == TRAIL) {
            memmove(trail, trail + 1, sizeof(trail[0]) * (TRAIL - 1));
            trail_n--;
        }
        trail[trail_n].ns = now, trail[trail_n].x = sx, trail[trail_n].y = sy;
        trail[trail_n].slow = slow;
        trail_n++;
    }
}

static void keep_on_screen(int id);

/* (41f-2) A content size the user asked for (a resize, a maximize) kept
 * within the window's limits (window_set_limits) and windowd's least. */
static void within_limits(int i, uint32_t *w, uint32_t *h)
{
    if (windows[i].max_w && *w > windows[i].max_w)
        *w = windows[i].max_w;
    if (windows[i].max_h && *h > windows[i].max_h)
        *h = windows[i].max_h;
    if (*w < windows[i].min_w)
        *w = windows[i].min_w;
    if (*h < windows[i].min_h)
        *h = windows[i].min_h;
    if (*w < MIN_CONTENT_W)
        *w = MIN_CONTENT_W;
    if (*h < MIN_CONTENT_H)
        *h = MIN_CONTENT_H;
}

/* (66b-5) The screen has a new mode (INPUT_SCREEN): a back buffer of the
 * new size, maximized windows asked to fill it, the others kept on it, the
 * pointer inside it, the rate the screen's own (unless `refresh` set one),
 * and everything drawn again. */
static void screen_changed(void)
{
    struct sys_display s;
    if (ritteros_display_info(&s) < 0)
        return;
    int fd = ritteros_share_create((size_t)s.width * s.height * 4);
    uint32_t *nb = fd < 0 ? 0 : ritteros_share_map(fd);
    if (!nb) {
        printf("windowd: no back buffer for %ux%u\n", s.width, s.height);
        if (fd >= 0)
            close(fd);
        return;
    }
    static struct sys_display_modes modes;
    uint64_t rate = 0;
    if (!refresh_fixed && ritteros_display_modes(&modes) == 0 && modes.now.refresh_mhz)
        rate = 1000000000ull / modes.now.refresh_mhz;
    struct sys_display_buffers sb;                  /* (66d-3) their new size */
    int flip_now = use_flip && ritteros_display_buffers(&sb) == 0;
    take();
    if (use_flip) {
        use_flip = flip_now;
        if (flip_now)
            screen_buffers = sb;
        flip_hidden = 1;                            /* (a mode change shows buffer 0) */
        flip_last_any = 0;                          /* both stale: composite_all below */
    }
    uint32_t *old = back;
    size_t old_size = (size_t)screen.width * screen.height * 4;
    int old_fd = back_fd;
    screen = s;
    back = nb;
    back_fd = fd;
    if (rate)
        frame_us = rate;
    if (cursor_x >= (int)screen.width)
        cursor_x = (int)screen.width - 1;
    if (cursor_y >= (int)screen.height)
        cursor_y = (int)screen.height - 1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].used)
            continue;
        if (windows[i].fullscreen) {                /* (41f-6) the whole new screen */
            struct wmsg rz = { WMSG_RESIZE, screen.width, screen.height, 0 };
            windows[i].pending_resize = 1;
            windows[i].pending_ox = 0;
            windows[i].pending_oy = 0;
            enqueue(i, rz);
        } else if (windows[i].maximized) {          /* fill the new screen */
            int cw = (int)screen.width - 2 * BORDER;
            int ch = (int)screen.height - TITLEBAR - menubar_h(i) - BORDER;
            struct wmsg rz = { WMSG_RESIZE, (uint32_t)(cw < MIN_CONTENT_W ? MIN_CONTENT_W : cw),
                               (uint32_t)(ch < MIN_CONTENT_H ? MIN_CONTENT_H : ch), 0 };
            windows[i].pending_resize = 1;
            windows[i].pending_ox = 0;
            windows[i].pending_oy = 0;
            enqueue(i, rz);
        } else {
            keep_on_screen(i);
        }
    }
    composite_all();
    give();
    if (old)
        munmap(old, old_size);
    if (old_fd >= 0)
        close(old_fd);
    printf("windowd: screen now %ux%u\n", screen.width, screen.height);
}

/* One event of the keyboard or the pointer (from the display, or 41h-3's
 * WMSG_INPUT into a headless windowd): focus, moves, menus, and what goes
 * to which window. */
static void input_event(struct sys_input_event e)
{
    take();
    struct wmsg m = {0};
    int to = -1, close_win = -1, resize_win = -1, menu_to = -1, menu_id = 0;
    int desk_action = 0;
    uint32_t resize_w = 0, resize_h = 0;
    if (e.type == INPUT_POINTER) {
        steady(&e);                                 /* (15q) a touchpad's creep held */
        /* Ctrl + click is a right-click (a touchpad or a one-button
         * mouse has no right button here): the left button, pressed with
         * Ctrl held, counts as the right one until it is let go. */
        if ((e.buttons & MOUSE_LEFT) && !(prev_buttons & (MOUSE_LEFT | MOUSE_RIGHT)) &&
            (e.modifiers & INPUT_CTRL))
            ctrl_click = 1;
        if (ctrl_click) {
            if (e.buttons & MOUSE_LEFT)
                e.buttons = (uint8_t)((e.buttons & ~MOUSE_LEFT) | MOUSE_RIGHT);
            else
                ctrl_click = 0;
        }
        int down = (e.buttons & MOUSE_LEFT) && !(prev_buttons & MOUSE_LEFT);
        int up = !(e.buttons & MOUSE_LEFT) && (prev_buttons & MOUSE_LEFT);
        int rdown = (e.buttons & MOUSE_RIGHT) && !(prev_buttons & MOUSE_RIGHT);
        prev_buttons = e.buttons;
        struct rect dirty = rect_union(rect_cursor(cursor_x, cursor_y),
                                       rect_cursor(e.x, e.y));   /* the pointer moved */
        cursor_x = e.x;
        cursor_y = e.y;
        hover_follow(e.x, e.y);
        if (up && drag_text)                        /* (41f-6) a drag let go: drop it */
            drop_at(e.x, e.y);
        if (rdown) {                                     /* a right-click */
            int tb = taskbar_hit(e.x, e.y);
            int hit = tb >= 0 ? -1 : window_at(e.x, e.y);
            if (menu_win >= 0) {                         /* a menu is open: close it */
                menu_win = menu_idx = -1;
                composite_all();
            } else if (hit < 0 && tb < 0) {              /* the desktop background: its menu */
                open_desktop_menu(e.x, e.y);
                composite_all();
            } else {                                     /* over a window: the client handles it */
                composite_rect(dirty);
            }
        } else if (down && menu_win >= 0) {              /* a menu is open */
            int di = dropdown_hit(e.x, e.y);
            if (di >= 0) {                              /* chose an item */
                struct wmenu_item *items;
                int mx, my;
                open_menu_items(&items, &mx, &my);
                if (menu_win == MENU_DESKTOP)           /* the desktop menu: windowd acts */
                    desk_action = items[di].id;
                else {                                  /* a window's menu: tell its client */
                    menu_to = menu_win;
                    menu_id = items[di].id;
                }
                menu_win = menu_idx = -1;
            } else {                                    /* off the dropdown */
                int wi = window_at(e.x, e.y);
                int mi = wi >= 0 ? menubar_title_at(wi, e.x, e.y) : -1;
                if (mi >= 0) {                          /* another bar menu: open it */
                    menu_win = wi;
                    menu_idx = mi;
                } else {
                    menu_win = menu_idx = -1;           /* elsewhere: close the menu */
                }
            }
            composite_all();
        } else if (down) {
            int tb = taskbar_hit(e.x, e.y);
            int hit = tb >= 0 ? -1 : window_at(e.x, e.y);
            int mbar = hit >= 0 ? menubar_title_at(hit, e.x, e.y) : -1;
            int dlg = hit >= 0 ? dialog_of(hit) : -1;
            if (dlg >= 0) {                             /* (41f-5) a dialog open: it, not the window */
                windows[dlg].z = ++ztop;
                composite_all();
            } else if (tb >= 0) {                       /* a taskbar button: restore it */
                windows[tb].minimized = 0;
                windows[tb].z = ++ztop;
                composite_all();
            } else if (hit >= 0 && on_closebox(hit, e.x, e.y)) {
                close_win = hit;                        /* ask the client to close */
                composite_rect(dirty);
            } else if (hit >= 0 && on_minbox(hit, e.x, e.y)) {
                windows[hit].minimized = 1;             /* minimize: off to a taskbar button */
                if (dragging == hit)
                    dragging = -1;
                composite_all();
            } else if (hit >= 0 && on_maxbox(hit, e.x, e.y)) {
                int i = hit;                            /* maximize, or restore if already */
                resize_win = i;
                if (windows[i].maximized) {             /* back to the saved size and place */
                    resize_w = windows[i].saved_w;
                    resize_h = windows[i].saved_h;
                    windows[i].pending_ox = windows[i].saved_ox;
                    windows[i].pending_oy = windows[i].saved_oy;
                    windows[i].maximized = 0;
                } else {                                /* fill the screen; remember where it was */
                    windows[i].saved_ox = windows[i].ox;
                    windows[i].saved_oy = windows[i].oy;
                    windows[i].saved_w = windows[i].w;
                    windows[i].saved_h = windows[i].h;
                    int cw = (int)screen.width - 2 * BORDER;
                    int ch = (int)screen.height - TITLEBAR - menubar_h(i) - BORDER;
                    resize_w = (uint32_t)(cw < MIN_CONTENT_W ? MIN_CONTENT_W : cw);
                    resize_h = (uint32_t)(ch < MIN_CONTENT_H ? MIN_CONTENT_H : ch);
                    within_limits(i, &resize_w, &resize_h);
                    windows[i].pending_ox = 0;
                    windows[i].pending_oy = 0;
                    windows[i].maximized = 1;
                }
                windows[i].z = ++ztop;                  /* raise it to the front */
                windows[i].pending_resize = 1;          /* applied when its buffer arrives */
                composite_rect(dirty);
            } else if (mbar >= 0) {                      /* a menu-bar title: open its dropdown */
                windows[hit].z = ++ztop;                /* focus the window too */
                menu_win = hit;
                menu_idx = mbar;
                composite_all();
            } else if (hit >= 0) {
                int old_foc = focused_window();         /* its title bar will dim */
                if (old_foc >= 0 && old_foc != hit)
                    dirty = rect_union(dirty, rect_window(old_foc));
                windows[hit].z = ++ztop;                /* focus and raise */
                dirty = rect_union(dirty, rect_window(hit));
                int edges = resize_edges_at(hit, e.x, e.y);
                if (edges) {                            /* grab an edge/corner: resize */
                    resizing = hit;
                    resize_edges = edges;
                    windows[hit].maximized = 0;         /* a hand resize is no longer "maximized" */
                    struct rect fr = rect_window(hit);
                    rs_gx = fr.x; rs_gy = fr.y; rs_gw = fr.w; rs_gh = fr.h;
                    resize_outline = fr;
                } else if (on_titlebar(hit, e.x, e.y)) {
                    dragging = hit;
                    windows[hit].maximized = 0;         /* dragging it away un-maximizes it */
                    drag_dx = e.x - windows[hit].ox;
                    drag_dy = e.y - windows[hit].oy;
                }
                composite_rect(dirty);
            } else {
                composite_rect(dirty);
            }
        } else if (menu_win >= 0) {                      /* a menu is open: track the highlight */
            composite_all();
        } else if (up) {
            if (resizing >= 0) {                        /* finish the resize */
                int i = resizing;
                resize_win = i;
                int cw = resize_outline.w - 2 * BORDER;
                int ch = resize_outline.h - TITLEBAR - menubar_h(i) - BORDER;
                resize_w = (uint32_t)(cw < MIN_CONTENT_W ? MIN_CONTENT_W : cw);
                resize_h = (uint32_t)(ch < MIN_CONTENT_H ? MIN_CONTENT_H : ch);
                within_limits(i, &resize_w, &resize_h);
                windows[i].pending_resize = 1;          /* applied when its buffer arrives */
                windows[i].pending_ox = resize_outline.x;
                windows[i].pending_oy = resize_outline.y;
                struct rect ob = resize_outline;
                resizing = -1;
                resize_edges = 0;
                composite_rect(rect_union(ob, dirty));  /* erase the outline */
            } else {
                dragging = -1;
                composite_rect(dirty);
            }
        } else if (resizing >= 0 && (e.buttons & MOUSE_LEFT)) {
            struct rect old_ol = resize_outline;        /* drag the outline */
            int L = rs_gx, T = rs_gy, R = rs_gx + rs_gw, B = rs_gy + rs_gh;
            if (resize_edges & RZ_LEFT)   L = e.x;
            if (resize_edges & RZ_RIGHT)  R = e.x;
            if (resize_edges & RZ_TOP)    T = e.y;
            if (resize_edges & RZ_BOTTOM) B = e.y;
            int minw = MIN_CONTENT_W + 2 * BORDER;
            int minh = MIN_CONTENT_H + TITLEBAR + menubar_h(resizing) + BORDER;
            if (R - L < minw) { if (resize_edges & RZ_LEFT) L = R - minw; else R = L + minw; }
            if (B - T < minh) { if (resize_edges & RZ_TOP)  T = B - minh; else B = T + minh; }
            if (L < 0) L = 0;
            if (T < 0) T = 0;
            struct rect nr = { L, T, R - L, B - T };
            resize_outline = nr;
            composite_rect(rect_union(rect_union(old_ol, nr), dirty));
        } else if (dragging >= 0 && (e.buttons & MOUSE_LEFT)) {
            dirty = rect_union(dirty, rect_window(dragging));   /* where it was */
            int nx = e.x - drag_dx, ny = e.y - drag_dy;     /* move the window */
            if (nx < 0)
                nx = 0;
            if (ny < 0)
                ny = 0;
            if (nx > (int)screen.width - 1)
                nx = (int)screen.width - 1;
            if (ny > (int)screen.height - 1)
                ny = (int)screen.height - 1;
            windows[dragging].ox = nx;
            windows[dragging].oy = ny;
            dirty = rect_union(dirty, rect_window(dragging));   /* where it is now */
            composite_rect(dirty);
        } else {
            composite_rect(dirty);                      /* a plain move */
        }
        int foc = focused_window();
        if (foc >= 0 && menu_win < 0) {             /* no event to the app while a menu is open */
            to = foc;
            m.type = WMSG_POINTER;
            m.a = (uint32_t)(e.x - content_left(foc));
            m.b = (uint32_t)(e.y - content_top(foc));
            m.c = e.buttons;
        }
    } else if (e.type == INPUT_KEYCODE) {             /* (15p) a physical key, to the front */
        int foc = focused_window();
        keys_follow_focus(foc);
        uint8_t bit = (uint8_t)(1u << (e.key & 7));
        if (e.buttons)
            keys_held[e.key / 8] |= bit;
        else
            keys_held[e.key / 8] &= (uint8_t)~bit;
        if (foc >= 0) {
            to = foc;
            m.type = WMSG_KEYCODE;
            m.a = e.key;
            m.b = e.buttons ? 1 : 0;
        }
    } else if (e.type == INPUT_WHEEL) {               /* (15o) to the window under the pointer */
        int under = window_at(e.x, e.y);
        if (under >= 0 && menu_win < 0 && !windows[under].minimized) {
            to = under;
            m.type = WMSG_SCROLL;
            m.a = (uint32_t)(int32_t)(int8_t)e.key;  /* steps, negative up */
            m.b = (uint32_t)(e.x - content_left(under));
            m.c = (uint32_t)(e.y - content_top(under));
        }
    } else if (e.type == INPUT_KEY) {
        int foc = focused_window();
        if (foc >= 0) {
            struct wmsg key = { WMSG_KEY, e.key, 0, 0 };
            enqueue(foc, key);
            uint32_t c = text_of((uint8_t)e.key);   /* (41f-4) and as text, once whole */
            if (c) {
                struct wmsg text = { WMSG_TEXT, c, 0, 0 };
                enqueue(foc, text);
            }
        }
    }
    keys_follow_focus(focused_window());          /* (a click may have changed it, 15p) */
    if (resize_win >= 0) {                           /* the clients' threads send these */
        struct wmsg rz = { WMSG_RESIZE, resize_w, resize_h, 0 };
        enqueue(resize_win, rz);
    }
    if (close_win >= 0) {
        struct wmsg c = { WMSG_CLOSE, 0, 0, 0 };
        enqueue(close_win, c);
    }
    if (menu_to >= 0) {
        struct wmsg mm = { WMSG_MENU, (uint32_t)menu_id, 0, 0 };  /* the chosen item */
        enqueue(menu_to, mm);
    }
    if (to >= 0)
        enqueue(to, m);
    if (to >= 0 && m.type == WMSG_POINTER && double_click(to, &e, &m)) {
        struct wmsg dc = { WMSG_DOUBLE_CLICK, m.a, m.b, 0 };
        enqueue(to, dc);                         /* after the press's own pointer event */
    }
    give();
    if (desk_action) {                               /* a desktop-menu choice (outside the lock) */
        if (desk_action >= DESK_APP && desk_action < DESK_APP + WINDOW_MENU_ITEMS)
            spawn_program(desk_apps[desk_action - DESK_APP]);   /* (41g) an app */
        else if (desk_action == DESK_QUIT) {         /* return to the text console */
            take();                                  /* (65e-34b: no frame in flight; the
                                              * compositor's flush would draw over the
                                              * console, a row at a time) */
            if (!headless)
                ritteros_display_release();
            printf("windowd: window server closed\n");
            exit(0);
        }
    }
}

static int input_thread(void *arg)
{
    (void)arg;
    for (;;) {
        struct sys_input_event e;
        int r = ritteros_display_event(&e, 0);
        if (r < 0)
            return 0;                               /* interrupted: windowd is ending */
        if (r == 0)
            continue;
        if (e.type == INPUT_SCREEN) {                   /* (66b-5) a new mode */
            screen_changed();
            continue;
        }
        if (e.type == INPUT_BLANK) {                    /* (37e) the screen off, or on */
            take();
            blank_state = e.key ? 1 : 0;
            composite_all();                            /* (black, or everything again) */
            give();
            continue;
        }
        input_event(e);
    }
}

/* Read one menu item (uint16 id, uint8 label_len, label) from a blob at *p. */
static int read_item(const uint8_t *b, int len, int *p, struct wmenu_item *it)
{
    if (*p + 3 > len)
        return 0;
    it->id = b[*p] | (b[*p + 1] << 8);
    *p += 2;
    int ll = b[(*p)++];
    if (ll >= WINDOW_LABEL_MAX)
        ll = WINDOW_LABEL_MAX - 1;
    if (*p + ll > len)
        return 0;
    memcpy(it->label, b + *p, ll);
    it->label[ll] = 0;
    *p += ll;
    return 1;
}

/* Parse a menu-bar blob into window id's menus (see windowproto.h). */
static void parse_menubar(int id, const uint8_t *b, int len)
{
    int p = 0, nm = len > 0 ? b[p++] : 0;
    if (nm > WINDOW_MENU_MAX)
        nm = WINDOW_MENU_MAX;
    windows[id].n_menus = 0;
    for (int mi = 0; mi < nm && p < len; mi++) {
        struct wmenu *mn = &windows[id].menus[windows[id].n_menus];
        int tl = b[p++];
        if (tl >= WINDOW_LABEL_MAX)
            tl = WINDOW_LABEL_MAX - 1;
        if (p + tl > len)
            break;
        memcpy(mn->title, b + p, tl);
        mn->title[tl] = 0;
        p += tl;
        if (p >= len)
            break;
        int ni = b[p++];
        if (ni > WINDOW_MENU_ITEMS)
            ni = WINDOW_MENU_ITEMS;
        mn->n_items = 0;
        for (int ii = 0; ii < ni; ii++) {
            if (!read_item(b, len, &p, &mn->items[mn->n_items]))
                break;
            mn->n_items++;
        }
        windows[id].n_menus++;
    }
}

/* Keep window id on the screen, above the taskbar (the user's: a window
 * opening for a large picture or video went past the screen's edges). It is
 * moved left and up as far as needed; if it is still too big, its client is
 * asked for the largest content that fits (the same WMSG_RESIZE a resize by
 * the user sends), and it goes to the corner when the new buffer comes.
 * A maximized window is the user's choice and stays as it is. Lock held. */
static void keep_on_screen(int id)
{
    struct window *w = &windows[id];
    if (w->maximized || w->fullscreen || w->pending_resize)
        return;
    int aw = (int)screen.width, ah = (int)screen.height - TASKBAR_H;
    int ow = (int)w->w + 2 * BORDER, oh = (int)w->h + TITLEBAR + menubar_h(id) + BORDER;
    if (w->ox + ow > aw)
        w->ox = aw - ow;
    if (w->oy + oh > ah)
        w->oy = ah - oh;
    if (w->ox < 0)
        w->ox = 0;
    if (w->oy < 0)
        w->oy = 0;
    if (ow <= aw && oh <= ah)
        return;
    int cw = ow > aw ? aw - 2 * BORDER : (int)w->w;
    int ch = oh > ah ? ah - TITLEBAR - menubar_h(id) - BORDER : (int)w->h;
    struct wmsg rz = { WMSG_RESIZE, (uint32_t)(cw < MIN_CONTENT_W ? MIN_CONTENT_W : cw),
                       (uint32_t)(ch < MIN_CONTENT_H ? MIN_CONTENT_H : ch), 0 };
    w->pending_resize = 1;
    w->pending_ox = 0;
    w->pending_oy = 0;
    enqueue(id, rz);
}

/* (41f-6) Window id to the whole screen, without its frame, in front (on),
 * or back to its place and size before (off). Its limits do not apply.
 * Like a resize by the user, the size comes when its new buffer does.
 * Lock held. */
static void set_fullscreen(int id, int on)
{
    struct window *w = &windows[id];
    uint32_t cw, ch;
    if (on == w->fullscreen)
        return;
    if (on) {
        w->fs_ox = w->ox;
        w->fs_oy = w->oy;
        w->fs_w = w->w;
        w->fs_h = w->h;
        w->fullscreen = 1;
        w->minimized = 0;
        w->z = ++ztop;
        w->ox = w->oy = 0;
        cw = screen.width;
        ch = screen.height;
        w->pending_ox = w->pending_oy = 0;
    } else {
        w->fullscreen = 0;
        cw = w->fs_w;
        ch = w->fs_h;
        w->ox = w->pending_ox = w->fs_ox;
        w->oy = w->pending_oy = w->fs_oy;
    }
    if (dragging == id)
        dragging = -1;
    if (resizing == id)
        resizing = -1, resize_edges = 0;
    w->pending_resize = 1;
    struct wmsg rz = { WMSG_RESIZE, cw, ch, 0 };
    enqueue(id, rz);
    hover_follow(cursor_x, cursor_y);
    composite_all();
}

/* (41f-6) A drag's text (len bytes after the message) read from window
 * id's client; it is the drag only while the left button is still down on
 * the window in front (the one pressed: a drag starts from a press), else
 * dropped. -1 when the client left. */
static int drag_take(int id, int sock, uint32_t len)
{
    uint32_t keep = len > WINDOW_CLIPBOARD_MAX ? WINDOW_CLIPBOARD_MAX : len;
    char *text = malloc(keep + 1);
    if (!text)
        return -1;
    if (keep && client_read(id, sock, text, keep) < 0) {
        free(text);
        return -1;
    }
    char past[256];
    for (uint32_t left = len - keep; left > 0;) {
        uint32_t n = left > sizeof(past) ? (uint32_t)sizeof(past) : left;
        if (client_read(id, sock, past, n) < 0) {
            free(text);
            return -1;
        }
        left -= n;
    }
    text[keep] = 0;
    take();
    if ((prev_buttons & MOUSE_LEFT) && !ctrl_click && focused_window() == id) {
        free(drag_text);
        drag_text = text;
        drag_len = keep;
        text = 0;
        composite_rect(rect_cursor(cursor_x, cursor_y));    /* the page on the pointer */
    }
    give();
    free(text);
    return 0;
}

/* One client: its window, then its messages until it closes. */
static void client_work(int sock)
{
    struct wmsg m;
    if (read_full(sock, &m, sizeof(m)) < 0) {
        close(sock);
        return;
    }
    if (m.type == WMSG_NOTIFY)                       /* (41g-4) `notify`: no window */
        note_take(-1, sock, &m);
    if (m.type == WMSG_SHOT)                         /* (41h-2) `wshot`: no window */
        shot_send(sock);
    if (m.type == WMSG_WINDOWS)                      /* (41h-6) `wsend wait TITLE` */
        titles_send(sock);
    if (m.type == WMSG_INPUT) {                      /* (41h-3) `wsend`: no window */
        input_take(sock, m);
        close(sock);
        return;
    }
    if (m.type != WMSG_CREATE) {
        close(sock);
        return;
    }
    uint32_t w = m.a, h = m.b, tl = m.c & 0xFFFF;
    int parent = (int)((m.c & ~WMSG_CREATE_APP) >> 16) - 1;   /* (41f-5) a dialog's window */
    char title[WINDOW_TITLE_MAX] = {0};
    if (tl >= WINDOW_TITLE_MAX || (tl && read_full(sock, title, tl) < 0)) {
        close(sock);                                 /* bad title length, or short read */
        return;
    }
    char app[64] = {0};                              /* (41g-3) the program's name: its icon */
    uint8_t al = 0;
    if ((m.c & WMSG_CREATE_APP) && (read_full(sock, &al, 1) < 0 ||
                                    al >= sizeof(app) || (al && read_full(sock, app, al) < 0))) {
        close(sock);
        return;
    }
    int bufd = recv_shared(sock);                    /* the buffer was sent just before */
    uint32_t *pix = bufd < 0 ? 0 : ritteros_share_map(bufd);
    /* Larger than the screen is taken (a video's size), then made to fit by
     * keep_on_screen; WINDOW_SIZE_MAX only bounds what a client may ask. */
    if (!pix || w == 0 || h == 0 || w > WINDOW_SIZE_MAX || h > WINDOW_SIZE_MAX) {
        if (bufd >= 0)
            close(bufd);
        close(sock);
        return;
    }
    take();
    int id = -1;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (!windows[i].used) {
            id = i;
            break;
        }
    if (id >= 0) {
        int old_foc = focused_window();              /* it loses focus: its title bar dims */
        windows[id].used = 1;
        windows[id].sock = sock;
        windows[id].bufd = bufd;
        windows[id].pix = pix;
        windows[id].w = w;
        windows[id].h = h;
        windows[id].ox = 100 + id * 40;              /* tiled; the user can drag them */
        windows[id].oy = 100 + id * 40;
        windows[id].z = ++ztop;                      /* a new window opens in front */
        windows[id].minimized = 0;                   /* the slot may have held a minimized one */
        windows[id].n_menus = 0;                      /* no menu bar until the client sets one */
        windows[id].outq_n = 0;
        windows[id].focus_told = -1;                 /* (its thread tells it at once) */
        windows[id].min_w = windows[id].min_h = windows[id].max_w = windows[id].max_h = 0;
        windows[id].cursor = 0;
        windows[id].fullscreen = 0;
        memcpy(windows[id].app, app, sizeof(app));
        windows[id].parent = parent >= 0 && parent < MAX_WINDOWS && windows[parent].used && parent != id ? parent : -1;
        if (windows[id].parent >= 0) {               /* (41f-5) centred over its window */
            const struct window *p = &windows[windows[id].parent];
            windows[id].ox = p->ox + ((int)p->w - (int)w) / 2;
            windows[id].oy = p->oy + ((int)p->h + TITLEBAR - (int)h - TITLEBAR) / 2;
        }
        memcpy(windows[id].title, title, sizeof(title));
        windows[id].maximized = 0;
        windows[id].pending_resize = 0;
        keep_on_screen(id);
        struct rect dirty = rect_window(id);
        if (old_foc >= 0)
            dirty = rect_union(dirty, rect_window(old_foc));
        composite_rect(dirty);
    }
    give();
    if (id < 0) {
        close(bufd);
        close(sock);
        return;
    }
    struct wmsg reply = { WMSG_CREATED, (uint32_t)id, 0, 0 };
    write_full(sock, &reply, sizeof(reply));

    while (client_read(id, sock, &m, sizeof(m)) == 0) {
        if (m.type == WMSG_DAMAGE) {                  /* the client redrew: recomposite */
            take();
            if (!windows[id].minimized)              /* hidden: nothing to show */
                composite_rect(rect_window(id));     /* just this window's area */
            give();
        } else if (m.type == WMSG_BUFFER) {          /* a new buffer after a resize */
            int nbufd = recv_shared(sock);
            uint32_t *npix = nbufd < 0 ? 0 : ritteros_share_map(nbufd);
            if (npix && m.a > 0 && m.b > 0 && m.a <= WINDOW_SIZE_MAX && m.b <= WINDOW_SIZE_MAX) {
                take();
                int oldbufd = windows[id].bufd;
                windows[id].pix = npix;
                windows[id].bufd = nbufd;
                windows[id].w = m.a;
                windows[id].h = m.b;
                if (windows[id].pending_resize) {    /* move to where the resize left it */
                    windows[id].ox = windows[id].pending_ox;
                    windows[id].oy = windows[id].pending_oy;
                    windows[id].pending_resize = 0;
                }
                keep_on_screen(id);                  /* (a size the client chose itself) */
                composite_all();                     /* size/position changed: repaint all */
                give();
                close(oldbufd);                      /* unmaps and unrefs the old buffer */
            } else if (nbufd >= 0) {
                close(nbufd);
            }
        } else if (m.type == WMSG_CLIP_SET) {        /* (15w) the clipboard's new text */
            if (clip_set(id, sock, m.c) < 0)
                break;
        } else if (m.type == WMSG_CLIP_GET) {
            clip_send(id, sock);
        } else if (m.type == WMSG_CURSOR) {          /* (41f-4) the pointer's shape over its content */
            take();
            windows[id].cursor = (int)m.a;
            composite_rect(rect_cursor(cursor_x, cursor_y));
            give();
        } else if (m.type == WMSG_FULLSCREEN) {      /* (41f-6) the whole screen, or back */
            take();
            set_fullscreen(id, m.a != 0);
            give();
        } else if (m.type == WMSG_MINIMIZE) {        /* (41f-6) off to a taskbar button */
            take();
            if (!windows[id].minimized) {
                windows[id].minimized = 1;
                if (dragging == id)
                    dragging = -1;
                if (resizing == id)
                    resizing = -1, resize_edges = 0;
                hover_follow(cursor_x, cursor_y);
                composite_all();
            }
            give();
        } else if (m.type == WMSG_DRAG) {            /* (41f-6) a drag starts from this window */
            if (drag_take(id, sock, m.c) < 0)
                break;
        } else if (m.type == WMSG_NOTIFY) {          /* (41g-4) a notification */
            if (note_take(id, sock, &m) < 0)
                break;
        } else if (m.type == WMSG_LIMITS) {          /* (41f-2) sizes a resize keeps to */
            take();
            windows[id].min_w = m.a & 0xFFFF;
            windows[id].min_h = m.a >> 16;
            windows[id].max_w = m.b & 0xFFFF;
            windows[id].max_h = m.b >> 16;
            give();
        } else if (m.type == WMSG_SET_MENU) {        /* set/clear the window's menu bar */
            uint8_t blob[1024];
            uint32_t len = m.c > sizeof(blob) ? sizeof(blob) : m.c;
            if (len > 0 && client_read(id, sock, blob, len) < 0)
                break;
            take();
            parse_menubar(id, blob, (int)len);
            keep_on_screen(id);                      /* (a menu bar makes it taller) */
            composite_all();                         /* the menu bar changed the geometry */
            give();
        } else if (m.type == WMSG_POPUP) {           /* a context menu at a point in the window */
            uint8_t blob[1024];
            uint32_t len = m.c > sizeof(blob) ? sizeof(blob) : m.c;
            if (len > 0 && client_read(id, sock, blob, len) < 0)
                break;
            take();
            int p = 0;
            popup.n_items = len > 0 ? blob[p++] : 0;
            if (popup.n_items > WINDOW_MENU_ITEMS)
                popup.n_items = WINDOW_MENU_ITEMS;
            int got = 0;
            for (int i = 0; i < popup.n_items; i++)
                if (read_item(blob, (int)len, &p, &popup.items[got]))
                    got++;
            popup.n_items = got;
            if (got > 0) {                           /* open it at the client's point */
                menu_win = id;
                menu_idx = -1;
                popup_x = content_left(id) + (int)m.a;
                popup_y = content_top(id) + (int)m.b;
            }
            composite_all();
            give();
        }
    }
    take();                                          /* the client left: drop its window */
    int was_min = windows[id].minimized;
    free(windows[id].drop);                          /* (41f-6) a drop it never got */
    windows[id].drop = 0;
    struct rect gone = rect_window(id);              /* its area shows the desktop again */
    windows[id].used = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)            /* (41f-5) its dialogs belong to nobody now */
        if (windows[i].parent == id)
            windows[i].parent = -1;
    if (hover_win == id)
        hover_win = -1;                             /* (41f-3: its slot may be another's next) */
    bufd = windows[id].bufd;                         /* its current buffer (a resize closed the first) */
    if (menu_win == id)                              /* its menu was open: close it */
        menu_win = menu_idx = -1;
    if (was_min) {                                   /* its taskbar button must go too */
        composite_all();
    } else {
        int newfoc = focused_window();               /* the next window gains focus: brightens */
        if (newfoc >= 0)
            gone = rect_union(gone, rect_window(newfoc));
        composite_rect(gone);
    }
    give();
    close(bufd);
    close(sock);
}

/* A client's thread: it records itself as finished so the accept loop can
 * reap it (this OS has no thread_detach; an unreaped thread stays a zombie
 * and uses up one of the process's 16 thread numbers). */
static int client_thread(void *arg)
{
    client_work((int)(intptr_t)arg);
    take();
    if (done_count < (int)(sizeof(done_threads) / sizeof(done_threads[0])))
        done_threads[done_count++] = thread_self();
    give();
    return 0;
}

/* Join the client threads that have finished (they recorded themselves, so
 * thread_join returns at once). Called before each new client is spawned. */
static void reap_clients(void)
{
    int nums[16], n;
    take();
    n = done_count;
    for (int i = 0; i < n; i++)
        nums[i] = done_threads[i];
    done_count = 0;
    give();
    for (int i = 0; i < n; i++) {
        int result;
        thread_join(nums[i], &result);
    }
}

/* --- the session program ---------------------------------------------------- */

/* windowd has no UI of its own, so it launches a session program (a terminal
 * by default) when it starts: that is how a user on real hardware, with no
 * serial console, reaches a shell and other programs. The program is named by
 * `startup =` in the user's ~/.config/windows, else /system/config/windows (a
 * bare name is in /system/<arch>/bin);
 * an empty value means none (a bare compositor, for the tests). It, and the
 * programs the desktop menu launches, are windowd's children; the reaper
 * thread waits on them (RitterOS has no auto-reap: an unwaited child stays a
 * zombie). windowd keeps the desktop up when the startup session (or any
 * program) ends -- the empty background waits, and the user opens another
 * from the desktop menu or quits with its "Close window server" (or
 * Ctrl-Alt-Delete). */


/* Spawn a program by command line (e.g. "owl", "term", "term echo hi"): a
 * bare name is a windowed app (apps/) or else a plain program (bin/). Returns
 * its pid, or -1. The child shares windowd's console/log for its messages. */
static int spawn_program(const char *name)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "%s", name);     /* a writable copy to tokenize */
    char *argv[16];
    int argc = 0;
    for (char *s = cmd; *s && argc < 15;) {
        while (*s == ' ')
            *s++ = '\0';
        if (!*s)
            break;
        argv[argc++] = s;
        while (*s && *s != ' ')
            s++;
    }
    argv[argc] = 0;
    if (argc == 0)
        return -1;
    char path[160];
    if (strchr(argv[0], '/')) {
        snprintf(path, sizeof(path), "%s", argv[0]);
    } else {                                    /* a windowed app (apps), else a program (bin) */
        struct stat st;
        snprintf(path, sizeof(path), "/system/%s/apps/%s.program/%s", ARCH_DIR, argv[0], argv[0]);
        if (stat(path, &st) != 0)
            snprintf(path, sizeof(path), "/system/%s/bin/%s", ARCH_DIR, argv[0]);
    }
    int fds[3] = { 0, 1, 2 };
    return ritteros_spawn(path, argv, SPAWN_NEW_GROUP | SPAWN_NEW_SESSION, fds);
}

/* Wait on windowd's children so they do not pile up as zombies. windowd stays
 * running when they end -- the desktop is up until the user quits it (its
 * "Close window server" or Ctrl-Alt-Delete). */
static int reaper_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int status;
        int pid = waitpid(-1, &status, 0);      /* any child that ended */
        if (pid < 0)                            /* no children just now: wait and look again */
            usleep(200000);
        /* a child ended and has now been reaped; the desktop stays up */
    }
}

/* A key of the window system's settings: the user's ~/.config/windows
 * first (15l), else /system/config/windows; > 0 when found. */
static int windows_setting(const char *key, char *value, size_t size)
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

/* (37e-1b) Its settings are the user's: ~/.config/windows, written with its
 * text (every key a comment at its default) when missing, as login does. */
static void user_settings_ready(void)
{
    struct passwd *pw = getpwuid(getuid());
    if (!pw)
        return;
    char dir[300], path[320];
    snprintf(dir, sizeof(dir), "%s/.config", pw->pw_dir);
    struct stat st;
    if (stat(dir, &st) < 0 && mkdir(dir, 0700) < 0)
        return;
    snprintf(path, sizeof(path), "%s/" SETTINGS_USER_WINDOWS, pw->pw_dir);
    settings_make(path, SETTINGS_USER_WINDOWS_TEXT);
}

static void start_session(void)
{
    char cmd[128];
    int r = windows_setting("startup", cmd, sizeof(cmd));
    if (r <= 0)
        strcpy(cmd, "owl");                 /* no file or no key: the file browser by default */
    if (cmd[0] == '\0')
        return;                             /* startup = (empty): a bare compositor */
    if (spawn_program(cmd) < 0)             /* could not start it: the empty desktop waits */
        printf("windowd: cannot start %s: %s\n", cmd, strerror(errno));
}

int main(int argc, char **argv)
{
    /* (66c-1) SIGUSR1 (the frame report) only to the compositor thread:
     * elsewhere it would end a waiting call, which the input and accept
     * loops take as windowd ending. Threads inherit this; the compositor
     * unblocks it. */
    sigset_t usr1 = 1UL << SIGUSR1;
    sigprocmask(SIG_BLOCK, &usr1, 0);
    unsigned hw = 0, hh = 0;                         /* (41h-1) headless WxH */
    char hx = 0;
    if (argc > 1 && (argc != 3 || strcmp(argv[1], "headless") ||
                     sscanf(argv[2], "%ux%u%c", &hw, &hh, &hx) != 2 || hw < 64 || hh < 64 ||
                     hw > WINDOW_SIZE_MAX || hh > WINDOW_SIZE_MAX)) {
        dprintf(2, "usage: windowd [headless WxH] (W and H 64-%d)\n", WINDOW_SIZE_MAX);
        return 2;
    }
    headless = argc == 3;
    const char *sock_path = getenv("WINDOW_SOCKET");    /* (41h-1) where to listen */
    if (!sock_path || !*sock_path)
        sock_path = WINDOW_SOCKET;
    if (headless) {
        /* The clients' pixels are the screen's format (libwindow asks the
         * display), so the back buffer keeps it; only the size is ours. */
        if (ritteros_display_info(&screen) < 0)
            screen = (struct sys_display){ .bytes_per_pixel = 4, .red_shift = 16, .green_shift = 8 };
        screen.width = screen.stride = hw;
        screen.height = hh;
    } else if (ritteros_display_acquire() < 0) {
        printf("windowd: cannot take the screen: %s\n", strerror(errno));
        return 1;
    } else if (ritteros_display_info(&screen) < 0) {
        printf("windowd: no screen info: %s\n", strerror(errno));
        return 1;
    }
    back_fd = ritteros_share_create((size_t)screen.width * screen.height * 4);
    back = back_fd < 0 ? 0 : ritteros_share_map(back_fd);
    if (!back) {
        printf("windowd: no back buffer\n");
        return 1;
    }
    cursor_x = screen.width / 2;
    cursor_y = screen.height / 2;
    int listener = ritteros_local_listen(sock_path, 8);
    if (listener < 0) {
        printf("windowd: cannot listen on %s: %s\n", sock_path, strerror(errno));
        return 1;
    }
    user_settings_ready();                           /* (37e-1b) */
    char theme[16], accent[16];                      /* (41g-5) as libwindow's window_color */
    pal = windows_setting("theme", theme, sizeof(theme)) > 0 && !strcasecmp(theme, "light")
              ? palette_light : palette_dark;
    selection = 0x5E81AC;                            /* (both themes' SELECTION) */
    if (windows_setting("accent", accent, sizeof(accent)) > 0) {
        char *end;
        unsigned long a = strtoul(accent, &end, 16);
        if (strlen(accent) == 6 && *end == 0)
            selection = (uint32_t)a;
        else
            printf("windowd: accent = %s: not RRGGBB\n", accent);
    }
    char ms[16];
    if (windows_setting("double_click", ms, sizeof(ms)) > 0) {    /* (15n) */
        long v = 0;
        for (const char *c = ms; *c >= '0' && *c <= '9'; c++)
            v = v * 10 + (*c - '0');
        if (v >= 100 && v <= 5000)
            double_click_ns = (uint64_t)v * 1000000u;
        else
            printf("windowd: double_click = %s: not 100-5000 (ms); 500 it is\n", ms);
    }
    char steady_value[8];                            /* (15q) on unless "off" */
    if (windows_setting("click_steady", steady_value, sizeof(steady_value)) > 0 &&
        !strcmp(steady_value, "off"))
        click_steady = false;
    /* (65e-34b) The frame rate: the setting `refresh` (auto, or frames a
     * second), auto the screen's own (its EDID through DISPLAY_MODES),
     * else 60. */
    char rv[16];
    uint32_t mhz = 0;
    const char *from = "the default";
    if (windows_setting("refresh", rv, sizeof(rv)) > 0 && strcmp(rv, "auto")) {
        double hz = atof(rv);
        if (hz >= 10 && hz <= 1000) {
            mhz = (uint32_t)(hz * 1000 + 0.5);
            from = "the refresh setting";
            refresh_fixed = 1;
        } else {
            printf("windowd: refresh = %s: not auto or 10-1000 (frames a second)\n", rv);
        }
    }
    static struct sys_display_modes modes;
    if (!mhz && !headless && ritteros_display_modes(&modes) == 0 && modes.now.refresh_mhz) {
        mhz = modes.now.refresh_mhz;
        from = "the screen's EDID";
    }
    if (!mhz)
        mhz = 60000;
    frame_us = 1000000000ull / mhz;
    /* (66c-3, 66d-3) In step with the screen where a driver tells its
     * vertical blanks (unless `refresh` chose a rate), flipping between
     * its screen buffers where it has them; else the timer. */
    uint64_t vframe;
    use_vblank = !headless && !refresh_fixed && ritteros_display_vblank(&vframe, 100) == 1;
    use_flip = use_vblank && ritteros_display_buffers(&screen_buffers) == 0 &&
               screen_buffers.count >= 2 && screen_buffers.width == screen.width &&
               screen_buffers.height == screen.height;
    take();
    composite_all();                                 /* an empty desktop and the pointer */
    give();
    signal(SIGUSR1, on_usr1);                        /* (66c-1) the frame report */
    if (thread_create(compositor_thread, 0) < 0)     /* (65e-34b) */
        printf("windowd: no compositor thread\n");
    if (thread_create(notes_thread, 0) < 0)          /* (41g-4) */
        printf("windowd: no notification thread\n");
    printf("windowd: listening on %s, screen %ux%u%s\n", sock_path, screen.width, screen.height,
           headless ? " (headless)" : "");
    if (use_flip)
        printf("windowd: page flipping between %u screen buffers (%u.%03u Hz)\n",
               screen_buffers.count, mhz / 1000, mhz % 1000);
    else if (use_vblank)
        printf("windowd: drawing at the screen's vertical blank (%u.%03u Hz)\n", mhz / 1000,
               mhz % 1000);
    else
        printf("windowd: %u.%03u frames a second at most (%s)\n", mhz / 1000, mhz % 1000, from);
    if (!headless && thread_create(input_thread, 0) < 0)
        printf("windowd: no input thread\n");
    if (!headless)
        start_session();                             /* launch the terminal (or the configured app) */
    if (thread_create(reaper_thread, 0) < 0)         /* reap children; exit when the session ends */
        printf("windowd: no reaper thread\n");
    for (;;) {
        int sock = ritteros_accept(listener, 0, 0);
        if (sock < 0)
            break;
        int uid = ritteros_peer_uid(sock);          /* (15k) its own user's and root's only */
        if (uid != (int)getuid() && uid != 0) {
            printf("windowd: refused a program of user %d (this is user %d's screen)\n", uid,
                   (int)getuid());
            close(sock);
            continue;
        }
        reap_clients();                             /* free the slots of clients that left */
        if (thread_create(client_thread, (void *)(intptr_t)sock) < 0)
            close(sock);
    }
    return 0;
}

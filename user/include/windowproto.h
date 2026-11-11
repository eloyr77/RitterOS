/*
 * The window protocol between windowd and its clients (step 15e). Clients
 * connect to the local socket WINDOW_SOCKET and exchange fixed-size messages;
 * a window's pixels travel as a shared buffer passed over the same socket.
 * libwindow (15f) will wrap this; for now clients use it directly.
 *
 * Creating a window: the client makes a shared buffer of width*height*4
 * native pixels, sends it with ritteros_share_send, then sends WMSG_CREATE
 * with the size. windowd receives the buffer, shows the window, and replies
 * WMSG_CREATED with its id. windowd then sends WMSG_POINTER and WMSG_KEY as
 * input arrives for the window.
 */
#ifndef RITTEROS_WINDOWPROTO_H
#define RITTEROS_WINDOWPROTO_H

#include <stdint.h>

#define WINDOW_SOCKET   "/run/windowd"

/* client -> windowd */
#define WMSG_CREATE     1       /* a: width, b: height, c: title length (a shared buffer was
                                 * just sent; c bytes of title follow this message) 
                                 * (41f-5) | (the parent's id + 1) << 16 for a dialog
                                 * (41g-3) | 1 << 31: after the title a uint8 length
                                 * and the program's name (getprogname: its app) */
#define WMSG_CREATE_APP (1u << 31)
#define WMSG_DAMAGE     5       /* the window's pixels changed: please recomposite */
#define WMSG_BUFFER     7       /* a: width, b: height; a new buffer (sent just before) replaces
                                 * the window's, at the new size (the client's reply to RESIZE) */
#define WMSG_SET_MENU   9       /* c: length of a menu-bar blob that follows (see below); sets
                                 * the window's menu bar (c = 0 clears it) */
#define WMSG_POPUP      11      /* a: x, b: y (in the window), c: length of an item blob that
                                 * follows; windowd pops up a context menu there */
#define WMSG_CLIP_SET   15      /* c: length of the text that follows (UTF-8, up to
                                 * WINDOW_CLIPBOARD_MAX): the clipboard now holds it (15w) */
#define WMSG_CLIP_GET   16      /* please send the clipboard (WMSG_CLIP_TEXT) */
/* windowd -> client */
#define WMSG_CREATED    2       /* a: the window id */
#define WMSG_POINTER    3       /* a: x in the window, b: y in the window, c: buttons */
#define WMSG_KEY        4       /* a: the byte */
#define WMSG_CLOSE      6       /* the user clicked the close box: please close the window */
#define WMSG_RESIZE     8       /* a: width, b: height; please become this content size
                                 * (reply with a new buffer via WMSG_BUFFER, or ignore) */
#define WMSG_MENU       10      /* a: the id of the menu item the user chose */
#define WMSG_KEYCODE    14      /* a: a physical key's USB HID usage ID, b: 1 pressed, 0 released
                                 * (15p; beside WMSG_KEY's bytes) */
#define WMSG_SCROLL     13      /* a: wheel steps (int32, negative up), b, c: the pointer in the
                                 * window: the wheel turned over it (15o) */
#define WMSG_DOUBLE_CLICK 12    /* a: x, b: y in the window: the left button's second press
                                 * there within the double_click time (windowd decides, 15n) */
#define WMSG_CLIP_TEXT  17      /* c: length of the clipboard's text that follows (the answer
                                 * to WMSG_CLIP_GET; 0: empty) */
#define WMSG_FOCUS      18      /* (41f-1) a: 1 the window got the keyboard (it is the front one),
                                 * 0 it lost it; also once when the window is made */
#define WMSG_LIMITS     19      /* (41f-2) a: the content's least width | height << 16, b: its
                                 * greatest (0: none); a resize or a maximize keeps to them */
#define WMSG_HOVER      20      /* (41f-3) a: 1 the pointer came over the window's content, 0 it
                                 * left it (for another window, a frame, the desktop, a menu) */
#define WMSG_CURSOR     21      /* (41f-4) a: the pointer's shape over the content (WINDOW_CURSOR_*) */
#define WMSG_TEXT       22      /* (41f-4) a: a typed character (Unicode), sent after its WMSG_KEY bytes */
#define WMSG_FULLSCREEN 23      /* (41f-6) a: 1 the whole screen without a frame, 0 back (a
                                 * WMSG_RESIZE follows, as for the user's resize) */
#define WMSG_MINIMIZE   24      /* (41f-6) off to a taskbar button (the user brings it back) */
#define WMSG_DRAG       25      /* (41f-6) c: length of the text that follows (up to
                                 * WINDOW_CLIPBOARD_MAX): a drag starts, while the left button is
                                 * held (else ignored) */
#define WMSG_DROP       26      /* (41f-6) windowd -> client: a: x, b: y in the window where a
                                 * drag was let go, c: length of its text that follows */
#define WMSG_NOTIFY     27      /* (41g-4) a: title length (< WINDOW_TITLE_MAX), b: text length
                                 * (< WINDOW_NOTIFY_MAX), the title and text follow: a
                                 * notification, shown at the top right for 5 s; also the one
                                 * message of a connection without a window (`notify`) */
#define WMSG_SHOT       28      /* (41h-2) the one message of a connection without a window
                                 * (`wshot`): windowd answers WMSG_SHOT (a: width, b: height,
                                 * c: red_shift | green_shift << 8 | blue_shift << 16), then
                                 * sends a shared buffer holding a copy of the whole screen
                                 * (width x height pixels, rows width apart) */
#define WMSG_INPUT      29      /* (41h-3) a connection without a window (`wsend`) sends these
                                 * one after another, each a keyboard or pointer event (a
                                 * struct sys_input_event: a: type | buttons << 8 | key << 16 |
                                 * modifiers << 24, b: x | y << 16) windowd handles as from the
                                 * devices; it answers each with WMSG_INPUT, a: 1 taken, 0
                                 * refused (only a headless windowd takes them) */
#define WMSG_WINDOWS    30      /* (41h-6) the one message of a connection without a window
                                 * (`wsend wait TITLE`): windowd answers WMSG_WINDOWS (a: how
                                 * many windows, c: the length of the text that follows), then
                                 * their titles, each ending in a newline */

/* A menu-bar blob (WMSG_SET_MENU): uint8 n_menus, then per menu
 *   uint8 title_len, title bytes, uint8 n_items, then per item
 *     uint16 id (little-endian), uint8 label_len, label bytes.
 * A pop-up blob (WMSG_POPUP) is one menu's items: uint8 n_items, then the items.
 * Ids are the client's own; windowd sends the chosen one back in WMSG_MENU. */
#define WINDOW_MENU_MAX   6     /* top-level menus per window */
#define WINDOW_MENU_ITEMS 16    /* items per menu */
#define WINDOW_LABEL_MAX  32    /* a menu label, with its NUL */

#define WINDOW_TITLE_MAX 64     /* a title, with its NUL */
#define WINDOW_CLIPBOARD_MAX 65536  /* the clipboard's text, bytes (15w) */
#define WINDOW_NOTIFY_MAX 512   /* a notification's text, with its NUL (41g-4) */

struct wmsg {
    uint32_t type;
    uint32_t a, b, c;
};

#endif

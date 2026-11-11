/*
 * libwindow (step 15f): the toolkit for graphical programs. It wraps the
 * window protocol (windowproto.h) so a program need not speak it directly:
 * open a window, draw into its pixels (fill rectangles, draw text in the
 * shared font), show the result, and read pointer and keyboard events. The
 * same drawing a program uses is the drawing every program uses, so windows
 * look alike.
 */
#ifndef RITTEROS_WINDOW_H
#define RITTEROS_WINDOW_H

#include <stddef.h>
#include <stdint.h>

struct window;

/* Events a window receives. */
#define WINDOW_NONE     0
#define WINDOW_POINTER  1       /* the pointer moved or a button changed */
#define WINDOW_KEY      2       /* a key: `key` is the byte (as the terminal gets) */
#define WINDOW_CLOSED   3       /* the window was closed (the close box, or windowd went away) */
#define WINDOW_RESIZE   4       /* the user resized the window: x, y is the new content size;
                                 * call window_resize to adopt it, then redraw */
#define WINDOW_MENU     5       /* the user chose a menu item: `menu` is its id */
#define WINDOW_DOUBLE_CLICK 6   /* the left button pressed twice in a row at (x, y), within the
                                 * user's double_click time and 4 pixels (after the second
                                 * press's WINDOW_POINTER); the same rule for every program */
#define WINDOW_SCROLL   7       /* the mouse wheel turned over the window: y = steps, negative
                                 * up (away from the user); a list moves 3 rows a step (15o) */
#define WINDOW_KEY_DOWN 8       /* a physical key pressed: `key` is its WKEY_* number (the USB
                                 * HID usage ID), modifiers too; once, not repeated (15p) */
#define WINDOW_KEY_UP   9       /* that key released (also when the window loses the front) */
#define WINDOW_TIMER    10      /* (41a) a timer of window_timer came due: `key` is its id */
#define WINDOW_FOCUS    11      /* (41f-1) the window got the keyboard (`key` 1) or lost it (0);
                                 * also once right after window_open */
#define WINDOW_ENTER    12      /* (41f-3) the pointer came over the window's content */
#define WINDOW_LEAVE    13      /* (41f-3) it left it: for another window, the frame, the desktop
                                 * or a menu (hover highlights end) */
#define WINDOW_TEXT     14      /* (41f-4) a typed character, whole: `key` its Unicode code point
                                 * (after the WINDOW_KEY bytes; never control bytes or a key's
                                 * escape sequence) */
#define WINDOW_DROP     15      /* (41f-6) a drag was let go over the window at (x, y): its text
                                 * is window_drop_text's */

/* Physical keys (WINDOW_KEY_DOWN/UP): the USB HID usage IDs, the same for
 * every keyboard and layout (where the key is, not what it types: WKEY_A
 * is the key labelled A on a US keyboard). Others by number. */
#define WKEY_A          0x04    /* ... WKEY_Z 0x1D, in order */
#define WKEY_B          0x05
#define WKEY_C          0x06
#define WKEY_D          0x07
#define WKEY_E          0x08
#define WKEY_F          0x09
#define WKEY_G          0x0A
#define WKEY_H          0x0B
#define WKEY_I          0x0C
#define WKEY_J          0x0D
#define WKEY_K          0x0E
#define WKEY_L          0x0F
#define WKEY_M          0x10
#define WKEY_N          0x11
#define WKEY_O          0x12
#define WKEY_P          0x13
#define WKEY_Q          0x14
#define WKEY_R          0x15
#define WKEY_S          0x16
#define WKEY_T          0x17
#define WKEY_U          0x18
#define WKEY_V          0x19
#define WKEY_W          0x1A
#define WKEY_X          0x1B
#define WKEY_Y          0x1C
#define WKEY_Z          0x1D
#define WKEY_1          0x1E    /* ... WKEY_9 0x26, then WKEY_0 */
#define WKEY_2          0x1F
#define WKEY_3          0x20
#define WKEY_4          0x21
#define WKEY_5          0x22
#define WKEY_6          0x23
#define WKEY_7          0x24
#define WKEY_8          0x25
#define WKEY_9          0x26
#define WKEY_0          0x27
#define WKEY_ENTER      0x28
#define WKEY_ESCAPE     0x29
#define WKEY_BACKSPACE  0x2A
#define WKEY_TAB        0x2B
#define WKEY_SPACE      0x2C
#define WKEY_MINUS      0x2D
#define WKEY_EQUAL      0x2E
#define WKEY_LBRACKET   0x2F
#define WKEY_RBRACKET   0x30
#define WKEY_BACKSLASH  0x31
#define WKEY_SEMICOLON  0x33
#define WKEY_APOSTROPHE 0x34
#define WKEY_GRAVE      0x35
#define WKEY_COMMA      0x36
#define WKEY_PERIOD     0x37
#define WKEY_SLASH      0x38
#define WKEY_CAPSLOCK   0x39
#define WKEY_F1         0x3A    /* ... WKEY_F12 0x45, in order */
#define WKEY_F2         0x3B
#define WKEY_F3         0x3C
#define WKEY_F4         0x3D
#define WKEY_F5         0x3E
#define WKEY_F6         0x3F
#define WKEY_F7         0x40
#define WKEY_F8         0x41
#define WKEY_F9         0x42
#define WKEY_F10        0x43
#define WKEY_F11        0x44
#define WKEY_F12        0x45
#define WKEY_PRINTSCREEN 0x46
#define WKEY_SCROLLLOCK 0x47
#define WKEY_PAUSE      0x48
#define WKEY_INSERT     0x49
#define WKEY_HOME       0x4A
#define WKEY_PAGEUP     0x4B
#define WKEY_DELETE     0x4C
#define WKEY_END        0x4D
#define WKEY_PAGEDOWN   0x4E
#define WKEY_RIGHT      0x4F
#define WKEY_LEFT       0x50
#define WKEY_DOWN       0x51
#define WKEY_UP         0x52
#define WKEY_LCTRL      0xE0
#define WKEY_LSHIFT     0xE1
#define WKEY_LALT       0xE2
#define WKEY_LGUI       0xE3    /* the Windows/Command key */
#define WKEY_RCTRL      0xE4
#define WKEY_RSHIFT     0xE5
#define WKEY_RALT       0xE6    /* AltGr on the US International layout */
#define WKEY_RGUI       0xE7

struct window_event {
    int type;
    int x, y;                   /* WINDOW_POINTER: position in the window; WINDOW_RESIZE: new size */
    unsigned buttons;           /* WINDOW_POINTER: buttons held */
    int key;                    /* WINDOW_KEY: the byte; WINDOW_KEY_DOWN/UP: WKEY_*;
                                 * WINDOW_TIMER: the timer's id; WINDOW_FOCUS: 1 or 0 */
    int menu;                   /* WINDOW_MENU: the chosen item's id */
};

/* A menu the program declares; windowd draws it and reports the chosen id.
 * A menu bar is an array of these (each a dropdown); a context menu is one
 * list of items. */
struct window_menu_item { int id; const char *label; };
struct window_menu { const char *title; const struct window_menu_item *items; int count; };

/* Set the window's menu bar (drawn below the title bar); `count` 0 clears it. */
int window_set_menu(struct window *win, const struct window_menu *menus, int count);

/* Pop up a context menu of `items` at (x, y) in the window (e.g. on a
 * right-click the program received). The chosen id comes back as WINDOW_MENU. */
int window_popup_menu(struct window *win, int x, int y,
                      const struct window_menu_item *items, int count);

/* Open a window of w x h pixels with a title. 0 if windowd is unreachable or
 * out of memory, or (errno EACCES) if the windowd running is another user's
 * (only its own user's and root's programs may use it). (41h-1) The
 * windowd is the one listening at /run/windowd, or where the environment
 * variable WINDOW_SOCKET says (a headless windowd's, for tests). */
struct window *window_open(int w, int h, const char *title);
void window_close(struct window *win);

/* (41f-5) A dialog: a window tied to `parent`, opened centred over it and
 * in front; while it is open the parent takes no input (a click on it
 * brings the dialog forward), and when it closes the parent is in front
 * again. As window_open otherwise. */
struct window *window_dialog_open(struct window *parent, int w, int h, const char *title);

int window_width(struct window *win);
int window_height(struct window *win);

/* Scroll bars (15o), the same in every app. window_scrollbar draws one at
 * (x, y), WINDOW_SCROLLBAR_W wide and h high, for a list of `total` rows of
 * which `shown` fit, starting at row `first`: a trough and a thumb as long
 * as the shown part (nothing to scroll: the trough alone).
 * window_scrollbar_hit gives the first row for a click or a drag at py (a y
 * in the window) on such a bar: the thumb centred there. */
#define WINDOW_SCROLLBAR_W 12
void window_scrollbar(struct window *win, int x, int y, int h, int first, int shown, int total);
int window_scrollbar_hit(int y, int h, int py, int shown, int total);

/* Adopt a new size after a WINDOW_RESIZE event: makes a new pixel buffer of
 * w x h, hands it to windowd, and replaces the window's. The new pixels are
 * blank, so redraw and window_flush after. 0 on success, -1 on failure (the
 * old buffer is kept). */
int window_resize(struct window *win, int w, int h);

/* A native pixel value for the (r, g, b) color. */
uint32_t window_rgb(struct window *win, int r, int g, int b);

/* Drawing into the window's pixels (clipped to the window). */
void window_fill(struct window *win, int x, int y, int w, int h, uint32_t color);
void window_text(struct window *win, int x, int y, const char *utf8, uint32_t fg, uint32_t bg);
/* The same with a font of wfont.h (wfont_find; 0: the default). */
struct font;
void window_text_font(struct window *win, const struct font *font, int x, int y, const char *utf8,
                      uint32_t fg, uint32_t bg);
#define WINDOW_TRANSPARENT 0xFFFFFFFFu  /* bg that leaves the pixels behind the text */

/* Copy a w x h block of native pixels (as window_rgb makes) into the window
 * at (x, y), clipped. For images and video frames (libimage/libvideo). */
void window_blit(struct window *win, int x, int y, int w, int h, const uint32_t *src);

/* The window's native pixel layout: the left shifts of the red, green and
 * blue channels, so a program can build native pixels itself in bulk. */
void window_shifts(struct window *win, int *red, int *green, int *blue);

/* (41b) The window's own pixels (window_width x window_height, rows that
 * wide, in window_shifts' layout): a libdraw canvas draws on them in
 * place. They move with window_resize: ask again after it. */
uint32_t *window_pixels(struct window *win);

/* Show what was drawn (windowd recomposites). */
void window_flush(struct window *win);

/* Wait for the next event; 1 with *ev filled, or 0 when the window closed. */
int window_wait(struct window *win, struct window_event *ev);

/* (41a) The event loop beside other work. window_wait_timeout waits at
 * most ms milliseconds (0: not at all; negative: as long as it takes): 1
 * with *ev filled, 0 when none came in time (ev->type WINDOW_NONE), -1
 * when the window closed (WINDOW_CLOSED). window_timer makes timer `id`
 * come due every ms milliseconds as a WINDOW_TIMER event, from now on,
 * until it is set again (ms 0 removes it); a late one is not caught up
 * (at most one event each time it is looked at); 0, or -1 when the
 * window has 8 timers already. Both waits hand out timers that are due.
 * window_fd is the window's connection to windowd, readable when windowd
 * sent something: a program waiting on its own descriptors puts it in its
 * poll (POLLIN), and after any wake-up takes every event with
 * window_wait_timeout(win, &ev, 0) until that returns 0 (events read early
 * by window_clipboard_get are kept there, not in the descriptor). Timers
 * do not make it readable: such a poll waits no longer than the program's
 * next timer. Never read or write the descriptor itself. */
int window_wait_timeout(struct window *win, struct window_event *ev, int ms);
int window_timer(struct window *win, int ms, int id);
int window_fd(struct window *win);

/* (41f-4) The pointer's shape while it is over the window's content. */
#define WINDOW_CURSOR_ARROW 0
#define WINDOW_CURSOR_TEXT  1       /* an I-beam: over text that can be edited */
#define WINDOW_CURSOR_HAND  2       /* a hand: over a link */
void window_set_cursor(struct window *win, int shape);

/* (41f-2) The sizes (content, pixels) the user's resizing and maximizing
 * keep to: at least min_w x min_h, at most max_w x max_h (0: no limit
 * that way). The window's present size is not changed. */
void window_set_limits(struct window *win, int min_w, int min_h, int max_w, int max_h);

/* (41f-6) window_fullscreen(win, 1): the window fills the screen, without
 * its frame and title bar, in front of the other windows and the taskbar
 * (and follows a change of the screen's mode); window_fullscreen(win, 0)
 * puts it back where and as large as it was. The new size comes as a
 * WINDOW_RESIZE (answer it with window_resize, as a resize by the user);
 * the limits of window_set_limits do not apply. Leaving full screen is the
 * program's to offer (a key, a button): windowd has none.
 * window_minimize: the window goes to a taskbar button, as by its - box;
 * the user brings it back. */
void window_fullscreen(struct window *win, int on);
void window_minimize(struct window *win);

/* (41f-6) Drag and drop between programs. window_drag, called while the
 * left button pressed in the window is still held (on the pointer's move
 * that starts the drag), makes n bytes of text (up to 64 KiB) what the
 * pointer carries; windowd shows a page beside it, and when the button is
 * let go over a window's content, that window gets WINDOW_DROP with where
 * it fell (its own window too). Called after the button is let go, it does
 * nothing. Files are dragged as their full paths, one per line.
 * window_drop_text is the text of the last WINDOW_DROP (NUL-ended; "" if
 * none), kept until the next one. */
int window_drag(struct window *win, const char *text, size_t n);
const char *window_drop_text(struct window *win);

/* (41g-2) The program a double click starts for the file `path`, by its
 * ending (in any case): (42a) the user's ~/.config/filetypes (written with
 * its defaults when missing), else /system/config/filetypes. Its NAME goes
 * into buf; the result is 0 for an app, (42b) a bundle in
 * /system/<arch>/apps (run NAME PATH: its window), 1 for a program in bin
 * (term NAME PATH), -1 when none is set or the program is in neither. */
int window_app_for(const char *path, char *buf, size_t size);

/* (41g-4) A notification: a box at the top right of the screen, over
 * everything, for 5 seconds, with the title (bold; 0 or "" for none) and
 * the text below it, wrapped (a newline starts a line; up to 511 bytes,
 * the rest cut). Up to four show at once, the newest on top. win may be 0
 * (a program without a window: it reaches windowd by a connection of its
 * own). 0, or -1 when windowd could not be reached. */
int window_notify(struct window *win, const char *title, const char *text);

/* (41g-5) The user's colours, as 0xRRGGBB (window_rgb takes them apart):
 * `theme = dark` (the default) or `light`, and `accent = RRGGBB`, in
 * ~/.config/windows, else /system/config/windows; read once. The accent,
 * when set, is both ACCENT and SELECTION; otherwise each is the theme's.
 * An unknown `which` gives the theme's TEXT. */
#define WINDOW_COLOR_BACKGROUND   0     /* a window's background */
#define WINDOW_COLOR_PANEL        1     /* an area set apart on it (a list, a field) */
#define WINDOW_COLOR_FACE         2     /* a button */
#define WINDOW_COLOR_FACE_PRESSED 3     /* a button held down */
#define WINDOW_COLOR_HOVER        4     /* a button under the pointer */
#define WINDOW_COLOR_TEXT         5     /* text on any of them */
#define WINDOW_COLOR_EDGE         6     /* lines: edges, boxes, separators */
#define WINDOW_COLOR_ACCENT       7     /* focus, progress, a slider's part done */
#define WINDOW_COLOR_SELECTION    8     /* what is chosen: rows, title bars of the window in front */
uint32_t window_color(int which);

/* The clipboard (15w), windowd's, shared by the screen's user's programs:
 * set it to n bytes of UTF-8 text (up to 64 KiB), or get its text into buf
 * (NUL-ended, cut to size - 1): its length, or -1. Get from the thread
 * that calls window_wait (events arriving meanwhile are kept for it). */
int window_clipboard_set(struct window *win, const char *utf8, size_t n);
int window_clipboard_get(struct window *win, char *buf, size_t size);

/* A one-line text field (15w), the same in every app: at (x, y), w pixels
 * wide (its height: window_field_height). Give it every event of the
 * window: window_field_event returns -1 for one not its own (the app's),
 * 0 for one it took, 1 when Enter was pressed in it. A click focuses it, a
 * drag or Shift selects, a double click selects a word; Ctrl+A, Ctrl+C,
 * Ctrl+X and Ctrl+V select all, copy, cut and paste through the
 * clipboard. It draws and shows itself after a change it took; the app
 * calls window_field_draw when it redraws its window. window_field_focus
 * gives it the keyboard (with all its text selected) or takes it away. */
struct window_field;
struct window_field *window_field_new(struct window *win, int x, int y, int w);
void window_field_free(struct window_field *f);
void window_field_place(struct window_field *f, int x, int y, int w);
int window_field_height(const struct window_field *f);
void window_field_draw(struct window_field *f);
int window_field_event(struct window_field *f, const struct window_event *ev);
const char *window_field_text(const struct window_field *f);
void window_field_set_text(struct window_field *f, const char *utf8);
void window_field_focus(struct window_field *f, int on);
int window_field_focused(const struct window_field *f);

/* The "Open file" dialog every app shares: a window of its own listing
 * `folder` (0 or not a folder: "/"), titled `title` (0: "Open"). The user
 * picks a file with clicks (a second click on a row opens it), the arrow keys
 * and Enter, or types a name or path; Backspace with nothing typed goes up.
 * It returns when the user is done: 1 with the file's full path in `path`
 * (`size` bytes), 0 if cancelled (Cancel, the close box), -1 if no window
 * could open. Modal: `parent` (may be 0) gets no events meanwhile. */
int window_choose_file(struct window *parent, const char *title, const char *folder,
                       char *path, int size);

/* (42c) The same dialog, choosing what `accept(path, data)` says may be
 * chosen (non-zero): every other file is shown greyed and cannot be
 * picked, and an app bundle (a folder NAME.program) is one item, shown as
 * NAME, chosen or greyed as accept says (never entered); plain folders are
 * browsed. accept 0 is window_choose_file. The chosen path (a bundle's
 * too, without a slash at its end) goes into `path`. */
int window_choose(struct window *parent, const char *title, const char *folder,
                  int (*accept)(const char *path, void *data), void *data, char *path, int size);

/* (42c) Can the app bundle at `bundle` (a folder NAME.program, holding
 * its program NAME) open `file`: does its info's `types` list the file's
 * ending (any case)? 1 or 0. */
int window_app_opens(const char *bundle, const char *file);

#endif

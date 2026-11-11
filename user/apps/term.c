/*
 * term: a terminal in a window (step 15g). It opens a window with libwindow,
 * runs the shell -- or, given a command (term CMD ARGS...), that command (15k
 * step 17a, so the file manager can open a program in a terminal) -- on a
 * pseudo-terminal, renders the output as a grid of characters, and sends the
 * keys it receives to the program. A small VT100 interpreter handles the
 * cursor moves and erases the line editor and programs use; colors are not
 * kept yet (light grey on black; 41g-5: the light theme's dark text on
 * its light background).
 *
 * The shell's terminal exits when the shell does (so closing it leaves the
 * GUI); a terminal running a given command keeps its window open after the
 * command exits (showing an "[exited]" line) so its output can be read, until
 * the user closes the window.
 *
 * One thread reads the pty and draws; the main thread reads window events and
 * writes keys to the pty. The grid and window are shared by the resize, so a
 * small lock keeps the two apart.
 *
 * Scrollback (15o): the last HISTORY lines that left the top are kept; the
 * wheel (3 lines a step) or the scroll bar at the right shows them, and a key
 * or new output brings the view back to the bottom.
 *
 * Selection (15x, the user's): dragging across the text selects it (shown
 * inverted), a double click selects a word; Ctrl+Shift+C copies it to the
 * clipboard (lines joined, their trailing blanks dropped), Ctrl+Shift+V
 * types the clipboard into the program (line breaks as Enter). Plain Ctrl+C
 * stays the interrupt. The selection is kept by absolute line numbers (lines
 * counted as they scroll away), so it stays on its text while output goes on.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread.h>
#include <unistd.h>
#include "ritteros.h"
#include "wfont.h"
#include "window.h"

PROGRAM_HELP("term [-f FONT] [CMD [ARG...]]",
             "a terminal in a window (FONT of /system/fonts); with a command, runs it (needs windowd)");

#define MAXCOLS 170                     /* the grid is a fixed max (no malloc); cols/rows are */
#define MAXROWS 60                      /* the part in use, recomputed when the window resizes */
static int cw = 9, ch = 15;             /* the font's cell (9x15 unless -f FONT) */
static const struct font *font;
static char font_name[32] = "9x15";

#if defined(__x86_64__)
#define ARCH "x86_64"
#else
#define ARCH "aarch64"
#endif

static struct window *win;
static int master;                      /* the pty master */
static uint32_t grid[MAXROWS][MAXCOLS]; /* code points, 0 = blank */
#define HISTORY 1000                    /* lines kept after they leave the top (15o) */
#define CLIP_MAX 65536                  /* the clipboard's most (windowd's) */
static uint32_t history[HISTORY][MAXCOLS];  /* a ring: the oldest at head - count */
static int hist_head, hist_count;
static uint64_t scrolled;               /* lines that ever left the top: absolute line numbers */
static int back;                        /* lines the view is scrolled back (0: the live screen) */
static bool dragging_bar;               /* the scroll bar's thumb held */
static int cols = 80, rows = 25;        /* the part of the grid in use */
static int cx, cy;                      /* the cursor */
static uint32_t fg, bg;
static int child_pid;                   /* the shell or command running on the pty */
static int running_command;             /* a given command (keep the window open at its exit) */
static char child_name[48];             /* its name, for the exit line and the title */

/* The grid and the window belong to the reader thread; the resize (on the main
 * thread) also touches them, so a small lock keeps the two apart. It is a
 * ticket lock, taken in the order asked: a waiter sleeps a tick at a time,
 * and with a plain flag the reader -- giving the lock back and taking it
 * again at once while output streams in -- won every time, so a resize (and
 * with it the main thread's reading of window events, the close box too)
 * could wait forever. */
static unsigned next_ticket, now_serving;
static void take(void)
{
    unsigned mine = __atomic_fetch_add(&next_ticket, 1, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&now_serving, __ATOMIC_ACQUIRE) != mine)
        usleep(200);
}
static void give(void) { __atomic_fetch_add(&now_serving, 1, __ATOMIC_RELEASE); }

/* --- the character grid ------------------------------------------------------ */

static void scroll_up(void)
{
    memcpy(history[hist_head], grid[0], sizeof(grid[0]));   /* into the scrollback */
    scrolled++;
    hist_head = (hist_head + 1) % HISTORY;
    if (hist_count < HISTORY)
        hist_count++;
    memmove(&grid[0], &grid[1], sizeof(grid[0]) * (rows - 1));
    memset(&grid[rows - 1], 0, sizeof(grid[0]));
}

static void newline(void)
{
    if (++cy >= rows) {
        scroll_up();
        cy = rows - 1;
    }
}

static void put(uint32_t cp)
{
    if (cx >= cols) {
        cx = 0;
        newline();
    }
    grid[cy][cx++] = cp;
}

/* --- a small VT100 interpreter ----------------------------------------------- */

static enum { NORMAL, ESC, CSI } state;
static int params[8], nparam;
static uint32_t utf8_cp;                /* a UTF-8 character being assembled */
static int utf8_left;

static void csi(char final)
{
    int a = nparam > 0 ? params[0] : 0;
    switch (final) {
    case 'A': cy -= a > 0 ? a : 1; if (cy < 0) cy = 0; break;
    case 'B': cy += a > 0 ? a : 1; if (cy >= rows) cy = rows - 1; break;
    case 'C': cx += a > 0 ? a : 1; if (cx >= cols) cx = cols - 1; break;
    case 'D': cx -= a > 0 ? a : 1; if (cx < 0) cx = 0; break;
    case 'H': case 'f':
        cy = (nparam > 0 ? params[0] : 1) - 1;
        cx = (nparam > 1 ? params[1] : 1) - 1;
        if (cx < 0) cx = 0;
        if (cy < 0) cy = 0;
        if (cx >= cols) cx = cols - 1;
        if (cy >= rows) cy = rows - 1;
        break;
    case 'J':                           /* erase display: 2 = all */
        if (a == 2) {
            memset(grid, 0, sizeof(grid));
            cx = cy = 0;
        } else {                        /* 0 (default): to the end of the screen */
            for (int c = cx; c < cols; c++) grid[cy][c] = 0;
            for (int r = cy + 1; r < rows; r++) memset(&grid[r], 0, sizeof(grid[0]));
        }
        break;
    case 'K':                           /* erase line */
        if (a == 1) for (int c = 0; c <= cx && c < cols; c++) grid[cy][c] = 0;
        else if (a == 2) memset(&grid[cy], 0, sizeof(grid[0]));
        else for (int c = cx; c < cols; c++) grid[cy][c] = 0;
        break;
    default:                            /* m (colors) and the rest: ignored */
        break;
    }
}

static void feed(uint8_t b)
{
    if (state == ESC) {
        state = b == '[' ? CSI : NORMAL;
        nparam = 0;
        params[0] = 0;
        return;
    }
    if (state == CSI) {
        if (b >= '0' && b <= '9') {
            if (nparam == 0) nparam = 1;
            params[nparam - 1] = params[nparam - 1] * 10 + (b - '0');
        } else if (b == ';') {
            if (nparam < 8) params[nparam++] = 0;
        } else if (b == '?') {
            /* a private marker, ignored */
        } else {
            csi((char)b);
            state = NORMAL;
        }
        return;
    }
    if (utf8_left) {                    /* continuation of a UTF-8 character */
        if ((b & 0xC0) == 0x80) {
            utf8_cp = (utf8_cp << 6) | (b & 0x3F);
            if (--utf8_left == 0)
                put(utf8_cp > 0xFFFF ? 0xFFFD : utf8_cp);   /* the font is BMP-only */
            return;
        }
        utf8_left = 0;                  /* malformed: fall through */
    }
    if (b == 0x1B) {
        state = ESC;
    } else if (b == '\n') {
        newline();
    } else if (b == '\r') {
        cx = 0;
    } else if (b == '\b') {
        if (cx > 0) cx--;
    } else if (b == '\t') {
        cx = (cx / 8 + 1) * 8;
        if (cx >= cols) cx = cols - 1;
    } else if (b == 0x7F) {
        if (cx > 0) grid[cy][--cx] = 0;
    } else if (b >= 0x20 && b < 0x7F) {
        put(b);
    } else if (b >= 0xC0) {             /* a UTF-8 lead byte */
        if ((b & 0xE0) == 0xC0) { utf8_cp = b & 0x1F; utf8_left = 1; }
        else if ((b & 0xF0) == 0xE0) { utf8_cp = b & 0x0F; utf8_left = 2; }
        else if ((b & 0xF8) == 0xF0) { utf8_cp = b & 0x07; utf8_left = 3; }
    }
}

/* --- drawing ----------------------------------------------------------------- */

/* Row r of the view: from the scrollback while scrolled back, else the grid. */
static const uint32_t *view_row(int r)
{
    int g = hist_count - back + r;              /* the line's place in history + screen */
    if (g < hist_count)
        return history[(hist_head - hist_count + g + HISTORY) % HISTORY];
    return grid[g - hist_count];
}

/* --- the selection (15x) ------------------------------------------------------ */

struct place { uint64_t line; int col; };      /* an absolute line number and a column */
static struct place sel_from, sel_to;          /* as the mouse made it (either order) */
static bool selecting, selected;               /* a drag going on; a selection shown */
static bool ctrl_held, shift_held;

/* The row of absolute line `line`, or 0 if it is no longer kept. */
static const uint32_t *line_row(uint64_t line)
{
    uint64_t first = scrolled - (uint64_t)hist_count;
    if (line < first || line >= scrolled + (uint64_t)rows)
        return 0;
    int g = (int)(line - first);
    return g < hist_count ? history[(hist_head - hist_count + g + HISTORY) % HISTORY]
                          : grid[g - hist_count];
}

/* The selection in order (start <= end). */
static void selection(struct place *a, struct place *b)
{
    bool forward = sel_from.line < sel_to.line ||
                   (sel_from.line == sel_to.line && sel_from.col <= sel_to.col);
    *a = forward ? sel_from : sel_to;
    *b = forward ? sel_to : sel_from;
}

/* Columns [c0, c1) of a row as UTF-8 into out; its length. */
static int encode(const uint32_t *row, int c0, int c1, char *out)
{
    int n = 0;
    for (int c = c0; c < c1; c++) {
        uint32_t cp = row[c] ? row[c] : ' ';
        if (cp < 0x80) {
            out[n++] = (char)cp;
        } else if (cp < 0x800) {
            out[n++] = (char)(0xC0 | cp >> 6);
            out[n++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[n++] = (char)(0xE0 | cp >> 12);
            out[n++] = (char)(0x80 | (cp >> 6 & 0x3F));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[n] = 0;
    return n;
}

static void render(void)
{
    window_fill(win, 0, 0, cols * cw, rows * ch, bg);
    struct place a, b;
    selection(&a, &b);
    for (int r = 0; r < rows; r++) {
        char line[MAXCOLS * 3 + 1];
        const uint32_t *row = view_row(r);
        encode(row, 0, cols, line);
        window_text_font(win, font, 0, r * ch, line, fg, WINDOW_TRANSPARENT);
        uint64_t abs = scrolled - (uint64_t)back + (uint64_t)r;
        if (selected && abs >= a.line && abs <= b.line) {  /* the selected part, inverted */
            int c0 = abs == a.line ? a.col : 0, c1 = abs == b.line ? b.col + 1 : cols;
            if (c1 > cols)
                c1 = cols;
            if (c0 < c1) {
                encode(row, c0, c1, line);
                window_fill(win, c0 * cw, r * ch, (c1 - c0) * cw, ch, fg);
                window_text_font(win, font, c0 * cw, r * ch, line, bg, WINDOW_TRANSPARENT);
            }
        }
    }
    if (!back)
        window_fill(win, cx * cw, cy * ch + ch - 2, cw, 2, fg);   /* the cursor */
    window_scrollbar(win, cols * cw, 0, rows * ch, hist_count - back, rows, hist_count + rows);
    window_flush(win);
}

/* The window was resized to nc x nr character cells: adopt the new buffer,
 * tell the pty, and redraw. Newly-exposed cells are cleared (so stale content
 * from an earlier, larger size cannot show). Holds the lock. */
static void resize_to(int nc, int nr)
{
    if (nc < 1) nc = 1;
    if (nc > MAXCOLS) nc = MAXCOLS;
    if (nr < 1) nr = 1;
    if (nr > MAXROWS) nr = MAXROWS;
    take();
    if ((nc != cols || nr != rows) &&
        window_resize(win, nc * cw + WINDOW_SCROLLBAR_W, nr * ch) == 0) {
        for (int r = 0; r < nr; r++)
            for (int c = 0; c < nc; c++)
                if (r >= rows || c >= cols)             /* a cell that was out of view */
                    grid[r][c] = 0;
        cols = nc;
        rows = nr;
        if (cx >= cols) cx = cols - 1;
        if (cy >= rows) cy = rows - 1;
        ritteros_pty_size(master, rows, cols);          /* the shell and programs see it */
        render();
    }
    give();
}

/* Scroll the view to n lines back (0-hist_count); a line in the log for the
 * tests when it moves. Holds the lock. */
static void scroll_back_to(int n)
{
    n = n < 0 ? 0 : n > hist_count ? hist_count : n;
    if (n == back)
        return;
    back = n;
    printf("term: scrolled back %d of %d lines\n", back, hist_count);
    render();
}

/* The place under a pointer at (x, y) in the window. */
static struct place place_at(int x, int y)
{
    struct place p;
    int r = y / ch, c = x / cw;
    r = r < 0 ? 0 : r >= rows ? rows - 1 : r;
    p.col = c < 0 ? 0 : c >= cols ? cols - 1 : c;
    p.line = scrolled - (uint64_t)back + (uint64_t)r;
    return p;
}

static bool blank(uint32_t cp)
{
    return cp == 0 || cp == ' ';
}

/* Copy the selection to the clipboard: its lines, trailing blanks dropped. */
static void copy_selection(void)
{
    static char text[CLIP_MAX + 1];
    size_t n = 0;
    take();
    struct place a, b;
    selection(&a, &b);
    for (uint64_t line = a.line; selected && line <= b.line; line++) {
        const uint32_t *row = line_row(line);
        if (!row)
            continue;
        int c0 = line == a.line ? a.col : 0, c1 = line == b.line ? b.col + 1 : cols;
        while (c1 > c0 && blank(row[c1 - 1]))
            c1--;
        char part[MAXCOLS * 3 + 1];
        int k = c1 > c0 ? encode(row, c0, c1, part) : 0;
        if (n + (size_t)k + 1 >= sizeof(text))
            break;
        memcpy(text + n, part, (size_t)k);
        n += (size_t)k;
        if (line < b.line)
            text[n++] = '\n';
    }
    give();
    if (n)
        window_clipboard_set(win, text, n);
}

/* Type the clipboard into the program: line breaks as Enter. */
static void paste(void)
{
    static char text[CLIP_MAX + 1];
    int n = window_clipboard_get(win, text, sizeof(text));
    for (int i = 0; i < n; i++)
        if (text[i] == '\n')
            text[i] = '\r';
    for (int done = 0; n > 0 && done < n;) {
        ssize_t w = write(master, text + done, (size_t)(n - done));
        if (w <= 0)
            break;
        done += (int)w;
    }
}

static void feed_str(const char *s)
{
    while (*s)
        feed((uint8_t)*s++);
}

static int reader(void *arg)
{
    (void)arg;
    uint8_t buf[2048];
    for (;;) {
        ssize_t n = read(master, buf, sizeof(buf));
        if (n <= 0) {                   /* the program on the pty ended */
            if (!running_command) {
                window_close(win);      /* the shell: close the window and leave the GUI */
                exit(0);
            }
            int status = 0;             /* a given command: keep the window to read its output */
            waitpid(child_pid, &status, 0);
            char line[96];
            snprintf(line, sizeof(line), "\r\n[%s exited (%d) - close this window]\r\n",
                     child_name, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            take();
            feed_str(line);
            render();
            give();
            return 0;                   /* stop reading; main keeps the window until it is closed */
        }
        take();
        back = 0;                       /* new output: the view goes back to the bottom */
        for (ssize_t i = 0; i < n; i++)
            feed(buf[i]);
        render();
        give();
    }
}

int main(int argc, char **argv)
{
    /* The program to run: the shell, or a given command (term CMD ARGS...). */
    char path[160];
    char *shell_argv[] = { "sh", 0 };
    char **child_argv;
    const char *title;
    int first = 1;                                  /* where the command's words start */
    font = wfont_regular();
    if (argc > 2 && strcmp(argv[1], "-f") == 0) {   /* -f FONT: a font of /system/fonts */
        const struct font *f = wfont_find(argv[2]);
        if (f) {
            font = f;
            snprintf(font_name, sizeof(font_name), "%s", argv[2]);
        } else {
            printf("term: no font %s in " WFONT_DIR "; using 9x15\n", argv[2]);
        }
        first = 3;
    }
    cw = wfont_width(font);
    ch = wfont_height(font);
    if (argc > first) {
        running_command = 1;
        child_argv = &argv[first];
        const char *slash = strrchr(argv[first], '/');
        snprintf(child_name, sizeof(child_name), "%s", slash ? slash + 1 : argv[first]);
        title = child_name;
        if (strchr(argv[first], '/')) {
            snprintf(path, sizeof(path), "%s", argv[first]);    /* a path: run it */
        } else {                                                /* a bare name: bin, then apps */
            struct stat st;
            snprintf(path, sizeof(path), "/system/" ARCH "/bin/%s", argv[first]);
            if (stat(path, &st) != 0)
                snprintf(path, sizeof(path), "/system/" ARCH "/apps/%s.program/%s", argv[first],
                         argv[first]);                         /* (42b) its bundle */
        }
    } else {
        child_argv = shell_argv;
        snprintf(child_name, sizeof(child_name), "sh");
        snprintf(path, sizeof(path), "/system/" ARCH "/bin/sh");
        title = "Terminal";
    }

    win = window_open(cols * cw + WINDOW_SCROLLBAR_W, rows * ch, title);   /* (text, its bar) */
    if (!win) {
        if (errno == EACCES)                    /* (another user's windowd, 15k) */
            printf("term: cannot open a window: the screen is another user's\n");
        else
            printf("term: cannot open a window (is windowd running?)\n");
        return 1;
    }
    uint32_t back = window_color(WINDOW_COLOR_BACKGROUND);      /* (41g-5) the user's theme */
    if ((back >> 16 & 0xFF) + (back >> 8 & 0xFF) + (back & 0xFF) > 3 * 128) {
        fg = window_rgb(win, 0x2E, 0x34, 0x40);
        bg = window_rgb(win, 0xEC, 0xEF, 0xF4);
    } else {
        fg = window_rgb(win, 220, 220, 220);
        bg = window_rgb(win, 0, 0, 0);
    }

    int fds[2];
    if (ritteros_pty_open(fds) < 0) {
        printf("term: no pseudo-terminal: %s\n", strerror(errno));
        return 1;
    }
    master = fds[0];
    ritteros_pty_size(master, rows, cols);
    int slave[3] = { fds[1], fds[1], fds[1] };
    int pid = ritteros_spawn(path, child_argv, SPAWN_NEW_GROUP | SPAWN_NEW_SESSION, slave);
    close(fds[1]);                      /* the child holds it now */
    if (pid < 0) {
        printf("term: cannot start %s: %s\n", child_name, strerror(errno));
        return 1;
    }
    child_pid = pid;
    render();                           /* a blank terminal to start */
    printf("term: started, font %s\n", font_name);
    if (thread_create(reader, 0) < 0) {
        printf("term: no reader thread\n");
        return 1;
    }
    struct window_event ev;
    unsigned prev = 0;
    while (window_wait(win, &ev)) {
        if (ev.type == WINDOW_KEY_DOWN || ev.type == WINDOW_KEY_UP) {
            bool down = ev.type == WINDOW_KEY_DOWN;
            if (ev.key == WKEY_LCTRL || ev.key == WKEY_RCTRL)
                ctrl_held = down;
            else if (ev.key == WKEY_LSHIFT || ev.key == WKEY_RSHIFT)
                shift_held = down;
        } else if (ev.type == WINDOW_KEY && ctrl_held && shift_held &&
                   (ev.key == 0x03 || ev.key == 0x16)) {
            if (ev.key == 0x03)         /* Ctrl+Shift+C: copy (not the interrupt) */
                copy_selection();
            else                        /* Ctrl+Shift+V: paste */
                paste();
        } else if (ev.type == WINDOW_DOUBLE_CLICK && ev.x < cols * cw) {
            take();                     /* a word: the non-blank run under the pointer */
            struct place at = place_at(ev.x, ev.y);
            const uint32_t *row = line_row(at.line);
            if (row && !blank(row[at.col])) {
                int c0 = at.col, c1 = at.col;
                while (c0 > 0 && !blank(row[c0 - 1]))
                    c0--;
                while (c1 + 1 < cols && !blank(row[c1 + 1]))
                    c1++;
                sel_from.line = sel_to.line = at.line;
                sel_from.col = c0, sel_to.col = c1;
                selected = true;
                selecting = false;
                render();
            }
            give();
        } else if (ev.type == WINDOW_KEY) {
            if (back) {                 /* typing: the view back at the bottom */
                take();
                back = 0;
                render();
                give();
            }
            uint8_t b = (uint8_t)ev.key;
            write(master, &b, 1);       /* the key goes to the shell */
        } else if (ev.type == WINDOW_RESIZE) {
            resize_to((ev.x - WINDOW_SCROLLBAR_W) / cw, ev.y / ch);  /* whole cells, and reflow */
        } else if (ev.type == WINDOW_SCROLL) {  /* the wheel: 3 lines a step, negative up */
            take();
            scroll_back_to(back - 3 * ev.y);
            give();
        } else if (ev.type == WINDOW_POINTER) { /* the scroll bar: a press jumps, a drag follows */
            bool press = (ev.buttons & 1) && !(prev & 1);
            prev = ev.buttons;
            if (press && ev.x >= cols * cw)
                dragging_bar = true;
            if (press && ev.x < cols * cw) {    /* (15x) a press in the text: a new selection */
                take();
                sel_from = sel_to = place_at(ev.x, ev.y);
                selecting = true;
                if (selected) {
                    selected = false;
                    render();
                }
                give();
            } else if (selecting && (ev.buttons & 1)) {
                take();
                struct place p = place_at(ev.x, ev.y);
                if (p.line != sel_to.line || p.col != sel_to.col || !selected) {
                    sel_to = p;
                    selected = sel_to.line != sel_from.line || sel_to.col != sel_from.col;
                    render();
                }
                give();
            } else if (selecting) {
                selecting = false;              /* (the release: the selection stays) */
            }
            if (dragging_bar && !(ev.buttons & 1))
                dragging_bar = false;
            if (dragging_bar) {
                take();
                scroll_back_to(hist_count -
                               window_scrollbar_hit(0, rows * ch, ev.y, rows, hist_count + rows));
                give();
            }
        }
    }
    window_close(win);
    return 0;
}

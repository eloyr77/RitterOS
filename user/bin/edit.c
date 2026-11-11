/*
 * edit FILE: a full-screen text editor. The text fills the screen, a bar
 * in reverse video below it shows the file's name, whether it changed and
 * where the cursor is, and the last line shows the keys or a message.
 *
 * Typing inserts at the cursor; the arrow keys, Home, End, Page Up and
 * Page Down move; Backspace and Delete erase; Enter breaks the line.
 * Ctrl-S saves, Ctrl-Q quits (twice if there are unsaved changes).
 *
 * A file whose first line ends in CR LF (Windows' line breaks, as the
 * PlutoSDR's config.txt) is edited without its CRs and saved with CR LF
 * again (50i); the bar says CRLF.
 *
 * The text is a gap buffer: one array with the text before the cursor at
 * its start, the text after it at its end, and the free space (the gap)
 * in between, so typing at the cursor moves nothing. Without malloc yet,
 * the array is fixed: files up to 1 MiB.
 *
 * The screen is redrawn after every key, but only rows that changed are
 * sent (the program keeps a copy of what each row shows), so a key costs
 * one row and the bars, not the whole screen.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("edit FILE", "edit a text file (Ctrl-S saves, Ctrl-Q quits)");

#define CAPACITY    (1024 * 1024)
#define TAB_WIDTH   8
#define MAX_ROWS    160             /* the console's limits (kernel/console.c) */
#define MAX_COLS    480
#define ROW_BYTES   (MAX_COLS * 4 + 32)     /* a row of UTF-8 and its escapes */

static char text[CAPACITY];
static size_t gap_start, gap_end = CAPACITY;    /* the gap: text[gap_start..gap_end) */

static const char *path;
static bool modified, quit_asked;
static bool crlf;                       /* (50i) the file's line breaks are CR LF */
static int rows, cols, text_rows;       /* the screen; text_rows above the two bars */
static size_t top;                      /* the first line shown (its offset) */
static int left;                        /* the first column shown */
static size_t cursor;                   /* an offset into the text */
static int wanted_col = -1;             /* up/down keep this column (-1: the cursor's) */
static char message[128];

/* --- the text --------------------------------------------------------------- */

static size_t length(void)
{
    return gap_start + (CAPACITY - gap_end);
}

static char at(size_t i)
{
    return i < gap_start ? text[i] : text[i + (gap_end - gap_start)];
}

/* Move the gap to offset pos. */
static void move_gap(size_t pos)
{
    if (pos < gap_start) {
        size_t n = gap_start - pos;
        memmove(text + gap_end - n, text + pos, n);
        gap_start -= n, gap_end -= n;
    } else if (pos > gap_start) {
        size_t n = pos - gap_start;
        memmove(text + gap_start, text + gap_end, n);
        gap_start += n, gap_end += n;
    }
}

static bool insert(const char *s, size_t n)
{
    if (gap_end - gap_start < n) {
        snprintf(message, sizeof(message), "The text is full (at most 1 MiB).");
        return false;
    }
    move_gap(cursor);
    memcpy(text + gap_start, s, n);
    gap_start += n;
    cursor += n;
    modified = true;
    return true;
}

/* Erase n bytes at offset pos. */
static void erase(size_t pos, size_t n)
{
    move_gap(pos);
    gap_end += n;
    modified = true;
}

/* A byte that continues a UTF-8 character (the cursor never stops there). */
static bool continuation(size_t i)
{
    return i < length() && (at(i) & 0xC0) == 0x80;
}

static size_t next_char(size_t i)
{
    if (i < length())
        i++;
    while (continuation(i))
        i++;
    return i;
}

static size_t previous_char(size_t i)
{
    if (i > 0)
        i--;
    while (i > 0 && continuation(i))
        i--;
    return i;
}

static size_t line_start(size_t i)
{
    while (i > 0 && at(i - 1) != '\n')
        i--;
    return i;
}

static size_t line_end(size_t i)
{
    while (i < length() && at(i) != '\n')
        i++;
    return i;
}

/* The column at which offset i of its line is shown (tabs widen). */
static int column_of(size_t i)
{
    int col = 0;
    for (size_t p = line_start(i); p < i; p = next_char(p))
        col = at(p) == '\t' ? (col / TAB_WIDTH + 1) * TAB_WIDTH : col + 1;
    return col;
}

/* The offset in the line starting at start that is shown at column col,
 * or the line's end. */
static size_t offset_at_column(size_t start, int col)
{
    size_t p = start, end = line_end(start);
    int c = 0;
    while (p < end) {
        int next = at(p) == '\t' ? (c / TAB_WIDTH + 1) * TAB_WIDTH : c + 1;
        if (next > col)
            break;
        c = next;
        p = next_char(p);
    }
    return p;
}

static size_t line_number(size_t i)    /* 1 for the first */
{
    size_t n = 1;
    for (size_t p = 0; p < i; p++)
        n += at(p) == '\n';
    return n;
}

/* --- files ------------------------------------------------------------------ */

/* Read the file; false (and a message on stderr) if it cannot be edited. */
static bool load(void)
{
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        dprintf(STDERR_FILENO, "edit: %s: %s\n", path, strerror(EISDIR));
        return false;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            snprintf(message, sizeof(message), "New file.");
            return true;
        }
        dprintf(STDERR_FILENO, "edit: %s: %s\n", path, strerror(errno));
        return false;
    }
    ssize_t n;
    while ((n = read(fd, text + gap_start, gap_end - gap_start)) > 0)
        gap_start += (size_t)n;
    close(fd);
    if (n < 0) {
        dprintf(STDERR_FILENO, "edit: %s: %s\n", path, strerror(errno));
        return false;
    }
    if (gap_start == gap_end) {
        dprintf(STDERR_FILENO, "edit: %s: too large (at most 1 MiB)\n", path);
        return false;
    }
    size_t bytes = gap_start;
    /* CR LF line breaks (the first line says): the CRs go while editing. */
    char *first = memchr(text, '\n', gap_start);
    crlf = first && first > text && first[-1] == '\r';
    if (crlf) {
        size_t kept = 0;
        for (size_t i = 0; i < gap_start; i++)
            if (!(text[i] == '\r' && i + 1 < gap_start && text[i + 1] == '\n'))
                text[kept++] = text[i];
        gap_start = kept;
    }
    size_t lines = line_number(gap_start) - (gap_start && text[gap_start - 1] == '\n');
    snprintf(message, sizeof(message), "%zu lines, %zu bytes%s.", lines, bytes,
             crlf ? ", CR LF line breaks" : "");
    move_gap(0);
    return true;
}

static bool write_all(int fd, const char *s, size_t n)
{
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w <= 0)
            return false;
        s += w, n -= (size_t)w;
    }
    return true;
}

/* n bytes of the text to fd, each line break as CR LF when the file has
 * them (50i); *written counts the bytes in the file. */
static bool write_text(int fd, const char *s, size_t n, size_t *written)
{
    if (!crlf) {
        *written += n;
        return write_all(fd, s, n);
    }
    static char chunk[8192];
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n')
            chunk[used++] = '\r';
        chunk[used++] = s[i];
        if (used >= sizeof(chunk) - 2) {
            if (!write_all(fd, chunk, used))
                return false;
            *written += used;
            used = 0;
        }
    }
    *written += used;
    return write_all(fd, chunk, used);
}

/* Save; a text file ends with a line break, so one is added if missing. */
static void save(void)
{
    size_t keep = cursor;
    if (length() && at(length() - 1) != '\n') {
        cursor = length();
        insert("\n", 1);
        cursor = keep;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(message, sizeof(message), "Not saved: %s.", strerror(errno));
        return;
    }
    size_t written = 0;
    bool ok = write_text(fd, text, gap_start, &written) &&
              write_text(fd, text + gap_end, CAPACITY - gap_end, &written);
    int error = errno;
    if (close(fd) < 0 && ok) {                      /* the commit to the disk */
        ok = false;
        error = errno;
    }
    if (!ok) {
        snprintf(message, sizeof(message), "Not saved: %s.", strerror(error));
        return;
    }
    modified = false;
    size_t lines = line_number(length()) - (length() && at(length() - 1) == '\n');
    snprintf(message, sizeof(message), "Saved: %zu lines, %zu bytes%s.", lines, written,
             crlf ? ", CR LF line breaks" : "");
}

/* --- the screen ------------------------------------------------------------- */

static char screen[MAX_ROWS][ROW_BYTES];        /* what each row shows now */
static char out[MAX_ROWS * (ROW_BYTES + 16) + 64];
static size_t out_used;

static void emit(const char *s, size_t n)
{
    if (out_used + n > sizeof(out))
        n = sizeof(out) - out_used;
    memcpy(out + out_used, s, n);
    out_used += n;
}

static void emits(const char *s)
{
    emit(s, strlen(s));
}

/* Send row r (width columns wide) if it differs from what the screen
 * shows. The rest of a shorter row is erased; a full row is not, because
 * a terminal's cursor stays on the last column, and ESC [ K would erase
 * that character. */
static void put_row(int r, const char *row, int width)
{
    if (strcmp(screen[r], row) == 0)
        return;
    snprintf(screen[r], ROW_BYTES, "%s", row);
    char go[24];
    snprintf(go, sizeof(go), "\033[%d;1H", r + 1);
    emits(go);
    emits(row);
    emits("\033[0m");
    if (width < cols)
        emits("\033[K");
}

/* The text line starting at offset start, as shown from column left;
 * returns the columns it takes. */
static int render_line(size_t start, char *row)
{
    size_t used = 0, end = line_end(start);
    int col = 0;
    for (size_t p = start; p < end && col < left + cols; p = next_char(p)) {
        char c = at(p);
        if (c == '\t') {
            int to = (col / TAB_WIDTH + 1) * TAB_WIDTH;
            for (; col < to; col++)
                if (col >= left && col < left + cols)
                    row[used++] = ' ';
            continue;
        }
        if (col >= left) {
            if ((unsigned char)c < 0x20 || c == 0x7F) {
                row[used++] = '?';                  /* a control character */
            } else {
                for (size_t q = p; q < next_char(p); q++)
                    row[used++] = at(q);
            }
        }
        col++;
    }
    row[used] = 0;
    return col > left ? col - left : 0;
}

/* Scroll so that the cursor is on the screen. */
static void follow_cursor(void)
{
    size_t here = line_start(cursor);
    if (here < top)
        top = here;
    size_t lines = 0;
    for (size_t p = top; p < here; p++)
        lines += at(p) == '\n';
    while (lines >= (size_t)text_rows) {
        top = line_end(top) + 1;
        lines--;
    }
    int col = column_of(cursor);
    if (col < left)
        left = col;
    if (col >= left + cols)
        left = col - cols + 1;
}

static void redraw(void)
{
    static char row[ROW_BYTES];
    follow_cursor();
    out_used = 0;
    emits("\033[?25l");                             /* no cursor while drawing */
    size_t p = top;
    bool more = true;
    for (int r = 0; r < text_rows; r++) {
        int width = 0;
        row[0] = 0;                                 /* below the end of the text */
        if (more) {
            width = render_line(p, row);
            size_t end = line_end(p);
            more = end < length();
            p = end + 1;
        }
        put_row(r, row, width);
    }

    /* The bar: name and state on the left, the cursor's place on the right. */
    char left_part[192], right_part[64];
    snprintf(left_part, sizeof(left_part), " edit: %s%s%s", path, crlf ? "  CRLF" : "",
             modified ? "  (modified)" : "");
    snprintf(right_part, sizeof(right_part), "line %zu/%zu, column %d ", line_number(cursor),
             line_number(length()), column_of(cursor) + 1);
    int ll = (int)strlen(left_part), rl = (int)strlen(right_part);
    int width = cols;
    if (ll + rl > width)
        ll = width - rl > 0 ? width - rl : 0;
    int used = snprintf(row, sizeof(row), "\033[7m%.*s", ll, left_part);
    for (int i = ll; i < width - rl; i++)
        row[used++] = ' ';
    snprintf(row + used, sizeof(row) - used, "%.*s", width - ll < rl ? width - ll : rl, right_part);
    put_row(text_rows, row, cols);

    /* The last line: a message, or the keys (one column short of the
     * corner, so no terminal scrolls). */
    int n = snprintf(row, sizeof(row), "%.*s", cols - 1,
                     message[0] ? message : "Ctrl-S save   Ctrl-Q quit");
    put_row(text_rows + 1, row, n);

    char go[48];
    snprintf(go, sizeof(go), "\033[%d;%dH\033[?25h",
             (int)(line_number(cursor) - line_number(top)) + 1, column_of(cursor) - left + 1);
    emits(go);
    write(STDOUT_FILENO, out, out_used);
}

/* --- keys ------------------------------------------------------------------- */

enum { KEY_UP = 256, KEY_DOWN, KEY_RIGHT, KEY_LEFT, KEY_HOME, KEY_END, KEY_DELETE,
       KEY_PAGE_UP, KEY_PAGE_DOWN };

#define CTRL(c) ((c) & 0x1F)

/* Up or down by n lines, keeping the wanted column. */
static void move_lines(int n)
{
    if (wanted_col < 0)
        wanted_col = column_of(cursor);
    size_t start = line_start(cursor);
    for (; n < 0 && start > 0; n++)
        start = line_start(start - 1);
    for (; n > 0; n--) {
        size_t end = line_end(start);
        if (end >= length())
            break;
        start = end + 1;
    }
    cursor = offset_at_column(start, wanted_col);
}

/* Page Up and Page Down: the view moves by a page, and the cursor with it. */
static void move_page(int n)
{
    for (int i = 0; i < text_rows; i++) {
        if (n < 0 && top > 0)
            top = line_start(top - 1);
        else if (n > 0 && line_end(top) < length())
            top = line_end(top) + 1;
    }
    move_lines(n * text_rows);
}

/* Returns false when the editor should end. */
static bool key(int k)
{
    bool vertical = k == KEY_UP || k == KEY_DOWN || k == KEY_PAGE_UP || k == KEY_PAGE_DOWN;
    if (!vertical)
        wanted_col = -1;
    if (k != CTRL('Q'))
        quit_asked = false;
    message[0] = 0;
    switch (k) {
    case CTRL('Q'):
        if (modified && !quit_asked) {
            quit_asked = true;
            snprintf(message, sizeof(message),
                     "Unsaved changes: Ctrl-Q again quits without saving, Ctrl-S saves.");
            return true;
        }
        return false;
    case CTRL('S'):
        save();
        break;
    case CTRL('C'):
        snprintf(message, sizeof(message), "Ctrl-Q quits.");
        break;
    case KEY_LEFT:
        cursor = previous_char(cursor);
        break;
    case KEY_RIGHT:
        cursor = next_char(cursor);
        break;
    case KEY_UP:
        move_lines(-1);
        break;
    case KEY_DOWN:
        move_lines(1);
        break;
    case KEY_PAGE_UP:
        move_page(-1);
        break;
    case KEY_PAGE_DOWN:
        move_page(1);
        break;
    case KEY_HOME:
        cursor = line_start(cursor);
        break;
    case KEY_END:
        cursor = line_end(cursor);
        break;
    case 0x7F:
    case '\b':
        if (cursor > 0) {
            size_t from = previous_char(cursor);
            erase(from, cursor - from);
            cursor = from;
        }
        break;
    case KEY_DELETE:
        if (cursor < length())
            erase(cursor, next_char(cursor) - cursor);
        break;
    case '\r':
    case '\n':
        insert("\n", 1);
        break;
    default:
        if (k == '\t' || (k >= 0x20 && k < 0x7F) || (k >= 0x80 && k < 0x100)) {
            char c = (char)k;
            insert(&c, 1);
        }
        break;
    }
    return true;
}

/*
 * Bytes from the terminal to keys: the VT100 sequences of the arrow keys
 * and friends (ESC [ A, ESC [ 3 ~, ESC O H, ...) become KEY_*; a sequence
 * may arrive split between reads. CR LF counts as one Enter.
 */
static bool feed(unsigned char b)
{
    static enum { PLAIN, ESCAPE, CSI } state;
    static int number;
    static unsigned char previous;
    unsigned char before = previous;
    previous = b;
    if (state == ESCAPE) {
        if (b == '[' || b == 'O') {
            state = CSI;
            number = 0;
            return true;
        }
        state = PLAIN;                              /* ESC alone: ignored */
    } else if (state == CSI) {
        if (b >= '0' && b <= '9') {
            number = number * 10 + (b - '0');
            return true;
        }
        if (b == ';')
            return true;
        state = PLAIN;
        switch (b) {
        case 'A': return key(KEY_UP);
        case 'B': return key(KEY_DOWN);
        case 'C': return key(KEY_RIGHT);
        case 'D': return key(KEY_LEFT);
        case 'H': return key(KEY_HOME);
        case 'F': return key(KEY_END);
        case '~':
            switch (number) {
            case 1: case 7: return key(KEY_HOME);
            case 4: case 8: return key(KEY_END);
            case 3: return key(KEY_DELETE);
            case 5: return key(KEY_PAGE_UP);
            case 6: return key(KEY_PAGE_DOWN);
            }
            return true;
        }
        return true;                                /* unknown: ignored */
    }
    if (b == 0x1B) {
        state = ESCAPE;
        return true;
    }
    if (b == '\n' && before == '\r')
        return true;
    return key(b);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        dprintf(STDERR_FILENO, "usage: edit FILE\n");
        return 2;
    }
    path = argv[1];
    if (!load())
        return 1;
    ritteros_terminal_size(&rows, &cols);
    if (rows > MAX_ROWS)
        rows = MAX_ROWS;
    if (cols > MAX_COLS)
        cols = MAX_COLS;
    text_rows = rows - 2;
    if (text_rows < 1 || cols < 20) {
        dprintf(STDERR_FILENO, "edit: the screen is too small\n");
        return 1;
    }
    if (ritteros_terminal_raw(true) < 0) {
        dprintf(STDERR_FILENO, "edit: the terminal: %s\n", strerror(errno));
        return 1;
    }
    signal(SIGINT, SIG_IGN);                        /* Ctrl-C is a key here */
    write(STDOUT_FILENO, "\033[0m\033[2J", 8);
    for (int r = 0; r < MAX_ROWS; r++)
        screen[r][0] = 0;                           /* the screen is empty now */

    bool running = true;
    while (running) {
        redraw();
        unsigned char bytes[64];
        ssize_t n = read(STDIN_FILENO, bytes, sizeof(bytes));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        for (ssize_t i = 0; i < n && running; i++)
            running = feed(bytes[i]);
    }
    write(STDOUT_FILENO, "\033[0m\033[2J\033[H", 11);
    ritteros_terminal_raw(false);
    return 0;
}

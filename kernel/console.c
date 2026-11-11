#include "blank.h"
#include "gpu.h"
#include "console.h"
#include "kprintf.h"
#include "arch.h"
#include "font.h"
#include "heap.h"
#include "sched.h"
#include "timer.h"
#include "string.h"

#define TITLE_HEIGHT    40
#define STATUS_HEIGHT   16
#define TEXT_TOP        (TITLE_HEIGHT + 8)
#define TEXT_LEFT       12
#define MAX_COLS        480     /* 4K screens: 3840 / 9 = 426 columns */
#define MAX_ROWS        160     /* 2160 / 15 = 144 rows */
#define TAB_WIDTH       8

struct cell {
    uint32_t codepoint;
    uint32_t fg, bg;       /* 0xRRGGBB */
    uint8_t bold;
};

static const struct framebuffer *screen;

/*
 * Once the heap exists, drawing goes into a copy of the screen in RAM (the
 * canvas), and only what changed is copied to the screen. Scrolling then
 * moves pixels within RAM: video memory is fast to write but very slow to
 * read, and redrawing every character for each new line took most of the
 * x86 boot (2.6 s of 3.9 s in QEMU).
 */
static struct framebuffer canvas;
static const struct framebuffer *target;    /* &canvas, or the screen itself */
static const struct font *const regular = &font_regular;
static bool shown;
static uint32_t cols, rows;
static uint32_t cursor_col, cursor_row;
static uint32_t current_fg = COLOR_TEXT;
static bool current_bold, current_reverse;
static uint32_t status_color = STATUS_RUNNING;
/* Character cells, row by row; a row is `cols` cells long (the screen's
 * width, not MAX_COLS), so scrolling moves only cells in use. */
static struct cell cells[MAX_ROWS * MAX_COLS];

static struct cell *cell_at(uint32_t row, uint32_t col)
{
    return &cells[row * cols + col];
}

/*
 * Batched screen updates (console_start_updates): drawing marks the rows of
 * the canvas it changed (dirty_top .. dirty_bottom), and they are copied to
 * the screen at most every UPDATE_MS; the "console" thread copies what is
 * still waiting after that. A burst of lines (the boot log) then costs a
 * few screen copies instead of one per line (5 ms each on a J1900 at
 * 1600x1200). Before the scheduler runs, and after a crash, every change
 * is copied at once.
 */
#define UPDATE_MS 20
static bool batching;

/* While a program owns the screen (SYS_DISPLAY), the console still draws into
 * its canvas but does not touch the screen; release redraws everything. */
static bool suspended;

/* (37e) The screen is off (kernel/blank.c): black, nothing copied to it. */
static bool blanked;
static bool sleeping;           /* (37g-5d) a sleep: the screen's device is not there */

/*
 * On the canvas, the text rows form a ring: scrolling advances top_row (the
 * canvas text row shown at the top) and clears one row, instead of moving
 * all the pixels; copies to the screen take the rows in screen order. A
 * character never straddles the wrap: rows are whole character heights.
 */
static uint32_t top_row;
static uint32_t dirty_top, dirty_bottom;    /* pixel rows; empty if equal */
static uint64_t last_update;                /* arch_counter() of the last copy */

/* Measured with arch_counter: what drawing costs (console_report). */
static uint64_t lines_scrolled, draw_counts, scroll_counts, updates, update_counts;

/* (66b-3) Writes to the screen, by where they land: within the firmware's
 * framebuffer (its first `fast_bytes`, mapped write-combining at the boot)
 * or beyond it (a larger mode set later: the direct map's ordinary
 * mapping there). Bytes and counter ticks; console_screen_writes. */
static uint64_t fast_bytes, writes[2], write_ticks[2];

/* Copy one row of pixels to the screen at (x, y), counted. */
static void screen_row(uint32_t x, uint32_t y, const void *pixels, uint32_t w)
{
    uint64_t at = ((uint64_t)y * screen->stride + x) * 4, start = arch_counter();
    memcpy((void *)(screen->base + (uint64_t)y * screen->stride + x), pixels, (uint64_t)w * 4);
    int beyond = at + (uint64_t)w * 4 > fast_bytes;
    write_ticks[beyond] += arch_counter() - start;
    writes[beyond] += (uint64_t)w * 4;
}

static uint32_t native(uint32_t rgb)
{
    return fb_rgb(screen, (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
}

/* Copy a rectangle of the canvas to the screen (writes only). */
static void copy_to_screen(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (suspended || blanked || sleeping)
        return;                 /* a program owns the screen, it is off, or asleep: the console waits */
    if (target != &canvas || x >= screen->width || y >= screen->height)
        return;
    if (w > screen->width - x)
        w = screen->width - x;
    if (h > screen->height - y)
        h = screen->height - y;
    uint32_t text_height = rows * regular->height;
    for (uint32_t row = y; row < y + h; row++) {
        uint32_t from = row;
        if (row >= TEXT_TOP && row < TEXT_TOP + text_height)
            from = TEXT_TOP + (row - TEXT_TOP + top_row * regular->height) % text_height;
        screen_row(x, row, (const void *)(canvas.base + (uint64_t)from * canvas.stride + x), w);
    }
}

/* Where text row `row` (0 = top of the screen) is drawn: on the canvas,
 * its place in the ring. */
static uint32_t row_y(uint32_t row)
{
    if (target == &canvas)
        row = (row + top_row) % rows;
    return TEXT_TOP + row * regular->height;
}

/* Copy the waiting rows to the screen. */
static void update_screen(void)
{
    if (dirty_top == dirty_bottom)
        return;
    uint64_t start = arch_counter();
    copy_to_screen(0, dirty_top, screen->width, dirty_bottom - dirty_top);
    dirty_top = dirty_bottom = 0;
    last_update = arch_counter();
    update_counts += last_update - start;
    updates++;
}

/* A rectangle of the canvas changed: copy it now, or mark its rows. */
static void flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!batching) {
        copy_to_screen(x, y, w, h);
        return;
    }
    if (dirty_top == dirty_bottom) {
        dirty_top = y;
        dirty_bottom = y + h;
    } else {
        dirty_top = y < dirty_top ? y : dirty_top;
        dirty_bottom = y + h > dirty_bottom ? y + h : dirty_bottom;
    }
}

/* After a change: copy now if the last copy is UPDATE_MS old. */
static void maybe_update(void)
{
    if (batching && timer_elapsed_ms(last_update) >= UPDATE_MS)
        update_screen();
}

static void draw_cell_colors(uint32_t row, uint32_t col, uint32_t fg, uint32_t bg)
{
    const struct cell *c = cell_at(row, col);
    uint32_t x = TEXT_LEFT + col * regular->width;
    fb_draw_char(target, c->bold ? &font_bold : regular, x, row_y(row), c->codepoint,
                 native(fg), native(bg));
    flush(x, TEXT_TOP + row * regular->height, regular->width, regular->height);
}

static void draw_cell(uint32_t row, uint32_t col)
{
    const struct cell *c = cell_at(row, col);
    draw_cell_colors(row, col, c->fg, c->bg);
}

static void draw_text_area(void)
{
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t c = 0; c < cols; c++) {
            const struct cell *cell = cell_at(r, c);
            fb_draw_char(target, cell->bold ? &font_bold : regular,
                         TEXT_LEFT + c * regular->width, row_y(r), cell->codepoint,
                         native(cell->fg), native(cell->bg));
        }
    flush(0, TEXT_TOP, screen->width, rows * regular->height);
}

/*
 * The cursor: the cell at the cursor drawn with its colors swapped. It is
 * taken away before a write changes anything and drawn again afterwards,
 * so drawing and scrolling never see it. A cursor past the last column
 * (the next character wraps) is shown on the last one.
 */
static bool cursor_enabled = true;              /* ESC [ ? 25 h / l */
static bool cursor_drawn;
static uint32_t cursor_drawn_row, cursor_drawn_col;

static void cursor_hide(void)
{
    if (!cursor_drawn)
        return;
    cursor_drawn = false;
    draw_cell(cursor_drawn_row, cursor_drawn_col);
}

static void cursor_show(void)
{
    if (!cursor_enabled || !shown || cursor_drawn)
        return;
    uint32_t col = cursor_col < cols ? cursor_col : cols - 1;
    const struct cell *c = cell_at(cursor_row, col);
    draw_cell_colors(cursor_row, col, c->bg, c->fg);
    cursor_drawn = true;
    cursor_drawn_row = cursor_row;
    cursor_drawn_col = col;
}

static void draw_title_bar(void)
{
    uint32_t bar = native(0x1E223C);
    fb_fill_rect(target, 0, 0, screen->width, TITLE_HEIGHT, bar);
    uint32_t x = fb_draw_text(target, &font_bold, 16, 12, "RitterOS", native(0xFFFFFF), bar);
    fb_draw_text(target, regular, x, 12, " kernel · console", native(0xAABEE6), bar);
    flush(0, 0, screen->width, TITLE_HEIGHT);
}

static void draw_status(void)
{
    fb_fill_rect(target, 0, screen->height - STATUS_HEIGHT, screen->width, STATUS_HEIGHT,
                 native(status_color));
    flush(0, screen->height - STATUS_HEIGHT, screen->width, STATUS_HEIGHT);
}

static void clear_row(uint32_t row)
{
    for (uint32_t c = 0; c < cols; c++)
        *cell_at(row, c) = (struct cell){ ' ', COLOR_TEXT, COLOR_BACKGROUND, 0 };
}

/* Erase cells from..to-1 of a row, on the screen too (one rectangle). */
static void erase(uint32_t row, uint32_t from, uint32_t to)
{
    if (to > cols)
        to = cols;
    if (from >= to)
        return;
    for (uint32_t c = from; c < to; c++)
        *cell_at(row, c) = (struct cell){ ' ', COLOR_TEXT, COLOR_BACKGROUND, 0 };
    if (!shown)
        return;
    uint32_t x = TEXT_LEFT + from * regular->width, w = (to - from) * regular->width;
    fb_fill_rect(target, x, row_y(row), w, regular->height, native(COLOR_BACKGROUND));
    flush(x, TEXT_TOP + row * regular->height, w, regular->height);
}

void console_init(const struct framebuffer *fb)
{
    screen = fb;
    fast_bytes = (uint64_t)fb->stride * fb->height * 4;
    target = fb;
    cols = (fb->width - 2 * TEXT_LEFT) / regular->width;
    rows = (fb->height - TEXT_TOP - STATUS_HEIGHT - 4) / regular->height;
    if (cols > MAX_COLS)
        cols = MAX_COLS;
    if (rows > MAX_ROWS)
        rows = MAX_ROWS;
    for (uint32_t r = 0; r < rows; r++)
        clear_row(r);
    cursor_col = cursor_row = 0;
    shown = false;
}

void console_show(void)
{
    if (!screen)
        return;
    shown = true;
    fb_fill_rect(target, 0, 0, screen->width, screen->height, native(COLOR_BACKGROUND));
    draw_title_bar();
    draw_text_area();
    draw_status();
    cursor_drawn = false;
    cursor_show();
    dirty_top = dirty_bottom = 0;
    copy_to_screen(0, 0, screen->width, screen->height);
}

bool console_use_buffer(void)
{
    if (!screen || target == &canvas)
        return target == &canvas;
    uint64_t bytes = (uint64_t)screen->stride * screen->height * 4;
    void *copy = kmalloc(bytes);
    if (!copy)
        return false;
    canvas = *screen;
    canvas.base = copy;
    target = &canvas;
    top_row = 0;
    if (shown)
        console_show();                     /* the canvas starts as the screen is */
    return true;
}

bool console_is_shown(void)
{
    return shown;
}

void console_set_color(uint32_t fg)
{
    current_fg = fg;
}

void console_set_bold(bool bold)
{
    current_bold = bold;
}

void console_set_status(uint32_t color)
{
    status_color = color;
    if (shown) {
        draw_status();
        maybe_update();
    }
}

static void newline(void)
{
    cursor_col = 0;
    if (cursor_row + 1 < rows) {
        cursor_row++;
        return;
    }
    /* Scroll: move every row up by one in memory. On the canvas, the ring
     * turns by a row and only the new row is cleared; without one, the
     * whole text area is redrawn. Then the screen gets the text area. */
    memmove(cell_at(0, 0), cell_at(1, 0), sizeof(struct cell) * cols * (rows - 1));
    clear_row(rows - 1);
    if (shown) {
        uint64_t start = arch_counter();
        if (target == &canvas) {
            uint32_t fh = regular->height;
            top_row = (top_row + 1) % rows;
            fb_fill_rect(&canvas, 0, row_y(rows - 1), screen->width, fh,
                         native(COLOR_BACKGROUND));
            flush(0, TEXT_TOP, screen->width, rows * fh);
        } else {
            draw_text_area();
        }
        scroll_counts += arch_counter() - start;
        lines_scrolled++;
    }
}

/*
 * VT100 escape sequences, the few that full-screen programs need (listed
 * with SYS_TERMINAL in common/syscalls.h): ESC '[', numbers separated by
 * ';' (an optional '?' first), then a final byte from '@' to '~'. Others
 * are taken in and ignored, so they never show as text.
 */
#define MAX_PARAMS 4
static enum { PLAIN, ESCAPE, CSI } escape_state;
static uint32_t params[MAX_PARAMS];
static uint32_t param_count;
static bool private_mode;

/* SGR colors 30-37 and 90-97 (bright), readable on the dark background. */
static const uint32_t palette[16] = {
    0x404040, COLOR_ERROR, COLOR_OK, COLOR_WARNING, 0x6080FF, 0xD080FF, COLOR_HEADING, COLOR_TEXT,
    0x808080, 0xFF8080, 0x80FF80, 0xFFFF80, 0x80A0FF, 0xFF80FF, 0x80FFFF, 0xFFFFFF,
};

static void set_rendition(uint32_t p)
{
    if (p == 0) {
        current_fg = COLOR_TEXT;
        current_bold = current_reverse = false;
    } else if (p == 1) {
        current_bold = true;
    } else if (p == 22) {
        current_bold = false;
    } else if (p == 7) {
        current_reverse = true;
    } else if (p == 27) {
        current_reverse = false;
    } else if (p >= 30 && p <= 37) {
        current_fg = palette[p - 30];
    } else if (p >= 90 && p <= 97) {
        current_fg = palette[p - 90 + 8];
    } else if (p == 39) {
        current_fg = COLOR_TEXT;
    }
}

static void run_sequence(uint32_t final)
{
    uint32_t p0 = param_count ? params[0] : 0;
    uint32_t n = p0 ? p0 : 1;                       /* a count: 0 or none means 1 */
    if (cursor_col >= cols && final >= 'A' && final <= 'D')
        cursor_col = cols - 1;                      /* a move ends a pending wrap */
    switch (final) {
    case 'H':
    case 'f':
        cursor_row = p0 ? p0 - 1 : 0;
        cursor_col = param_count > 1 && params[1] ? params[1] - 1 : 0;
        if (cursor_row >= rows)
            cursor_row = rows - 1;
        if (cursor_col >= cols)
            cursor_col = cols - 1;
        break;
    case 'A':
        cursor_row = cursor_row > n ? cursor_row - n : 0;
        break;
    case 'B':
        cursor_row = cursor_row + n < rows ? cursor_row + n : rows - 1;
        break;
    case 'C':
        cursor_col = cursor_col + n < cols ? cursor_col + n : cols - 1;
        break;
    case 'D':
        cursor_col = cursor_col > n ? cursor_col - n : 0;
        break;
    case 'J':
        if (p0 == 0) {
            erase(cursor_row, cursor_col, cols);
            for (uint32_t r = cursor_row + 1; r < rows; r++)
                erase(r, 0, cols);
        } else if (p0 == 2) {
            for (uint32_t r = 0; r < rows; r++)
                erase(r, 0, cols);
        }
        break;
    case 'K':
        if (p0 == 0)
            erase(cursor_row, cursor_col, cols);
        else if (p0 == 2)
            erase(cursor_row, 0, cols);
        break;
    case 'm':
        if (param_count == 0)
            set_rendition(0);
        for (uint32_t i = 0; i < param_count; i++)
            set_rendition(params[i]);
        break;
    case 'h':
    case 'l':
        if (private_mode && p0 == 25)
            cursor_enabled = final == 'h';
        break;
    }
}

/* A character while a sequence is open. */
static void escape_char(uint32_t c)
{
    if (escape_state == ESCAPE) {
        escape_state = c == '[' ? CSI : PLAIN;      /* ESC + anything else: ignored */
        memset(params, 0, sizeof(params));
        param_count = 0;
        private_mode = false;
        return;
    }
    if (c >= '0' && c <= '9') {
        if (param_count == 0)
            param_count = 1;
        uint32_t *v = &params[param_count - 1];
        if (*v < 100000)
            *v = *v * 10 + (c - '0');
    } else if (c == ';') {
        if (param_count == 0)
            param_count = 1;
        if (param_count < MAX_PARAMS)
            param_count++;
    } else if (c == '?') {
        private_mode = true;
    } else if (c >= 0x40 && c <= 0x7E) {
        escape_state = PLAIN;
        run_sequence(c);
    } else if (c < 0x20 || c > 0x7E) {
        escape_state = PLAIN;                       /* not a sequence after all */
    }
}

void console_reset_style(void)
{
    current_fg = COLOR_TEXT;
    current_bold = current_reverse = false;
    escape_state = PLAIN;
    if (!cursor_enabled) {
        cursor_enabled = true;
        cursor_show();
        maybe_update();
    }
}

bool console_size(uint32_t *rows_out, uint32_t *cols_out)
{
    if (!screen)
        return false;
    *rows_out = rows;
    *cols_out = cols;
    return true;
}

bool console_screen_size(uint32_t *width, uint32_t *height)
{
    if (!screen)
        return false;
    *width = screen->width;
    *height = screen->height;
    return true;
}

static void put_char(uint32_t c)
{
    if (escape_state != PLAIN) {
        escape_char(c);
        return;
    }
    if (c == 0x1B) {
        escape_state = ESCAPE;
        return;
    }
    if (c == '\n') {
        newline();
        return;
    }
    if (c == '\r') {
        cursor_col = 0;
        return;
    }
    if (c == '\b') {                /* back one column, as on a terminal */
        if (cursor_col > 0)
            cursor_col--;
        return;
    }
    if (c == '\t') {
        do
            put_char(' ');
        while (cursor_col % TAB_WIDTH != 0);
        return;
    }
    if (cursor_col >= cols)
        newline();
    *cell_at(cursor_row, cursor_col) =
        current_reverse ? (struct cell){ c, COLOR_BACKGROUND, current_fg, current_bold }
                        : (struct cell){ c, current_fg, COLOR_BACKGROUND, current_bold };
    if (shown)
        draw_cell(cursor_row, cursor_col);
    cursor_col++;
}

void console_write(const char *utf8)
{
    if (!screen)
        return;
    uint64_t start = arch_counter();
    uint32_t c;
    cursor_hide();
    while ((c = utf8_next(&utf8)) != 0)
        put_char(c);
    cursor_show();
    if (shown) {
        maybe_update();
        draw_counts += arch_counter() - start;
    }
}

/* The "console" thread: copies what drawing left waiting. */
static void updater(void *arg)
{
    (void)arg;
    for (;;) {
        timer_sleep_ms(UPDATE_MS);
        blank_check();                              /* (37e) the screen off, or on again */
        if (dirty_top != dirty_bottom && timer_elapsed_ms(last_update) >= UPDATE_MS) {
            bool locked = kprintf_lock();           /* not while kprintf draws */
            update_screen();
            kprintf_unlock(locked);
        }
    }
}

bool console_start_updates(void)
{
    if (target != &canvas || batching || !arch_counter_hz())
        return false;
    if (!thread_create("console", updater, 0))
        return false;
    last_update = arch_counter();
    batching = true;
    return true;
}

void console_emergency(void)
{
    batching = false;
    suspended = false;          /* a crash takes the screen back from any owner */
    blanked = false;
    sleeping = false;
    gpu_backlight_emergency();  /* (37e-2b) and lights it again */
    if (screen && target == &canvas)
        copy_to_screen(0, 0, screen->width, screen->height);
    update_screen();
}

void console_blank(bool off)
{
    if (!screen || target != &canvas)
        return;
    bool locked = kprintf_lock();           /* (not while kprintf draws) */
    blanked = off;
    if (off && !suspended) {
        uint64_t start = arch_counter();
        for (uint32_t y = 0; y < screen->height; y++)
            memset((void *)(screen->base + (uint64_t)y * screen->stride), 0,
                   (uint64_t)screen->width * 4);
        write_ticks[1] += arch_counter() - start;
        writes[1] += (uint64_t)screen->width * screen->height * 4;
    } else if (!off) {
        copy_to_screen(0, 0, screen->width, screen->height);
    }
    kprintf_unlock(locked);
}

void console_sleep(bool going)
{
    if (!screen)
        return;
    bool locked = kprintf_lock();           /* (not while kprintf draws) */
    sleeping = going;
    if (!going && target == &canvas)
        copy_to_screen(0, 0, screen->width, screen->height);
    kprintf_unlock(locked);
}

bool console_display_acquire(void)
{
    if (!screen)
        return false;
    suspended = true;
    return true;
}

void console_display_release(void)
{
    suspended = false;
    if (screen && target == &canvas)
        copy_to_screen(0, 0, screen->width, screen->height);    /* redraw the console */
}

bool console_screen_format(uint32_t *width, uint32_t *height, uint32_t *stride,
                           uint8_t *red_shift, uint8_t *green_shift, uint8_t *blue_shift)
{
    if (!screen)
        return false;
    *width = screen->width;
    *height = screen->height;
    *stride = screen->stride;
    *red_shift = screen->red_shift;
    *green_shift = screen->green_shift;
    *blue_shift = screen->blue_shift;
    return true;
}

void console_display_row(uint32_t x, uint32_t y, const uint32_t *pixels, uint32_t w)
{
    if (!screen || y >= screen->height || x >= screen->width)
        return;
    if (w > screen->width - x)
        w = screen->width - x;
    screen_row(x, y, pixels, w);
}

void console_screen_writes(uint64_t *fast, uint64_t *fast_ticks, uint64_t *slow,
                           uint64_t *slow_ticks)
{
    *fast = writes[0], *fast_ticks = write_ticks[0];
    *slow = writes[1], *slow_ticks = write_ticks[1];
}

bool console_set_screen(const struct framebuffer *fb)
{
    static struct framebuffer now;
    uint32_t new_cols = (fb->width - 2 * TEXT_LEFT) / regular->width;
    uint32_t new_rows = (fb->height - TEXT_TOP - STATUS_HEIGHT - 4) / regular->height;
    if (new_cols > MAX_COLS)
        new_cols = MAX_COLS;
    if (new_rows > MAX_ROWS)
        new_rows = MAX_ROWS;
    void *copy = kmalloc((uint64_t)fb->stride * fb->height * 4);
    struct cell *kept = kmalloc(sizeof(struct cell) * rows * cols);
    bool locked = kprintf_lock();                   /* nothing draws meanwhile */
    if (kept)                                       /* the rows up to the cursor's */
        memcpy(kept, cells, sizeof(struct cell) * rows * cols);
    uint32_t old_cols = cols, last = cursor_row, first = last + 1 > new_rows ? last + 1 - new_rows : 0;
    cols = new_cols;
    rows = new_rows;
    for (uint32_t r = 0; r < rows; r++)
        clear_row(r);
    for (uint32_t r = first; kept && r <= last; r++)
        memcpy(cell_at(r - first, 0), &kept[r * old_cols], sizeof(struct cell) *
               (old_cols < cols ? old_cols : cols));
    cursor_row = last - first;
    if (cursor_col >= cols)
        cursor_col = cols - 1;
    if (target == &canvas)
        kfree((void *)canvas.base);
    now = *fb;
    screen = &now;
    if (copy) {
        canvas = now;
        canvas.base = copy;
        target = &canvas;
    } else {
        target = &now;
        batching = false;                           /* (straight to the screen) */
    }
    top_row = 0;
    dirty_top = dirty_bottom = 0;
    cursor_drawn = false;
    if (shown)
        console_show();
    kprintf_unlock(locked);
    if (kept)
        kfree(kept);
    return copy != 0;
}

void console_report(void)
{
    uint64_t hz = arch_counter_hz();
    if (!hz)
        return;
    kprintf("  console:       %ux%u characters, %lu lines scrolled; drawing took %lu ms "
            "(%lu ms of it scrolling); %lu screen updates (%lu ms), %s\n", cols, rows,
            lines_scrolled, draw_counts * 1000 / hz, scroll_counts * 1000 / hz, updates,
            update_counts * 1000 / hz,
            batching ? "batched every 20 ms" : target == &canvas ? "through a copy of the "
                                                                   "screen in RAM" : "directly");
}

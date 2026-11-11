/*
 * Text console on the framebuffer.
 *
 * The console keeps every character cell in memory (character, colors, bold),
 * so it can collect text before it is shown, redraw itself after something
 * else used the screen, and scroll without reading back video memory (which
 * is very slow on real graphics hardware).
 *
 * Screen layout: title bar at the top, text area, status strip at the bottom.
 */
#ifndef RITTEROS_CONSOLE_H
#define RITTEROS_CONSOLE_H

#include <stdbool.h>
#include <stdint.h>
#include "framebuffer.h"

/* Colors as 0xRRGGBB. */
#define COLOR_TEXT      0xC8C8C8
#define COLOR_DIM       0x8088A0
#define COLOR_HEADING   0x80C8FF
#define COLOR_OK        0x50DC78
#define COLOR_WARNING   0xFFD050
#define COLOR_ERROR     0xFF6060
#define COLOR_BACKGROUND 0x0C0E1C

/* Status strip colors. */
#define STATUS_RUNNING  0x3050A0
#define STATUS_OK       0x00C850
#define STATUS_CRASH    0xDC0000

/* Set up the console for fb. Text is collected but not drawn until
 * console_show() is called. */
void console_init(const struct framebuffer *fb);

/* Draw the whole console (title bar, all text, status strip) and keep
 * drawing new text from now on. */
void console_show(void);

bool console_is_shown(void);

/* Write UTF-8 text at the cursor. Handles \n, \r, \b, \t and a few VT100
 * escape sequences (listed in common/syscalls.h); wraps and scrolls. */
void console_write(const char *utf8);

/* Style for text written from now on. */
void console_set_color(uint32_t fg);
void console_set_bold(bool bold);

/* Back to the normal style, the cursor shown, no escape sequence open
 * (after a full-screen program). */
void console_reset_style(void);

/* The text area in characters; false without a screen. */
bool console_size(uint32_t *rows, uint32_t *cols);

/* The screen in pixels; false without a screen. (A pointer clamps to it.) */
bool console_screen_size(uint32_t *width, uint32_t *height);

/* Color of the status strip along the bottom edge. */
void console_set_status(uint32_t color);

/* Draw through a copy of the screen in RAM from now on (needs the heap):
 * scrolling then moves pixels in RAM instead of redrawing every character.
 * False if there is not enough memory; the console then draws directly. */
bool console_use_buffer(void);

/* Batch screen updates from now on (needs the scheduler and the copy in
 * RAM): changes reach the screen at most every 20 ms, the rest by the
 * thread "console". */
bool console_start_updates(void);

/* Copy at once from now on: after a crash, nothing else runs. */
void console_emergency(void);
/* (37g-5d) A sleep begins (true): no writes to the screen (its graphics
 * device loses its state) until the drivers are back (false: drawn whole). */
void console_sleep(bool going);

/* The display handoff (SYS_DISPLAY): a program (windowd) takes over the
 * screen. While parked, the console still collects text in RAM but does not
 * touch the screen; release redraws the whole console. */
bool console_display_acquire(void);     /* park the console; false if no screen */

/* (37e) The screen off (black, nothing copied to it) or on again (the
 * console redrawn, unless a program owns the screen). kernel/blank.c. */
void console_blank(bool off);
void console_display_release(void);     /* give the screen back and redraw */

/* The screen's size and native pixel format; false if there is no screen. */
bool console_screen_format(uint32_t *width, uint32_t *height, uint32_t *stride,
                           uint8_t *red_shift, uint8_t *green_shift, uint8_t *blue_shift);

/* Copy `w` native pixels to the screen at (x, y) (clipped to the screen).
 * For the display owner's flush. */
void console_display_row(uint32_t x, uint32_t y, const uint32_t *pixels, uint32_t w);

/* (66b-2) The screen has a new mode (a graphics driver set it): draw on fb
 * from now on, the text kept (its last rows, cut to the new width) and
 * everything drawn again. False if there was no memory for the new copy of
 * the screen (the console then draws on fb directly). */
bool console_set_screen(const struct framebuffer *fb);

/* (66b-3) Bytes written to the screen and the counter ticks they took:
 * within the firmware's framebuffer (write-combining) and beyond it. */
void console_screen_writes(uint64_t *fast, uint64_t *fast_ticks, uint64_t *slow,
                           uint64_t *slow_ticks);

/* Log how many lines scrolled and how long drawing took so far. */
void console_report(void);

#endif

/*
 * (37e, roadmap item 37) The screen goes off after a while without input:
 * /system/config/power's `screen_off = MINUTES` (0 never; 5 by default).
 * The console paints the screen black and stops copying to it; a program
 * that owns the screen (windowd) gets INPUT_BLANK (key 1) and does the
 * same. The next input brings it back (INPUT_BLANK, key 0), and that key
 * press or click only wakes it (the user's choice: it could type into or
 * click on something unseen). (37e-2b) The panels' backlight goes off
 * with it; (37e-2c) at `screen_dim` they dim to a third first, and input
 * then is not dropped (the screen was seen).
 */
#ifndef RITTEROS_BLANK_H
#define RITTEROS_BLANK_H

#include <stdbool.h>

/* The settings (acpi_events_settings reads them), minutes; 0 never.
 * (37e-2c) The panels dim at screen_dim when that comes before screen_off.
 * (37g-7) After sleep_after minutes without use the machine sleeps
 * (acpi_events_idle). */
void blank_set(unsigned screen_off_minutes, unsigned screen_dim_minutes,
               unsigned sleep_after_minutes);

/* A key press or a mouse event (interrupt context): noted; true when the
 * screen was off and this input woke it: the caller drops it. */
bool blank_input(void);

/* Other input (the serial port's bytes): noted, wakes the screen, never
 * dropped. */
void blank_activity(void);

/* (37g-7) Use that is not input here (typing into a pseudo-terminal):
 * counts for sleep_after only, the screen stays as it is. */
void blank_used(void);

/* (37g-7) After a sleep: the screen on, idle from now. */
void blank_woke(void);

/* The console's thread, every 20 ms: dimmed, then off after the settings'
 * minutes, back after input; logs each. */
void blank_check(void);

#endif

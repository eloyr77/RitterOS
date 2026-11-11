/*
 * Keyboards, whatever their kind: names (kbd0, kbd1, ... in the order
 * found), what a key does (US layout for now), modifiers, Caps Lock and
 * key repeat. Drivers report key events as HID usage IDs (the numbering of
 * the USB HID usage tables, page 7), which USB keyboards send directly and
 * the PS/2 driver translates its scan codes into.
 *
 * Key events come from interrupt handlers; so does the timer tick that
 * repeats held keys. Interrupt handlers do not interrupt each other.
 */
#ifndef RITTEROS_KEYBOARD_H
#define RITTEROS_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>
#include "devname.h"

/* HID modifier bits (as in a USB keyboard report's first byte); usage IDs
 * 0xE0-0xE7 are the same keys, in this bit order. */
#define KEYBOARD_CTRL       0x11        /* left or right */
#define KEYBOARD_SHIFT      0x22
#define KEYBOARD_ALT        0x44        /* left or right Alt */
#define KEYBOARD_ALTGR      0x40        /* right Alt */

struct keyboard;

/* A keyboard being set up gets its name (kbdN, devname.h: its old one if
 * it was here before); events_name says what its driver counts ("reports",
 * "scan codes"). 0 if there are too many. keyboard_ready() makes it an
 * input of the terminal; keyboard_drop() (setup failed) and
 * keyboard_remove() (unplugged) end it, its name stays reserved. */
struct keyboard *keyboard_new(const char *events_name, const struct devname_id *id);
void keyboard_ready(struct keyboard *k);
void keyboard_drop(struct keyboard *k);
void keyboard_remove(struct keyboard *k);
const char *keyboard_name(const struct keyboard *k);

/* A key went down or up (HID usage ID). A key that is already down does
 * nothing (PS/2 keyboards repeat held keys themselves); modifier keys
 * (0xE0-0xE7) change the modifiers. Interrupt context. */
void keyboard_key(struct keyboard *k, uint8_t usage, bool down);

/* The modifiers held on any keyboard (KEYBOARD_* bits): for pointer events
 * (Ctrl + click is a right-click in windowd). */
uint8_t keyboard_modifiers_held(void);

/* Set all modifiers at once (a USB report lists them as a byte). */
void keyboard_set_modifiers(struct keyboard *k, uint8_t modifiers);

/* Count one event of the driver (a report, a scan code). */
void keyboard_count_event(struct keyboard *k);

/* Log each keyboard's events and key presses. */
void keyboard_report(void);

#endif

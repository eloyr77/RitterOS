/*
 * Mice, whatever their kind: names (mouse0, mouse1, ... in the order
 * found), a pointer position (clamped to the screen), the buttons held and
 * the counters the `mouse` rescue command shows. Drivers report relative
 * motion, a wheel step and a button bitmask; USB HID boot mice and the PS/2
 * mouse both reduce to that.
 *
 * There is no window server yet (roadmap item 15), so a mouse is not an
 * input of the terminal the way a keyboard is: its events only move the
 * pointer and count. windowd (15e) will read them.
 *
 * Motion comes from interrupt handlers, which do not interrupt each other.
 */
#ifndef RITTEROS_MOUSE_H
#define RITTEROS_MOUSE_H

#include <stdbool.h>
#include <stdint.h>
#include "devname.h"

/* Button bits (as a USB HID boot-mouse report's first byte). */
#define MOUSE_LEFT      0x01
#define MOUSE_RIGHT     0x02
#define MOUSE_MIDDLE    0x04

struct mouse;

/* A mouse being set up gets its name (mouseN, devname.h: its old one if it
 * was here before); events_name says what its driver counts ("reports",
 * "packets"). 0 if there are too many. mouse_ready() announces it;
 * mouse_drop() (setup failed) and mouse_remove() (unplugged) end it, its
 * name stays reserved. */
struct mouse *mouse_new(const char *events_name, const struct devname_id *id);
void mouse_ready(struct mouse *m);
void mouse_drop(struct mouse *m);
void mouse_remove(struct mouse *m);
const char *mouse_name(const struct mouse *m);

/* The pointer moved by (dx, dy) pixels and the wheel by `wheel` steps, with
 * `buttons` now held (MOUSE_* bits). A button that goes down counts as a
 * click. Interrupt context. */
void mouse_event(struct mouse *m, int dx, int dy, int wheel, uint8_t buttons);

/* The pointer is at an absolute position: x in [0, x_range], y in [0, y_range]
 * (as an absolute pointing device such as a USB tablet reports), scaled to the
 * screen. Otherwise like mouse_event. Interrupt context. */
void mouse_absolute(struct mouse *m, int x, int y, int x_range, int y_range, int wheel,
                    uint8_t buttons);

/* (66b-5) The screen has a new size: every mouse's pointer is clamped to it
 * from now on (and moved inside it). Thread context. */
void mouse_screen(uint32_t width, uint32_t height);

/* Count one event of the driver (a report, a packet). */
void mouse_count_event(struct mouse *m);

/* Log each mouse: its counters, where the pointer is and the buttons held
 * (the boot summary and the `mouse` rescue command). */
void mouse_report(void);

#endif

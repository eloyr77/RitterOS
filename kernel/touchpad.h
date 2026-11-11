/*
 * Touchpad gestures (15v; the user's: one finger moves and clicks, taps
 * click, two fingers scroll -- the content follows the fingers --
 * and two fingers pressing or tapping are the right button; three fingers
 * nothing yet). A touch device hands over frames of contacts (its own
 * units, absolute); this turns them into a pointing device's motion, wheel
 * steps and buttons (mouse.h), so windowd and every app get them as from a
 * mouse. For any touchpad, whatever its bus (HID over I2C first).
 */
#ifndef RITTEROS_TOUCHPAD_H
#define RITTEROS_TOUCHPAD_H

#include <stdbool.h>
#include <stdint.h>
#include "mouse.h"

#define TOUCH_MAX 5                     /* contacts followed at once */

struct touch_contact {
    uint8_t id;                         /* the device's contact ID */
    bool tip;                           /* on the surface */
    bool confident;                     /* not a palm (the device's judgment) */
    int32_t x, y;                       /* the device's units */
};

struct touchpad;

/* A touchpad with x_units and y_units per millimetre, reporting through
 * the pointing device m. */
struct touchpad *touchpad_new(struct mouse *m, int32_t x_units_per_mm, int32_t y_units_per_mm);

/* One frame: n contacts (those lifted have tip false), the physical button,
 * the time in milliseconds. Not in interrupt context. */
void touchpad_frame(struct touchpad *t, const struct touch_contact *c, int n, bool button,
                    uint64_t ms);

#endif

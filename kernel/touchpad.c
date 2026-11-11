/*
 * Touchpad gestures. A "touch" lasts from the first finger down to the last
 * one up. In it:
 * - one finger moving moves the pointer: millimetres times a gain that
 *   grows with the speed (GAIN_* below: slow moves are precise, fast ones
 *   cross the screen), with the fractions kept;
 * - two fingers moving together scroll: a wheel step per SCROLL_MM of their
 *   mean vertical movement, fingers up as the wheel down (the user's: the
 *   content follows the fingers);
 * - the physical button is the left button, or the right one when two or
 *   more fingers are down as it closes (the two-finger click);
 * - a touch that ends within TAP_MS, moved less than TAP_MM and never
 *   pressed the button is a tap: one finger the left button, two the right
 *   (pressed and released at once); three fingers do nothing.
 * - a press stays where it was aimed (the user's, from the laptop: a finger
 *   pressing the pad rolls): pointer movement of the last UNDO_MS before
 *   the button closes is taken back when it was small (UNDO_PX), the
 *   pointer stays while the button is held until the finger moves
 *   PRESS_HOLD_MM (then it is a drag), and for AFTER_MS after the release
 *   until it moves AFTER_MM;
 * When the number of fingers changes, that frame does not move anything
 * (a finger landing or lifting shifts the contacts' positions).
 * Contacts the device does not trust (a palm) are left out.
 */
#include "touchpad.h"
#include "arch.h"
#include "heap.h"
#include "string.h"

#define TAP_MS          180
#define TAP_MM          2
#define SCROLL_MM       3
#define GAIN_SLOW       8               /* pixels per millimetre at rest... */
#define GAIN_PER_SPEED  4               /* ...plus this per 100 mm/s... */
#define GAIN_MAX        28              /* ...up to this */
#define UNDO_MS         150
#define UNDO_PX         16
#define PRESS_HOLD_MM   3
#define AFTER_MS        200
#define AFTER_MM        2
#define HISTORY         32

struct finger {
    bool used;
    uint8_t id;
    int32_t x, y;
};

struct touchpad {
    struct mouse *mouse;
    int32_t xu, yu;                     /* units per millimetre */
    struct finger last[TOUCH_MAX];
    int last_down;
    uint64_t last_ms;
    /* the touch */
    bool touching, pressed_in_touch;
    uint64_t touch_ms;
    int max_fingers;
    int64_t travel;                     /* units moved in all (any finger, both axes) */
    /* outputs */
    uint8_t buttons;
    int64_t fx, fy;                     /* pointer motion not yet reported, 1/256 pixel */
    int64_t scroll;                     /* units of scrolling not yet reported */
    struct { uint64_t ms; int dx, dy; } moved[HISTORY];   /* recent pointer motion */
    int nmoved;
    int64_t hold;                       /* units the finger moved while the pointer is held */
    bool holding;                       /* held: pressed, or just released */
    uint64_t released_ms;
};

struct touchpad *touchpad_new(struct mouse *m, int32_t x_units_per_mm, int32_t y_units_per_mm)
{
    struct touchpad *t = kzalloc(sizeof(*t));
    if (!t)
        return 0;
    t->mouse = m;
    t->xu = x_units_per_mm > 0 ? x_units_per_mm : 30;
    t->yu = y_units_per_mm > 0 ? y_units_per_mm : 30;
    return t;
}

static void emit(struct touchpad *t, int dx, int dy, int wheel)
{
    uint64_t state = arch_irq_save();           /* (the mouse layer expects interrupt context) */
    mouse_event(t->mouse, dx, dy, wheel, t->buttons);
    arch_irq_restore(state);
}

static const struct finger *previous(const struct touchpad *t, uint8_t id)
{
    for (int i = 0; i < TOUCH_MAX; i++)
        if (t->last[i].used && t->last[i].id == id)
            return &t->last[i];
    return 0;
}

static int32_t magnitude(int32_t v)
{
    return v < 0 ? -v : v;
}

void touchpad_frame(struct touchpad *t, const struct touch_contact *c, int n, bool button,
                    uint64_t ms)
{
    struct finger now[TOUCH_MAX];
    int down = 0;
    memset(now, 0, sizeof(now));
    for (int i = 0; i < n && down < TOUCH_MAX; i++)
        if (c[i].tip && c[i].confident)
            now[down].used = true, now[down].id = c[i].id, now[down].x = c[i].x,
            now[down].y = c[i].y, down++;
    uint64_t dt = ms > t->last_ms && ms - t->last_ms < 100 ? ms - t->last_ms : 7;

    if (down && !t->touching) {                 /* a touch begins */
        t->touching = true;
        t->pressed_in_touch = false;
        t->touch_ms = ms;
        t->max_fingers = 0;
        t->travel = 0;
        t->scroll = 0;
    }
    if (down > t->max_fingers)
        t->max_fingers = down;

    /* Movement of the fingers seen in the last frame too. */
    int32_t sum_dy = 0, best_dx = 0, best_dy = 0, moved = 0;
    for (int i = 0; i < down; i++) {
        const struct finger *p = previous(t, now[i].id);
        if (!p)
            continue;
        int32_t dx = now[i].x - p->x, dy = now[i].y - p->y;
        sum_dy += dy, moved++;
        t->travel += magnitude(dx) + magnitude(dy);
        if (magnitude(dx) + magnitude(dy) > magnitude(best_dx) + magnitude(best_dy))
            best_dx = dx, best_dy = dy;
    }
    bool steady = down == t->last_down;         /* (fingers landing or lifting: no motion) */

    /* The physical button: left, or right for a two-finger click. */
    bool was_pressed = t->buttons != 0;
    if (button && !was_pressed) {
        /* the creep of the pressing finger before the button closed: back */
        int ux = 0, uy = 0;
        for (int i = 0; i < t->nmoved; i++)
            if (ms - t->moved[i].ms <= UNDO_MS)
                ux += t->moved[i].dx, uy += t->moved[i].dy;
        if (magnitude(ux) + magnitude(uy) <= UNDO_PX && (ux || uy))
            emit(t, -ux, -uy, 0);
        t->nmoved = 0;
        t->buttons = down >= 2 ? MOUSE_RIGHT : MOUSE_LEFT;
        t->pressed_in_touch = true;
        t->holding = true;
        t->hold = 0;
        emit(t, 0, 0, 0);
    } else if (!button && was_pressed) {
        t->buttons = 0;
        t->holding = true;
        t->hold = 0;
        t->released_ms = ms;
        emit(t, 0, 0, 0);
    }
    if (t->holding && !t->buttons && ms - t->released_ms > AFTER_MS)
        t->holding = false;

    if (steady && moved && (down == 1 || (button && down >= 1))) {
        /* the pointer: the finger that moved most (a click held with one
         * finger, dragging with another) */
        if (t->holding) {                       /* a press or a release: held until it moves */
            t->hold += magnitude(best_dx) + magnitude(best_dy);
            int32_t limit = (t->buttons ? PRESS_HOLD_MM : AFTER_MM) * (t->xu + t->yu) / 2;
            if (t->hold <= limit)
                goto done;
            t->holding = false;
        }
        int64_t mmx = (int64_t)best_dx * 256 / t->xu, mmy = (int64_t)best_dy * 256 / t->yu;
        int64_t dist = magnitude((int32_t)mmx) + magnitude((int32_t)mmy);   /* mm * 256 */
        int64_t speed = dist * 1000 / (int64_t)dt / 256;                     /* mm/s */
        int64_t gain = GAIN_SLOW + speed * GAIN_PER_SPEED / 100;
        if (gain > GAIN_MAX)
            gain = GAIN_MAX;
        t->fx += mmx * gain;
        t->fy += mmy * gain;
        int dx = (int)(t->fx / 256), dy = (int)(t->fy / 256);
        t->fx -= (int64_t)dx * 256, t->fy -= (int64_t)dy * 256;
        if (dx || dy) {
            emit(t, dx, dy, 0);
            if (t->nmoved == HISTORY) {
                memmove(t->moved, t->moved + 1, sizeof(t->moved[0]) * (HISTORY - 1));
                t->nmoved--;
            }
            t->moved[t->nmoved].ms = ms, t->moved[t->nmoved].dx = dx, t->moved[t->nmoved].dy = dy;
            t->nmoved++;
        }
    } else if (steady && moved && down == 2 && !button) {
        /* scrolling: the mean vertical movement; fingers up (y falling) is the wheel down */
        t->scroll += sum_dy / moved;
        int32_t step = SCROLL_MM * t->yu;
        int steps = (int)(t->scroll / step);
        if (steps) {
            t->scroll -= (int64_t)steps * step;
            emit(t, 0, 0, steps);
        }
    }

done:
    if (!down && t->touching) {                 /* the touch ends: a tap? */
        t->touching = false;
        bool quick = ms - t->touch_ms <= TAP_MS;
        bool still = t->travel <= (int64_t)TAP_MM * (t->xu + t->yu) / 2;
        if (quick && still && !t->pressed_in_touch && t->max_fingers <= 2 && !t->buttons) {
            t->buttons = t->max_fingers == 2 ? MOUSE_RIGHT : MOUSE_LEFT;
            emit(t, 0, 0, 0);
            t->buttons = 0;
            emit(t, 0, 0, 0);
        }
    }

    memcpy(t->last, now, sizeof(now));
    t->last_down = down;
    t->last_ms = ms;
}

/*
 * The display owner's input queue (see winput.h). A ring of events under a
 * spinlock; the drivers add from interrupt context, the owner's DISPLAY_EVENT
 * takes them. When the queue overflows the oldest event is dropped (a pointer
 * that moved fast is no worse for it).
 */
#include "winput.h"
#include "keyboard.h"
#include "sched.h"
#include "sync.h"
#include "timer.h"

#define QUEUE   256

static struct sys_input_event queue[QUEUE];
static unsigned head, count;
static bool captured;
static struct spinlock lock = SPINLOCK_INIT("winput");
static struct wait_queue waiters;

void winput_capture(void)
{
    uint64_t state = spin_lock(&lock);
    head = count = 0;
    captured = true;
    spin_unlock(&lock, state);
}

void winput_release(void)
{
    uint64_t state = spin_lock(&lock);
    captured = false;
    count = 0;
    spin_unlock(&lock, state);
    wait_queue_wake_all(&waiters);          /* let a blocked reader return */
}

bool winput_captured(void)
{
    return captured;
}

static void enqueue(const struct sys_input_event *e)
{
    uint64_t state = spin_lock(&lock);
    if (captured) {
        if (count == QUEUE) {
            head = (head + 1) % QUEUE;      /* drop the oldest */
            count--;
        }
        queue[(head + count) % QUEUE] = *e;
        count++;
    }
    spin_unlock(&lock, state);
    wait_queue_wake_all(&waiters);
}

void winput_pointer(int x, int y, uint8_t buttons)
{
    struct sys_input_event e = {0};
    e.type = INPUT_POINTER;
    e.buttons = buttons;
    uint8_t held = keyboard_modifiers_held();
    e.modifiers = (uint8_t)((held & KEYBOARD_CTRL ? INPUT_CTRL : 0) |
                            (held & KEYBOARD_SHIFT ? INPUT_SHIFT : 0) |
                            (held & KEYBOARD_ALT ? INPUT_ALT : 0));
    e.x = x < 0 ? 0 : (uint16_t)x;
    e.y = y < 0 ? 0 : (uint16_t)y;
    enqueue(&e);
}

void winput_wheel(int x, int y, int steps)
{
    struct sys_input_event e = {0};
    e.type = INPUT_WHEEL;
    e.key = (uint8_t)(int8_t)(steps < -127 ? -127 : steps > 127 ? 127 : steps);
    e.x = x < 0 ? 0 : (uint16_t)x;
    e.y = y < 0 ? 0 : (uint16_t)y;
    enqueue(&e);
}

void winput_keycode(uint8_t usage, bool down)
{
    struct sys_input_event e = {0};
    e.type = INPUT_KEYCODE;
    e.key = usage;
    e.buttons = down ? 1 : 0;
    enqueue(&e);
}

void winput_key(uint8_t byte)
{
    struct sys_input_event e = {0};
    e.type = INPUT_KEY;
    e.key = byte;
    enqueue(&e);
}

void winput_screen(uint32_t width, uint32_t height)
{
    struct sys_input_event e = {0};
    e.type = INPUT_SCREEN;
    e.x = (uint16_t)width;
    e.y = (uint16_t)height;
    enqueue(&e);
}

void winput_blank(bool off)
{
    struct sys_input_event e = {0};
    e.type = INPUT_BLANK;
    e.key = off ? 1 : 0;
    enqueue(&e);
}

int winput_next(struct sys_input_event *out, uint32_t timeout_ms)
{
    uint64_t deadline = timeout_ms ? timer_ticks() + ((uint64_t)timeout_ms * TIMER_HZ + 999) / 1000
                                   : 0;
    if (timeout_ms)
        wait_event_until(&waiters, count || !captured, deadline);
    else
        wait_event_interruptible(&waiters, count || !captured);
    if (thread_current()->stop_requested)
        return -ERR_INTR;
    uint64_t state = spin_lock(&lock);
    if (!count) {
        spin_unlock(&lock, state);
        return 0;                           /* timed out, or the display was released */
    }
    *out = queue[head];
    head = (head + 1) % QUEUE;
    count--;
    spin_unlock(&lock, state);
    return 1;
}

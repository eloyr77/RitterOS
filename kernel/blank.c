/*
 * The screen off after a while without input (37e). See blank.h.
 *
 * Input arrives in interrupt context (keyboards, mice, the serial port):
 * it only notes the time, or marks the screen waking. The console's thread
 * does the rest (blank_check, every 20 ms): it may draw and log.
 */
#include "blank.h"
#include "acpi_events.h"
#include "console.h"
#include "gpu.h"
#include "kprintf.h"
#include "sync.h"
#include "timer.h"
#include "winput.h"

/* (37e-2c) DIMMED: the panels dimmed, still seen; UNDIMMING and WAKING:
 * input came, the console's thread brings the brightness or the screen
 * back. */
enum { SHOWN, DIMMED, UNDIMMING, OFF, WAKING };

static struct spinlock lock = SPINLOCK_INIT("blank");
static int state = SHOWN;
static uint64_t last_input;             /* timer_ticks() */
static uint64_t off_ticks = 5 * 60 * (uint64_t)TIMER_HZ;
static uint64_t dim_ticks = 2 * 60 * (uint64_t)TIMER_HZ;
static unsigned off_minutes = 5, dim_minutes = 2;
/* (37g-7) sleep_after: the last use (input, or typing into a pseudo-
 * terminal: a user over SSH), and whether this idle stretch asked */
static uint64_t last_use, sleep_ticks;
static bool sleep_asked;

void blank_set(unsigned screen_off_minutes, unsigned screen_dim_minutes,
               unsigned sleep_after_minutes)
{
    uint64_t s = spin_lock(&lock);
    off_minutes = screen_off_minutes;
    off_ticks = (uint64_t)screen_off_minutes * 60 * TIMER_HZ;
    dim_minutes = screen_dim_minutes;
    dim_ticks = (uint64_t)screen_dim_minutes * 60 * TIMER_HZ;
    sleep_ticks = (uint64_t)sleep_after_minutes * 60 * TIMER_HZ;
    last_input = last_use = timer_ticks();
    sleep_asked = false;
    spin_unlock(&lock, s);
}

void blank_used(void)
{
    uint64_t s = spin_lock(&lock);
    last_use = timer_ticks();
    sleep_asked = false;
    spin_unlock(&lock, s);
}

bool blank_input(void)
{
    uint64_t s = spin_lock(&lock);
    last_input = last_use = timer_ticks();
    sleep_asked = false;
    bool woke = state == OFF;
    if (woke)
        state = WAKING;
    else if (state == DIMMED)
        state = UNDIMMING;              /* (seen: the input goes on) */
    spin_unlock(&lock, s);
    return woke;
}

void blank_activity(void)
{
    blank_input();
}

/* (37g-7) The timer's ticks do not count the sleep: idle from the wake on. */
void blank_woke(void)
{
    blank_input();
}

void blank_check(void)
{
    uint64_t s = spin_lock(&lock);
    int now = state, next = now;
    uint64_t idle = timer_ticks() - last_input;
    if ((now == SHOWN || now == DIMMED) && off_ticks && idle >= off_ticks)
        next = OFF;
    else if (now == SHOWN && dim_ticks && (!off_ticks || dim_ticks < off_ticks) &&
             idle >= dim_ticks)
        next = DIMMED;
    else if (now == WAKING || now == UNDIMMING)
        next = SHOWN;
    state = next;
    bool ask = sleep_ticks && !sleep_asked && timer_ticks() - last_use >= sleep_ticks;
    if (ask)
        sleep_asked = true;                         /* (once per idle stretch) */
    spin_unlock(&lock, s);
    if (ask)
        acpi_events_idle();                         /* (37g-7) its thread starts the sleep */
    if (next == now)
        return;
    char lit[96];                       /* (37e-2b) the outputs whose backlight changed */
    char said[96];                      /* (37e-2c) the brightness changed */
    if (next == DIMMED) {
        gpu_dim(true, said, sizeof(said));
        if (said[0])
            kprintf("screen: dimmed after %u minute%s without input (%s)\n", dim_minutes,
                    dim_minutes == 1 ? "" : "s", said);
    } else if (next == OFF) {
        console_blank(true);
        if (winput_captured())
            winput_blank(true);
        gpu_backlight(false, lit, sizeof(lit));
        kprintf("screen: off after %u minute%s without input%s%s%s\n", off_minutes,
                off_minutes == 1 ? "" : "s", lit[0] ? " (" : "", lit, lit[0] ? ")" : "");
    } else if (now == UNDIMMING) {
        gpu_dim(false, said, sizeof(said));
        if (said[0])
            kprintf("screen: the brightness back (input: %s)\n", said);
    } else if (now == WAKING) {
        gpu_dim(false, said, sizeof(said));     /* (while still dark) */
        gpu_backlight(true, lit, sizeof(lit));
        if (winput_captured())
            winput_blank(false);
        console_blank(false);
        kprintf("screen: on (input)%s%s%s%s%s%s\n", lit[0] ? " (" : "", lit, lit[0] ? ")" : "",
                said[0] ? " (the brightness: " : "", said, said[0] ? ")" : "");
    }
}

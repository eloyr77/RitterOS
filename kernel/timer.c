#include "timer.h"
#include "arch.h"
#include "kprintf.h"
#include "percpu.h"
#include "sched.h"

#define CHECK_MS 500

static volatile uint64_t ticks;
static bool running;

#define MAX_TICK_HANDLERS 8

static struct {
    void (*fn)(void *context);
    void *context;
} tick_handlers[MAX_TICK_HANDLERS];
static int tick_handler_count;

bool timer_add_tick_handler(void (*fn)(void *context), void *context)
{
    if (tick_handler_count == MAX_TICK_HANDLERS)
        return false;
    tick_handlers[tick_handler_count].fn = fn;
    tick_handlers[tick_handler_count].context = context;
    tick_handler_count++;
    return true;
}

void timer_tick(void)
{
    /* (65c) Every core ticks; the clock and the tick handlers (devices'
     * safety nets, key repeat) are the boot core's alone. */
    struct cpu *c = arch_this_cpu();
    c->ticks++;
    if (c->index == 0) {
        ticks++;
        for (int i = 0; i < tick_handler_count; i++)
            tick_handlers[i].fn(tick_handlers[i].context);
    }
    sched_tick();
}

uint64_t timer_ticks(void)
{
    return ticks;
}

void timer_init(void)
{
    if (!arch_interrupts_init(TIMER_HZ)) {
        kprintf("  timer:         no interrupts, waiting is busy-waiting\n");
        return;
    }
    arch_interrupts_enable();
    running = true;

    /* Count the ticks during a busy wait timed by an independent clock
     * (x86: the PIT, ARM: the counter itself). */
    uint64_t start = ticks;
    arch_delay_ms(CHECK_MS);
    uint64_t counted = ticks - start;
    kprintf("  timer:         %u Hz tick, %lu ticks during a %u ms busy wait (expected %u)\n",
            TIMER_HZ, counted, CHECK_MS, TIMER_HZ * CHECK_MS / 1000);
}

void timer_sleep_ms(uint32_t ms)
{
    if (!running) {
        arch_delay_ms(ms);
        return;
    }
    /* The first tick may come at once, so wait one more than the duration. */
    sched_sleep_until(ticks + ((uint64_t)ms * TIMER_HZ + 999) / 1000 + 1);
}

uint64_t timer_elapsed_ms(uint64_t start)
{
    uint64_t hz = arch_counter_hz();
    return hz ? (arch_counter() - start) * 1000 / hz : 0;
}

/*
 * The timer tick: once interrupts work, a timer interrupts the CPU TIMER_HZ
 * times a second and the kernel counts these ticks. Waiting no longer needs a
 * busy loop: the CPU sleeps (hlt / wfi) until the next interrupt.
 */
#ifndef RITTEROS_TIMER_H
#define RITTEROS_TIMER_H

#include <stdbool.h>
#include <stdint.h>

#define TIMER_HZ 100

/* Set up the interrupt controller and the tick (arch_interrupts_init), switch
 * interrupts on and check the tick rate against the busy-wait clock. Needs
 * ACPI (acpi_init). */
void timer_init(void);

/* Called by the architecture's interrupt handler on every tick. */
void timer_tick(void);

/* Call fn(context) on every tick, in interrupt context (no console, no
 * heap). False if all places for handlers are taken. */
bool timer_add_tick_handler(void (*fn)(void *context), void *context);

/* Ticks since timer_init. */
uint64_t timer_ticks(void);

/* Milliseconds since start, an arch_counter() value (0 while the counter's
 * rate is unknown). For measuring how long something took. */
uint64_t timer_elapsed_ms(uint64_t start);

/* Wait at least ms milliseconds; the CPU sleeps between ticks. Falls back to
 * a busy wait if there are no interrupts. */
void timer_sleep_ms(uint32_t ms);

#endif

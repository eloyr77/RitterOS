/*
 * Delays with the 8254 Programmable Interval Timer, without interrupts:
 * channel 2 counts down at 1.193182 MHz and its output can be polled via
 * port 0x61.
 */
#include "arch.h"
#include "cpu.h"
#include "io.h"
#include "kprintf.h"

#define PIT_FREQUENCY   1193182     /* Hz, the original PC's 14.31818 MHz / 12 */
#define PIT_CHANNEL2    0x42
#define PIT_COMMAND     0x43
#define PORT_B          0x61        /* system control port B */
#define PORT_B_GATE2    0x01        /* channel 2 gate: enables counting */
#define PORT_B_SPEAKER  0x02        /* connects channel 2 to the PC speaker */
#define PORT_B_OUT2     0x20        /* channel 2 output (read only) */

/* Command: channel 2, write low then high byte, mode 0 (interrupt on
 * terminal count: the output goes high when the count reaches zero), binary. */
#define CMD_CH2_MODE0   0xB0

#define CHUNK_MS        50          /* 50 ms = 59659 ticks, fits the 16-bit counter */

/* A read of port 0x61 takes about 1 us: a 50 ms chunk needs some 50,000
 * polls. Far more means channel 2 does not count (some PCs switch the PIT
 * off); delays then fall back to writes to port 0x80 (unused, ~1 us each:
 * the PC's traditional I/O delay), so the boot goes on. */
#define MAX_POLLS       2000000
#define PORT_DELAY      0x80

static bool pit_broken;

static void wait_ticks(uint16_t ticks)
{
    uint8_t b = inb(PORT_B);
    outb(PORT_B, (uint8_t)((b & ~PORT_B_SPEAKER) | PORT_B_GATE2));   /* gate on, silent */

    outb(PIT_COMMAND, CMD_CH2_MODE0);       /* output goes low */
    outb(PIT_CHANNEL2, ticks & 0xFF);
    outb(PIT_CHANNEL2, ticks >> 8);         /* counting starts */

    for (uint32_t polls = 0; (inb(PORT_B) & PORT_B_OUT2) == 0; polls++) {
        if (polls == MAX_POLLS) {
            pit_broken = true;
            kprintf("  PIT:           channel 2 does not count; delays use port 0x80 "
                    "(timing is approximate)\n");
            return;
        }
    }
}

void arch_delay_ms(uint32_t ms)
{
    if (pit_broken) {
        for (uint64_t us = 0; us < (uint64_t)ms * 1000; us++)
            outb(PORT_DELAY, 0);
        return;
    }
    while (ms > 0) {
        uint32_t chunk = ms < CHUNK_MS ? ms : CHUNK_MS;
        wait_ticks((uint16_t)(PIT_FREQUENCY * chunk / 1000));
        ms -= chunk;
    }
}

static uint64_t tsc_hz;
static uint64_t tsc_offset;             /* (37g-1) carries the counter over a sleep */

static uint64_t tsc_raw(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

uint64_t arch_counter(void)
{
    return tsc_raw() + tsc_offset;
}

/* (37g-1) After a sleep, which resets the time stamp counter: the counter
 * goes on from now (what it said before plus the time asleep). Every core
 * reads the same offset; they were reset together. */
void x86_counter_continue(uint64_t now)
{
    tsc_offset = now - tsc_raw();
}

uint64_t arch_counter_hz(void)
{
    return tsc_hz;
}

/* APERF counts the core's real cycles and MPERF cycles at the time stamp
 * counter's rate (AMD: P0's, Intel: the base clock), both only while it
 * runs; CPUID leaf 6, ECX bit 0 says they are there. */
bool arch_clock_counts(uint64_t *actual, uint64_t *base)
{
    if (cpuid(0, 0).eax < 6 || !(cpuid(6, 0).ecx & 1))
        return false;
    *base = rdmsr(0xE7);                /* IA32_MPERF */
    *actual = rdmsr(0xE8);              /* IA32_APERF */
    return true;
}

void timer_set_counter_hz(uint64_t hz)
{
    tsc_hz = hz;
}

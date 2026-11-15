/*
 * Delays with the 8254 Programmable Interval Timer, without interrupts:
 * channel 2 counts down at 1.193182 MHz and its output can be polled via
 * port 0x61.
 */
#include "arch.h"
#include "cpu.h"
#include "syscalls.h"
#include "io.h"
#include "kprintf.h"
#include "acpi.h"
#include "sched.h"
#include "string.h"
#include "timer.h"
#include "uefi.h"

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

/* (68p) Programs read the time stamp counter too (CR4.TSD is never set). */
uint32_t arch_counter_user(uint64_t *offset)
{
    *offset = tsc_offset;
    return SYS_CLOCK_TSC;
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

/* (68u) The rate measured again against the ACPI PM timer (3.579545 MHz,
 * from the same 14.318 MHz clock as the PIT): the boot's 50 ms against the
 * PIT through port I/O is off by a few hundred ppm (the VivoBook's clock
 * lost 270 ppm, 23 s a day). Two readings 2 s apart, each the TSC on both
 * sides of one read of the timer (the closest pair of 16, interrupts off:
 * a read is about a microsecond, so 2 s gives the rate to about 1 ppm),
 * then uefi_clock_rate. The timer is the FADT's port (24 bits, 32 with
 * TMR_VAL_EXT), else its X_PM_TMR_BLK if that is a port; none on a
 * hardware-reduced machine. A rate more than 0.5% from the boot's is not
 * trusted (logged, the boot's kept). */
#define PM_TIMER_HZ 3579545ULL
#define FADT_TMR_VAL_EXT (1u << 8)

static uint16_t pm_timer_port(uint32_t *mask)
{
    const struct acpi_fadt *f = acpi_fadt();
    if (!f || (f->flags & FADT_HARDWARE_REDUCED))
        return 0;
    *mask = f->flags & FADT_TMR_VAL_EXT ? 0xFFFFFFFFu : 0xFFFFFFu;
    if (f->pm_timer_block && f->pm_timer_block <= 0xFFFF)
        return (uint16_t)f->pm_timer_block;
    const uint8_t *t = (const uint8_t *)f;          /* X_PM_TMR_BLK: a GAS at 208 */
    uint64_t address;
    if (f->header.length < 220 || t[208] != 1)       /* (space 1: system I/O) */
        return 0;
    memcpy(&address, t + 212, 8);
    return address && address <= 0xFFFF ? (uint16_t)address : 0;
}

static void pm_sample(uint16_t port, uint32_t mask, uint64_t *tsc, uint32_t *pm)
{
    uint64_t best = ~0ULL;
    for (int i = 0; i < 16; i++) {
        uint64_t state = arch_irq_save();
        uint64_t before = arch_counter();
        uint32_t value = inl(port) & mask;
        uint64_t after = arch_counter();
        arch_irq_restore(state);
        if (after - before < best) {
            best = after - before;
            *tsc = before + (after - before) / 2;
            *pm = value;
        }
    }
}

static void tscrate(void *arg)
{
    (void)arg;
    uint32_t mask = 0;
    uint16_t port = pm_timer_port(&mask);
    uint64_t boot = tsc_hz;
    if (!port || !boot) {
        kprintf("  time stamp counter: %lu Hz, as measured at the boot (no ACPI PM timer)\n", boot);
        return;
    }
    timer_sleep_ms(1000);                           /* (the boot's busiest second first) */
    uint64_t t0, t1;
    uint32_t p0, p1;
    pm_sample(port, mask, &t0, &p0);
    timer_sleep_ms(2000);                           /* (24 bits wrap after 4.7 s) */
    pm_sample(port, mask, &t1, &p1);
    uint64_t pm = (p1 - p0) & mask;
    uint64_t hz = pm ? (t1 - t0) * PM_TIMER_HZ / pm : 0;
    bool up = hz >= boot;
    uint64_t off = up ? hz - boot : boot - hz;
    uint64_t tenths = off * 10000000ULL / boot;     /* tenths of a ppm */
    if (!hz || tenths > 50000) {
        kprintf("  time stamp counter: %lu Hz, as measured at the boot (the ACPI PM timer "
                "says %lu Hz: not trusted)\n", boot, hz);
        return;
    }
    uint64_t state = arch_irq_save();               /* (the readings around the change only) */
    uint64_t c0 = arch_counter(), before = uefi_clock_monotonic_ns();
    tsc_hz = hz;
    x86_tick_counter_hz(hz);
    uefi_clock_rate(hz);
    uint64_t after = uefi_clock_monotonic_ns(), c1 = arch_counter();
    arch_irq_restore(state);
    /* No jump: the clock went on by what the change took (the counter's
     * counts at the new rate), within the readings' own nanoseconds. */
    uint64_t took = (c1 - c0) * 1000000000ULL / hz;
    uint64_t went = after - before;
    kprintf("  time stamp counter: %lu Hz, refined against the ACPI PM timer (%c%lu.%lu ppm "
            "from the boot's); over the change the clock went on %lu ns in %lu ns\n", hz,
            up ? '+' : '-', tenths / 10, tenths % 10, went, took);
}

void arch_counter_refine_start(void)
{
    if (!thread_create("tscrate", tscrate, 0))
        kprintf("  time stamp counter: no thread to measure its rate again\n");
}

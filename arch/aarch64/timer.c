/*
 * Delays with the ARM generic timer: every core has a 64-bit counter that
 * runs at a fixed frequency (CNTFRQ_EL0), readable without any setup.
 */
#include "arch.h"
#include "cpu.h"
#include "syscalls.h"

void arch_delay_ms(uint32_t ms)
{
    uint64_t frequency = read_sysreg(CNTFRQ_EL0);
    isb();
    uint64_t start = read_sysreg(CNTVCT_EL0);
    uint64_t ticks = frequency * ms / 1000;
    while (read_sysreg(CNTVCT_EL0) - start < ticks)
        arch_cpu_relax();
}

/* (68u) CNTFRQ_EL0 is the counter's own rate: nothing to measure again. */
void arch_counter_refine_start(void)
{
}

uint64_t arch_counter(void)
{
    isb();                                  /* not ahead of earlier instructions */
    return read_sysreg(CNTVCT_EL0);
}

uint64_t arch_counter_hz(void)
{
    return read_sysreg(CNTFRQ_EL0);
}

/* (68p) Programs read CNTVCT_EL0 too: each core lets them (CNTKCTL_EL1's
 * EL0VCTEN, set as its timer starts, gic.c). */
uint32_t arch_counter_user(uint64_t *offset)
{
    *offset = 0;
    return SYS_CLOCK_CNTVCT;
}

bool arch_clock_counts(uint64_t *actual, uint64_t *base)
{
    (void)actual, (void)base;
    return false;                       /* (the activity monitors are optional, and not read) */
}

bool arch_clock_set(enum arch_clock mode, char *out, size_t size)
{
    (void)mode, (void)out, (void)size;
    return false;                       /* (64t) the firmware's clock (PSCI has no say in it) */
}

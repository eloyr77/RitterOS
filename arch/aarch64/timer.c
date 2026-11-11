/*
 * Delays with the ARM generic timer: every core has a 64-bit counter that
 * runs at a fixed frequency (CNTFRQ_EL0), readable without any setup.
 */
#include "arch.h"
#include "cpu.h"

void arch_delay_ms(uint32_t ms)
{
    uint64_t frequency = read_sysreg(CNTFRQ_EL0);
    isb();
    uint64_t start = read_sysreg(CNTVCT_EL0);
    uint64_t ticks = frequency * ms / 1000;
    while (read_sysreg(CNTVCT_EL0) - start < ticks)
        arch_cpu_relax();
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

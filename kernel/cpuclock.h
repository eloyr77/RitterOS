/* The processor's clock setting (64t): /system/config/cpu. */
#ifndef RITTEROS_CPUCLOCK_H
#define RITTEROS_CPUCLOCK_H

/* Read /system/config/cpu from the system's file system (mounted) and
 * apply its `clock` (performance when the file or the key is missing) on
 * every core; logs one line, also given to out (a program's: SYS_POWER's
 * POWER_CLOCK) when not 0. After the root mount, and (69a) again when asked. */
void cpu_clock_load(void (*out)(const char *line, void *ctx), void *ctx);

/* (69a) This core applies the kept mode (nothing before the first load, or
 * with clock = firmware): a core at its start, after a sleep, and when
 * cpus_apply_clock asks. Interrupts may be off; it takes no lock. */
void cpu_clock_core(void);

#endif

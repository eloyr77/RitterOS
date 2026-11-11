/* The processor's clock setting (64t): /system/config/cpu. */
#ifndef RITTEROS_CPUCLOCK_H
#define RITTEROS_CPUCLOCK_H

/* Read /system/config/cpu from the system's file system (mounted) and
 * apply its `clock` (performance when the file or the key is missing);
 * logs one line. Called once, after the root mount. */
void cpu_clock_load(void);

#endif

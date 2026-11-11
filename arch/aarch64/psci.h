/* AArch64: PSCI calls into the firmware or hypervisor (power.c). */
#ifndef RITTEROS_PSCI_H
#define RITTEROS_PSCI_H

#include <stdbool.h>
#include <stdint.h>

#define PSCI_CPU_ON         0xC4000003u     /* (65b) target MPIDR, entry, context */

/* Is PSCI there (the FADT's ARM boot flags)? Chooses HVC or SMC. Quiet. */
bool psci_ready(void);

/* A call with up to three arguments; PSCI's result (negative: an error). */
int64_t psci_call3(uint32_t function, uint64_t a1, uint64_t a2, uint64_t a3);

#endif

/* Device register access for AArch64 (see kernel/arch_api.h); also used by the bootloader. */
#ifndef RITTEROS_MMIO_H
#define RITTEROS_MMIO_H

#include <stdint.h>

/*
 * Device registers. A hypervisor emulates a device access from the fault's
 * syndrome, which ARM only provides for simple loads and stores: not for
 * pairs (ldp/stp) or forms that also update the address register, which a
 * compiler might pick for a plain C access. So these use fixed instructions.
 */
#define MMIO_READ(bits, insn, reg)                                              \
    static inline uint##bits##_t mmio_read##bits(const volatile void *a)        \
    {                                                                           \
        uint##bits##_t v;                                                       \
        __asm__ volatile (insn " %" reg "0, [%1]" : "=r"(v) : "r"(a) : "memory"); \
        return v;                                                               \
    }
#define MMIO_WRITE(bits, insn, reg)                                             \
    static inline void mmio_write##bits(volatile void *a, uint##bits##_t v)     \
    {                                                                           \
        __asm__ volatile (insn " %" reg "0, [%1]" : : "r"(v), "r"(a) : "memory"); \
    }
MMIO_READ(8, "ldrb", "w")
MMIO_READ(16, "ldrh", "w")
MMIO_READ(32, "ldr", "w")
MMIO_READ(64, "ldr", "x")
MMIO_WRITE(8, "strb", "w")
MMIO_WRITE(16, "strh", "w")
MMIO_WRITE(32, "str", "w")
MMIO_WRITE(64, "str", "x")
#undef MMIO_READ
#undef MMIO_WRITE

#endif

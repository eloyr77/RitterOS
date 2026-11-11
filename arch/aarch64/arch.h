/* AArch64 implementation of the architecture interface (kernel/arch_api.h). */
#ifndef RITTEROS_ARCH_H
#define RITTEROS_ARCH_H

#include "arch_api.h"
#include "mmio.h"

#define ARCH_NAME "aarch64"
#define ARCH_RETURN_INSTRUCTION { 0xC0, 0x03, 0x5F, 0xD6 }   /* RET (x30) */

static inline void arch_cpu_relax(void)
{
    __asm__ volatile ("yield");
}

/* ARM may reorder memory accesses: the data synchronization barrier makes
 * earlier writes complete (and visible to devices) before later ones. */
static inline void arch_write_barrier(void)
{
    __asm__ volatile ("dsb st" ::: "memory");
}

static inline void arch_read_barrier(void)
{
    __asm__ volatile ("dsb ld" ::: "memory");
}

/* Clear the I bit of DAIF: IRQs are taken. */
static inline void arch_interrupts_enable(void)
{
    __asm__ volatile ("msr daifclr, #2" ::: "memory");
}

static inline void arch_interrupts_disable(void)
{
    __asm__ volatile ("msr daifset, #2" ::: "memory");
}

/* WFI wakes up when an interrupt is pending, even a masked one; unmasking
 * afterwards lets it be taken. */
static inline void arch_wait_for_interrupt(void)
{
    __asm__ volatile ("wfi; msr daifclr, #2" ::: "memory");
}

#define ARCH_FPU_STATE_SIZE 528             /* V0-V31, FPCR, FPSR */

/* Disable interrupts and return the previous mask (DAIF). */
static inline uint64_t arch_irq_save(void)
{
    uint64_t daif;
    __asm__ volatile ("mrs %0, daif; msr daifset, #2" : "=r"(daif) : : "memory");
    return daif;
}

static inline void arch_irq_restore(uint64_t state)
{
    __asm__ volatile ("msr daif, %0" : : "r"(state) : "memory");
}

/* (65a) This core's block (kernel/percpu.h): TPIDR_EL1, which programs
 * cannot read or write. */
struct cpu;
static inline struct cpu *arch_this_cpu(void)
{
    struct cpu *c;
    __asm__ volatile ("mrs %0, tpidr_el1" : "=r"(c));
    return c;
}

static inline void arch_set_this_cpu(struct cpu *c)
{
    __asm__ volatile ("msr tpidr_el1, %0" : : "r"(c) : "memory");
}

#endif

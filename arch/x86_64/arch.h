/* x86-64 implementation of the architecture interface (kernel/arch_api.h). */
#ifndef RITTEROS_ARCH_H
#define RITTEROS_ARCH_H

#include "arch_api.h"
#include "mmio.h"

#define ARCH_NAME "x86_64"
#define ARCH_RETURN_INSTRUCTION { 0xC3 }        /* RET */

static inline void arch_cpu_relax(void)
{
    __asm__ volatile ("pause");
}

/* x86 keeps stores in order (also towards uncacheable device memory); the
 * fence also orders them against the write-combining kind. */
static inline void arch_write_barrier(void)
{
    __asm__ volatile ("sfence" ::: "memory");
}

/* x86 does not reorder loads with other loads: only the compiler must not. */
static inline void arch_read_barrier(void)
{
    __asm__ volatile ("" ::: "memory");
}

static inline void arch_interrupts_enable(void)
{
    __asm__ volatile ("sti" ::: "memory");
}

static inline void arch_interrupts_disable(void)
{
    __asm__ volatile ("cli" ::: "memory");
}

/* STI takes effect only after the next instruction, so no interrupt can
 * slip in between it and HLT. */
static inline void arch_wait_for_interrupt(void)
{
    __asm__ volatile ("sti; hlt" ::: "memory");
}

#define ARCH_FPU_STATE_SIZE 832             /* XSAVE area with AVX (FXSAVE uses 512) */

/* Disable interrupts and return whether they were enabled (RFLAGS.IF). */
/* timer.c: the time stamp counter's frequency, measured by apic.c. */
void timer_set_counter_hz(uint64_t hz);

/* (37g-1) Sleep's pieces in the x86 code: the counter carried over
 * (timer.c), the serial line set up again (uart.c) and receiving
 * (uart_input.c). */
void x86_counter_continue(uint64_t now);
void x86_serial_setup_again(void);
void x86_serial_wake(void);
/* (37g-5d) Stopping for a reset (smp.c): this core halted for good, the
 * count of those halted, a reboot moved to the boot processor; the reset
 * itself (power.c) on the boot processor with interrupts off. */
__attribute__((noreturn)) void x86_core_stopped(void);
int x86_cores_stopped(void);
__attribute__((noreturn)) void x86_reboot_cores(void);
__attribute__((noreturn)) void x86_reset_machine(void);

static inline uint64_t arch_irq_save(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags & (1u << 9);
}

static inline void arch_irq_restore(uint64_t state)
{
    if (state)
        __asm__ volatile ("sti" ::: "memory");
}

/* (65a) This core's block (kernel/percpu.h): the GS base holds it while
 * the kernel runs (the entry code swaps it with the program's). */
struct cpu;
static inline struct cpu *arch_this_cpu(void)
{
    struct cpu *c;
    __asm__ volatile ("movq %%gs:0, %0" : "=r"(c));
    return c;
}

/* Make c this core's block: the GS base, and the program's (0) behind it. */
void arch_set_this_cpu(struct cpu *c);

#endif

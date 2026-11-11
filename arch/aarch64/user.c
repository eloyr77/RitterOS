/*
 * User mode on AArch64: programs run at EL0. System calls use the SVC
 * instruction, which raises a synchronous exception from EL0 (vectors.S,
 * exceptions.c). Entering user mode: user.S.
 */
#include "arch.h"
#include "cpu.h"
#include "kprintf.h"
#include "string.h"

#define CPACR_FPEN      (3ULL << 20)    /* FP/SIMD not trapped at EL0 and EL1 */

void arch_user_init(void)
{
    write_sysreg(CPACR_EL1, read_sysreg(CPACR_EL1) | CPACR_FPEN);
    isb();
    kprintf("  user mode:     EL0; system calls by SVC, exceptions from EL0 on the "
            "program's kernel stack\n");
    kprintf("                 FP/SIMD: V0-V31, FPCR, FPSR (%u bytes per program)\n",
            ARCH_FPU_STATE_SIZE);
}

void arch_fpu_clean(void *area)
{
    memset(area, 0, ARCH_FPU_STATE_SIZE);   /* registers 0, FPCR 0: round to nearest */
}

void arch_set_kernel_stack(uint64_t kernel_stack_top)
{
    (void)kernel_stack_top;                 /* the EL0 entry and return code keep SP_EL1 right */
}

void arch_code_written(const void *addr, uint64_t size)
{
    /* The code went through the data cache: write those lines out to the
     * point where instruction fetches see them, then drop stale ones from
     * the instruction cache. */
    uint64_t line = 4ULL << ((read_sysreg(CTR_EL0) >> 16) & 0xF);
    uint64_t start = (uint64_t)addr & ~(line - 1);
    for (uint64_t a = start; a < (uint64_t)addr + size; a += line)
        __asm__ volatile ("dc cvau, %0" : : "r"(a) : "memory");
    dsb_ish();
    __asm__ volatile ("ic ialluis" ::: "memory");   /* (65c-2) every core's */
    dsb_ish();
    isb();
}

uint64_t arch_thread_pointer(void)
{
    return read_sysreg(TPIDR_EL0);
}

void arch_set_thread_pointer(uint64_t tp)
{
    write_sysreg(TPIDR_EL0, tp);
}

uint64_t arch_user_segments(void)
{
    return 0;                               /* (65c-3) nothing such on ARM */
}

void arch_set_user_segments(uint64_t segments)
{
    (void)segments;
}

/* --- threads ------------------------------------------------------------------ */

extern void thread_start(void);

uint64_t arch_thread_init(uint64_t stack_top, void (*fn)(void *arg), void *arg)
{
    /* What arch_context_switch restores: X19 (fn), X20 (arg), X21-X28,
     * X29 (frame pointer: 0), X30 (where ret goes: thread_start). */
    uint64_t *sp = (uint64_t *)(stack_top - 96);
    for (int i = 0; i < 12; i++)
        sp[i] = 0;
    sp[0] = (uint64_t)fn;
    sp[1] = (uint64_t)arg;
    sp[11] = (uint64_t)thread_start;
    return (uint64_t)sp;
}

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

/* An ID register's 4-bit field at bit `at`. */
static unsigned field(uint64_t reg, int at)
{
    return (unsigned)(reg >> at) & 0xF;
}

/* (43b-6) Linux's HWCAP_* bits (arch/arm64's uapi hwcap.h), from the ID
 * registers as its cpufeature.c reads them. */
void arch_hwcaps(uint64_t *hwcap, uint64_t *hwcap2)
{
    uint64_t pfr0 = read_sysreg(id_aa64pfr0_el1), isar0 = read_sysreg(id_aa64isar0_el1);
    uint64_t isar1 = read_sysreg(id_aa64isar1_el1), h = 0;
    unsigned fp = field(pfr0, 16), simd = field(pfr0, 20);
    if (fp != 0xF)
        h |= 1ULL << 0 | (fp >= 1 ? 1ULL << 9 : 0);             /* FP, FPHP */
    if (simd != 0xF)
        h |= 1ULL << 1 | (simd >= 1 ? 1ULL << 10 : 0);          /* ASIMD, ASIMDHP */
    if (field(isar0, 4) >= 1)
        h |= 1ULL << 3 | (field(isar0, 4) >= 2 ? 1ULL << 4 : 0);    /* AES, PMULL */
    if (field(isar0, 8) >= 1)
        h |= 1ULL << 5;                                         /* SHA1 */
    if (field(isar0, 12) >= 1)
        h |= 1ULL << 6 | (field(isar0, 12) >= 2 ? 1ULL << 21 : 0);  /* SHA2, SHA512 */
    if (field(isar0, 16) >= 1)
        h |= 1ULL << 7;                                         /* CRC32 */
    if (field(isar0, 20) >= 2)
        h |= 1ULL << 8;                                         /* ATOMICS */
    if (field(isar0, 28) >= 1)
        h |= 1ULL << 12;                                        /* ASIMDRDM */
    if (field(isar0, 32) >= 1)
        h |= 1ULL << 17;                                        /* SHA3 */
    if (field(isar0, 36) >= 1)
        h |= 1ULL << 18;                                        /* SM3 */
    if (field(isar0, 40) >= 1)
        h |= 1ULL << 19;                                        /* SM4 */
    if (field(isar0, 44) >= 1)
        h |= 1ULL << 20;                                        /* ASIMDDP */
    if (field(isar0, 48) >= 1)
        h |= 1ULL << 23;                                        /* ASIMDFHM */
    if (field(isar1, 0) >= 1)
        h |= 1ULL << 16;                                        /* DCPOP */
    if (field(isar1, 12) >= 1)
        h |= 1ULL << 13;                                        /* JSCVT */
    if (field(isar1, 16) >= 1)
        h |= 1ULL << 14;                                        /* FCMA */
    if (field(isar1, 20) >= 1)
        h |= 1ULL << 15 | (field(isar1, 20) >= 2 ? 1ULL << 26 : 0);     /* LRCPC, ILRCPC */
    *hwcap = h;
    *hwcap2 = 0;
}

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

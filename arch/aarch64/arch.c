/* AArch64: CPU setup, halting and the ARM-specific crash tests. */
#include "arch.h"
#include "cpu.h"
#include "crashtest.h"
#include "kprintf.h"
#include "percpu.h"

extern char exception_vectors[], exception_stack[], exception_stack_top[];

const char arch_crash_kinds[] = "ud align pc sp";

#define SCTLR_A  (1ULL << 1)            /* alignment check of every access */
#define SCTLR_SA (1ULL << 3)            /* SP alignment check at EL1 */

void arch_cpu_init(void)
{
    /* Where the CPU goes on an exception: our vector table. */
    write_sysreg(VBAR_EL1, (uint64_t)exception_vectors);
    /* (65c-2) The entry from EL0 finds this core's exception stack in its
     * block (vectors.S). */
    arch_this_cpu()->exception_stack = (uint64_t)exception_stack_top;
    /* Fault on loads and stores through a misaligned stack pointer: the ABI
     * keeps SP 16-byte aligned, so a misaligned one means a broken stack. */
    write_sysreg(SCTLR_EL1, read_sysreg(SCTLR_EL1) | SCTLR_SA);
    isb();
    kprintf("  exceptions:    vector table %p (VBAR_EL1), running at EL%u\n",
            (void *)exception_vectors, current_el());
    kprintf("                 kernel stack SP_EL0, exception stack SP_EL1 %p - %p\n",
            (void *)exception_stack, (void *)(exception_stack_top - 1));
}

void arch_exception_selftest(void)
{
    /* BRK raises a breakpoint exception; the handler reports it and returns. */
    kprintf("  testing exceptions: executing brk\n");
    __asm__ volatile ("brk #0");
    kprintf("  back from the breakpoint handler\n");
}

void arch_halt(void)
{
    __asm__ volatile ("msr daifset, #0xf");
    for (;;)
        __asm__ volatile ("wfe");
}

static bool is(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static uint64_t align_target[2];

bool arch_crash_test(const char *kind)
{
    if (is(kind, "ud")) {
        crash_announce("executing an undefined instruction (udf)");
        __asm__ volatile ("udf #0");
    } else if (is(kind, "align")) {
        /* Normally misaligned loads just work (even exclusive ones on CPUs
         * with FEAT_LSE2); strict alignment checking makes them fault. */
        crash_announce("misaligned load with alignment checking on (SCTLR_EL1.A)");
        write_sysreg(SCTLR_EL1, read_sysreg(SCTLR_EL1) | SCTLR_A);
        isb();
        uint64_t value;
        __asm__ volatile ("ldr %0, [%1]" : "=r"(value) : "r"((char *)align_target + 1) : "memory");
        (void)value;
    } else if (is(kind, "pc")) {
        /* Instructions are 4 bytes, aligned: a branch to address 2 mod 4 faults. */
        crash_announce("branching to a misaligned address");
        __asm__ volatile ("adr x9, 1f\n\tadd x9, x9, #2\n\tbr x9\n1:\tnop" ::: "x9");
    } else if (is(kind, "sp")) {
        /* The kernel stack (SP_EL0) is broken; the handler runs on SP_EL1. */
        crash_announce("loading through a misaligned stack pointer");
        __asm__ volatile ("mov x9, sp\n\tsub x9, x9, #4\n\tmov sp, x9\n\tldr x10, [sp]"
                          ::: "x9", "x10", "memory");
    } else {
        return false;
    }
    return true;
}

/* QEMU's virt machine and ARM PCs have no PS/2 keyboard controller. */
void arch_input_init(void)
{
}

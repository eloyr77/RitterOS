/* x86-64: CPU setup, halting and the x86-specific crash tests. */
#include "arch.h"
#include "crashtest.h"
#include "gdt.h"
#include "idt.h"
#include "kprintf.h"
#include "percpu.h"
#include <stdint.h>


const char arch_crash_kinds[] = "ud de gp df";

void arch_cpu_init(void)
{
    gdt_init();
    arch_set_this_cpu(&boot_cpu);           /* (65a) loading GS above cleared its base */
    idt_init();
}

void arch_exception_selftest(void)
{
    /* INT3 raises #BP; the handler reports it and returns here. */
    kprintf("  testing exceptions: executing int3\n");
    __asm__ volatile ("int3");
    kprintf("  back from the breakpoint handler\n");
}

void arch_halt(void)
{
    for (;;)
        __asm__ volatile ("cli; hlt");
}

static bool is(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

bool arch_crash_test(const char *kind)
{
    if (is(kind, "ud")) {
        crash_announce("executing an invalid instruction (ud2)");
        __asm__ volatile ("ud2");
    } else if (is(kind, "de")) {
        crash_announce("dividing by zero");
        volatile uint64_t zero = 0;   /* volatile: keep the compiler from folding it */
        uint64_t result;
        __asm__ volatile ("divq %2" : "=a"(result) : "a"(1ULL), "r"(zero), "d"(0ULL));
        (void)result;
    } else if (is(kind, "gp")) {
        /* One past the GDT's last slot (65c: two per core's TSS). */
        uint16_t bad = gdt_size();
        char what[64];
        ksnprintf(what, sizeof(what), "loading segment selector 0x%x, which is beyond the GDT",
                  bad);
        crash_announce(what);
        __asm__ volatile ("movw %0, %%ds" : : "r"(bad));
    } else if (is(kind, "df")) {
        /*
         * Point RSP at unmapped memory and push. The push page-faults, but
         * delivering #PF needs the same broken stack, so the CPU raises a
         * double fault, whose IDT gate switches to the IST1 stack.
         */
        crash_announce("pushing onto a stack in unmapped memory");
        __asm__ volatile ("movq %0, %%rsp\n\tpushq $0" : : "r"(CRASH_UNMAPPED_ADDRESS + 0x1000));
    } else {
        return false;
    }
    return true;
}

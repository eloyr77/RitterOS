#include "exceptions.h"
#include "apic.h"
#include "console.h"
#include "cpu.h"
#include "kprintf.h"
#include "percpu.h"
#include "panic.h"
#include "process.h"
#include "syscalls.h"
#include "sched.h"

#define VEC_BREAKPOINT    3
#define VEC_DOUBLE_FAULT  8
#define VEC_PAGE_FAULT    14
#define SYSCALL_VECTOR    256           /* user.S: syscall_entry */

void syscall_handler(struct interrupt_frame *frame);

static const char *const exception_names[32] = {
    "divide error (#DE)",          "debug (#DB)",
    "non-maskable interrupt",      "breakpoint (#BP)",
    "overflow (#OF)",              "bound range exceeded (#BR)",
    "invalid opcode (#UD)",        "device not available (#NM)",
    "double fault (#DF)",          "coprocessor segment overrun",
    "invalid TSS (#TS)",           "segment not present (#NP)",
    "stack-segment fault (#SS)",   "general protection fault (#GP)",
    "page fault (#PF)",            "reserved (15)",
    "x87 floating-point (#MF)",    "alignment check (#AC)",
    "machine check (#MC)",         "SIMD floating-point (#XM)",
    "virtualization (#VE)",        "control protection (#CP)",
    "reserved (22)",               "reserved (23)",
    "reserved (24)",               "reserved (25)",
    "reserved (26)",               "reserved (27)",
    "hypervisor injection (#HV)",  "VMM communication (#VC)",
    "security (#SX)",              "reserved (31)",
};

/* Explain the error code of a page fault (Intel SDM vol. 3, 4.7). */
static void explain_page_fault(uint64_t error)
{
    kprintf("%s%s%s%s%s",
            error & 1 ? "protection violation" : "page not present",
            error & 2 ? ", write" : ", read",
            error & 16 ? " (instruction fetch)" : "",
            error & 4 ? ", user mode" : ", kernel mode",
            error & 8 ? ", reserved bit set in page table" : "");
}

/* Explain a segment-selector error code (#TS, #NP, #SS, #GP). */
static void explain_selector(uint64_t error)
{
    static const char *const tables[4] = { "GDT", "IDT", "LDT", "IDT" };
    if (error == 0) {
        kprintf("no selector involved");
        return;
    }
    kprintf("%s entry %lu%s", tables[(error >> 1) & 3], (error >> 3) & 0x1FFF,
            error & 1 ? " (during an external event)" : "");
}

/* An exception in a program (ring 3): the program ends, the kernel goes on. */
static void user_exception(struct interrupt_frame *f)
{
    char what[96];
    uint64_t address = ~0ULL;
    if (f->vector == VEC_PAGE_FAULT) {
        address = read_cr2();
        /* A page of a mapping that is made when touched (22e): made, and the
         * program goes on. Interrupts on meanwhile, as for a system call. */
        __asm__ volatile ("sti");
        int fault = process_page_fault(address, f->error_code & 2, f->error_code & 16);
        __asm__ volatile ("cli");
        if (fault == 0) {
            process_before_return(f);
            sched_preempt_point(true);
            return;
        }
        if (fault == SIGBUS && process_fault_signal(f, SIGBUS))
            return;                         /* (22r: its handler runs) */
        if (fault == SIGBUS)
            process_fault("bus error (past the end of a mapped file)", f->rip, address, SIGBUS);
        ksnprintf(what, sizeof(what), "page fault (%s%s)",
                  f->error_code & 1 ? "access not allowed" : "page not present",
                  f->error_code & 16 ? ", instruction fetch"
                                     : f->error_code & 2 ? ", write" : ", read");
    } else {
        ksnprintf(what, sizeof(what), "CPU exception %lu, %s", f->vector,
                  exception_names[f->vector]);
    }
    /* As on Unix: invalid instructions SIGILL, division SIGFPE, the rest
     * (page faults, protection errors) SIGSEGV. */
    int signal = f->vector == 6 ? SIGILL : f->vector == 0 ? SIGFPE : SIGSEGV;
    if (process_fault_signal(f, signal))
        return;                             /* (22r: its handler runs) */
    process_fault(what, f->rip, address, signal);
}

extern volatile bool x86_cores_stopping;    /* smp.c */

void exception_handler(struct interrupt_frame *f)
{
    if (f->vector == 2 && x86_cores_stopping)   /* (65c) another core stops us */
        x86_core_stopped();
    if (f->vector == SYSCALL_VECTOR) {
        syscall_handler(f);
        return;
    }
    if (f->vector >= 32) {
        sched_irq_enter();
        apic_interrupt(f->vector);      /* an interrupt, not an exception */
        sched_irq_exit();
        if ((f->cs & 3) == 3)
            process_before_return(f);   /* it interrupted a program */
        sched_preempt_point(f->rflags & (1u << 9));
        return;
    }
    if ((f->cs & 3) == 3) {
        user_exception(f);          /* returns only for a page it made (22e) */
        return;
    }
    if (f->vector == VEC_BREAKPOINT) {
        /* A trap: RIP already points after the INT3, so we can just return. */
        kprintf("RitterOS: breakpoint (#BP) at RIP %p, continuing\n", (void *)f->rip);
        return;
    }

    uint64_t cr2 = read_cr2();      /* read first: a later fault would overwrite it */
    kprintf_emergency();

    /* Make sure the crash is visible, even if it happened before the console
     * was shown. */
    if (!console_is_shown())
        console_show();

    console_set_color(COLOR_ERROR);
    console_set_bold(true);
    kprintf("\n=== RitterOS crash: CPU exception %lu, %s ===\n", f->vector,
            f->vector < 32 ? exception_names[f->vector] : "unknown");
    console_set_bold(false);

    kprintf("  error code 0x%04lX: ", f->error_code);
    if (f->vector == VEC_PAGE_FAULT) {
        explain_page_fault(f->error_code);
        kprintf("\n  fault address (CR2) %p", (void *)cr2);
    } else if (f->vector == 10 || f->vector == 11 || f->vector == 12 || f->vector == 13) {
        explain_selector(f->error_code);
    } else if (f->vector == VEC_DOUBLE_FAULT) {
        /* Often a page fault that could not be delivered: CR2 says where. */
        kprintf("-\n  last page fault address (CR2) %p", (void *)cr2);
    } else {
        kprintf("-");
    }
    kprintf("\n");

    console_set_color(COLOR_TEXT);
    kprintf("  RIP    0x%016lX  CS  0x%016lX  RFLAGS 0x%016lX\n", f->rip, f->cs, f->rflags);
    kprintf("  RSP    0x%016lX  SS  0x%016lX  RBP    0x%016lX\n", f->rsp, f->ss, f->rbp);
    kprintf("  RAX    0x%016lX  RBX 0x%016lX  RCX    0x%016lX\n", f->rax, f->rbx, f->rcx);
    kprintf("  RDX    0x%016lX  RSI 0x%016lX  RDI    0x%016lX\n", f->rdx, f->rsi, f->rdi);
    kprintf("  R8     0x%016lX  R9  0x%016lX  R10    0x%016lX\n", f->r8, f->r9, f->r10);
    kprintf("  R11    0x%016lX  R12 0x%016lX  R13    0x%016lX\n", f->r11, f->r12, f->r13);
    kprintf("  R14    0x%016lX  R15 0x%016lX\n", f->r14, f->r15);
    kprintf("  CR0    0x%016lX  CR3 0x%016lX  CR4    0x%016lX\n",
            read_cr0(), read_cr3(), read_cr4());

    uint64_t handler_rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(handler_rsp));
    kprintf("  handler running on stack %p\n", (void *)handler_rsp);
    struct thread *running = arch_this_cpu()->current;    /* (65c-3) on several cores */
    kprintf("  core %d, thread '%s'\n", arch_this_cpu()->index, running ? running->name : "?");

    console_set_color(COLOR_ERROR);
    kprintf("System halted.\n");
    console_set_status(STATUS_CRASH);
    halt_forever();
}

#include "exceptions.h"
#include "gic.h"
#include "console.h"
#include "cpu.h"
#include "kprintf.h"
#include "percpu.h"
#include "panic.h"
#include "process.h"
#include "syscalls.h"
#include "sched.h"
#include "serial.h"

/* Exception classes: ESR_EL1 bits 31-26. */
#define EC_UNKNOWN          0x00        /* e.g. an undefined instruction */
#define EC_INSTR_ABORT_LOW  0x20
#define EC_INSTR_ABORT      0x21        /* instruction fetch fault at EL1 */
#define EC_PC_ALIGNMENT     0x22
#define EC_DATA_ABORT_LOW   0x24
#define EC_DATA_ABORT       0x25        /* data access fault at EL1 */
#define EC_SP_ALIGNMENT     0x26
#define EC_SVC              0x15        /* SVC instruction: a system call */
#define EC_SERROR           0x2F
#define EC_BRK              0x3C        /* BRK instruction */

#define SCTLR_A             (1ULL << 1) /* strict alignment checking */

static bool reporting;                  /* a crash report is being printed */

static const char *const vector_names[16] = {
    "synchronous", "IRQ", "FIQ", "SError",                       /* EL1, SP_EL0 */
    "synchronous (in handler)", "IRQ (in handler)", "FIQ (in handler)", "SError (in handler)",
    "synchronous from EL0", "IRQ from EL0", "FIQ from EL0", "SError from EL0",
    "synchronous from AArch32", "IRQ from AArch32", "FIQ from AArch32", "SError from AArch32",
};

static const char *class_name(unsigned ec)
{
    switch (ec) {
    case EC_UNKNOWN:         return "undefined instruction";
    case 0x01:               return "WFI/WFE trapped";
    case 0x07:               return "FP/SIMD access trapped";
    case 0x15:               return "system call (SVC)";
    case 0x18:               return "system register access trapped";
    case EC_INSTR_ABORT_LOW:
    case EC_INSTR_ABORT:     return "instruction abort";
    case EC_PC_ALIGNMENT:    return "PC alignment fault";
    case EC_DATA_ABORT_LOW:
    case EC_DATA_ABORT:      return "data abort";
    case EC_SP_ALIGNMENT:    return "SP alignment fault";
    case EC_SERROR:          return "SError (system error)";
    case EC_BRK:             return "breakpoint (BRK)";
    default:                 return "other exception";
    }
}

/* Explain the fault status code of an abort (ESR_EL1 bits 5-0). */
static void explain_abort(char *out, size_t size, uint64_t esr, bool instruction)
{
    unsigned fsc = esr & 0x3F;
    unsigned level = fsc & 3;
    const char *access = instruction ? ", instruction fetch" : (esr >> 6) & 1 ? ", write" : ", read";
    if (fsc >= 0x04 && fsc <= 0x07)
        ksnprintf(out, size, "page not present (translation fault, level %u)%s", level, access);
    else if (fsc >= 0x09 && fsc <= 0x0B)
        ksnprintf(out, size, "access flag fault, level %u%s", level, access);
    else if (fsc >= 0x0D && fsc <= 0x0F)
        ksnprintf(out, size, "protection violation (permission fault, level %u)%s", level,
                  access);
    else if (fsc == 0x21)
        ksnprintf(out, size, "alignment fault%s", access);
    else if (fsc == 0x10)
        ksnprintf(out, size, "external abort%s", access);
    else
        ksnprintf(out, size, "fault status 0x%02x%s", fsc, access);
}

static bool is_abort(unsigned ec)
{
    return ec == EC_DATA_ABORT || ec == EC_DATA_ABORT_LOW || ec == EC_INSTR_ABORT ||
           ec == EC_INSTR_ABORT_LOW;
}

/* An exception from a program (EL0): a system call, or a fault that ends
 * the program while the kernel goes on. */
static void user_exception(struct exception_frame *f, unsigned ec)
{
    /* (65c-3) The frame is on the running thread's own kernel stack (SP_EL1
     * was set to it at the last return to EL0, on this core). */
    struct thread *t = thread_current();
    uint64_t top = thread_stack_top(t);
    if ((uint64_t)f >= top || (uint64_t)f < top - 32 * 1024)
        panic("an exception from EL0 on core %d saved its frame at %p, outside thread '%s''s "
              "stack (top %p)", arch_this_cpu()->index, (void *)f, t->name, (void *)top);
    if (f->vector == 8 && ec == EC_SVC && f->x[8] == SYS_SIGRETURN) {
        process_sigreturn(f);               /* (22r) every register comes back: no result */
        process_before_return(f);
        sched_preempt_point(true);
        return;
    }
    if (f->vector == 8 && ec == EC_SVC && f->x[8] == SYS_FORK) {
        struct exception_frame child = *f;  /* (56a) the child gets a copy of the frame */
        child.x[0] = 0;                     /* (fork returns 0 there) */
        __asm__ volatile ("msr daifclr, #2");
        int64_t result = process_fork(&child, sizeof(child));
        __asm__ volatile ("msr daifset, #2");
        f->x[0] = (uint64_t)result;
        process_before_return(f);
        sched_preempt_point(true);
        return;
    }
    if (f->vector == 8 && ec == EC_SVC) {
        const uint64_t args[6] = { f->x[0], f->x[1], f->x[2], f->x[3], f->x[4], f->x[5] };
        __asm__ volatile ("msr daifclr, #2");       /* interrupts on while the kernel works */
        int64_t result = process_syscall(f->x[8], args);
        __asm__ volatile ("msr daifset, #2");
        f->x[0] = (uint64_t)result;
        process_before_return(f);
        sched_preempt_point(true);
        return;
    }
    if (ec == EC_DATA_ABORT_LOW || ec == EC_INSTR_ABORT_LOW) {
        unsigned fsc = (unsigned)(f->esr & 0x3F);
        if ((fsc >= 0x04 && fsc <= 0x0B) || (fsc >= 0x0D && fsc <= 0x0F)) {
            /* A translation or permission fault in a mapping made when
             * touched (22e): made, and the program goes on. (35b) An
             * access flag fault (0x08-0x0B): pageout cleared the flag of a
             * page to see whether it is used; it is set again. */
            __asm__ volatile ("msr daifclr, #2");
            int fault = process_page_fault(f->far, ec == EC_DATA_ABORT_LOW && ((f->esr >> 6) & 1),
                                           ec == EC_INSTR_ABORT_LOW);
            __asm__ volatile ("msr daifset, #2");
            if (fault == 0) {
                process_before_return(f);
                sched_preempt_point(true);
                return;
            }
            if (fault == SIGBUS && process_fault_signal(f, SIGBUS))
                return;                     /* (22r: its handler runs) */
            if (fault == SIGBUS)
                process_fault("bus error (past the end of a mapped file)", f->elr, f->far, SIGBUS);
        }
    }
    char what[128], detail[80];
    detail[0] = 0;
    if (is_abort(ec))
        explain_abort(detail, sizeof(detail), f->esr,
                      ec == EC_INSTR_ABORT || ec == EC_INSTR_ABORT_LOW);
    ksnprintf(what, sizeof(what), "%s (EC 0x%02x)%s%s", f->vector == 8 ? class_name(ec)
              : vector_names[f->vector], ec, detail[0] ? ", " : "", detail);
    /* As on Unix: undefined (and privileged) instructions SIGILL, the rest
     * (aborts, alignment) SIGSEGV. */
    int signal = ec == EC_UNKNOWN ? SIGILL : SIGSEGV;
    if (process_fault_signal(f, signal))
        return;                             /* (22r: its handler runs) */
    process_fault(what, f->elr, is_abort(ec) || ec == EC_PC_ALIGNMENT ? f->far : ~0ULL, signal);
}

void exception_handler(struct exception_frame *f)
{
    unsigned ec = (unsigned)(f->esr >> 26) & 0x3F;

    if (f->vector == 1 || f->vector == 9) { /* IRQ while the kernel or a program ran */
        sched_irq_enter();
        gic_handle_irq();
        sched_irq_exit();
        if (f->vector == 9)
            process_before_return(f);
        sched_preempt_point(!(f->spsr & (1u << 7)));    /* PSTATE.I was clear */
        return;
    }
    if (f->vector >= 8 && f->vector <= 11) {
        user_exception(f, ec);
        return;
    }

    if (ec == EC_BRK && f->vector == 0) {
        /* BRK does not advance the PC: step over it and continue. */
        kprintf("RitterOS: breakpoint (brk) at PC %p, continuing\n", (void *)f->elr);
        f->elr += 4;
        return;
    }

    /* The report must work whatever state the crashed code left: compiled
     * code may make misaligned accesses. */
    write_sysreg(SCTLR_EL1, read_sysreg(SCTLR_EL1) & ~SCTLR_A);
    isb();

    /* An exception while reporting one: say so as simply as possible and
     * stop, instead of faulting again and again (ARM has no double fault). */
    if (reporting) {
        serial_write("\n=== RitterOS: exception in the exception handler, ESR ");
        serial_write_hexn(f->esr, 8);
        serial_write(", PC ");
        serial_write_hex(f->elr);
        serial_write(", FAR ");
        serial_write_hex(f->far);
        serial_write("\nSystem halted.\n");
        halt_forever();
    }
    reporting = true;
    kprintf_emergency();

    if (!console_is_shown())
        console_show();
    console_set_color(COLOR_ERROR);
    console_set_bold(true);
    kprintf("\n=== RitterOS crash: CPU exception, %s (EC 0x%02x, %s) ===\n", class_name(ec), ec,
            f->vector < 16 ? vector_names[f->vector] : "?");
    console_set_bold(false);

    kprintf("  syndrome (ESR) 0x%08lX: ", f->esr);
    if (is_abort(ec)) {
        char detail[80];
        explain_abort(detail, sizeof(detail), f->esr,
                      ec == EC_INSTR_ABORT || ec == EC_INSTR_ABORT_LOW);
        kprintf("%s\n  fault address (FAR) %p", detail, (void *)f->far);
    } else if (ec == EC_PC_ALIGNMENT) {
        kprintf("instruction address not a multiple of 4\n  fault address (FAR) %p",
                (void *)f->far);
    } else {
        kprintf("-");
    }
    kprintf("\n");

    console_set_color(COLOR_TEXT);
    kprintf("  PC     0x%016lX  SP  0x%016lX  PSTATE 0x%016lX\n", f->elr, f->sp, f->spsr);
    for (int i = 0; i < 30; i += 3)
        kprintf("  X%-2d    0x%016lX  X%-2d 0x%016lX  X%-2d    0x%016lX\n", i, f->x[i], i + 1,
                f->x[i + 1], i + 2, f->x[i + 2]);
    kprintf("  X30 (LR) 0x%016lX\n", f->x[30]);

    uint64_t handler_sp;
    __asm__ volatile ("mov %0, sp" : "=r"(handler_sp));
    kprintf("  handler running on stack %p\n", (void *)handler_sp);
    struct thread *running = arch_this_cpu()->current;    /* (65c-3) on several cores */
    kprintf("  core %d, thread '%s'\n", arch_this_cpu()->index, running ? running->name : "?");

    console_set_color(COLOR_ERROR);
    kprintf("System halted.\n");
    console_set_status(STATUS_CRASH);
    halt_forever();
}

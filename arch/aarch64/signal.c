/*
 * Signal handlers on AArch64 (step 22r). Delivering one: the program's
 * registers (X0-X30, SP_EL0, ELR, SPSR, as its exception frame holds them)
 * and its FP/SIMD state (V0-V31, FPCR, FPSR) go into a context on its stack
 * (16-byte aligned; the AAPCS64 has no red zone); the handler starts with
 * SP at the context, X0 the signal and X30 the library's restorer, which
 * gives SYS_SIGRETURN the context's address (SP again after the handler).
 * Coming back, nothing from the context may give the program more than it
 * had: SPSR keeps only the condition flags (EL0t, interrupts unmasked).
 */
#include <stddef.h>
#include <stdint.h>
#include "arch.h"
#include "arch_api.h"
#include "exceptions.h"
#include "process.h"
#include "string.h"

#define CONTEXT_MAGIC   0x52534947434F4E54ULL       /* "RSIGCONT" */
#define SPSR_NZCV       0xF0000000ULL

struct context {
    uint64_t x[31];
    uint64_t sp, pc, pstate;
    uint64_t blocked, magic;
    uint8_t fpu[ARCH_FPU_STATE_SIZE];   /* at 288: 16-byte aligned */
};

_Static_assert(offsetof(struct context, fpu) % 16 == 0, "the FP area needs 16-byte alignment");
_Static_assert(sizeof(struct context) % 16 == 0, "the context keeps SP aligned");

bool arch_signal_enter(void *frame, uint64_t handler, int signal, uint64_t restorer,
                       uint64_t blocked)
{
    struct exception_frame *f = frame;
    struct context c __attribute__((aligned(64)));
    memcpy(c.x, f->x, sizeof(c.x));
    c.sp = f->sp;
    c.pc = f->elr;
    c.pstate = f->spsr;
    c.blocked = blocked;
    c.magic = CONTEXT_MAGIC;
    arch_fpu_save(c.fpu);               /* the program's own (live while the kernel runs) */
    if (f->sp < sizeof(c) + 64)
        return false;
    uint64_t at = (f->sp - sizeof(c)) & ~15ULL;
    __asm__ volatile ("msr daifclr, #2");
    bool ok = process_copy_to_user(at, &c, sizeof(c));
    __asm__ volatile ("msr daifset, #2");
    if (!ok)
        return false;
    f->sp = at;
    f->elr = handler;
    f->x[0] = (uint64_t)signal;
    f->x[30] = restorer;
    return true;
}

bool arch_signal_return(void *frame, uint64_t *blocked)
{
    struct exception_frame *f = frame;
    struct context c __attribute__((aligned(64)));
    __asm__ volatile ("msr daifclr, #2");
    bool ok = process_copy_from_user(&c, f->x[0], sizeof(c));
    __asm__ volatile ("msr daifset, #2");
    if (!ok || c.magic != CONTEXT_MAGIC)
        return false;
    memcpy(f->x, c.x, sizeof(c.x));
    f->sp = c.sp;
    f->elr = c.pc;                      /* (a kernel address only faults at EL0) */
    f->spsr = c.pstate & SPSR_NZCV;     /* EL0t, DAIF clear */
    arch_fpu_restore(c.fpu);
    *blocked = c.blocked;
    return true;
}

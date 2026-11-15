/*
 * Signal handlers on AArch64 (step 22r). Delivering one: the program's
 * registers (X0-X30, SP_EL0, ELR, SPSR, as its exception frame holds them)
 * and its FP/SIMD state (V0-V31, FPCR, FPSR) go into a context on its stack
 * (16-byte aligned; the AAPCS64 has no red zone); the handler starts with
 * SP at the context, X0 the signal and X30 the library's restorer, which
 * gives SYS_SIGRETURN the context's address (SP again after the handler).
 * Coming back, nothing from the context may give the program more than it
 * had: SPSR keeps only the condition flags (EL0t, interrupts unmasked).
 * (43b-5) The context is the library's ucontext_t, the signal's info above
 * it; the handler gets (signal, &info, &context) in X0-X2, on the thread's
 * alternate stack if asked.
 */
#include <stddef.h>
#include <stdint.h>
#include "arch.h"
#include "arch_api.h"
#include "exceptions.h"
#include "process.h"
#include "string.h"
#include "syscalls.h"
#include "vmlayout.h"

#define CONTEXT_MAGIC   0x52534947434F4E54ULL       /* "RSIGCONT" */
#define SPSR_NZCV       0xF0000000ULL

/* (43b-5) The context is the library's ucontext_t (sys/ucontext.h, as
 * glibc's): uc_sigmask before uc_mcontext, whose fault_address, regs[31],
 * sp, pc and pstate come before its records: Linux's FP/SIMD one (magic
 * 0x46508001, 528 bytes: fpsr, fpcr, V0-V31), then an empty one ending
 * them, then our magic. */
#define FPSIMD_MAGIC    0x46508001u

struct context {
    uint64_t flags, link;
    uint64_t stack_sp;                  /* uc_stack: the thread's alternate stack */
    int32_t stack_flags, stack_pad;
    uint64_t stack_size;
    uint64_t blocked;                   /* uc_sigmask, at 40 */
    uint64_t fault_address;             /* uc_mcontext, at 48 */
    uint64_t x[31];
    uint64_t sp, pc, pstate;
    uint64_t pad;
    uint32_t fp_magic, fp_size;         /* __reserved, at 336: the FP/SIMD record */
    uint32_t fpsr, fpcr;
    uint8_t v[512];
    uint32_t end_magic, end_size;       /* the empty record after it */
    uint64_t magic;
};

_Static_assert(offsetof(struct context, blocked) == 40, "uc_sigmask as the library has it");
_Static_assert(offsetof(struct context, x) == 56, "uc_mcontext.regs as the library has it");
_Static_assert(offsetof(struct context, fp_magic) == 336, "__reserved as the library has it");
_Static_assert(sizeof(struct context) == 880, "the library's ucontext_t");
_Static_assert(sizeof(struct context) % 16 == 0, "the context keeps SP aligned");

bool arch_signal_enter(void *frame, uint64_t handler, int signal, uint64_t restorer,
                       uint64_t blocked, const struct sys_siginfo *info,
                       struct signal_stack *alt, bool on_alt)
{
    struct exception_frame *f = frame;
    struct context c __attribute__((aligned(64)));
    uint8_t fpu[ARCH_FPU_STATE_SIZE] __attribute__((aligned(16)));
    memset(&c, 0, sizeof(c));           /* (no kernel stack bytes to the program) */
    memcpy(c.x, f->x, sizeof(c.x));
    c.sp = f->sp;
    c.pc = f->elr;
    c.pstate = f->spsr;
    c.fault_address = info && (signal == SIGSEGV || signal == SIGBUS) ? info->a : 0;
    bool there = alt->set && f->sp > alt->sp && f->sp - alt->sp <= alt->size;
    c.stack_sp = alt->sp;
    c.stack_size = alt->size;
    c.stack_flags = !alt->set ? SIGSTACK_DISABLE : there ? SIGSTACK_ONSTACK : 0;
    c.blocked = blocked;
    c.magic = CONTEXT_MAGIC;
    arch_fpu_save(fpu);                 /* the program's own (live while the kernel runs) */
    c.fp_magic = FPSIMD_MAGIC;
    c.fp_size = 528;
    memcpy(c.v, fpu, 512);
    uint64_t fpcr, fpsr;
    memcpy(&fpcr, fpu + 512, 8);
    memcpy(&fpsr, fpu + 520, 8);
    c.fpcr = (uint32_t)fpcr;
    c.fpsr = (uint32_t)fpsr;
    uint64_t top = on_alt && alt->set && !there ? alt->sp + alt->size : f->sp;
    alt->used = there || (on_alt && alt->set);
    if (top < sizeof(c) + sizeof(struct sys_siginfo) + 256 || top > USER_TOP)
        return false;
    uint64_t info_at = (top - sizeof(struct sys_siginfo)) & ~15ULL;
    uint64_t at = (info_at - sizeof(c)) & ~15ULL;
    struct sys_siginfo none;
    memset(&none, 0, sizeof(none));
    none.signo = signal;
    __asm__ volatile ("msr daifclr, #2");
    bool ok = process_copy_to_user(info_at, info ? info : &none, sizeof(none)) &&
              process_copy_to_user(at, &c, sizeof(c));
    __asm__ volatile ("msr daifset, #2");
    if (!ok)
        return false;
    f->sp = at;
    f->elr = handler;
    f->x[0] = (uint64_t)signal;
    f->x[1] = info_at;
    f->x[2] = at;
    f->x[30] = restorer;
    return true;
}

bool arch_signal_return(void *frame, uint64_t *blocked, uint64_t *sp)
{
    struct exception_frame *f = frame;
    struct context c __attribute__((aligned(64)));
    uint8_t fpu[ARCH_FPU_STATE_SIZE] __attribute__((aligned(16)));
    __asm__ volatile ("msr daifclr, #2");
    bool ok = process_copy_from_user(&c, f->x[0], sizeof(c));
    __asm__ volatile ("msr daifset, #2");
    if (!ok || c.magic != CONTEXT_MAGIC || c.fp_magic != FPSIMD_MAGIC)
        return false;
    memcpy(f->x, c.x, sizeof(c.x));
    f->sp = c.sp;
    f->elr = c.pc;                      /* (a kernel address only faults at EL0) */
    f->spsr = c.pstate & SPSR_NZCV;     /* EL0t, DAIF clear */
    memcpy(fpu, c.v, 512);
    uint64_t fpcr = c.fpcr, fpsr = c.fpsr;
    memcpy(fpu + 512, &fpcr, 8);
    memcpy(fpu + 520, &fpsr, 8);
    arch_fpu_restore(fpu);
    *blocked = c.blocked;
    *sp = f->sp;
    return true;
}

void arch_syscall_restart(void *frame, uint64_t number, uint64_t arg0)
{
    struct exception_frame *f = frame;
    f->x[0] = arg0;                     /* (the result took its place; x8 is the number) */
    (void)number;
    f->elr -= 4;                        /* SVC: ELR is the instruction after it */
}

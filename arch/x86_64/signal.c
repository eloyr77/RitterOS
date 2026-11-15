/*
 * Signal handlers on x86-64 (step 22r). Delivering one: the program's
 * registers (as its interrupt frame holds them) and its FP/SIMD state
 * (FXSAVE) go into a context on its stack, below the 128-byte red zone the
 * System V ABI lets a function use without moving RSP; under the context
 * lies the return address (the library's restorer), so the handler starts
 * as if called, RSP 8 off a multiple of 16. The restorer gives SYS_SIGRETURN
 * the context's address (RSP after the handler's RET). Coming back, nothing
 * from the context may give the program more than it had: CS and SS are the
 * user selectors, RFLAGS only the arithmetic flags (and IF), RIP and RSP in
 * the lower half, and MXCSR without reserved bits (FXRSTOR would fault);
 * (32a) with XSAVE, the header's parts only those XCR0 has on and the rest
 * of it zero (XRSTOR faults on anything else). (43b-5) The context is
 * the library's ucontext_t, the signal's info above it; the handler gets
 * (signal, &info, &context), on the thread's alternate stack if asked.
 */
#include <stddef.h>
#include <stdint.h>
#include "arch.h"
#include "arch_api.h"
#include "gdt.h"
#include "idt.h"
#include "process.h"
#include "string.h"
#include "syscalls.h"
#include "vmlayout.h"

#define CONTEXT_MAGIC   0x52534947434F4E54ULL       /* "RSIGCONT" */
#define RED_ZONE        128
#define USER_FLAGS      0xCD5ULL                    /* CF PF AF ZF SF DF OF */
#define FLAG_IF         0x200ULL
#define FLAG_ALWAYS     0x2ULL                      /* (bit 1 reads as one) */
#define MXCSR_OFFSET    24                          /* in the FXSAVE area */
#define MXCSR_VALID     0xFFBFu                     /* (without DAZ, which every CPU takes) */
#define XSAVE_HEADER    512                         /* XSTATE_BV, XCOMP_BV, reserved */

uint64_t x86_xcr0(void);                            /* user.c: 0 without XSAVE */

/* (43b-5) The context is the library's ucontext_t (sys/ucontext.h): its
 * registers in glibc's gregs order (REG_R8 ... REG_CR2), fpregs pointing
 * at the FXSAVE/XSAVE area inside it. The magic lies in the reserved
 * words; sigreturn takes the FP state from the area, never the pointer. */
struct context {
    uint64_t flags, link;
    uint64_t stack_sp;                  /* uc_stack: the thread's alternate stack */
    int32_t stack_flags, stack_pad;
    uint64_t stack_size;
    uint64_t gregs[23];                 /* at 40 */
    uint64_t fpregs;                    /* at 224: the program's address of fpu */
    uint64_t magic, reserved[7];
    uint64_t blocked;                   /* uc_sigmask, at 296 */
    uint64_t unused[2];
    uint8_t fpu[ARCH_FPU_STATE_SIZE];   /* at 320 */
};

enum { R8, R9, R10, R11, R12, R13, R14, R15, RDI, RSI, RBP, RBX, RDX, RAX, RCX, RSP, RIP, EFL,
       CSGSFS, ERR, TRAPNO, OLDMASK, CR2 };

_Static_assert(offsetof(struct context, gregs) == 40, "uc_mcontext as the library has it");
_Static_assert(offsetof(struct context, blocked) == 296, "uc_sigmask as the library has it");
_Static_assert(offsetof(struct context, fpu) == 320, "__fpregs_mem as the library has it");
_Static_assert(offsetof(struct context, fpu) % 64 == 0, "XSAVE needs 64-byte alignment");

static void frame_to(struct context *c, const struct interrupt_frame *f)
{
    c->gregs[R8] = f->r8, c->gregs[R9] = f->r9, c->gregs[R10] = f->r10, c->gregs[R11] = f->r11;
    c->gregs[R12] = f->r12, c->gregs[R13] = f->r13, c->gregs[R14] = f->r14, c->gregs[R15] = f->r15;
    c->gregs[RDI] = f->rdi, c->gregs[RSI] = f->rsi, c->gregs[RBP] = f->rbp, c->gregs[RBX] = f->rbx;
    c->gregs[RDX] = f->rdx, c->gregs[RAX] = f->rax, c->gregs[RCX] = f->rcx, c->gregs[RSP] = f->rsp;
    c->gregs[RIP] = f->rip, c->gregs[EFL] = f->rflags;
    c->gregs[CSGSFS] = f->cs | f->ss << 48;
    c->gregs[ERR] = f->error_code;
    c->gregs[TRAPNO] = f->vector;
}

static void frame_from(struct interrupt_frame *f, const struct context *c)
{
    f->r8 = c->gregs[R8], f->r9 = c->gregs[R9], f->r10 = c->gregs[R10], f->r11 = c->gregs[R11];
    f->r12 = c->gregs[R12], f->r13 = c->gregs[R13], f->r14 = c->gregs[R14], f->r15 = c->gregs[R15];
    f->rdi = c->gregs[RDI], f->rsi = c->gregs[RSI], f->rbp = c->gregs[RBP], f->rbx = c->gregs[RBX];
    f->rdx = c->gregs[RDX], f->rax = c->gregs[RAX], f->rcx = c->gregs[RCX];
}

bool arch_signal_enter(void *frame, uint64_t handler, int signal, uint64_t restorer,
                       uint64_t blocked, const struct sys_siginfo *info,
                       struct signal_stack *alt, bool on_alt)
{
    struct interrupt_frame *f = frame;
    struct context c __attribute__((aligned(64)));
    memset(&c, 0, sizeof(c));           /* (no kernel stack bytes to the program) */
    frame_to(&c, f);
    if (info && (signal == SIGSEGV || signal == SIGBUS) && info->a)
        c.gregs[CR2] = info->a;
    bool there = alt->set && f->rsp > alt->sp && f->rsp - alt->sp <= alt->size;
    c.stack_sp = alt->sp;
    c.stack_size = alt->size;
    c.stack_flags = !alt->set ? SIGSTACK_DISABLE : there ? SIGSTACK_ONSTACK : 0;
    c.blocked = blocked;
    c.magic = CONTEXT_MAGIC;
    arch_fpu_save(c.fpu);               /* the program's own (live while the kernel runs) */
    uint64_t top = on_alt && alt->set && !there ? alt->sp + alt->size : f->rsp - RED_ZONE;
    alt->used = there || (on_alt && alt->set);
    uint64_t info_at = (top - sizeof(struct sys_siginfo)) & ~15ULL;
    uint64_t at = (info_at - sizeof(c)) & ~63ULL;
    uint64_t ret = at - 8;
    if (top < sizeof(c) + sizeof(struct sys_siginfo) + 256 || top > USER_TOP)
        return false;
    c.fpregs = at + offsetof(struct context, fpu);
    struct sys_siginfo none;
    memset(&none, 0, sizeof(none));
    none.signo = signal;
    __asm__ volatile ("sti");
    bool ok = process_copy_to_user(info_at, info ? info : &none, sizeof(none)) &&
              process_copy_to_user(at, &c, sizeof(c)) &&
              process_copy_to_user(ret, &restorer, sizeof(restorer));
    __asm__ volatile ("cli");
    if (!ok)
        return false;
    f->rsp = ret;
    f->rip = handler;
    f->rdi = (uint64_t)signal;
    f->rsi = info_at;
    f->rdx = at;
    f->rflags &= ~(0x400ULL | 0x100ULL);        /* DF clear (the ABI), no single step */
    return true;
}

bool arch_signal_return(void *frame, uint64_t *blocked, uint64_t *sp)
{
    struct interrupt_frame *f = frame;
    struct context c __attribute__((aligned(64)));
    __asm__ volatile ("sti");
    bool ok = process_copy_from_user(&c, f->rdi, sizeof(c));
    __asm__ volatile ("cli");
    if (!ok || c.magic != CONTEXT_MAGIC || c.gregs[RIP] >= USER_TOP || c.gregs[RSP] >= USER_TOP)
        return false;
    frame_from(f, &c);
    f->rip = c.gregs[RIP];
    f->rsp = c.gregs[RSP];
    f->rflags = (c.gregs[EFL] & USER_FLAGS) | FLAG_IF | FLAG_ALWAYS;
    f->cs = GDT_USER_CODE;
    f->ss = GDT_USER_DATA;
    uint32_t mxcsr;
    memcpy(&mxcsr, c.fpu + MXCSR_OFFSET, 4);
    mxcsr &= MXCSR_VALID;
    memcpy(c.fpu + MXCSR_OFFSET, &mxcsr, 4);
    uint64_t xcr0 = x86_xcr0();
    if (xcr0) {
        uint64_t parts;
        memcpy(&parts, c.fpu + XSAVE_HEADER, 8);
        parts &= xcr0;
        memset(c.fpu + XSAVE_HEADER, 0, 64);
        memcpy(c.fpu + XSAVE_HEADER, &parts, 8);
    }
    arch_fpu_restore(c.fpu);
    *blocked = c.blocked;
    *sp = f->rsp;
    return true;
}

void arch_syscall_restart(void *frame, uint64_t number, uint64_t arg0)
{
    struct interrupt_frame *f = frame;
    f->rax = number;                    /* (rdi, the first argument, is still there) */
    (void)arg0;
    f->rip -= 2;                        /* SYSCALL, 0F 05 (its RIP was RCX: after it) */
}

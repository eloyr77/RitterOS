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
 * of it zero (XRSTOR faults on anything else).
 */
#include <stddef.h>
#include <stdint.h>
#include "arch.h"
#include "arch_api.h"
#include "gdt.h"
#include "idt.h"
#include "process.h"
#include "string.h"
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

struct context {
    uint64_t regs[15];                  /* R15 ... RAX, in interrupt_frame's order */
    uint64_t rip, rflags, rsp;
    uint64_t blocked, magic;
    uint64_t unused[4];
    uint8_t fpu[ARCH_FPU_STATE_SIZE];   /* at 192 */
};

_Static_assert(offsetof(struct context, fpu) % 64 == 0, "XSAVE needs 64-byte alignment");
_Static_assert(offsetof(struct interrupt_frame, rbx) + 8 == offsetof(struct interrupt_frame, rax),
               "the 15 registers lie together in the frame");

bool arch_signal_enter(void *frame, uint64_t handler, int signal, uint64_t restorer,
                       uint64_t blocked)
{
    struct interrupt_frame *f = frame;
    struct context c __attribute__((aligned(64)));
    memset(&c, 0, sizeof(c));           /* (no kernel stack bytes to the program) */
    memcpy(c.regs, &f->r15, sizeof(c.regs));
    c.rip = f->rip;
    c.rflags = f->rflags;
    c.rsp = f->rsp;
    c.blocked = blocked;
    c.magic = CONTEXT_MAGIC;
    arch_fpu_save(c.fpu);               /* the program's own (live while the kernel runs) */
    uint64_t at = (f->rsp - RED_ZONE - sizeof(c)) & ~15ULL;
    uint64_t ret = at - 8;
    if (f->rsp < RED_ZONE + sizeof(c) + 64)
        return false;
    __asm__ volatile ("sti");
    bool ok = process_copy_to_user(at, &c, sizeof(c)) &&
              process_copy_to_user(ret, &restorer, sizeof(restorer));
    __asm__ volatile ("cli");
    if (!ok)
        return false;
    f->rsp = ret;
    f->rip = handler;
    f->rdi = (uint64_t)signal;
    f->rflags &= ~(0x400ULL | 0x100ULL);        /* DF clear (the ABI), no single step */
    return true;
}

bool arch_signal_return(void *frame, uint64_t *blocked)
{
    struct interrupt_frame *f = frame;
    struct context c __attribute__((aligned(64)));
    __asm__ volatile ("sti");
    bool ok = process_copy_from_user(&c, f->rdi, sizeof(c));
    __asm__ volatile ("cli");
    if (!ok || c.magic != CONTEXT_MAGIC || c.rip >= USER_TOP || c.rsp >= USER_TOP)
        return false;
    memcpy(&f->r15, c.regs, sizeof(c.regs));
    f->rip = c.rip;
    f->rsp = c.rsp;
    f->rflags = (c.rflags & USER_FLAGS) | FLAG_IF | FLAG_ALWAYS;
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
    return true;
}

/*
 * sys/ucontext.h (43b-5): a signal handler's context (sigaction's
 * SA_SIGINFO handlers get it as their third argument), with glibc's
 * names: uc_mcontext holds the registers the program had when the signal
 * came, uc_sigmask the signals it blocked, uc_stack the thread's
 * alternate stack. A handler may change them: sigreturn puts them back
 * (the FP/SIMD state from the context's own area, never through a
 * pointer); only what a program may have comes back. (getcontext,
 * setcontext, makecontext and swapcontext are not done.)
 *   x86-64: uc_mcontext.gregs[REG_RIP], [REG_RSP], [REG_RAX]...,
 *           [REG_CR2] a fault's address; fpregs the FXSAVE area.
 *   AArch64: uc_mcontext.regs[0..30], sp, pc, pstate, fault_address;
 *           __reserved: Linux's FP/SIMD record (FPSIMD_MAGIC), then an
 *           empty one.
 */
#ifndef RITTEROS_SYS_UCONTEXT_H
#define RITTEROS_SYS_UCONTEXT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <signal.h>
#include <stdint.h>

#if defined(__x86_64__)
typedef long long greg_t;
#define NGREG   23
#define __NGREG NGREG
typedef greg_t gregset_t[NGREG];
enum {
    REG_R8 = 0, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15, REG_RDI, REG_RSI,
    REG_RBP, REG_RBX, REG_RDX, REG_RAX, REG_RCX, REG_RSP, REG_RIP, REG_EFL, REG_CSGSFS, REG_ERR,
    REG_TRAPNO, REG_OLDMASK, REG_CR2
};
struct _libc_fpxreg {
    unsigned short significand[4];
    unsigned short exponent;
    unsigned short __pad[3];
};
struct _libc_xmmreg {
    uint32_t element[4];
};
struct _libc_fpstate {          /* FXSAVE's layout */
    uint16_t cwd, swd, ftw, fop;
    uint64_t rip, rdp;
    uint32_t mxcsr, mxcr_mask;
    struct _libc_fpxreg _st[8];
    struct _libc_xmmreg _xmm[16];
    uint32_t __reserved[24];
};
typedef struct _libc_fpstate *fpregset_t;
typedef struct {
    gregset_t gregs;
    fpregset_t fpregs;
    unsigned long long __reserved1[8];
} mcontext_t;
typedef struct ucontext_t {
    unsigned long uc_flags;
    struct ucontext_t *uc_link;
    stack_t uc_stack;
    mcontext_t uc_mcontext;
    sigset_t uc_sigmask;
    unsigned char __fpregs_mem[832] __attribute__((aligned(64)));
} ucontext_t;
#elif defined(__aarch64__)
#define FPSIMD_MAGIC 0x46508001
struct _aarch64_ctx {
    uint32_t magic;
    uint32_t size;
};
struct fpsimd_context {
    struct _aarch64_ctx head;
    uint32_t fpsr;
    uint32_t fpcr;
    unsigned __int128 vregs[32];        /* (glibc's __uint128_t: our cc knows only this name) */
};
typedef struct {
    unsigned long long fault_address;
    unsigned long long regs[31];
    unsigned long long sp;
    unsigned long long pc;
    unsigned long long pstate;
    unsigned char __reserved[544] __attribute__((aligned(16)));
} mcontext_t;
typedef struct ucontext_t {
    unsigned long uc_flags;
    struct ucontext_t *uc_link;
    stack_t uc_stack;
    sigset_t uc_sigmask;
    mcontext_t uc_mcontext;
} ucontext_t;
#endif

#ifdef __cplusplus
}
#endif

#endif

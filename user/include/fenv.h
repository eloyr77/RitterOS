/*
 * The floating-point environment (step 22u): the rounding direction and
 * the exception flags (sticky: set by an operation that was invalid,
 * divided by zero, overflowed, underflowed or was inexact) of the calling
 * thread, as the processor keeps them (x86-64: SSE's MXCSR and the x87's
 * control and status words, both set together; AArch64: FPCR and FPSR).
 * Every thread and every new program starts rounding to nearest with no
 * flags set; the kernel keeps them per thread. Exceptions never trap.
 * math.h's functions expect rounding to nearest.
 */
#ifndef RITTEROS_FENV_H
#define RITTEROS_FENV_H

/* (The same numbers on both architectures; the library translates them
 * into the processor's bits.) */
#define FE_INVALID      0x01
#define FE_DIVBYZERO    0x02
#define FE_OVERFLOW     0x04
#define FE_UNDERFLOW    0x08
#define FE_INEXACT      0x10
#define FE_ALL_EXCEPT   0x1F
#define FE_TONEAREST    0
#define FE_UPWARD       1
#define FE_DOWNWARD     2
#define FE_TOWARDZERO   3
typedef unsigned int fexcept_t;
/* The processor's registers as they were: x86-64 the x87's control word
 * (low half) and status word (high half), then MXCSR; AArch64 FPCR, FPSR. */
typedef struct {
    unsigned int control, status;
} fenv_t;

/* The environment every program starts with (for fesetenv, feupdateenv). */
#define FE_DFL_ENV ((const fenv_t *)-1)

/* The exception flags: cleared, saved, raised (set; nothing traps), set
 * back from a saved copy, tested (which of excepts are set). 0 on success. */
int feclearexcept(int excepts);
int fegetexceptflag(fexcept_t *flags, int excepts);
int feraiseexcept(int excepts);
int fesetexceptflag(const fexcept_t *flags, int excepts);
int fetestexcept(int excepts);
/* The rounding direction: FE_TONEAREST and the others; fesetround 0 on
 * success (nonzero for a value that is not one of them). */
int fegetround(void);
int fesetround(int round);
/* The whole environment: saved, set; feholdexcept saves it and clears the
 * flags, feupdateenv sets a saved one and raises the flags set meanwhile. */
int fegetenv(fenv_t *env);
int feholdexcept(fenv_t *env);
int fesetenv(const fenv_t *env);
int feupdateenv(const fenv_t *env);

#endif

/*
 * fenv.h (step 22u): the processor's floating-point control and status,
 * read and written directly. fenv.h's numbers are the same on both
 * architectures (flags: invalid 1, divide by zero 2, overflow 4, underflow
 * 8, inexact 16; directions 0-3 in FPCR's order), translated here.
 * x86-64: doubles are computed by SSE (MXCSR: flags in bits 0-5 with
 * "denormal" at bit 1, the direction in bits 13-14) and long doubles by
 * the x87 (status word: the same flag bits; control word: the direction
 * in bits 10-11): both are kept alike, the flags of either count.
 * AArch64: FPCR bits 22-23 the direction, FPSR the flags as fenv.h's.
 */
#include <fenv.h>
#include <stdint.h>

#if defined(__x86_64__)

/* fenv.h's flags <-> the x87's and MXCSR's (which have "denormal" at 2) */
static uint32_t to_hw(int e) { return (uint32_t)((e & 1) | (e & 0x1E) << 1); }
static int from_hw(uint32_t h) { return (int)((h & 1) | (h >> 1 & 0x1E)); }
#define HW_FLAGS 0x3Du

/* fenv.h's directions <-> the x87's 2-bit field (nearest, down, up, zero) */
static const uint16_t x87_round[4] = { 0, 2, 1, 3 };
#define X87_ROUND_SHIFT 10
#define MXCSR_ROUND_SHIFT 13

struct x87_env {                                    /* what fnstenv stores */
    uint16_t control, pad0, status, pad1, tag, pad2;
    uint32_t ip;
    uint16_t cs, opcode;
    uint32_t dp;
    uint16_t ds, pad3;
};

static uint32_t get_mxcsr(void)
{
    uint32_t m;
    __asm__ volatile ("stmxcsr %0" : "=m"(m));
    return m;
}

static void set_mxcsr(uint32_t m)
{
    __asm__ volatile ("ldmxcsr %0" : : "m"(m));
}

static void get_x87(struct x87_env *e)
{
    __asm__ volatile ("fnstenv %0" : "=m"(*e));
    __asm__ volatile ("fldenv %0" : : "m"(*e));    /* (fnstenv also masks exceptions) */
}

static void set_x87(const struct x87_env *e)
{
    __asm__ volatile ("fldenv %0" : : "m"(*e));
}

int feclearexcept(int excepts)
{
    uint32_t hw = to_hw(excepts & FE_ALL_EXCEPT);
    struct x87_env e;
    get_x87(&e);
    e.status &= (uint16_t)~hw;
    set_x87(&e);
    set_mxcsr(get_mxcsr() & ~hw);
    return 0;
}

int fetestexcept(int excepts)
{
    uint16_t status;
    __asm__ volatile ("fnstsw %0" : "=m"(status));
    return from_hw(get_mxcsr() | status) & excepts & FE_ALL_EXCEPT;
}

int feraiseexcept(int excepts)
{
    set_mxcsr(get_mxcsr() | to_hw(excepts & FE_ALL_EXCEPT));
    return 0;
}

int fegetround(void)
{
    uint16_t control;
    __asm__ volatile ("fnstcw %0" : "=m"(control));
    return x87_round[control >> X87_ROUND_SHIFT & 3];   /* (the table is its own inverse) */
}

int fesetround(int round)
{
    if (round < 0 || round > 3)
        return -1;
    uint32_t field = x87_round[round];
    uint16_t control;
    __asm__ volatile ("fnstcw %0" : "=m"(control));
    control = (uint16_t)((control & ~(3u << X87_ROUND_SHIFT)) | field << X87_ROUND_SHIFT);
    __asm__ volatile ("fldcw %0" : : "m"(control));
    set_mxcsr((get_mxcsr() & ~(3u << MXCSR_ROUND_SHIFT)) | field << MXCSR_ROUND_SHIFT);
    return 0;
}

int fegetenv(fenv_t *env)
{
    struct x87_env e;
    get_x87(&e);
    env->control = e.control | (uint32_t)e.status << 16;
    env->status = get_mxcsr();
    return 0;
}

int fesetenv(const fenv_t *env)
{
    struct x87_env e;
    get_x87(&e);
    if (env == FE_DFL_ENV) {
        e.control = 0x037F;                         /* fninit's: nearest, all masked */
        e.status = 0;
        set_x87(&e);
        set_mxcsr(0x1F80);                          /* the same for SSE */
        return 0;
    }
    e.control = (uint16_t)env->control;
    e.status = (uint16_t)(env->control >> 16);
    set_x87(&e);
    set_mxcsr(env->status);
    return 0;
}

#else /* AArch64 */

#define FPCR_ROUND_SHIFT 22

static uint64_t get_fpcr(void)
{
    uint64_t v;
    __asm__ volatile ("mrs %0, fpcr" : "=r"(v));
    return v;
}

static void set_fpcr(uint64_t v)
{
    __asm__ volatile ("msr fpcr, %0" : : "r"(v));
}

static uint64_t get_fpsr(void)
{
    uint64_t v;
    __asm__ volatile ("mrs %0, fpsr" : "=r"(v));
    return v;
}

static void set_fpsr(uint64_t v)
{
    __asm__ volatile ("msr fpsr, %0" : : "r"(v));
}

int feclearexcept(int excepts)
{
    set_fpsr(get_fpsr() & ~(uint64_t)(excepts & FE_ALL_EXCEPT));
    return 0;
}

int fetestexcept(int excepts)
{
    return (int)(get_fpsr() & (uint64_t)(excepts & FE_ALL_EXCEPT));
}

int feraiseexcept(int excepts)
{
    set_fpsr(get_fpsr() | (uint64_t)(excepts & FE_ALL_EXCEPT));
    return 0;
}

int fegetround(void)
{
    return (int)(get_fpcr() >> FPCR_ROUND_SHIFT & 3);
}

int fesetround(int round)
{
    if (round < 0 || round > 3)
        return -1;
    set_fpcr((get_fpcr() & ~(3ull << FPCR_ROUND_SHIFT)) | (uint64_t)round << FPCR_ROUND_SHIFT);
    return 0;
}

int fegetenv(fenv_t *env)
{
    env->control = (unsigned)get_fpcr();
    env->status = (unsigned)get_fpsr();
    return 0;
}

int fesetenv(const fenv_t *env)
{
    if (env == FE_DFL_ENV) {
        set_fpcr(0);                                /* what every program starts with */
        set_fpsr(0);
        return 0;
    }
    set_fpcr(env->control);
    set_fpsr(env->status);
    return 0;
}

#endif

int fegetexceptflag(fexcept_t *flags, int excepts)
{
    *flags = (fexcept_t)fetestexcept(excepts);
    return 0;
}

int fesetexceptflag(const fexcept_t *flags, int excepts)
{
    feclearexcept(excepts);
    return feraiseexcept((int)*flags & excepts);
}

int feholdexcept(fenv_t *env)
{
    fegetenv(env);
    feclearexcept(FE_ALL_EXCEPT);
    return 0;
}

int feupdateenv(const fenv_t *env)
{
    int raised = fetestexcept(FE_ALL_EXCEPT);
    fesetenv(env);
    return feraiseexcept(raised);
}

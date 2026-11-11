/*
 * User mode on x86-64: programs run in ring 3. System calls use the SYSCALL
 * instruction, configured by three MSRs: LSTAR (where it jumps), STAR (the
 * segments it loads), SFMASK (RFLAGS bits it clears, here the interrupt
 * flag). Entering user mode: user.S.
 */
#include "percpu.h"
#include "arch.h"
#include "cpu.h"
#include "gdt.h"
#include "idt.h"
#include "kprintf.h"
#include "process.h"
#include "sched.h"
#include "string.h"
#include "syscalls.h"
#include "vmlayout.h"

#define MSR_STAR        0xC0000081
#define MSR_LSTAR       0xC0000082
#define MSR_SFMASK      0xC0000084
#define EFER_SCE        (1u << 0)       /* SYSCALL/SYSRET enabled */
#define RFLAGS_TF       (1u << 8)
#define RFLAGS_IF       (1u << 9)
#define RFLAGS_DF       (1u << 10)
#define RFLAGS_AC       (1u << 18)

extern void syscall_entry(void);
extern void fpu_clean_state(void *area);

#define CR0_MP          (1u << 1)       /* WAIT/FWAIT honors TS */
#define CR0_EM          (1u << 2)       /* x87 emulation: must be off */
#define CR0_TS          (1u << 3)       /* task switched: would trap FP use */
#define CR4_OSFXSR      (1u << 9)       /* FXSAVE/FXRSTOR and SSE */
#define CR4_OSXMMEXCPT  (1u << 10)      /* SSE exceptions as #XM */
#define CR4_OSXSAVE     (1u << 18)      /* XSAVE and XCR0 (AVX's switch) */
#define CPUID1_XSAVE    (1u << 26)      /* in ECX */
#define CPUID1_AVX      (1u << 28)
#define XCR0_X87        (1u << 0)
#define XCR0_SSE        (1u << 1)
#define XCR0_AVX        (1u << 2)       /* the YMM registers' upper halves */
#define XSAVE_HEADER    512             /* XSTATE_BV, then XCOMP_BV and reserved: 64 bytes */
#define FX_XMM          160             /* XMM0-15 in the FXSAVE area */

/* (32a) Where the processor has AVX, programs get it: XSAVE saves their
 * state, XCR0 = x87, SSE and AVX on every core (AVX-512's parts stay off).
 * Chosen once by the boot core (fpu_choose), before the others start. */
uint8_t fpu_xsave;                      /* fpu.S: XSAVE instead of FXSAVE */
static uint64_t xcr0;                   /* 0: FXSAVE */

/* What a new program's FP/SIMD registers start as. */
static uint8_t clean_fpu[ARCH_FPU_STATE_SIZE] __attribute__((aligned(64)));
extern __attribute__((noreturn)) void user_jump(uint64_t entry, uint64_t user_stack);
extern __attribute__((noreturn)) void user_resume(const struct interrupt_frame *frame);

_Static_assert(sizeof(struct interrupt_frame) <= ARCH_FRAME_MAX, "fork copies the frame");

static void fpu_choose(void)
{
    struct cpuid_result one = cpuid(1, 0);
    if (!(one.ecx & CPUID1_XSAVE) || !(one.ecx & CPUID1_AVX) || cpuid(0, 0).eax < 0xD)
        return;
    uint64_t want = XCR0_X87 | XCR0_SSE | XCR0_AVX;
    struct cpuid_result parts = cpuid(0xD, 0), avx = cpuid(0xD, 2);
    if ((parts.eax & want) != want || avx.ebx + avx.eax > ARCH_FPU_STATE_SIZE)
        return;                         /* (an area our size cannot hold: FXSAVE) */
    xcr0 = want;
    fpu_xsave = 1;
}

uint64_t x86_xcr0(void)
{
    return xcr0;
}

/* (65c) What each core needs for programs: SYSCALL and FP/SIMD on. */
void x86_user_init_core(void)
{
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    /* SYSCALL loads CS = STAR[47:32] (kernel code; SS = that + 8); SYSRET
     * would load CS = STAR[63:48] + 16 and SS = STAR[63:48] + 8 (user code
     * and data): the GDT order kernel code, kernel data, user data, user code. */
    wrmsr(MSR_STAR, (uint64_t)GDT_KERNEL_DATA << 48 | (uint64_t)GDT_KERNEL_CODE << 32);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, RFLAGS_TF | RFLAGS_IF | RFLAGS_DF | RFLAGS_AC);

    /* FP/SIMD (x87, SSE) for programs: no emulation, no trap on use. */
    uint64_t cr0, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr0 = (cr0 & ~(uint64_t)(CR0_EM | CR0_TS)) | CR0_MP;
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    if (xcr0)
        cr4 |= CR4_OSXSAVE;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));
    if (xcr0)
        __asm__ volatile ("xsetbv" : : "c"(0), "a"((uint32_t)xcr0), "d"((uint32_t)(xcr0 >> 32)));
}

void arch_user_init(void)
{
    fpu_choose();
    x86_user_init_core();
    fpu_clean_state(clean_fpu);
    /* The vector registers start at zero, not as the firmware left them;
     * with XSAVE, x87 and SSE come from the area, AVX's part initialized. */
    memset(clean_fpu + FX_XMM, 0, 16 * 16);
    if (xcr0) {
        memset(clean_fpu + XSAVE_HEADER, 0, ARCH_FPU_STATE_SIZE - XSAVE_HEADER);
        clean_fpu[XSAVE_HEADER] = XCR0_X87 | XCR0_SSE;
    }
    if (xcr0)
        kprintf("  user mode:     ring 3; system calls by SYSCALL, entry %p; FP/SIMD: x87, SSE "
                "and AVX (XSAVE, %u bytes per program)\n", (void *)syscall_entry,
                ARCH_FPU_STATE_SIZE);
    else
        kprintf("  user mode:     ring 3; system calls by SYSCALL, entry %p; FP/SIMD: x87 and SSE "
                "(FXSAVE, %u bytes per program)\n", (void *)syscall_entry, 512);
}

void arch_set_kernel_stack(uint64_t kernel_stack_top)
{
    uint64_t irq = arch_irq_save();         /* (65c-3: this core's, not a torn write) */
    tss_set_rsp0(kernel_stack_top);         /* interrupts and exceptions from ring 3 */
    arch_this_cpu()->syscall_stack = kernel_stack_top;  /* system calls (65a: per core) */
    arch_irq_restore(irq);
}

#define MSR_GS_BASE         0xC0000101
#define MSR_KERNEL_GS_BASE  0xC0000102

void arch_set_this_cpu(struct cpu *c)
{
    wrmsr(MSR_GS_BASE, (uint64_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, 0);           /* a program's, until it sets its own */
}

void arch_enter_user(uint64_t entry, uint64_t user_stack, uint64_t kernel_stack_top)
{
    arch_set_kernel_stack(kernel_stack_top);
    user_jump(entry, user_stack);
}

void arch_resume_user(const void *frame, uint64_t kernel_stack_top)
{
    arch_set_kernel_stack(kernel_stack_top);
    user_resume(frame);
}

void arch_fpu_clean(void *area)
{
    memcpy(area, clean_fpu, ARCH_FPU_STATE_SIZE);
}

void arch_code_written(const void *addr, uint64_t size)
{
    (void)addr, (void)size;                 /* x86 instruction fetches see every write */
}

#define MSR_FS_BASE 0xC0000100

uint64_t arch_thread_pointer(void)
{
    return rdmsr(MSR_FS_BASE);
}

void arch_set_thread_pointer(uint64_t tp)
{
    wrmsr(MSR_FS_BASE, tp);
}

uint64_t arch_user_segments(void)
{
    uint16_t gs;
    __asm__ volatile ("mov %%gs, %0" : "=r"(gs));
    return gs;
}

/* Loading a selector sets the GS base from its descriptor: the core's block
 * is put back at once (interrupts are off: the scheduler's lock). */
void arch_set_user_segments(uint64_t segments)
{
    if (arch_user_segments() == (uint16_t)segments)
        return;
    struct cpu *c = arch_this_cpu();
    uint64_t program_base = rdmsr(MSR_KERNEL_GS_BASE);
    __asm__ volatile ("mov %0, %%gs" : : "r"((uint16_t)segments) : "memory");
    wrmsr(MSR_GS_BASE, (uint64_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, program_base);
}

/* A system call (from syscall_entry through isr_common), on the program's
 * kernel stack. Interrupts are on while the kernel works for the program. */
void syscall_handler(struct interrupt_frame *f)
{
    if (f->rax == SYS_SIGRETURN) {          /* (22r) every register comes back: no result */
        process_sigreturn(f);
        process_before_return(f);
        sched_preempt_point(true);
        return;
    }
    if (f->rax == SYS_FORK) {               /* (56a) the child gets a copy of the frame */
        struct interrupt_frame child = *f;
        child.rax = 0;                      /* (fork returns 0 there) */
        __asm__ volatile ("sti");
        int64_t result = process_fork(&child, sizeof(child));
        __asm__ volatile ("cli");
        f->rax = (uint64_t)result;
        process_before_return(f);
        sched_preempt_point(true);
        return;
    }
    const uint64_t args[6] = { f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9 };
    __asm__ volatile ("sti");
    int64_t result = process_syscall(f->rax, args);
    __asm__ volatile ("cli");
    f->rax = (uint64_t)result;
    process_before_return(f);
    sched_preempt_point(true);              /* back to the program: interrupts on there */
}

/* --- threads ------------------------------------------------------------------ */

extern void thread_start(void);

uint64_t arch_thread_init(uint64_t stack_top, void (*fn)(void *arg), void *arg)
{
    /* What arch_context_switch pops: R15, R14, R13 (arg), R12 (fn), RBX,
     * RBP, then the return address. After that "ret", RSP is stack_top:
     * 16-byte aligned, as the calls in thread_start need. */
    uint64_t *sp = (uint64_t *)stack_top;
    *--sp = (uint64_t)thread_start;
    *--sp = 0;                              /* RBP */
    *--sp = 0;                              /* RBX */
    *--sp = (uint64_t)fn;                   /* R12 */
    *--sp = (uint64_t)arg;                  /* R13 */
    *--sp = 0;                              /* R14 */
    *--sp = 0;                              /* R15 */
    return (uint64_t)sp;
}

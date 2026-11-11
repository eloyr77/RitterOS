/*
 * Entering user mode (ring 3), and the system call entry.
 *
 * user_jump "returns" to the program with IRETQ, which loads the user code
 * and stack segments (privilege level 3), RIP, RSP and RFLAGS in one step.
 * It never comes back: from then on the thread enters the kernel only
 * through interrupts, exceptions and SYSCALL, on its kernel stack
 * (TSS.RSP0, syscall_stack_top).
 *
 * SYSCALL jumps to syscall_entry (MSR_LSTAR) with interrupts off, the user's
 * RIP in RCX and RFLAGS in R11, but still on the user's stack. The entry
 * switches to the thread's kernel stack and builds the same frame an
 * interrupt from ring 3 would (struct interrupt_frame, vector SYSCALL_VECTOR),
 * so the common interrupt path handles it and returns with IRETQ.
 *
 * (65a) The kernel finds its core's block (kernel/percpu.h) through the GS
 * base; a program has its own GS base, which the CPU keeps in
 * MSR_KERNEL_GS_BASE while the kernel runs. SWAPGS exchanges the two on
 * every way in from ring 3 and out to it (here and in isr.S), so a program
 * that loads GS changes only its own.
 */

#define USER_DATA       (0x18 | 3)          /* GDT_USER_DATA */
#define USER_CODE       (0x20 | 3)          /* GDT_USER_CODE */
#define RFLAGS_IF       0x202               /* interrupts on (bit 1 is always 1) */
#define SYSCALL_VECTOR  256                 /* not a CPU vector: marks a system call */

    .section .text

    /* void user_jump(uint64_t entry, uint64_t user_stack) */
    .global user_jump
user_jump:
    cli
    pushq   $USER_DATA                  /* SS */
    pushq   %rsi                        /* RSP */
    pushq   $RFLAGS_IF                  /* RFLAGS */
    pushq   $USER_CODE                  /* CS */
    pushq   %rdi                        /* RIP */
    /* The program starts with no kernel values in its registers. */
    xorl    %eax, %eax
    xorl    %ebx, %ebx
    xorl    %ecx, %ecx
    xorl    %edx, %edx
    xorl    %esi, %esi
    xorl    %edi, %edi
    xorl    %ebp, %ebp
    xorl    %r8d, %r8d
    xorl    %r9d, %r9d
    xorl    %r10d, %r10d
    xorl    %r11d, %r11d
    xorl    %r12d, %r12d
    xorl    %r13d, %r13d
    xorl    %r14d, %r14d
    xorl    %r15d, %r15d
    swapgs                              /* the program's GS base from here */
    iretq

    /* void user_resume(const struct interrupt_frame *frame): fork's child
     * leaves through the interrupt path's end with the frame given. */
    .global user_resume
user_resume:
    cli
    movq    %rdi, %rsp
    jmp     isr_return

    .global syscall_entry
syscall_entry:
    swapgs                              /* the kernel's GS base: this core's block */
    movq    %rsp, %gs:8                 /* struct cpu.user_rsp (interrupts are off) */
    movq    %gs:16, %rsp                /* struct cpu.syscall_stack */
    pushq   $USER_DATA                  /* SS */
    pushq   %gs:8                       /* RSP */
    pushq   %r11                        /* RFLAGS */
    pushq   $USER_CODE                  /* CS */
    pushq   %rcx                        /* RIP */
    pushq   $0                          /* error code */
    pushq   $SYSCALL_VECTOR
    jmp     isr_save                    /* (past isr_common's SWAPGS: done) */

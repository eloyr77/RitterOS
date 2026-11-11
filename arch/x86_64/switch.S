/*
 * Switching between threads (kernel/sched.c).
 *
 * void arch_context_switch(uint64_t *save, uint64_t load): push the
 * callee-saved registers (the C convention lets the caller assume they
 * survive), store RSP in *save, load RSP from load, pop the other thread's
 * registers and return: into the other thread, where it once called
 * arch_context_switch itself (or into thread_start, for a new thread).
 */
    .section .text
    .global arch_context_switch
arch_context_switch:
    pushq   %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    movq    %rsp, (%rdi)
    movq    %rsi, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret

/* A new thread starts here (arch_thread_init put R12 = fn, R13 = arg). */
    .global thread_start
thread_start:
    call    sched_thread_start
    movq    %r13, %rdi
    call    *%r12
    call    thread_exit
    ud2

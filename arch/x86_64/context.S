/*
 * (37g-1) A core's place kept across sleep (arch/x86_64/sleep.c).
 *
 * int x86_context_save(struct x86_context *c): the callee-saved registers,
 * RSP after the return and the return address into c; returns 0. When
 * x86_context_restore(c, done) later loads them, the call returns again,
 * with 1: as setjmp and longjmp, for a core that lost everything but its
 * memory.
 *
 * x86_context_restore(c, done): loads c, then stores 1 in *done (the
 * waker's sign that the start-up stack is free: nothing touches it after
 * RSP is loaded), and returns into x86_context_save's caller.
 *
 * struct x86_context: rbx 0, rbp 8, r12 16, r13 24, r14 32, r15 40,
 * rsp 48, rip 56.
 */
    .section .text
    .global x86_context_save
x86_context_save:
    movq    %rbx, 0(%rdi)
    movq    %rbp, 8(%rdi)
    movq    %r12, 16(%rdi)
    movq    %r13, 24(%rdi)
    movq    %r14, 32(%rdi)
    movq    %r15, 40(%rdi)
    leaq    8(%rsp), %rax
    movq    %rax, 48(%rdi)
    movq    (%rsp), %rax
    movq    %rax, 56(%rdi)
    xorl    %eax, %eax
    ret

    .global x86_context_restore
x86_context_restore:
    movq    0(%rdi), %rbx
    movq    8(%rdi), %rbp
    movq    16(%rdi), %r12
    movq    24(%rdi), %r13
    movq    32(%rdi), %r14
    movq    40(%rdi), %r15
    movq    48(%rdi), %rsp
    movq    56(%rdi), %rax
    movl    $1, (%rsi)
    pushq   %rax
    movl    $1, %eax
    ret

/* setjmp and longjmp on x86-64 (step 22n): part of the programs' library.
 * jmp_buf holds what the System V ABI says a call keeps (RBX, RBP, R12-R15),
 * the stack pointer after setjmp's return and the place it returns to. */

    .text
    /* int setjmp(jmp_buf env): 0 now, longjmp's value later. */
    .global setjmp
    .type   setjmp, @function
setjmp:
    movq    %rbx, 0(%rdi)
    movq    %rbp, 8(%rdi)
    movq    %r12, 16(%rdi)
    movq    %r13, 24(%rdi)
    movq    %r14, 32(%rdi)
    movq    %r15, 40(%rdi)
    leaq    8(%rsp), %rdx                   /* the stack as the caller has it again */
    movq    %rdx, 48(%rdi)
    movq    (%rsp), %rdx                    /* where setjmp returns to */
    movq    %rdx, 56(%rdi)
    xorl    %eax, %eax
    ret
    .size   setjmp, . - setjmp

    /* void longjmp(jmp_buf env, int value): setjmp returns again, with value
     * (1 if value is 0). */
    .global longjmp
    .type   longjmp, @function
longjmp:
    movl    %esi, %eax
    testl   %eax, %eax
    jnz     1f
    incl    %eax
1:  movq    0(%rdi), %rbx
    movq    8(%rdi), %rbp
    movq    16(%rdi), %r12
    movq    24(%rdi), %r13
    movq    32(%rdi), %r14
    movq    40(%rdi), %r15
    movq    48(%rdi), %rsp
    jmpq    *56(%rdi)
    .size   longjmp, . - longjmp

    .section .note.GNU-stack, "", @progbits

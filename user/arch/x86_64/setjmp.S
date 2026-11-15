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

    /* (43a-4) int sigsetjmp(sigjmp_buf env, int save): setjmp, and with
     * save the blocked signals too (env[22] the flag, env[23] the set),
     * which siglongjmp (cstd.c) puts back. The set is asked first; then
     * setjmp is jumped to, so it sees the caller's stack and return. */
    .global sigsetjmp
    .type   sigsetjmp, @function
sigsetjmp:
    movq    %rsi, 176(%rdi)
    testl   %esi, %esi
    jz      1f
    pushq   %rdi                            /* (the stack aligned for the call) */
    leaq    184(%rdi), %rdx
    xorl    %esi, %esi
    xorl    %edi, %edi                      /* SIG_BLOCK of nothing: only asks */
    call    sigprocmask@PLT
    popq    %rdi
1:  jmp     setjmp@PLT
    .size   sigsetjmp, . - sigsetjmp

    /* (43a-4) BSD's _setjmp and _longjmp: setjmp and longjmp (they never
     * touch the signal mask). */
    .global _setjmp
    .type   _setjmp, @function
_setjmp:
    jmp     setjmp@PLT
    .size   _setjmp, . - _setjmp

    .global _longjmp
    .type   _longjmp, @function
_longjmp:
    jmp     longjmp@PLT
    .size   _longjmp, . - _longjmp

    .section .note.GNU-stack, "", @progbits

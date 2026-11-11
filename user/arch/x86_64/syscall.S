/* System calls and the thread start on x86-64: part of the programs' library
 * (libritteros.a and libritteros.so.1), split from start.S in step 22b. */

    .text
    /* A thread's start (thread_create): the stack pointer is at fn, arg
     * is above it; the stack is 16-byte aligned for the call. */
    .global __thread_start
__thread_start:
    movq    (%rsp), %rax
    movq    8(%rsp), %rdi
    xorl    %ebp, %ebp
    call    *%rax
    movl    %eax, %edi
    call    thread_exit
    ud2

    /* int64_t syscall6(number, a, b, c, d, e, f): the C convention passes
     * RDI, RSI, RDX, RCX, R8, R9, then the stack; SYSCALL wants the number
     * in RAX and the arguments in RDI, RSI, RDX, R10, R8, R9. */
    .global syscall6
syscall6:
    movq    %rdi, %rax
    movq    %rsi, %rdi
    movq    %rdx, %rsi
    movq    %rcx, %rdx
    movq    %r8, %r10
    movq    %r9, %r8
    movq    8(%rsp), %r9
    syscall                             /* RCX and R11 are overwritten */
    ret

    /* Where a signal handler returns (22r, kernel arch/x86_64/signal.c): RSP
     * is at the context the kernel kept, which SYS_SIGRETURN puts back. */
    .global __ritteros_signal_return
    .type   __ritteros_signal_return, @function
__ritteros_signal_return:
    movq    %rsp, %rdi
    movl    $77, %eax                   /* SYS_SIGRETURN */
    syscall
    ud2                                 /* (it never returns here) */
    .size   __ritteros_signal_return, . - __ritteros_signal_return

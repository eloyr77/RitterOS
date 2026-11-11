/* System calls and the thread start on AArch64: part of the programs' library
 * (libritteros.a and libritteros.so.1), split from start.S in step 22b. */

    .text
    /* A thread's start (thread_create): the stack pointer is at fn, arg
     * is above it; the stack is 16-byte aligned. */
    .global __thread_start
__thread_start:
    ldr     x9, [sp]
    ldr     x0, [sp, #8]
    mov     x29, #0
    mov     x30, #0
    blr     x9
    bl      thread_exit
    brk     #0

    /* int64_t syscall6(number, a, b, c, d, e, f): arguments arrive in X0-X6;
     * SVC wants the number in X8 and the arguments in X0-X5. */
    .global syscall6
syscall6:
    mov     x8, x0
    mov     x0, x1
    mov     x1, x2
    mov     x2, x3
    mov     x3, x4
    mov     x4, x5
    mov     x5, x6
    svc     #0
    ret

    /* Where a signal handler returns (22r, kernel arch/aarch64/signal.c): SP
     * is at the context the kernel kept, which SYS_SIGRETURN puts back. */
    .global __ritteros_signal_return
    .type   __ritteros_signal_return, %function
__ritteros_signal_return:
    mov     x0, sp
    mov     x8, #77                     /* SYS_SIGRETURN */
    svc     #0
    brk     #0                          /* (it never returns here) */
    .size   __ritteros_signal_return, . - __ritteros_signal_return

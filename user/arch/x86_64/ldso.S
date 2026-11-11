/* ld.so's entry on x86-64 (step 22b): the kernel starts here with the
 * program's first stack (argc, argv, environment, auxiliary vector). */

    .section .text.start, "ax"
    .global _start
_start:
    movq    %rsp, %rbx                  /* the stack as the kernel made it (callee-saved) */
    movq    %rsp, %rdi
    call    ldso_main                   /* maps and links the libraries: the entry point */
    movq    %rbx, %rsp                  /* the program sees the same stack */
    xorl    %ebp, %ebp
    jmp     *%rax

/* A failure in dlopen (22k) returns to the call instead of ending the
 * program: ldso_catch(buf) saves the registers a call keeps (0 then),
 * ldso_throw(buf) goes back there (1 then). buf: 8 words. */
    .text
    .global ldso_catch
ldso_catch:
    movq    %rbx, 0(%rdi)
    movq    %rbp, 8(%rdi)
    movq    %r12, 16(%rdi)
    movq    %r13, 24(%rdi)
    movq    %r14, 32(%rdi)
    movq    %r15, 40(%rdi)
    leaq    8(%rsp), %rdx               /* the stack as the caller will have it */
    movq    %rdx, 48(%rdi)
    movq    (%rsp), %rdx                /* where to return */
    movq    %rdx, 56(%rdi)
    xorl    %eax, %eax
    ret

    .global ldso_throw
ldso_throw:
    movq    0(%rdi), %rbx
    movq    8(%rdi), %rbp
    movq    16(%rdi), %r12
    movq    24(%rdi), %r13
    movq    32(%rdi), %r14
    movq    40(%rdi), %r15
    movq    48(%rdi), %rsp
    movl    $1, %eax
    jmp     *56(%rdi)

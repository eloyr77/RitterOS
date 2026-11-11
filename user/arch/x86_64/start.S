/* Program entry on x86-64 (linked into every program; the system
 * calls and the thread start are the library's, in syscall.S, since 22b). */

    .section .text.start, "ax"
    .global _start
_start:
    /* The kernel put argc at the stack pointer, the argv pointers above it
     * (System V ABI). Per-thread data first (22i: a static program's own; a
     * dynamic program's ld.so made already), then main(argc, argv, envp). */
    xorl    %ebp, %ebp                  /* end of the frame-pointer chain */
    movq    %rsp, %rdi
    call    __ritteros_start@PLT        /* (a position-independent program: by its PLT) */
    movq    (%rsp), %rdi
    leaq    8(%rsp), %rsi
    leaq    16(%rsp,%rdi,8), %rdx       /* envp: after argv's null (22t) */
    call    main
    movl    %eax, %edi
    call    exit@PLT
    ud2

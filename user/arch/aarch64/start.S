/* Program entry on AArch64 (linked into every program; the system
 * calls and the thread start are the library's, in syscall.S, since 22b). */

    .section .text.start, "ax"
    .global _start
_start:
    /* The kernel put argc at the stack pointer, the argv pointers above it
     * (System V ABI). Per-thread data first (22i: a static program's own; a
     * dynamic program's ld.so made already), then main(argc, argv, envp). */
    mov     x29, #0                     /* end of the frame-pointer chain */
    mov     x30, #0
    mov     x0, sp
    bl      __ritteros_start
    ldr     x0, [sp]
    add     x1, sp, #8
    add     x2, sp, #16                 /* envp: after argv's null (22t) */
    add     x2, x2, x0, lsl #3
    bl      main
    bl      exit
    brk     #0

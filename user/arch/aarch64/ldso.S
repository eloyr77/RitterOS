/* ld.so's entry on AArch64 (step 22b): the kernel starts here with the
 * program's first stack (argc, argv, environment, auxiliary vector). */

    .section .text.start, "ax"
    .global _start
_start:
    mov     x19, sp                     /* the stack as the kernel made it (callee-saved) */
    mov     x0, sp
    bl      ldso_main                   /* maps and links the libraries: the entry point */
    mov     sp, x19                     /* the program sees the same stack */
    mov     x29, #0
    mov     x30, #0
    br      x0

/* A per-thread data descriptor's function (22i, R_AARCH64_TLSDESC): the code
 * calls it with x0 = the descriptor and wants the variable's offset from the
 * thread pointer back in x0, every other register kept. Every module's
 * block is at a fixed offset, which ld.so put in the descriptor's second word. */
    .text
    .global ldso_tlsdesc_static
ldso_tlsdesc_static:
    ldr     x0, [x0, #8]
    ret

/* A failure in dlopen (22k) returns to the call instead of ending the
 * program: ldso_catch(buf) saves the registers a call keeps (0 then),
 * ldso_throw(buf) goes back there (1 then). buf: 22 words. */
    .global ldso_catch
ldso_catch:
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    mov     x2, sp
    str     x2, [x0, #96]
    stp     d8, d9, [x0, #104]
    stp     d10, d11, [x0, #120]
    stp     d12, d13, [x0, #136]
    stp     d14, d15, [x0, #152]
    mov     x0, #0
    ret

    .global ldso_throw
ldso_throw:
    ldp     x19, x20, [x0, #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]
    ldr     x2, [x0, #96]
    mov     sp, x2
    ldp     d8, d9, [x0, #104]
    ldp     d10, d11, [x0, #120]
    ldp     d12, d13, [x0, #136]
    ldp     d14, d15, [x0, #152]
    mov     x0, #1
    ret

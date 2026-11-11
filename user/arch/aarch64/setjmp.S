/* setjmp and longjmp on AArch64 (step 22n): part of the programs' library.
 * jmp_buf holds what the AAPCS64 says a call keeps: X19-X28, the frame
 * pointer X29, the return address X30, SP and the low halves of V8-V15
 * (D8-D15). */

    .text
    /* int setjmp(jmp_buf env): 0 now, longjmp's value later. */
    .global setjmp
    .type   setjmp, %function
setjmp:
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
    mov     w0, #0
    ret
    .size   setjmp, . - setjmp

    /* void longjmp(jmp_buf env, int value): setjmp returns again, with value
     * (1 if value is 0). */
    .global longjmp
    .type   longjmp, %function
longjmp:
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
    cmp     w1, #0
    csinc   w0, w1, wzr, ne                 /* value, or 1 for 0 */
    br      x30
    .size   longjmp, . - longjmp

    .section .note.GNU-stack, "", %progbits

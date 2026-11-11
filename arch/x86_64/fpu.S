/*
 * FP/SIMD registers of programs (x87, SSE and, since 32a, AVX): the
 * 512-byte FXSAVE area, or where the processor has AVX the XSAVE area
 * (FXSAVE's 512 bytes, a 64-byte header, the YMM registers' upper halves:
 * 832 bytes). user.c chooses at the boot (fpu_xsave). The kernel itself is
 * compiled without them; only programs' threads have their registers saved
 * and restored (kernel/sched.c).
 */
    .section .text

    /* void arch_fpu_save(void *area): area 64-byte aligned */
    .global arch_fpu_save
arch_fpu_save:
    cmpb    $0, fpu_xsave(%rip)
    je      1f
    movl    $-1, %eax                   /* every part XCR0 has on */
    movl    $-1, %edx
    xsave64 (%rdi)
    ret
1:  fxsave64 (%rdi)
    ret

    /* void arch_fpu_restore(const void *area) */
    .global arch_fpu_restore
arch_fpu_restore:
    cmpb    $0, fpu_xsave(%rip)
    je      1f
    movl    $-1, %eax
    movl    $-1, %edx
    xrstor64 (%rdi)
    ret
1:  fxrstor64 (%rdi)
    ret

    /* void fpu_clean_state(void *area): the state a new program starts
     * with: x87 initialized, SSE with all exceptions masked (MXCSR 0x1F80);
     * user.c clears the vector registers in it. */
    .global fpu_clean_state
fpu_clean_state:
    fninit
    subq    $8, %rsp
    movl    $0x1F80, (%rsp)
    ldmxcsr (%rsp)
    addq    $8, %rsp
    jmp     arch_fpu_save

/*
 * AArch64 kernel entry point. The bootloader jumps here at EL1 with the MMU
 * on (its translation tables), interrupts masked and X0 = struct boot_info *.
 *
 * Two stacks: the kernel runs on SP_EL0; whenever an exception is taken, the
 * CPU switches to SP_EL1 by itself. SP_EL1 therefore gets a stack of its own,
 * so that even a kernel stack overflow can be reported (like the IST stack
 * on x86).
 */

    .section .text.start, "ax"
    .global _start
_start:
    msr     daifset, #0xf               /* no interrupts (debug, SError, IRQ, FIQ) */
    adrp    x1, exception_stack_top     /* SPSel is 1: this sets SP_EL1 */
    add     x1, x1, :lo12:exception_stack_top
    mov     sp, x1
    msr     spsel, #0                   /* from now on SP means SP_EL0 */
    adrp    x1, __stack_top
    add     x1, x1, :lo12:__stack_top
    mov     sp, x1
    mov     x29, #0                     /* end of the frame-pointer chain */
    mov     x30, #0
    bl      kernel_main                 /* X0 still holds boot_info */

halt:                                   /* kernel_main should never return */
    msr     daifset, #0xf
    wfe
    b       halt

/* The boot stack. The linker script places it after an unmapped guard page
 * and defines __stack_bottom and __stack_top around it. */
    .section .stack, "aw", @nobits
    .balign 16
    .skip   64 * 1024

/* The exception stack (SP_EL1). */
    .section .bss
    .balign 16
    .global exception_stack, exception_stack_top
exception_stack:
    .skip   16 * 1024
exception_stack_top:

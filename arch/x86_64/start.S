/*
 * Kernel entry point. The bootloader jumps here in 64-bit long mode with
 * RDI = struct boot_info *. We are still running on the firmware's stack,
 * which lives in memory the kernel considers free, so switch to our own
 * stack first, then call into C.
 */

    .section .text.start, "ax"
    .global _start
_start:
    cli                         /* no interrupt handlers yet */
    cld                         /* the C ABI expects the direction flag clear */
    lea     __stack_top(%rip), %rsp
    xor     %ebp, %ebp          /* end of the frame-pointer chain for debuggers */
    call    kernel_main         /* RDI still holds boot_info */

halt:                           /* kernel_main should never return */
    cli
    hlt
    jmp     halt

/* The boot stack. The linker script places it after an unmapped guard page
 * and defines __stack_bottom and __stack_top around it. */
    .section .stack, "aw", @nobits
    .balign 16
    .skip   64 * 1024

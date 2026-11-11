/*
 * The exception vector table (VBAR_EL1). 16 entries of 128 bytes: four kinds
 * (synchronous, IRQ, FIQ, SError) for each of four origins:
 *   0x000  current EL, using SP_EL0   (the kernel, which runs on SP_EL0)
 *   0x200  current EL, using SP_EL1   (an exception inside a handler)
 *   0x400  lower EL, AArch64          (a program, at EL0)
 *   0x600  lower EL, AArch32
 * Each entry saves X0/X1, puts its number in X0 and jumps to the common
 * code, which saves the rest of the registers in a struct exception_frame
 * on the exception stack (SP_EL1) and calls exception_handler(frame).
 *
 * An IRQ while the kernel ran (entry 1) is handled on the interrupted
 * thread's own stack (SP_EL0), not on the shared exception stack: the
 * scheduler may switch threads at its end, and the frame must stay with
 * its thread. Exceptions (entry 0) keep the exception stack, so that even
 * a kernel stack overflow can be reported.
 *
 * From EL0, SP_EL1 is the program's kernel stack (arch_run_user sets it).
 * el0_common saves the frame there, points SP_EL1 back to the exception
 * stack and runs the handler on SP_EL0 below the frame, like all kernel
 * code: a fault in the kernel while it works for the program is reported
 * as usual. Returning, it sets SP_EL1 to the program's kernel stack again.
 */

#define FRAME_SIZE 304          /* sizeof(struct exception_frame) */

.macro VECTOR number
    .balign 0x80
    sub     sp, sp, #FRAME_SIZE
    stp     x0, x1, [sp, #0]
    mov     x0, #\number
    b       exception_common
.endm

.macro VECTOR_EL0 number
    .balign 0x80
    sub     sp, sp, #FRAME_SIZE
    stp     x0, x1, [sp, #0]
    mov     x0, #\number
    b       el0_common
.endm

    .section .text
    .balign 0x800
    .global exception_vectors
exception_vectors:
    VECTOR 0
    /* Entry 1: IRQ from EL1 on SP_EL0. */
    .balign 0x80
    msr     spsel, #0                   /* the interrupted thread's stack */
    sub     sp, sp, #FRAME_SIZE
    stp     x0, x1, [sp, #0]
    mov     x0, #1
    b       irq_kernel_common
    VECTOR 2
    VECTOR 3
    VECTOR 4
    VECTOR 5
    VECTOR 6
    VECTOR 7
    VECTOR_EL0 8
    VECTOR_EL0 9
    VECTOR_EL0 10
    VECTOR_EL0 11
    VECTOR 12
    VECTOR 13
    VECTOR 14
    VECTOR 15

exception_common:
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30, [sp, #240]
    mrs     x2, sp_el0
    mrs     x3, elr_el1
    mrs     x4, spsr_el1
    mrs     x5, esr_el1
    mrs     x6, far_el1
    stp     x2, x3, [sp, #248]          /* sp, elr */
    stp     x4, x5, [sp, #264]          /* spsr, esr */
    stp     x6, x0, [sp, #280]          /* far, vector number */

    mov     x0, sp
    bl      exception_handler

    ldp     x3, x4, [sp, #256]          /* the handler may change elr (e.g. after brk) */
    msr     elr_el1, x3
    msr     spsr_el1, x4
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30, [sp, #240]
    add     sp, sp, #FRAME_SIZE
    eret

el0_common:
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30, [sp, #240]
    mrs     x2, sp_el0                  /* the program's stack pointer */
    mrs     x3, elr_el1
    mrs     x4, spsr_el1
    mrs     x5, esr_el1
    mrs     x6, far_el1
    stp     x2, x3, [sp, #248]
    stp     x4, x5, [sp, #264]
    stp     x6, x0, [sp, #280]

    mov     x19, sp                     /* the frame; X19 survives the call */
    mrs     x1, tpidr_el1               /* SP_EL1: this core's exception stack */
    ldr     x1, [x1, #24]               /* (65c-2: struct cpu's exception_stack) */
    mov     sp, x1
    msr     spsel, #0                   /* kernel code runs on SP_EL0 ... */
    mov     sp, x19                     /* ... below the frame */
    mov     x0, x19
    bl      exception_handler

    msr     daifset, #2                 /* no interrupt while the stacks change */
    msr     spsel, #1
    add     x1, x19, #FRAME_SIZE        /* SP_EL1: the program's kernel stack */
    mov     sp, x1
    ldr     x2, [x19, #248]
    msr     sp_el0, x2
    ldp     x3, x4, [x19, #256]
    msr     elr_el1, x3
    msr     spsr_el1, x4
    ldp     x0, x1, [x19, #0]
    ldp     x2, x3, [x19, #16]
    ldp     x4, x5, [x19, #32]
    ldp     x6, x7, [x19, #48]
    ldp     x8, x9, [x19, #64]
    ldp     x10, x11, [x19, #80]
    ldp     x12, x13, [x19, #96]
    ldp     x14, x15, [x19, #112]
    ldp     x16, x17, [x19, #128]
    ldp     x20, x21, [x19, #160]
    ldp     x22, x23, [x19, #176]
    ldp     x24, x25, [x19, #192]
    ldp     x26, x27, [x19, #208]
    ldp     x28, x29, [x19, #224]
    ldr     x30, [x19, #240]
    ldp     x18, x19, [x19, #144]       /* last: X19 was the base */
    eret

/* IRQ from EL1, frame on SP_EL0 (SP_EL0 cannot be read with MRS while it is
 * the current stack: the frame records SP as it was before the frame). */
irq_kernel_common:
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30, [sp, #240]
    add     x2, sp, #FRAME_SIZE
    mrs     x3, elr_el1
    mrs     x4, spsr_el1
    mrs     x5, esr_el1
    mrs     x6, far_el1
    stp     x2, x3, [sp, #248]
    stp     x4, x5, [sp, #264]
    stp     x6, x0, [sp, #280]

    mov     x0, sp
    bl      exception_handler           /* may switch threads and come back later */

    ldp     x3, x4, [sp, #256]
    msr     elr_el1, x3
    msr     spsr_el1, x4
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30, [sp, #240]
    add     sp, sp, #FRAME_SIZE
    eret                                /* SPSR says EL1 with SP_EL0 */

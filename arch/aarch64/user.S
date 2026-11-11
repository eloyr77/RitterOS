/*
 * Entering user mode (EL0).
 *
 * arch_enter_user sets SP_EL1 to the thread's kernel stack (exceptions from
 * EL0 arrive there, see vectors.S) and SP_EL0 to the program's stack, then
 * "returns" to EL0 with ERET: SPSR_EL1 = 0 means EL0 with interrupts
 * unmasked. It never comes back: the thread's kernel code below it is
 * abandoned, and its stack is reused for exceptions from the program.
 */

    .section .text

    /* void arch_enter_user(uint64_t entry, uint64_t user_stack,
     *                      uint64_t kernel_stack_top) */
    /* void arch_resume_user(const struct exception_frame *frame,
     *                       uint64_t kernel_stack_top): fork's child (56a)
     * leaves as el0_common does, with every register from the frame. */
    .global arch_resume_user
arch_resume_user:
    msr     daifset, #2
    mov     x19, x0
    msr     spsel, #1
    mov     sp, x1                      /* SP_EL1: the program's kernel stack */
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

    .global arch_enter_user
arch_enter_user:
    msr     daifset, #2
    msr     spsel, #1
    mov     sp, x2                      /* SP_EL1: the program's kernel stack */
    msr     sp_el0, x1                  /* the program's own stack */
    msr     elr_el1, x0
    msr     spsr_el1, xzr               /* EL0t, nothing masked */
    /* The program starts with no kernel values in its registers. */
    mov     x0, #0
    mov     x1, #0
    mov     x2, #0
    mov     x3, #0
    mov     x4, #0
    mov     x5, #0
    mov     x6, #0
    mov     x7, #0
    mov     x8, #0
    mov     x9, #0
    mov     x10, #0
    mov     x11, #0
    mov     x12, #0
    mov     x13, #0
    mov     x14, #0
    mov     x15, #0
    mov     x16, #0
    mov     x17, #0
    mov     x18, #0
    mov     x19, #0
    mov     x20, #0
    mov     x21, #0
    mov     x22, #0
    mov     x23, #0
    mov     x24, #0
    mov     x25, #0
    mov     x26, #0
    mov     x27, #0
    mov     x28, #0
    mov     x29, #0
    mov     x30, #0
    eret

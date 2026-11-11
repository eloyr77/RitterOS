/*
 * Switching between threads (kernel/sched.c).
 *
 * void arch_context_switch(uint64_t *save, uint64_t load): save the
 * callee-saved registers X19-X30 on the stack, store SP in *save, load SP
 * from load, restore the other thread's registers and return (X30): into
 * the other thread, where it once called arch_context_switch itself (or
 * into thread_start, for a new thread). The kernel runs on SP_EL0.
 */
    .section .text
    .global arch_context_switch
arch_context_switch:
    sub     sp, sp, #96
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]
    mov     x2, sp
    str     x2, [x0]
    mov     sp, x1
    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    add     sp, sp, #96
    ret

/* A new thread starts here (arch_thread_init put X19 = fn, X20 = arg). */
    .global thread_start
thread_start:
    bl      sched_thread_start
    mov     x0, x20
    blr     x19
    bl      thread_exit
    brk     #0

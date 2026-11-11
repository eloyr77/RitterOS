/*
 * AArch64: where another core starts (65b, smp.c). PSCI's CPU_ON starts it
 * at arm_core_entry's physical address, at EL1 with the MMU and caches off,
 * on SP_EL1, x0 = the context given: the physical address of its start
 * block (struct start_block in smp.c, offsets below). The code turns the MMU
 * on with the kernel's tables in TTBR1 and, in TTBR0 for the moment, a map
 * of this code at its own address (else the next instruction would be gone),
 * then jumps to arm_core_high in the upper half, switches the lower half
 * off again, takes its stacks and calls arm_core_start(its struct cpu).
 */

#define BLOCK_MAIR       0
#define BLOCK_TCR_START  8
#define BLOCK_TTBR0      16
#define BLOCK_TTBR1      24
#define BLOCK_SCTLR      32
#define BLOCK_HIGH       40      /* arm_core_high, kernel address */
#define BLOCK_SELF       48      /* the block, kernel address */
#define BLOCK_TCR        56
#define BLOCK_VBAR       64
#define BLOCK_SP_EL1     72
#define BLOCK_SP_EL0     80
#define BLOCK_CPU        88

    .text
    .global arm_core_entry
    .balign 8
arm_core_entry:
    msr     daifset, #0xf
    mrs     x1, currentel
    cmp     x1, #4                      /* EL1 (bits 3:2) */
    b.ne    stuck
    ldr     x1, [x0, #BLOCK_MAIR]
    msr     mair_el1, x1
    ldr     x1, [x0, #BLOCK_TCR_START]
    msr     tcr_el1, x1
    ldr     x1, [x0, #BLOCK_TTBR0]
    msr     ttbr0_el1, x1
    ldr     x1, [x0, #BLOCK_TTBR1]
    msr     ttbr1_el1, x1
    ldr     x2, [x0, #BLOCK_HIGH]       /* read now: the block is reached */
    ldr     x3, [x0, #BLOCK_SELF]       /* by its physical address only */
    ldr     x4, [x0, #BLOCK_SCTLR]
    isb
    tlbi    vmalle1
    dsb     ish
    isb
    msr     sctlr_el1, x4               /* MMU and caches on */
    isb
    br      x2

stuck:                                  /* not at EL1: not supported (yet) */
    wfe
    b       stuck

    .global arm_core_high
arm_core_high:
    ldr     x1, [x3, #BLOCK_TCR]        /* the kernel's: lower half off */
    msr     tcr_el1, x1
    isb
    tlbi    vmalle1
    dsb     ish
    isb
    ldr     x1, [x3, #BLOCK_VBAR]
    msr     vbar_el1, x1
    ldr     x1, [x3, #BLOCK_SP_EL1]     /* SPSel is 1: this sets SP_EL1 */
    mov     sp, x1
    msr     spsel, #0                   /* the kernel runs on SP_EL0 */
    ldr     x1, [x3, #BLOCK_SP_EL0]
    mov     sp, x1
    ldr     x0, [x3, #BLOCK_CPU]
    mov     x29, #0
    mov     x30, #0
    bl      arm_core_start
    b       stuck

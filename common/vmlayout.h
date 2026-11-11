/*
 * The virtual address space (x86-64 and AArch64, 48-bit addresses):
 *
 *   0x0000000000000000 - 0x00007FFFFFFFFFFF  lower half: the running program
 *                                            (its own page tables), empty
 *                                            while the kernel runs alone
 *   0xFFFF800000000000 - ...                 direct map: physical address P is
 *                                            mapped at HHDM_BASE + P
 *   0xFFFFFFFF80000000 - ...                 kernel image (top 2 GiB, so the
 *                                            compiler's "kernel" code model can
 *                                            use 32-bit signed addresses)
 *   MODULE_BASE, MODULE_SIZE                 (67a) the kernel's modules, from
 *                                            32 MiB after the image's start:
 *                                            within the 2 GiB of x86's code
 *                                            model and the 128 MiB an ARM
 *                                            call reaches, from any of it
 */
#ifndef RITTEROS_VMLAYOUT_H
#define RITTEROS_VMLAYOUT_H

#define HHDM_BASE           0xFFFF800000000000ULL   /* "higher half direct map" */
#define KERNEL_VIRT_BASE    0xFFFFFFFF80000000ULL   /* must match kernel/linker.ld */
#define MODULE_BASE         (KERNEL_VIRT_BASE + 0x2000000ULL)
#define MODULE_SIZE         0x4000000ULL

/* Programs: loaded from USER_LOAD_MIN up (user/user.ld links them at
 * 0x400000), the stack ends at USER_STACK_TOP, with unmapped room below it
 * as its guard. Nothing at or above USER_TOP. Stacks (54d) are anonymous
 * mappings, their pages made when first touched: each thread's stack lies
 * in a slot of USER_STACK_SLOT bytes of its own (the main thread's is slot
 * 0), USER_STACK_SIZE for the main thread and by default, up to
 * USER_STACK_MAX when a thread asks (the rest of the slot is the guard). */
#define USER_LOAD_MIN       0x0000000000010000ULL
#define USER_STACK_TOP      0x00007FFFFFFF0000ULL
#define USER_STACK_SIZE     (8ULL * 1024 * 1024)
#define USER_STACK_SLOT     (64ULL * 1024 * 1024)
#define USER_STACK_MAX      (USER_STACK_SLOT - 64ULL * 1024)
#define USER_TOP            0x0000800000000000ULL

#endif

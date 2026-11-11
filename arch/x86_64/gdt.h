/*
 * Global Descriptor Table and Task State Segment.
 *
 * In 64-bit mode segmentation is mostly switched off: base and limit of
 * code/data segments are ignored. The GDT still tells the CPU the privilege
 * level (ring 0 kernel, ring 3 user) and that code runs in 64-bit mode, and
 * it holds the TSS descriptor, which supplies stacks for interrupts.
 */
#ifndef RITTEROS_GDT_H
#define RITTEROS_GDT_H

#include <stdint.h>

/* Segment selectors: byte offset into the GDT, plus the privilege level
 * in the low two bits for user segments. The order kernel code, kernel data,
 * user data, user code is required by the SYSCALL/SYSRET instructions. */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   (0x18 | 3)
#define GDT_USER_CODE   (0x20 | 3)
#define GDT_TSS         0x28

/* Build our GDT and TSS, load them, and reload all segment registers. */
void gdt_init(void);
void gdt_load(void);                    /* (65b) the GDT and segments, no TSS */

/* (65c) A started core's own TSS (core index), with its double fault stack;
 * loads the task register. */
void gdt_core_tss(int index, uint64_t double_fault_stack_top);
void gdt_reload_tss(int index);    /* (37g-1) after a sleep */

/* The GDT's size in bytes: the first selector beyond it. */
uint16_t gdt_size(void);

/* The stack for interrupts and exceptions that arrive while a program runs
 * (ring 3): the CPU switches to it before saving anything. */
void tss_set_rsp0(uint64_t stack_top);

/* Set Interrupt Stack Table entry n (1-7) to the given stack top. IDT gates
 * that name IST n always switch to this stack, whatever RSP was before. */
void tss_set_ist(int n, uint64_t stack_top);

#endif

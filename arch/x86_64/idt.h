/*
 * Interrupt Descriptor Table: for each of the 256 interrupt vectors, the
 * address of its handler and how to enter it. Vectors 0-31 are the CPU's
 * exceptions; the rest will be used for hardware interrupts later.
 */
#ifndef RITTEROS_IDT_H
#define RITTEROS_IDT_H

#include <stdint.h>

/* Register state saved by the stubs in isr.S, lowest address first. */
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    /* pushed by the CPU */
    uint64_t rip, cs, rflags, rsp, ss;
};

/* Build the IDT with the 32 exception handlers and load it. Needs the GDT
 * and TSS from gdt_init(). */
void idt_init(void);
void idt_load(void);                    /* (65b) on a starting core */

#endif

#include "idt.h"
#include "gdt.h"
#include "kprintf.h"

/* One 16-byte IDT entry (a "gate"). */
struct idt_entry {
    uint16_t offset_low;      /* handler address bits 0-15 */
    uint16_t selector;        /* code segment to run the handler in */
    uint8_t ist;              /* 0, or 1-7: switch to that TSS IST stack */
    uint8_t type_attr;        /* present | privilege | gate type */
    uint16_t offset_mid;      /* bits 16-31 */
    uint32_t offset_high;     /* bits 32-63 */
    uint32_t reserved;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* Present, ring 0, 64-bit interrupt gate (the CPU clears IF on entry). */
#define GATE_INTERRUPT 0x8E

#define IST_DOUBLE_FAULT 1

extern const uint64_t isr_stub_table[32];
extern char irq_stubs[];                    /* 16 bytes per vector from 32 */

static struct idt_entry idt[256] __attribute__((aligned(16)));

/* Dedicated stack for double faults, used even if the normal stack is broken. */
static uint8_t double_fault_stack[16 * 1024] __attribute__((aligned(16)));

static void set_gate(int vector, uint64_t handler, uint8_t ist)
{
    idt[vector].offset_low = handler & 0xFFFF;
    idt[vector].selector = GDT_KERNEL_CODE;
    idt[vector].ist = ist;
    idt[vector].type_attr = GATE_INTERRUPT;
    idt[vector].offset_mid = (handler >> 16) & 0xFFFF;
    idt[vector].offset_high = handler >> 32;
    idt[vector].reserved = 0;
}

/* Load the IDT (65b: also on a starting core). */
void idt_load(void)
{
    struct idtr idtr = { sizeof(idt) - 1, (uint64_t)idt };
    __asm__ volatile ("lidt %0" : : "m"(idtr));
}

void idt_init(void)
{
    uint64_t df_stack_top = (uint64_t)double_fault_stack + sizeof(double_fault_stack);
    tss_set_ist(IST_DOUBLE_FAULT, df_stack_top);

    for (int v = 0; v < 32; v++)
        set_gate(v, isr_stub_table[v], v == 8 ? IST_DOUBLE_FAULT : 0);
    for (int v = 32; v < 256; v++)
        set_gate(v, (uint64_t)irq_stubs + 16 * (uint64_t)(v - 32), 0);

    idt_load();

    struct idtr check;
    __asm__ volatile ("sidt %0" : "=m"(check));
    kprintf("  IDT:           %p, 256 vectors: exceptions 0-31, interrupts 32-255, #DF stack %p\n",
            (void *)check.base, (void *)df_stack_top);
}

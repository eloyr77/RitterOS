#include "gdt.h"
#include "arch.h"
#include "cpus.h"
#include "kprintf.h"
#include "percpu.h"

/* 64-bit Task State Segment. No longer used for task switching; the CPU
 * reads stack pointers from it when an interrupt arrives. */
struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];          /* stack for entering ring 0/1/2 from a lower privilege */
    uint64_t reserved1;
    uint64_t ist[7];          /* Interrupt Stack Table: dedicated stacks for chosen vectors */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;      /* offset of the I/O permission bitmap; none if >= limit */
} __attribute__((packed));

/* Operand of the LGDT instruction. */
struct gdtr {
    uint16_t limit;           /* size of the table in bytes, minus one */
    uint64_t base;
} __attribute__((packed));

/*
 * Segment descriptor bits (Intel SDM vol. 3, 3.4.5 / AMD APM vol. 2, 4.8):
 *   access byte: P (present) | DPL (privilege, 2 bits) | S (code/data) | type
 *   flags:       G (4 KiB granularity) | L (64-bit code) | D/B (32-bit)
 */
#define ACCESS_PRESENT   0x80
#define ACCESS_RING3     0x60
#define ACCESS_SEGMENT   0x10      /* code or data, not a system descriptor */
#define ACCESS_CODE      0x0A      /* executable, readable */
#define ACCESS_DATA      0x02      /* writable */
#define ACCESS_TSS       0x09      /* system descriptor: available 64-bit TSS */
#define FLAG_GRANULARITY 0x8
#define FLAG_LONG_MODE   0x2
#define FLAG_32BIT       0x4

static uint64_t descriptor(uint8_t access, uint8_t flags)
{
    /* Base 0, limit 0xFFFFF pages: ignored in 64-bit mode but conventional. */
    return 0x000F00000000FFFFULL | ((uint64_t)access << 40) | ((uint64_t)flags << 52);
}

/* (65c) A TSS per core: each has its own stack pointers. */
static struct tss tss[MAX_CPUS];

/* Five 8-byte slots, then two per core's TSS descriptor (it holds a 64-bit
 * base): core n's at GDT_TSS + 16 n. */
#define GDT_SLOTS (5 + 2 * MAX_CPUS)
static uint64_t gdt[GDT_SLOTS] __attribute__((aligned(16)));

static void set_tss_descriptor(int index, uint64_t base, uint32_t limit)
{
    gdt[index] = (limit & 0xFFFF) |
                 ((base & 0xFFFFFF) << 16) |
                 ((uint64_t)(ACCESS_PRESENT | ACCESS_TSS) << 40) |
                 ((uint64_t)((limit >> 16) & 0xF) << 48) |
                 (((base >> 24) & 0xFF) << 56);
    gdt[index + 1] = base >> 32;
}

void tss_set_rsp0(uint64_t stack_top)
{
    tss[arch_this_cpu()->index].rsp[0] = stack_top;
}

void tss_set_ist(int n, uint64_t stack_top)
{
    if (n >= 1 && n <= 7)
        tss[arch_this_cpu()->index].ist[n - 1] = stack_top;
}

/* (65c) A started core's TSS: its descriptor, the double fault's stack, and
 * the task register loaded (after gdt_load). */
void gdt_core_tss(int index, uint64_t double_fault_stack_top)
{
    struct tss *t = &tss[index];
    t->iomap_base = sizeof(*t);
    t->ist[0] = double_fault_stack_top;     /* IST1: idt.c's IST_DOUBLE_FAULT */
    set_tss_descriptor(5 + 2 * index, (uint64_t)t, sizeof(*t) - 1);
    __asm__ volatile ("ltr %w0" : : "r"((uint16_t)(GDT_TSS + 16 * index)));
}

/* (37g-1) After a sleep: core index's TSS again as it was (its descriptor
 * written anew: ltr refuses one marked busy, as the last ltr left it). */
void gdt_reload_tss(int index)
{
    struct tss *t = &tss[index];
    set_tss_descriptor(5 + 2 * index, (uint64_t)t, sizeof(*t) - 1);
    __asm__ volatile ("ltr %w0" : : "r"((uint16_t)(GDT_TSS + 16 * index)));
}

uint16_t gdt_size(void)
{
    return sizeof(gdt);
}

/* Current state of the segment registers, for the log. */
struct segments {
    uint16_t cs, ds, ss, tr;
    struct gdtr gdtr;
};

static void read_segments(struct segments *s)
{
    __asm__ volatile ("mov %%cs, %0" : "=r"(s->cs));
    __asm__ volatile ("mov %%ds, %0" : "=r"(s->ds));
    __asm__ volatile ("mov %%ss, %0" : "=r"(s->ss));
    __asm__ volatile ("str %0" : "=r"(s->tr));
    __asm__ volatile ("sgdt %0" : "=m"(s->gdtr));
}

static void log_segments(const char *label, const struct segments *s)
{
    kprintf("%scs=0x%04X ds=0x%04X ss=0x%04X tr=0x%04X, GDT at %p (%u slots)\n", label,
            s->cs, s->ds, s->ss, s->tr, (void *)s->gdtr.base, (s->gdtr.limit + 1u) / 8);
}

/* Load the GDT and the segment registers (65b: also a starting core's,
 * which gets no TSS until it runs threads). */
void gdt_load(void)
{
    struct gdtr gdtr = { sizeof(gdt) - 1, (uint64_t)gdt };
    __asm__ volatile ("lgdt %0" : : "m"(gdtr));

    /*
     * CS cannot be loaded with MOV. Push the new selector and a return
     * address, then "return" to the next instruction with a far return.
     * The data segment registers can be loaded directly.
     */
    __asm__ volatile (
        "pushq %[code]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movw %[data], %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        :
        : [code] "i"(GDT_KERNEL_CODE), [data] "i"(GDT_KERNEL_DATA)
        : "rax", "memory");
}

void gdt_init(void)
{
    struct segments before, after;
    read_segments(&before);

    gdt[0] = 0;                                                       /* null */
    gdt[1] = descriptor(ACCESS_PRESENT | ACCESS_SEGMENT | ACCESS_CODE,
                        FLAG_GRANULARITY | FLAG_LONG_MODE);           /* kernel code */
    gdt[2] = descriptor(ACCESS_PRESENT | ACCESS_SEGMENT | ACCESS_DATA,
                        FLAG_GRANULARITY | FLAG_32BIT);               /* kernel data */
    gdt[3] = descriptor(ACCESS_PRESENT | ACCESS_RING3 | ACCESS_SEGMENT | ACCESS_DATA,
                        FLAG_GRANULARITY | FLAG_32BIT);               /* user data */
    gdt[4] = descriptor(ACCESS_PRESENT | ACCESS_RING3 | ACCESS_SEGMENT | ACCESS_CODE,
                        FLAG_GRANULARITY | FLAG_LONG_MODE);           /* user code */
    tss[0].iomap_base = sizeof(tss[0]);                               /* no I/O bitmap */
    set_tss_descriptor(5, (uint64_t)&tss[0], sizeof(tss[0]) - 1);     /* the boot core's TSS */

    gdt_load();

    /* Load the task register; this marks the TSS descriptor as busy. */
    __asm__ volatile ("ltr %w0" : : "r"((uint16_t)GDT_TSS));

    read_segments(&after);
    log_segments("  segments:      firmware ", &before);
    log_segments("                 RitterOS ", &after);

    static const char *const names[] = {
        "null", "kernel code", "kernel data", "user data", "user code",
        "TSS (low)", "TSS (high)",
    };
    for (int i = 0; i < 7; i++)
        kprintf("    0x%02X  0x%016lX  %s\n", i * 8, gdt[i], names[i]);
}

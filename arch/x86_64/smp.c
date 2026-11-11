/*
 * x86-64's other cores (65b). The MADT lists every core's Local APIC; the
 * boot core starts another by sending it INIT and then STARTUP, whose
 * vector names a 4 KiB page below 1 MiB where the core begins, in real
 * mode (16-bit, no paging), as after a reset. The code there climbs:
 *
 *   16-bit: load a GDT of its own, switch protection on, jump to 32-bit code
 *   32-bit: PAE on, CR3 = start-up tables, EFER.LME (and NXE), paging on:
 *           long mode, still in a 32-bit code segment; jump to 64-bit code
 *   64-bit: the stack and the block from the page's data, jump to
 *           x86_core_start in the kernel (upper half)
 *
 * The start-up tables (below 4 GiB: a 32-bit CR3) map the kernel's upper
 * half and the first 2 MiB at their own address, executable, so the
 * instructions right after paging comes on are still there. The code is
 * written as bytes: it begins in 16-bit mode, which our own assembler does
 * not encode (each instruction's bytes are commented; they were compared
 * with clang's assembly of the same lines).
 *
 * x86_core_start loads the kernel's GDT, IDT, CR0, CR4, PAT and page tables,
 * takes its block (GS), its own TSS (65c) with a double fault stack, the
 * system-call registers and its Local APIC with the tick, and goes online
 * (kernel/cpus.c): its code becomes its idle thread. One core starts at a
 * time, so one page holds every core's data in turn.
 */
#include "apic.h"
#include "arch.h"
#include "cpu.h"
#include "cpus.h"
#include "gdt.h"
#include "idt.h"
#include "kprintf.h"
#include "madt.h"
#include "memory.h"
#include "paging.h"
#include "pmm.h"
#include "string.h"

#define MADT_X2APIC         0x09
#define MADT_ENABLED        (1u << 0)

#define EFER_SCE            (1ULL << 0)
#define EFER_LME            (1ULL << 8)

#define PTE_PRESENT         (1ULL << 0)
#define PTE_WRITABLE        (1ULL << 1)
#define PTE_LARGE           (1ULL << 7)     /* a 2 MiB page in a directory */

/* The start-up page: code at 0x000 (16-bit), 0x040 (32-bit), 0x080
 * (64-bit), data from 0xF00, the GDT at 0xF80. */
#define CODE32              0x040
#define CODE64              0x080
#define DATA_GDTR           0xF00   /* limit (2), base (4) */
#define DATA_CR3            0xF08   /* 32 bits */
#define DATA_ENTRY          0xF10   /* x86_core_start */
#define DATA_STACK          0xF18
#define DATA_CPU            0xF20   /* its struct cpu, the first argument */
#define DATA_JUMP32         0xF30   /* far pointer: offset (4), selector (2) */
#define DATA_JUMP64         0xF38
#define DATA_EFER           0xF40   /* 32 bits */
#define GDT_AT              0xF80
#define START_CODE32        0x08    /* the start-up GDT's selectors */
#define START_DATA          0x10
#define START_CODE64        0x18

static const uint8_t code16[] = {
    0xFA,                               /* cli                                  */
    0x66, 0x31, 0xDB,                   /* xor   %ebx, %ebx                     */
    0x8C, 0xCB,                         /* mov   %cs, %bx                       */
    0x66, 0xC1, 0xE3, 0x04,             /* shl   $4, %ebx       ebx = the page  */
    0x8C, 0xC8,                         /* mov   %cs, %ax                       */
    0x8E, 0xD8,                         /* mov   %ax, %ds                       */
    0x66, 0x0F, 0x01, 0x16, 0x00, 0x0F, /* lgdtl 0xF00                          */
    0x0F, 0x20, 0xC0,                   /* mov   %cr0, %eax                     */
    0x66, 0x83, 0xC8, 0x01,             /* or    $1, %eax       protection on   */
    0x0F, 0x22, 0xC0,                   /* mov   %eax, %cr0                     */
    0x66, 0xFF, 0x2E, 0x30, 0x0F,       /* ljmpl *0xF30                         */
};

static const uint8_t code32[] = {
    0x66, 0xB8, 0x10, 0x00,             /* mov   $0x10, %ax                     */
    0x8E, 0xD8,                         /* mov   %ax, %ds                       */
    0x8E, 0xC0,                         /* mov   %ax, %es                       */
    0x8E, 0xD0,                         /* mov   %ax, %ss                       */
    0x0F, 0x20, 0xE0,                   /* mov   %cr4, %eax                     */
    0x83, 0xC8, 0x20,                   /* or    $0x20, %eax    PAE             */
    0x0F, 0x22, 0xE0,                   /* mov   %eax, %cr4                     */
    0x8B, 0x83, 0x08, 0x0F, 0, 0,       /* mov   0xF08(%ebx), %eax              */
    0x0F, 0x22, 0xD8,                   /* mov   %eax, %cr3                     */
    0xB9, 0x80, 0x00, 0x00, 0xC0,       /* mov   $0xC0000080, %ecx   EFER       */
    0x8B, 0x83, 0x40, 0x0F, 0, 0,       /* mov   0xF40(%ebx), %eax              */
    0x31, 0xD2,                         /* xor   %edx, %edx                     */
    0x0F, 0x30,                         /* wrmsr                LME, NXE        */
    0x0F, 0x20, 0xC0,                   /* mov   %cr0, %eax                     */
    0x0D, 0x00, 0x00, 0x00, 0x80,       /* or    $0x80000000, %eax   paging     */
    0x0F, 0x22, 0xC0,                   /* mov   %eax, %cr0                     */
    0xFF, 0xAB, 0x38, 0x0F, 0, 0,       /* ljmp  *0xF38(%ebx)                   */
};

static const uint8_t code64[] = {
    0x89, 0xDB,                         /* mov   %ebx, %ebx     (upper half 0)  */
    0x48, 0x8B, 0xA3, 0x18, 0x0F, 0, 0, /* mov   0xF18(%rbx), %rsp              */
    0x48, 0x8B, 0xBB, 0x20, 0x0F, 0, 0, /* mov   0xF20(%rbx), %rdi              */
    0x48, 0x8B, 0x83, 0x10, 0x0F, 0, 0, /* mov   0xF10(%rbx), %rax              */
    0xFF, 0xE0,                         /* jmp   *%rax                          */
};

static uint64_t page;                   /* the start-up page (physical) */
static uint64_t *start_pml4;            /* the start-up tables' top */
static uint64_t kernel_cr0, kernel_cr4, kernel_pat, kernel_efer;

static void put32(uint64_t at, uint32_t v)
{
    memcpy((uint8_t *)phys_to_virt(page) + at, &v, 4);
}

static void put64(uint64_t at, uint64_t v)
{
    memcpy((uint8_t *)phys_to_virt(page) + at, &v, 8);
}

static uint64_t zeroed_page_below_4g(void)
{
    uint64_t p = pmm_alloc_pages_below(1, 1ULL << 32);
    if (p)
        memset(phys_to_virt(p), 0, PMM_PAGE_SIZE);
    return p;
}

/* The page's code and GDT, and the start-up tables. */
static const char *prepare(void)
{
    page = pmm_low_page();
    if (!page)
        return "no free page below 640 KiB for the start-up code";
    uint64_t pml4 = zeroed_page_below_4g(), pdpt = zeroed_page_below_4g(),
             pd = zeroed_page_below_4g();
    if (!pml4 || !pdpt || !pd)
        return "no memory for the start-up tables";
    start_pml4 = phys_to_virt(pml4);
    start_pml4[0] = pdpt | PTE_PRESENT | PTE_WRITABLE;
    ((uint64_t *)phys_to_virt(pdpt))[0] = pd | PTE_PRESENT | PTE_WRITABLE;
    ((uint64_t *)phys_to_virt(pd))[0] = 0 | PTE_PRESENT | PTE_WRITABLE | PTE_LARGE;  /* 0 - 2 MiB */

    uint8_t *p = phys_to_virt(page);
    memset(p, 0xCC, PMM_PAGE_SIZE);                 /* int3 anywhere else */
    memcpy(p, code16, sizeof(code16));
    memcpy(p + CODE32, code32, sizeof(code32));
    memcpy(p + CODE64, code64, sizeof(code64));
    static const uint64_t gdt[4] = {
        0,
        0x00CF9A000000FFFFULL,                      /* 32-bit code, 0 - 4 GiB */
        0x00CF92000000FFFFULL,                      /* data, 0 - 4 GiB */
        0x00AF9A000000FFFFULL,                      /* 64-bit code */
    };
    memcpy(p + GDT_AT, gdt, sizeof(gdt));
    uint16_t limit = sizeof(gdt) - 1;
    memcpy(p + DATA_GDTR, &limit, 2);
    put32(DATA_GDTR + 2, (uint32_t)(page + GDT_AT));
    put32(DATA_CR3, (uint32_t)pml4);
    put32(DATA_JUMP32, (uint32_t)(page + CODE32));
    memcpy(p + DATA_JUMP32 + 4, &(uint16_t){ START_CODE32 }, 2);
    put32(DATA_JUMP64, (uint32_t)(page + CODE64));
    memcpy(p + DATA_JUMP64 + 4, &(uint16_t){ START_CODE64 }, 2);

    kernel_cr0 = read_cr0();
    kernel_cr4 = read_cr4();
    kernel_pat = rdmsr(MSR_PAT);
    kernel_efer = rdmsr(MSR_EFER);
    put32(DATA_EFER, (uint32_t)(EFER_LME | (kernel_efer & EFER_NXE)));
    (void)START_DATA;
    return 0;
}

static bool listed(const struct cpu_entry *list, int n, uint64_t id)
{
    for (int i = 0; i < n; i++)
        if (list[i].hw_id == id)
            return true;
    return false;
}

int arch_cpus_list(struct cpu_entry *list, int max)
{
    uint32_t boot = apic_boot_id();
    int n = 0;
    const uint8_t *e;
    for (int i = 0; n < max && (e = madt_entry(MADT_LOCAL_APIC, i)) != 0; i++) {
        uint32_t flags;
        memcpy(&flags, e + 4, 4);
        if ((flags & MADT_ENABLED) && !listed(list, n, e[3]))
            list[n++] = (struct cpu_entry){ e[3], e[3] == boot, 0 };
    }
    for (int i = 0; n < max && (e = madt_entry(MADT_X2APIC, i)) != 0; i++) {
        uint32_t id, flags;
        memcpy(&id, e + 4, 4);
        memcpy(&flags, e + 8, 4);
        if ((flags & MADT_ENABLED) && !listed(list, n, id))
            list[n++] = (struct cpu_entry){ id, id == boot,
                                            id > 254 ? "its APIC ID needs x2APIC mode" : 0 };
    }
    if (!listed(list, n, boot) && n < max)
        list[n++] = (struct cpu_entry){ boot, true, 0 };
    const char *why = n > 1 ? prepare() : 0;
    for (int i = 0; why && i < n; i++)
        if (!list[i].boot && !list[i].refused)
            list[i].refused = why;
    return n;
}

static void wait_us(uint64_t us)
{
    uint64_t hz = arch_counter_hz(), start = arch_counter();
    uint64_t counts = hz ? us * (hz / 1000000) : us * 1000;
    while (arch_counter() - start < counts)
        arch_cpu_relax();
}

/* Wait up to us microseconds for c to report in. */
static bool reported(struct cpu *c, uint64_t us)
{
    uint64_t hz = arch_counter_hz(), start = arch_counter();
    uint64_t counts = hz ? us * (hz / 1000000) : us * 1000;
    while (arch_counter() - start < counts) {
        if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) != CPU_STARTING)
            return true;
        arch_cpu_relax();
    }
    return false;
}

__attribute__((noreturn)) void x86_core_start(struct cpu *c);
void x86_user_init_core(void);              /* user.c */

#define DOUBLE_FAULT_STACK_PAGES 4

/* (65c) Panic, power off, reboot: the other cores stop where they are (an
 * NMI, which comes even with their interrupts off; exceptions.c calls
 * x86_core_stopped). (37g-5d) As Linux's stop_this_cpu: the core's Local
 * APIC off, its caches written back, then halted for good, and counted.
 * A reboot asked elsewhere is the boot processor's (Linux's reboot_cpu):
 * its NMI runs x86_reset_machine instead. */
volatile bool x86_cores_stopping;
static volatile bool reset_on_boot;         /* the boot processor resets */
static volatile bool reset_started;
static volatile int cores_stopped;

void arch_stop_other_cores(void)
{
    if (cpus_online() < 2 || x86_cores_stopping)
        return;
    x86_cores_stopping = true;
    apic_nmi_others();
}

static bool on_boot_core(void)
{
    return arch_this_cpu()->hw_id == apic_boot_id();
}

__attribute__((noreturn)) void x86_core_stopped(void)
{
    if (reset_on_boot && on_boot_core() && !__atomic_exchange_n(&reset_started, true, __ATOMIC_ACQ_REL))
        x86_reset_machine();
    apic_shutdown_core();
    __asm__ volatile (".byte 0x0f, 0x09" : : : "memory");   /* wbinvd: caches to memory */
    __atomic_add_fetch(&cores_stopped, 1, __ATOMIC_RELEASE);
    for (;;)
        __asm__ volatile ("cli; hlt");
}

int x86_cores_stopped(void)
{
    return __atomic_load_n(&cores_stopped, __ATOMIC_ACQUIRE);
}

__attribute__((noreturn)) void x86_reboot_cores(void)
{
    if (on_boot_core() || cpus_online() < 2) {
        reset_started = true;
        arch_stop_other_cores();
        x86_reset_machine();
    }
    reset_on_boot = true;
    arch_stop_other_cores();
    for (int ms = 0; ms < 1000 && !__atomic_load_n(&reset_started, __ATOMIC_ACQUIRE); ms++)
        arch_delay_ms(1);
    if (!__atomic_exchange_n(&reset_started, true, __ATOMIC_ACQ_REL))
        x86_reset_machine();                /* (the boot processor never answered) */
    x86_core_stopped();
}

bool arch_wake_core(struct cpu *c)
{
    return apic_send_ipi((uint32_t)c->hw_id, VECTOR_RESCHEDULE);
}

/* (65c-2) A program's page unmapped: every other core that has its tables
 * in place (active_space) must forget the page too, or it would go on
 * reading and writing a page that may already be someone else's. One core
 * announces at a time; while it waits (for the flag, then for the answers)
 * it answers announcements to itself, so two cores never wait for each
 * other with their interrupts off. */
static volatile int tlb_busy;

void x86_tlb_serve(void)
{
    struct cpu *me = arch_this_cpu();
    if (__atomic_load_n(&me->tlb_pending, __ATOMIC_ACQUIRE)) {
        if (me->tlb_page == ~0ULL)          /* (65e) the whole space (paging_user_flush) */
            write_cr3(read_cr3());
        else
            __asm__ volatile ("invlpg (%0)" : : "r"(me->tlb_page) : "memory");
        __atomic_store_n(&me->tlb_pending, 0, __ATOMIC_RELEASE);
    }
}

void x86_tlb_others(uint64_t space, uint64_t virt)
{
    int n = cpus_online();
    if (n < 2)
        return;
    uint64_t irq = arch_irq_save();
    struct cpu *me = arch_this_cpu();       /* (after: not torn by a move, 65c-3) */
    while (__atomic_exchange_n(&tlb_busy, 1, __ATOMIC_ACQUIRE)) {
        x86_tlb_serve();
        arch_cpu_relax();
    }
    /* The entry is written; a core that puts these tables in place after
     * this look loads CR3, which forgets the page anyway. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    struct cpu *asked[MAX_CPUS];
    int count = 0;
    for (int i = 0; i < n; i++) {
        struct cpu *c = cpu_online_at(i);
        if (c == me || (space != ~0ULL &&           /* (66b-3) ~0: every core */
                         __atomic_load_n(&c->active_space, __ATOMIC_ACQUIRE) != space))
            continue;
        c->tlb_page = virt;
        __atomic_store_n(&c->tlb_pending, 1, __ATOMIC_RELEASE);
        apic_send_ipi((uint32_t)c->hw_id, VECTOR_TLB);
        asked[count++] = c;
    }
    for (int i = 0; i < count; i++)
        while (__atomic_load_n(&asked[i]->tlb_pending, __ATOMIC_ACQUIRE)) {
            x86_tlb_serve();
            arch_cpu_relax();
        }
    __atomic_store_n(&tlb_busy, 0, __ATOMIC_RELEASE);
    arch_irq_restore(irq);
}

/* (37g-1) The start-up page made, for a sleep too (a one-core machine
 * has none from the boot): its physical place, 0 without one. */
uint64_t x86_startup_page(void)
{
    if (!page && prepare())
        return 0;
    return page;
}

/* Where the next core to climb the page goes: entry(arg) on stack. */
void x86_startup_point(uint64_t entry, uint64_t stack, uint64_t arg)
{
    /* The kernel's upper half as it is now (the heap may have added top
     * entries since the last core). */
    const uint64_t *kernel = phys_to_virt(paging_kernel_root());
    for (int i = 256; i < 512; i++)
        start_pml4[i] = kernel[i];
    put64(DATA_ENTRY, entry);
    put64(DATA_STACK, stack);
    put64(DATA_CPU, arg);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* INIT, then STARTUP at the page: true once *done is no longer 0. */
static bool start_core(uint32_t id, volatile int *done)
{
    uint8_t vector = (uint8_t)(page >> 12);
    if (!apic_send_init(id))
        return false;
    wait_us(10000);                                 /* INIT takes 10 ms on old cores */
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!apic_send_startup(id, vector))
            return false;
        uint64_t hz = arch_counter_hz(), start = arch_counter();
        uint64_t counts = (attempt ? 100000 : 1000) * (hz ? hz / 1000000 : 1);
        while (arch_counter() - start < counts) {
            if (__atomic_load_n(done, __ATOMIC_ACQUIRE))
                return true;
            arch_cpu_relax();
        }
    }
    return false;
}

/* (37g-1) Core id started again after a sleep at entry(arg): true once it
 * says so in *done. */
bool x86_core_restart(uint32_t id, volatile int *done)
{
    return start_core(id, done);
}

bool arch_cpu_start(struct cpu *c)
{
    x86_startup_point((uint64_t)x86_core_start, c->stack_top - 8, (uint64_t)c);  /* as after a call */
    uint32_t id = (uint32_t)c->hw_id;
    uint8_t vector = (uint8_t)(page >> 12);
    c->start_counts = arch_counter();
    if (!apic_send_init(id))
        return false;
    wait_us(10000);                                 /* INIT takes 10 ms on old cores */
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!apic_send_startup(id, vector))
            return false;
        if (reported(c, attempt ? 100000 : 1000))   /* 1 ms, then 100 ms */
            return true;
    }
    return false;
}

/* A core fresh from the start-up page (or the firmware's wake): the
 * kernel's GDT, IDT, control registers, PAT and tables, and its block. */
void x86_core_registers(struct cpu *c)
{
    gdt_load();
    idt_load();
    /* INIT left caching off (CR0.CD, NW): the kernel's settings instead. */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(kernel_cr0) : "memory");
    __asm__ volatile ("mov %0, %%cr4" : : "r"(kernel_cr4) : "memory");
    wrmsr(MSR_PAT, kernel_pat);
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | (kernel_efer & (EFER_SCE | EFER_NXE)));
    write_cr3(paging_kernel_root());
    arch_set_this_cpu(c);
}

void x86_core_start(struct cpu *c)
{
    x86_core_registers(c);
    uint64_t df = pmm_alloc_pages(DOUBLE_FAULT_STACK_PAGES);
    gdt_core_tss(c->index, df ? (uint64_t)phys_to_virt(df) +
                                DOUBLE_FAULT_STACK_PAGES * PMM_PAGE_SIZE : 0);
    x86_user_init_core();
    apic_init_core();
    cpu_online(c);
}

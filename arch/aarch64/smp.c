/*
 * AArch64's other cores (65b). The MADT lists every core's GIC CPU
 * interface with its MPIDR; PSCI's CPU_ON starts one at a physical address
 * (smp.S's arm_core_entry) with its MMU off, given a start block: the
 * kernel's MAIR, TCR, SCTLR and TTBR1, and a TTBR0 for the moment with the
 * 1 GiB around the entry code at its own address. arm_core_start takes the
 * core's block (TPIDR_EL1), its part of the GIC and its tick (65c), and
 * goes online: its code becomes its idle thread.
 */
#include "arch.h"
#include "cpu.h"
#include "cpus.h"
#include "gic.h"
#include "heap.h"
#include "madt.h"
#include "memory.h"
#include "paging.h"
#include "pmm.h"
#include "psci.h"
#include "string.h"

#define GICC_FLAGS          12
#define GICC_MPIDR          68
#define GICC_ENABLED        (1u << 0)
#define MPIDR_AFFINITY      0xFF00FFFFFFULL

#define TCR_T0SZ_MASK       0x3FULL
#define TCR_EPD0            (1ULL << 7)
#define TCR_IRGN0_WB        (1ULL << 8)
#define TCR_ORGN0_WB        (1ULL << 10)
#define TCR_SH0_INNER       (3ULL << 12)
#define TCR_TG0_MASK        (3ULL << 14)        /* 0: 4 KiB granule */
#define TCR_LOW_BITS        (TCR_T0SZ_MASK | TCR_EPD0 | (3ULL << 8) | (3ULL << 10) | \
                             (3ULL << 12) | TCR_TG0_MASK)

#define DESC_TABLE          3ULL
#define DESC_BLOCK          1ULL
#define DESC_ATTR(i)        ((uint64_t)(i) << 2)
#define DESC_SH_INNER       (3ULL << 8)
#define DESC_AF             (1ULL << 10)
#define ONE_GIB             (1ULL << 30)

#define EXCEPTION_STACK_PAGES 4

/* smp.S reads it at fixed offsets. */
struct start_block {
    uint64_t mair, tcr_start, ttbr0, ttbr1, sctlr;
    uint64_t high, self, tcr, vbar, sp_el1, sp_el0, cpu;
};
_Static_assert(__builtin_offsetof(struct start_block, cpu) == 88, "smp.S's offsets");

extern char arm_core_entry[], arm_core_high[], exception_vectors[];
__attribute__((noreturn)) void arm_core_start(struct cpu *c);

static struct start_block *block;           /* one core starts at a time */
static uint64_t identity_root;

static uint64_t zeroed_page(void)
{
    uint64_t p = pmm_alloc_page();
    if (p)
        memset(phys_to_virt(p), 0, PMM_PAGE_SIZE);
    return p;
}

/* The memory type index of normal write-back memory in MAIR_EL1. */
static int normal_index(uint64_t mair)
{
    for (int i = 0; i < 8; i++)
        if ((uint8_t)(mair >> (8 * i)) == 0xFF)
            return i;
    return -1;
}

static void clean(const void *start, uint64_t size)
{
    for (uint64_t a = (uint64_t)start & ~63ULL; a < (uint64_t)start + size; a += 64)
        __asm__ volatile ("dc civac, %0" : : "r"(a) : "memory");
    __asm__ volatile ("dsb sy" ::: "memory");
}

static const char *prepare(void)
{
    if (!psci_ready())
        return "the firmware has no PSCI";
    uint64_t mair = read_sysreg(MAIR_EL1);
    int normal = normal_index(mair);
    uint64_t page = pmm_alloc_page(), l0 = zeroed_page(), l1 = zeroed_page();
    if (normal < 0 || !page || !l0 || !l1)
        return "no memory or memory type for the start-up tables";
    /* 48-bit lower half for the moment (T0SZ 16): level 0, then 1 GiB blocks. */
    uint64_t entry = virt_to_phys(arm_core_entry);
    ((uint64_t *)phys_to_virt(l0))[(entry >> 39) & 511] = l1 | DESC_TABLE;
    ((uint64_t *)phys_to_virt(l1))[(entry >> 30) & 511] =
        (entry & ~(ONE_GIB - 1)) | DESC_BLOCK | DESC_ATTR(normal) | DESC_SH_INNER | DESC_AF;
    clean(phys_to_virt(l0), PMM_PAGE_SIZE);
    clean(phys_to_virt(l1), PMM_PAGE_SIZE);
    identity_root = l0;

    block = phys_to_virt(page);
    memset(block, 0, sizeof(*block));
    uint64_t tcr = read_sysreg(TCR_EL1);
    block->mair = mair;
    block->tcr_start = (tcr & ~TCR_LOW_BITS) | 16 | TCR_IRGN0_WB | TCR_ORGN0_WB | TCR_SH0_INNER;
    block->ttbr0 = identity_root;
    block->ttbr1 = paging_kernel_root();
    block->sctlr = read_sysreg(SCTLR_EL1);
    block->high = (uint64_t)arm_core_high;
    block->self = (uint64_t)block;
    block->tcr = tcr | TCR_EPD0;
    block->vbar = (uint64_t)exception_vectors;
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
    uint64_t boot = read_sysreg(MPIDR_EL1) & MPIDR_AFFINITY;
    int n = 0;
    const uint8_t *e;
    for (int i = 0; n < max && (e = madt_entry(MADT_GIC_CPU, i)) != 0; i++) {
        uint32_t flags;
        uint64_t mpidr;
        if (e[1] < GICC_MPIDR + 8)
            continue;
        memcpy(&flags, e + GICC_FLAGS, 4);
        memcpy(&mpidr, e + GICC_MPIDR, 8);
        mpidr &= MPIDR_AFFINITY;
        if ((flags & GICC_ENABLED) && !listed(list, n, mpidr))
            list[n++] = (struct cpu_entry){ mpidr, mpidr == boot, 0 };
    }
    if (!listed(list, n, boot) && n < max)
        list[n++] = (struct cpu_entry){ boot, true, 0 };
    const char *why = n > 1 ? prepare() : 0;
    for (int i = 0; why && i < n; i++)
        if (!list[i].boot && !list[i].refused)
            list[i].refused = why;
    return n;
}

bool arch_cpu_start(struct cpu *c)
{
    uint64_t stack = pmm_alloc_pages(EXCEPTION_STACK_PAGES);
    if (!stack)
        return false;
    block->sp_el1 = (uint64_t)phys_to_virt(stack) + EXCEPTION_STACK_PAGES * PMM_PAGE_SIZE;
    c->exception_stack = block->sp_el1;     /* (65c-2) vectors.S's, from EL0 */
    block->sp_el0 = c->stack_top;
    block->cpu = (uint64_t)c;
    /* Its MMU is off at first: what it reads must be in memory, not only
     * in this core's cache. */
    clean(block, sizeof(*block));
    clean(arm_core_entry, (uint64_t)(arm_core_high - arm_core_entry));
    c->start_counts = arch_counter();
    int64_t r = psci_call3(PSCI_CPU_ON, c->hw_id, virt_to_phys(arm_core_entry),
                           virt_to_phys(block));
    if (r != 0)
        return false;
    uint64_t hz = arch_counter_hz(), start = arch_counter();
    while (arch_counter() - start < hz / 10) {          /* 100 ms */
        if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) != CPU_STARTING)
            return true;
        arch_cpu_relax();
    }
    return false;
}

#define CPACR_FPEN          (3ULL << 20)

void arm_core_start(struct cpu *c)
{
    arch_set_this_cpu(c);
    /* (65c-2) FP/SIMD not trapped: the scheduler saves and loads programs'
     * registers on every core (user.c does it for the boot core). */
    write_sysreg(CPACR_EL1, read_sysreg(CPACR_EL1) | CPACR_FPEN);
    isb();
    if (!gic_init_core())                       /* no interrupts for it: stays parked */
        for (;;)
            __asm__ volatile ("wfi");
    cpu_online(c);
}

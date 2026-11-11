/* x86-64 page tables (kernel/paging.h). */
#include "paging.h"

void x86_tlb_others(uint64_t space, uint64_t virt);     /* smp.c */
#define TLB_ALL (~0ULL)                     /* (65e) x86_tlb_others: every page of the space */
#define TLB_ANY_SPACE (~0ULL)               /* (66b-3) x86_tlb_others: every core (the kernel's half) */
#include "arch.h"
#include "cpu.h"
#include "kprintf.h"
#include "memory.h"
#include "panic.h"
#include "pmm.h"
#include "string.h"

/* Page table entry bits. */
#define PTE_PRESENT     (1ULL << 0)
#define PTE_WRITABLE    (1ULL << 1)
#define PTE_USER        (1ULL << 2)
#define PTE_WRITE_THROUGH (1ULL << 3)
#define PTE_CACHE_DISABLE (1ULL << 4)
#define PTE_LARGE       (1ULL << 7)     /* in a PD entry: 2 MiB page */
#define PTE_PAT_4K      (1ULL << 7)     /* in a PT entry: PAT index bit 2 */
#define PTE_PAT_2M      (1ULL << 12)    /* in a 2 MiB PD entry: PAT index bit 2 */
#define PTE_NO_EXECUTE  (1ULL << 63)
#define PTE_ADDRESS     0x000FFFFFFFFFF000ULL

/* Page tables come from this pool inside the kernel image: there is no
 * memory allocator yet when they are built. */
#define POOL_TABLES 64

static uint64_t pool[POOL_TABLES][512] __attribute__((aligned(4096)));
static unsigned pool_used;
static uint64_t *pml4;
static uint64_t *firmware_pml4;     /* the kernel's + physical memory at 0 */
static uint64_t nx;            /* PTE_NO_EXECUTE if the CPU supports it, else 0 */

extern char __kernel_start[], __kernel_end[];
extern char __text_start[], __text_end[], __rodata_start[], __rodata_end[];
extern char __data_start[], __stack_guard[], __stack_bottom[], __stack_top[];

#define FOUR_GIB (4ULL << 30)

static uint64_t *new_table(void)
{
    if (pool_used == POOL_TABLES)
        panic("paging: out of page tables (pool of %d)", POOL_TABLES);
    uint64_t *t = pool[pool_used++];
    memset(t, 0, PAGE_SIZE);
    return t;
}

/* Entry index at each level: bits 39-47, 30-38, 21-29, 12-20. */
static unsigned index_at(uint64_t addr, int level)
{
    return (addr >> (12 + 9 * level)) & 511;
}

/* Follow (or create) the table that entry `index` of `table` points to.
 * Entries hold physical addresses; the kernel reads tables via the direct map. */
static uint64_t *next_table(uint64_t *table, unsigned index)
{
    if (!(table[index] & PTE_PRESENT)) {
        /* Intermediate entries allow everything; the leaf entry decides. */
        table[index] = virt_to_phys(new_table()) | PTE_PRESENT | PTE_WRITABLE;
    } else if (table[index] & PTE_LARGE) {
        panic("paging: cannot map a 4 KiB page inside a 2 MiB page");
    }
    return phys_to_virt(table[index] & PTE_ADDRESS);
}

static void map_4k(uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pdpt = next_table(pml4, index_at(virt, 3));
    uint64_t *pd = next_table(pdpt, index_at(virt, 2));
    uint64_t *pt = next_table(pd, index_at(virt, 1));
    pt[index_at(virt, 0)] = phys | flags;
}

static void map_2m(uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pdpt = next_table(pml4, index_at(virt, 3));
    uint64_t *pd = next_table(pdpt, index_at(virt, 2));
    pd[index_at(virt, 1)] = phys | flags | PTE_LARGE;
}

/*
 * Map physical [start, end) into the direct map: 2 MiB pages where possible.
 * With write_combining, the pages use PAT entry 4 (set to WC in paging_init);
 * the PAT bit sits at a different position in 4 KiB and 2 MiB entries.
 */
static void map_direct_as(uint64_t start, uint64_t end, uint64_t flags, bool write_combining)
{
    uint64_t phys = start & ~(PAGE_SIZE - 1);
    while (phys < end) {
        if (phys % LARGE_PAGE_SIZE == 0 && phys + LARGE_PAGE_SIZE <= end) {
            map_2m(HHDM_BASE + phys, phys, flags | (write_combining ? PTE_PAT_2M : 0));
            phys += LARGE_PAGE_SIZE;
        } else {
            map_4k(HHDM_BASE + phys, phys, flags | (write_combining ? PTE_PAT_4K : 0));
            phys += PAGE_SIZE;
        }
    }
}

/* The framebuffer's physical range: mapped on its own, wherever it lies. */
static uint64_t fb_start, fb_end;

/* map_direct_as for normal memory, leaving out the framebuffer. */
static void map_direct(uint64_t start, uint64_t end, uint64_t flags)
{
    if (end <= fb_start || start >= fb_end) {
        map_direct_as(start, end, flags, false);
        return;
    }
    if (start < fb_start)
        map_direct_as(start, fb_start, flags, false);
    if (end > fb_end)
        map_direct_as(fb_end, end, flags, false);
}

/* Permissions for a page of the kernel image. */
static uint64_t kernel_page_flags(uint64_t virt)
{
    if (virt >= (uint64_t)__text_start && virt < (uint64_t)__text_end)
        return PTE_PRESENT;                             /* read + execute */
    if (virt >= (uint64_t)__rodata_start && virt < (uint64_t)__rodata_end)
        return PTE_PRESENT | nx;                        /* read only */
    if (virt >= (uint64_t)__stack_guard && virt < (uint64_t)__stack_bottom)
        return 0;                                       /* stack guard: unmapped */
    return PTE_PRESENT | PTE_WRITABLE | nx;             /* data, bss, stack */
}

bool paging_query(uint64_t addr, uint64_t *phys, uint64_t *flags)
{
    uint64_t *table = phys_to_virt(read_cr3() & PTE_ADDRESS);
    for (int level = 3; level >= 0; level--) {
        uint64_t e = table[index_at(addr, level)];
        if (!(e & PTE_PRESENT))
            return false;
        if (level == 0 || (level == 1 && (e & PTE_LARGE))) {
            uint64_t size = level == 0 ? PAGE_SIZE : LARGE_PAGE_SIZE;
            *phys = (e & PTE_ADDRESS & ~(size - 1)) | (addr & (size - 1));
            *flags = e & ~PTE_ADDRESS;
            if (level == 0)
                *flags &= ~PTE_LARGE;       /* bit 7 is the PAT bit in a 4 KiB entry */
            return true;
        }
        table = phys_to_virt(e & PTE_ADDRESS);
    }
    return false;
}

/* (66f-3) The table entry `*entry` points to, made (from the page
 * allocator: the boot's pool is small) when there is none; 0 without
 * memory. */
static uint64_t *table_below(uint64_t *entry)
{
    if (!(*entry & PTE_PRESENT)) {
        uint64_t table = pmm_alloc_page();
        if (!table)
            return 0;
        memset(phys_to_virt(table), 0, PAGE_SIZE);
        *entry = table | PTE_PRESENT | PTE_WRITABLE;
    }
    return phys_to_virt(*entry & PTE_ADDRESS);
}

bool paging_map_kernel(uint64_t virt, uint64_t phys, int access)
{
    uint64_t *pdpt = table_below(&pml4[index_at(virt, 3)]);
    uint64_t *pd = pdpt ? table_below(&pdpt[index_at(virt, 2)]) : 0;
    uint64_t *pt = pd ? table_below(&pd[index_at(virt, 1)]) : 0;
    if (!pt)
        return false;
    pt[index_at(virt, 0)] = phys | PTE_PRESENT | (access == PAGING_DATA ? PTE_WRITABLE : 0) |
                            (access == PAGING_TEXT ? 0 : nx);
    return true;                                /* (was not present: no one holds it in a TLB) */
}

bool paging_write_combining(uint64_t phys, uint64_t size)
{
    uint64_t start = phys & ~(PAGE_SIZE - 1);
    uint64_t end = (phys + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t irq = arch_irq_save();
    bool ok = true;
    for (uint64_t p = start; p < end && ok; p += PAGE_SIZE) {
        uint64_t virt = HHDM_BASE + p;
        uint64_t *pdpt = table_below(&pml4[index_at(virt, 3)]);
        uint64_t *pd = pdpt ? table_below(&pdpt[index_at(virt, 2)]) : 0;
        if (!pd) {
            ok = false;
            break;
        }
        uint64_t *pde = &pd[index_at(virt, 1)];
        if (!(*pde & PTE_PRESENT) && p % LARGE_PAGE_SIZE == 0 && p + LARGE_PAGE_SIZE <= end) {
            /* (66f-3) Not mapped yet (a graphics card's memory past the
             * firmware's framebuffer): a 2 MiB write-combining page. */
            *pde = p | PTE_PRESENT | PTE_WRITABLE | nx | PTE_LARGE | PTE_PAT_2M;
            p += LARGE_PAGE_SIZE - PAGE_SIZE;
            continue;
        }
        if (*pde & PTE_LARGE) {
            /* A 2 MiB page: split into 512 4 KiB ones, the same flags
             * (its PAT bit moves from bit 12 to bit 7). */
            uint64_t table = pmm_alloc_page();
            if (!table) {
                ok = false;
                break;
            }
            uint64_t *pt = phys_to_virt(table);
            uint64_t base = *pde & PTE_ADDRESS & ~(LARGE_PAGE_SIZE - 1);
            uint64_t flags = (*pde & ~PTE_ADDRESS & ~PTE_LARGE) |
                             (*pde & PTE_PAT_2M ? PTE_PAT_4K : 0);
            for (int k = 0; k < 512; k++)
                pt[k] = (base + (uint64_t)k * PAGE_SIZE) | flags;
            *pde = table | PTE_PRESENT | PTE_WRITABLE;
        }
        uint64_t *pt = table_below(pde);
        if (!pt) {
            ok = false;
            break;
        }
        uint64_t *pte = &pt[index_at(virt, 0)];
        if (!(*pte & PTE_PRESENT))
            *pte = p | PTE_PRESENT | PTE_WRITABLE | nx;
        *pte = (*pte & ~(PTE_CACHE_DISABLE | PTE_WRITE_THROUGH)) | PTE_PAT_4K;
    }
    write_cr3(read_cr3());                      /* this core */
    x86_tlb_others(TLB_ANY_SPACE, TLB_ALL);     /* the others */
    arch_irq_restore(irq);
    return ok;
}

volatile void *paging_map_mmio(uint64_t phys, uint64_t size)
{
    uint64_t start = phys & ~(PAGE_SIZE - 1);
    uint64_t end = (phys + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint64_t p = start; p < end; p += PAGE_SIZE) {
        uint64_t mapped, flags;
        if (paging_query(HHDM_BASE + p, &mapped, &flags))
            continue;
        map_4k(HHDM_BASE + p, p, PTE_PRESENT | PTE_WRITABLE | PTE_CACHE_DISABLE |
                                 PTE_WRITE_THROUGH | nx);
        __asm__ volatile ("invlpg (%0)" : : "r"(HHDM_BASE + p) : "memory");
    }
    return phys_to_virt(phys);
}

static void describe(const char *what, uint64_t addr)
{
    uint64_t phys, flags;
    if (!paging_query(addr, &phys, &flags)) {
        kprintf("    %-14s %p  not mapped\n", what, (void *)addr);
        return;
    }
    kprintf("    %-14s %p  read %s %s, %s page, physical %p\n", what, (void *)addr,
            flags & PTE_WRITABLE ? "write" : "-",
            flags & PTE_NO_EXECUTE ? "-" : "execute",
            flags & PTE_LARGE ? "2 MiB" : "4 KiB", (void *)phys);
}

void paging_init(const struct boot_info *info)
{
    /* The NX bit exists if CPUID 0x80000001 says so; it must be enabled in EFER. */
    if (cpuid(0x80000001, 0).edx & (1u << 20)) {
        wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
        nx = PTE_NO_EXECUTE;
    }

    /*
     * PAT entry 4 (normally write-back, unused here) becomes write-combining
     * for the framebuffer: the CPU merges pixel writes into bursts instead of
     * sending every 4 bytes on their own, as it must for uncached memory
     * (which the firmware's MTRRs usually make of a graphics card's memory).
     * WC in the PAT wins over an uncached MTRR.
     */
    uint64_t pat = rdmsr(MSR_PAT);
    wrmsr(MSR_PAT, (pat & ~(0xFFULL << 32)) | (0x01ULL << 32));

    pml4 = new_table();
    fb_start = virt_to_phys((const void *)info->framebuffer.base) & ~(PAGE_SIZE - 1);
    fb_end = (virt_to_phys((const void *)info->framebuffer.base) +
              (uint64_t)info->framebuffer.stride * info->framebuffer.height * 4 + PAGE_SIZE - 1) &
             ~(PAGE_SIZE - 1);

    /* 1. The kernel image at its link address, page by page. */
    uint64_t kernel_size = (uint64_t)__kernel_end - (uint64_t)__kernel_start;
    for (uint64_t off = 0; off < kernel_size; off += PAGE_SIZE) {
        uint64_t virt = (uint64_t)__kernel_start + off;
        uint64_t flags = kernel_page_flags(virt);
        if (flags)
            map_4k(virt, kernel_phys_base + off, flags);
    }

    /*
     * 2. The direct map, region by region: below 4 GiB everything (gaps hold
     *    the framebuffer and device registers), above 4 GiB only memory the
     *    kernel may need. Only UEFI runtime code is executable, so that runtime
     *    services keep working.
     */
    uint64_t data = PTE_PRESENT | PTE_WRITABLE | nx;
    uint64_t next = 0, highest = FOUR_GIB;
    for (uint64_t i = 0; i < info->memory_map_count; i++) {
        const struct memory_region *r = &info->memory_map[i];
        uint64_t start = r->base > next ? r->base : next;
        uint64_t end = r->base + r->length;
        if (end <= start)
            continue;
        if (start > next && next < FOUR_GIB)
            map_direct(next, start < FOUR_GIB ? start : FOUR_GIB, data);   /* gap */
        if (start < FOUR_GIB || (r->type != MEMORY_RESERVED && r->type != MEMORY_MMIO)) {
            map_direct(start, end, r->type == MEMORY_UEFI_RUNTIME_CODE
                                       ? PTE_PRESENT | PTE_WRITABLE : data);
            if (end > highest)
                highest = end;
        }
        next = end;
    }
    if (next < FOUR_GIB)
        map_direct(next, FOUR_GIB, data);

    /* 2b. The framebuffer, also when a graphics card has it above 4 GiB or
     *     above all RAM (common on real PCs). */
    map_direct_as(fb_start, fb_end, data, true);

    /* 3. Nothing in the lower half: it will belong to user programs. */

    uint64_t old_cr3 = read_cr3();
    write_cr3(virt_to_phys(pml4));
    firmware_pml4 = new_table();

    kprintf("  paging:        CR3 %p -> %p, %u page tables (%u KiB), NX %s\n",
            (void *)old_cr3, (void *)virt_to_phys(pml4), pool_used, pool_used * 4,
            nx ? "on" : "not supported");
    kprintf("                 framebuffer %p - %p write-combining (PAT 0x%016lX)\n",
            (void *)fb_start, (void *)(fb_end - 1), rdmsr(MSR_PAT));
    kprintf("                 kernel %p - %p (physical %p)\n", (void *)__kernel_start,
            (void *)(__kernel_end - 1), (void *)kernel_phys_base);
    kprintf("                 direct map %p - %p (physical 0 - %p)\n", (void *)HHDM_BASE,
            (void *)(HHDM_BASE + highest - 1), (void *)(highest - 1));
    describe("NULL page", 0);
    describe("lower half", 0x100000);
    describe("kernel code", (uint64_t)__text_start);
    describe("kernel rodata", (uint64_t)__rodata_start);
    describe("kernel data", (uint64_t)__data_start);
    describe("stack guard", (uint64_t)__stack_guard);
    describe("stack", (uint64_t)__stack_top - 8);
    describe("framebuffer", (uint64_t)info->framebuffer.base);
    describe("direct map", HHDM_BASE + FOUR_GIB);

    /* Check the firmware address space: a kernel variable read through its
     * physical address there must be the same. */
    static volatile uint64_t probe = 0x5249545445524F53ULL;    /* "RITTEROS" */
    uint64_t previous = paging_firmware_enter();
    uint64_t seen = *(volatile uint64_t *)virt_to_phys((const void *)&probe);
    paging_firmware_leave(previous);
    kprintf("                 firmware calls: own tables, physical memory also at 0 - %p "
            "(%s)\n", (void *)(highest - 1), seen == probe ? "checked" : "WRONG");
}

uint64_t paging_kernel_root(void)
{
    return virt_to_phys(pml4);
}

uint64_t paging_firmware_enter(void)
{
    /* The upper half is the kernel's (copied each time: the kernel may have
     * added entries); slot 0 shares the direct map's table for physical
     * 0 - 512 GiB, so the identity map costs no tables of its own. */
    uint64_t previous = read_cr3();
    for (int i = 256; i < 512; i++)
        firmware_pml4[i] = pml4[i];
    firmware_pml4[0] = pml4[index_at(HHDM_BASE, 3)];
    write_cr3(virt_to_phys(firmware_pml4));
    return previous;
}

void paging_firmware_leave(uint64_t previous)
{
    write_cr3(previous);
}

/* --- user address spaces ------------------------------------------------------ */

uint64_t paging_user_create(void)
{
    uint64_t phys = pmm_alloc_page();
    if (phys)
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
    return phys;
}

/* The table entry `index` of `table` points to, made from the page
 * allocator if missing (and create is set). Intermediate entries allow
 * user access; the leaf entry decides. (65e-38) Faults of one program run
 * at once: a new table goes in only where the entry is still empty, else
 * the other core's is used and ours freed. */
static uint64_t *user_next(uint64_t *table, unsigned index, bool create)
{
    uint64_t entry = __atomic_load_n(&table[index], __ATOMIC_ACQUIRE);
    if (!(entry & PTE_PRESENT)) {
        uint64_t phys = create ? pmm_alloc_page() : 0;
        if (!phys)
            return 0;
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
        uint64_t mine = phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        if (__atomic_compare_exchange_n(&table[index], &entry, mine, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            entry = mine;
        } else {
            pmm_free_page(phys);            /* (entry is the other core's now) */
        }
    }
    return phys_to_virt(entry & PTE_ADDRESS);
}

/* The leaf entry for virt in space, or 0. */
static uint64_t *user_entry(uint64_t space, uint64_t virt, bool create)
{
    if (virt >= USER_TOP)
        return 0;
    uint64_t *table = phys_to_virt(space);
    for (int level = 3; level > 0 && table; level--)
        table = user_next(table, index_at(virt, level), create);
    return table ? &table[index_at(virt, 0)] : 0;
}

bool paging_user_map(uint64_t space, uint64_t virt, uint64_t phys, unsigned access)
{
    uint64_t *e = user_entry(space, virt, true);
    if (!e)
        return false;
    /* access 0 (22f: mprotect PROT_NONE over a page that exists): present, so
     * the page stays, but not the program's to touch. (65e-38) Only over an
     * empty entry, in one step: another core's fault may put one at once. */
    uint64_t empty = 0;
    return __atomic_compare_exchange_n(e, &empty, phys | PTE_PRESENT | (access ? PTE_USER : 0) |
                                       (access & USER_WRITE ? PTE_WRITABLE : 0) |
                                       (access & USER_EXEC ? 0 : nx) |
                                       (access & USER_COMBINE ? PTE_PAT_4K : 0),
                                       false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

bool paging_user_query(uint64_t space, uint64_t virt, uint64_t *phys, unsigned *access)
{
    uint64_t *e = user_entry(space, virt, false);
    if (!e || !(*e & PTE_PRESENT))
        return false;
    *phys = (*e & PTE_ADDRESS) | (virt & (PAGE_SIZE - 1));
    *access = !(*e & PTE_USER) ? 0 : USER_READ | (*e & PTE_WRITABLE ? USER_WRITE : 0) |
              (*e & PTE_NO_EXECUTE ? 0 : USER_EXEC);
    return true;
}

void paging_user_unmap(uint64_t space, uint64_t virt)
{
    uint64_t *e = user_entry(space, virt, false);
    if (e && (*e & PTE_PRESENT)) {
        *e = 0;                             /* the physical page is not ours to free */
        /* (65e) This core's and the others' with interrupts off between:
         * a move to another core in between left that one unflushed (the
         * announcement skips the core it runs on) while it may hold the
         * entry from another thread of the program (no CR3 load between
         * threads of one program): stale reads under smpstress. */
        uint64_t irq = arch_irq_save();
        __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
        x86_tlb_others(space, virt);        /* (65c-2) other cores running it */
        arch_irq_restore(irq);
    }
}

bool paging_user_take(uint64_t space, uint64_t virt, uint64_t *phys)
{
    uint64_t *e = user_entry(space, virt, false);
    if (!e || !(*e & PTE_PRESENT))
        return false;
    *phys = *e & PTE_ADDRESS;
    *e = 0;
    return true;
}

bool paging_user_protect(uint64_t space, uint64_t virt, unsigned access)
{
    uint64_t *e = user_entry(space, virt, false);
    if (!e || !(*e & PTE_PRESENT))
        return false;
    *e = (*e & PTE_ADDRESS) | PTE_PRESENT | (access ? PTE_USER : 0) |
         (access & USER_WRITE ? PTE_WRITABLE : 0) | (access & USER_EXEC ? 0 : nx);
    return true;
}

void paging_user_flush(uint64_t space)
{
    uint64_t irq = arch_irq_save();         /* (65e: as in paging_user_unmap) */
    if ((read_cr3() & PTE_ADDRESS) == space)
        write_cr3(read_cr3());              /* this core's: all of it */
    x86_tlb_others(space, TLB_ALL);         /* the others' (smp.c) */
    arch_irq_restore(irq);
}

void paging_user_activate(uint64_t space)
{
    if (!space) {
        write_cr3(virt_to_phys(pml4));
        return;
    }
    /* The upper half is the kernel's in every address space. Copy its
     * entries each time: the kernel may have added some (device registers). */
    uint64_t *top = phys_to_virt(space);
    for (int i = 256; i < 512; i++)
        top[i] = pml4[i];
    write_cr3(space);                       /* also flushes the old program's TLB entries */
}

/* --- (35b) swap entries and the accessed bit ------------------------------------------ */

#define PTE_ACCESSED    (1ULL << 5)
#define SWAP_ENTRY(slot) ((slot) << 12 | 2)    /* not present: bit 0 clear, bit 1 set */

static bool is_swap_entry(uint64_t e)
{
    return !(e & PTE_PRESENT) && (e & 2);
}

bool paging_user_evict(uint64_t space, uint64_t virt, uint64_t phys, uint64_t slot)
{
    uint64_t *e = user_entry(space, virt, false);
    uint64_t now = e ? __atomic_load_n(e, __ATOMIC_ACQUIRE) : 0;
    /* (A compare-and-swap: the processor may set the accessed or dirty bit
     * of the entry meanwhile; then it is looked at again.) */
    while (e && (now & PTE_PRESENT) && (now & PTE_ADDRESS) == phys)
        if (__atomic_compare_exchange_n(e, &now, SWAP_ENTRY(slot), false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return true;
    return false;
}

bool paging_user_swapped(uint64_t space, uint64_t virt, uint64_t *slot)
{
    uint64_t *e = user_entry(space, virt, false);
    uint64_t now = e ? __atomic_load_n(e, __ATOMIC_ACQUIRE) : 0;
    if (!is_swap_entry(now))
        return false;
    *slot = now >> 12;
    return true;
}

bool paging_user_swap_in(uint64_t space, uint64_t virt, uint64_t slot, uint64_t phys,
                         unsigned access)
{
    uint64_t *e = user_entry(space, virt, false);
    uint64_t want = SWAP_ENTRY(slot);
    return e && __atomic_compare_exchange_n(e, &want, phys | PTE_PRESENT |
                                            (access ? PTE_USER : 0) |
                                            (access & USER_WRITE ? PTE_WRITABLE : 0) |
                                            (access & USER_EXEC ? 0 : nx),
                                            false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

bool paging_user_put_swapped(uint64_t space, uint64_t virt, uint64_t slot)
{
    uint64_t *e = user_entry(space, virt, true);
    uint64_t empty = 0;
    return e && __atomic_compare_exchange_n(e, &empty, SWAP_ENTRY(slot), false, __ATOMIC_RELEASE,
                                            __ATOMIC_RELAXED);
}

bool paging_user_take_swapped(uint64_t space, uint64_t virt, uint64_t *slot)
{
    uint64_t *e = user_entry(space, virt, false);
    if (!e || !is_swap_entry(*e))
        return false;
    *slot = *e >> 12;
    *e = 0;
    return true;
}

bool paging_user_young(uint64_t space, uint64_t virt)
{
    uint64_t *e = user_entry(space, virt, false);
    if (!e || !(*e & PTE_PRESENT))
        return false;
    return __atomic_fetch_and(e, ~PTE_ACCESSED, __ATOMIC_ACQ_REL) & PTE_ACCESSED;
}

bool paging_user_touch(uint64_t space, uint64_t virt)
{
    (void)space, (void)virt;
    return false;
}

/* Free what the entries of a table at this level (2: PDPT ... 0: page
 * table) point to: lower tables, and at the bottom the program's pages. */
static void free_below(uint64_t *table, int level)
{
    for (int i = 0; i < 512; i++) {
        if (!(table[i] & PTE_PRESENT))
            continue;
        uint64_t phys = table[i] & PTE_ADDRESS;
        if (level > 0)
            free_below(phys_to_virt(phys), level - 1);
        pmm_free_page(phys);
    }
}

void paging_user_destroy(uint64_t space)
{
    uint64_t *top = phys_to_virt(space);
    for (int i = 0; i < 256; i++) {         /* the lower half only */
        if (!(top[i] & PTE_PRESENT))
            continue;
        free_below(phys_to_virt(top[i] & PTE_ADDRESS), 2);
        pmm_free_page(top[i] & PTE_ADDRESS);
    }
    pmm_free_page(space);
}

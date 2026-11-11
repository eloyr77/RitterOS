/*
 * AArch64 kernel translation tables (kernel/paging.h).
 *
 * TTBR1_EL1 translates the upper half: the kernel image in 4 KiB pages with
 * permissions per section, and the direct map. The lower half has its own
 * register, TTBR0_EL1: it points to the running program's tables, and while
 * no program runs its translation is switched off (TCR_EL1.EPD0), so every
 * lower-half access faults.
 *
 * Memory types come only from these tables: RAM is Normal (cached) memory,
 * device registers are Device memory, the framebuffer is Normal
 * non-cacheable (writes are combined, like write-combining on x86). Unlike
 * x86, address ranges without RAM are not mapped up front: device registers
 * are mapped on demand by paging_map_mmio.
 */
#include "paging.h"
#include "arch.h"
#include "cpu.h"
#include "kprintf.h"
#include "memory.h"
#include "panic.h"
#include "pmm.h"
#include "string.h"
#include "uart.h"

#define POOL_TABLES 64

static uint64_t pool[POOL_TABLES][512] __attribute__((aligned(4096)));
static unsigned pool_used;
static uint64_t *root;
static uint64_t attr_normal, attr_device, attr_framebuffer;

extern char __kernel_start[], __kernel_end[];
extern char __text_start[], __text_end[], __rodata_start[], __rodata_end[];
extern char __data_start[], __stack_guard[], __stack_bottom[], __stack_top[];

static uint64_t *new_table(void)
{
    if (pool_used == POOL_TABLES)
        panic("paging: out of translation tables (pool of %d)", POOL_TABLES);
    uint64_t *t = pool[pool_used++];
    memset(t, 0, PAGE_SIZE);
    return t;
}

/* Table index at each level: 0 (bits 47-39) to 3 (bits 20-12). */
static unsigned index_at(uint64_t addr, int level)
{
    return (addr >> (39 - 9 * level)) & 511;
}

static uint64_t *next_table(uint64_t *table, unsigned index)
{
    uint64_t e = table[index];
    if (!(e & 1))
        table[index] = virt_to_phys(new_table()) | DESC_TABLE;
    else if ((e & 3) == DESC_BLOCK)
        panic("paging: cannot map a 4 KiB page inside a 2 MiB block");
    return phys_to_virt(table[index] & DESC_ADDRESS);
}

static void map_4k(uint64_t virt, uint64_t phys, uint64_t attrs)
{
    uint64_t *l1 = next_table(root, index_at(virt, 0));
    uint64_t *l2 = next_table(l1, index_at(virt, 1));
    uint64_t *l3 = next_table(l2, index_at(virt, 2));
    l3[index_at(virt, 3)] = phys | DESC_PAGE | attrs;
}

static void map_2m(uint64_t virt, uint64_t phys, uint64_t attrs)
{
    uint64_t *l1 = next_table(root, index_at(virt, 0));
    uint64_t *l2 = next_table(l1, index_at(virt, 1));
    l2[index_at(virt, 2)] = phys | DESC_BLOCK | attrs;
}

/* Map physical [start, end) into the direct map: 2 MiB blocks where possible.
 * Pages that are already mapped stay as they are. */
static void map_direct(uint64_t start, uint64_t end, uint64_t attrs)
{
    uint64_t phys = start & ~(PAGE_SIZE - 1);
    while (phys < end) {
        uint64_t mapped, flags;
        if (phys % LARGE_PAGE_SIZE == 0 && phys + LARGE_PAGE_SIZE <= end &&
            !paging_query(HHDM_BASE + phys, &mapped, &flags)) {
            uint64_t *l1 = next_table(root, index_at(HHDM_BASE + phys, 0));
            uint64_t *l2 = next_table(l1, index_at(HHDM_BASE + phys, 1));
            if (!(l2[index_at(HHDM_BASE + phys, 2)] & 1)) {     /* no 4 KiB pages below */
                map_2m(HHDM_BASE + phys, phys, attrs);
                phys += LARGE_PAGE_SIZE;
                continue;
            }
        }
        if (!paging_query(HHDM_BASE + phys, &mapped, &flags))
            map_4k(HHDM_BASE + phys, phys, attrs);
        phys += PAGE_SIZE;
    }
}

/* Permissions for a page of the kernel image. */
static uint64_t kernel_page_attrs(uint64_t virt)
{
    uint64_t normal = DESC_ATTR(attr_normal) | DESC_SH_INNER | DESC_AF | DESC_UXN;
    if (virt >= (uint64_t)__text_start && virt < (uint64_t)__text_end)
        return normal | DESC_AP_RO;                         /* read + execute */
    if (virt >= (uint64_t)__rodata_start && virt < (uint64_t)__rodata_end)
        return normal | DESC_AP_RO | DESC_PXN;              /* read only */
    if (virt >= (uint64_t)__stack_guard && virt < (uint64_t)__stack_bottom)
        return 0;                                           /* stack guard: unmapped */
    return normal | DESC_PXN;                               /* data, bss, stacks */
}

bool paging_query(uint64_t addr, uint64_t *phys, uint64_t *flags)
{
    if (!root || addr < HHDM_BASE)
        return false;
    uint64_t *table = root;
    for (int level = 0; level <= 3; level++) {
        uint64_t e = table[index_at(addr, level)];
        if (!(e & 1))
            return false;
        if (level == 3 || (e & 3) == DESC_BLOCK) {
            uint64_t size = level == 3 ? PAGE_SIZE : level == 2 ? LARGE_PAGE_SIZE : 1ULL << 30;
            *phys = (e & DESC_ADDRESS & ~(size - 1)) | (addr & (size - 1));
            *flags = (e & ~DESC_ADDRESS) | (size == LARGE_PAGE_SIZE ? 1ULL << 63 : 0);
            return true;
        }
        table = phys_to_virt(e & DESC_ADDRESS);
    }
    return false;
}

bool paging_write_combining(uint64_t phys, uint64_t size)
{
    (void)phys, (void)size;
    return false;               /* (66b-3) no driver sets modes on ARM yet */
}

/* The table entry `*entry` points to, made from the page allocator when
 * there is none; 0 without memory. */
static uint64_t *table_below(uint64_t *entry)
{
    if (!(*entry & 1)) {
        uint64_t table = pmm_alloc_page();
        if (!table)
            return 0;
        memset(phys_to_virt(table), 0, PAGE_SIZE);
        *entry = table | DESC_TABLE;
    }
    return phys_to_virt(*entry & DESC_ADDRESS);
}

bool paging_map_kernel(uint64_t virt, uint64_t phys, int access)
{
    uint64_t *l1 = table_below(&root[index_at(virt, 0)]);
    uint64_t *l2 = l1 ? table_below(&l1[index_at(virt, 1)]) : 0;
    uint64_t *l3 = l2 ? table_below(&l2[index_at(virt, 2)]) : 0;
    if (!l3)
        return false;
    uint64_t attrs = DESC_ATTR(attr_normal) | DESC_SH_INNER | DESC_AF | DESC_UXN;   /* as the image */
    if (access == PAGING_TEXT)
        attrs |= DESC_AP_RO;
    else if (access == PAGING_RODATA)
        attrs |= DESC_AP_RO | DESC_PXN;
    else
        attrs |= DESC_PXN;
    l3[index_at(virt, 3)] = phys | DESC_PAGE | attrs;
    dsb_ishst();                                /* (an invalid entry is in no TLB) */
    isb();
    return true;
}

volatile void *paging_map_mmio(uint64_t phys, uint64_t size)
{
    uint64_t start = phys & ~(PAGE_SIZE - 1);
    uint64_t end = (phys + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    map_direct(start, end, DESC_ATTR(attr_device) | DESC_AF | DESC_UXN | DESC_PXN);
    dsb_ishst();
    isb();
    return phys_to_virt(phys);
}

static void describe(const char *what, uint64_t addr)
{
    uint64_t phys, flags;
    if (!paging_query(addr, &phys, &flags)) {
        kprintf("    %-14s %p  not mapped\n", what, (void *)addr);
        return;
    }
    unsigned attr = (unsigned)(flags >> 2) & 7;
    kprintf("    %-14s %p  read %s %s, %s page, physical %p, %s\n", what, (void *)addr,
            flags & DESC_AP_RO ? "-" : "write", flags & DESC_PXN ? "-" : "execute",
            flags >> 63 ? "2 MiB" : "4 KiB", (void *)phys,
            attr == attr_normal ? "normal" : attr == attr_device ? "device" : "non-cacheable");
}

static int mair_index(uint64_t mair, uint8_t a, uint8_t b)
{
    for (int i = 0; i < 8; i++) {
        uint8_t v = (uint8_t)(mair >> (8 * i));
        if (v == a || v == b)
            return i;
    }
    return -1;
}

void paging_init(const struct boot_info *info)
{
    /* Use the memory types the firmware put into MAIR_EL1, as the bootloader did. */
    uint64_t mair = read_sysreg(MAIR_EL1);
    int normal = mair_index(mair, MAIR_NORMAL_WB, MAIR_NORMAL_WB);
    int device = mair_index(mair, MAIR_DEVICE_NGNRE, MAIR_DEVICE_NGNRNE);
    int nc = mair_index(mair, 0x44, 0x44);                  /* normal, non-cacheable */
    if (normal < 0 || device < 0)
        panic("paging: MAIR_EL1 lacks normal or device memory types");
    attr_normal = (uint64_t)normal;
    attr_device = (uint64_t)device;
    attr_framebuffer = nc >= 0 ? (uint64_t)nc : (uint64_t)device;

    root = new_table();

    /* 1. The kernel image at its link address, page by page. */
    uint64_t kernel_size = (uint64_t)__kernel_end - (uint64_t)__kernel_start;
    for (uint64_t off = 0; off < kernel_size; off += PAGE_SIZE) {
        uint64_t virt = (uint64_t)__kernel_start + off;
        uint64_t attrs = kernel_page_attrs(virt);
        if (attrs)
            map_4k(virt, kernel_phys_base + off, attrs);
    }

    /* 2. The direct map: RAM as normal memory (runtime services code stays
     *    executable), the firmware's runtime device registers as device memory. */
    uint64_t ram = DESC_ATTR(attr_normal) | DESC_SH_INNER | DESC_AF | DESC_UXN;
    uint64_t highest = 0;
    for (uint64_t i = 0; i < info->memory_map_count; i++) {
        const struct memory_region *r = &info->memory_map[i];
        uint64_t end = r->base + r->length;
        switch (r->type) {
        case MEMORY_RESERVED:
            continue;
        case MEMORY_MMIO:
            map_direct(r->base, end, DESC_ATTR(attr_device) | DESC_AF | DESC_UXN | DESC_PXN);
            continue;
        case MEMORY_UEFI_RUNTIME_CODE:
            map_direct(r->base, end, ram);
            break;
        default:
            map_direct(r->base, end, ram | DESC_PXN);
            break;
        }
        if (end > highest)
            highest = end;
    }

    /* 3. The framebuffer, which the firmware lists as reserved memory. */
    const struct framebuffer *fb = &info->framebuffer;
    uint64_t fb_phys = virt_to_phys((const void *)fb->base);
    map_direct(fb_phys, fb_phys + (uint64_t)fb->stride * fb->height * 4,
               DESC_ATTR(attr_framebuffer) | DESC_SH_INNER | DESC_AF | DESC_UXN | DESC_PXN);

    /* 4. The serial port, so that output keeps working after the switch. */
    paging_map_mmio(PL011_PHYS, PAGE_SIZE);

    /* Switch: the upper half to the new tables, the lower half off. The
     * kernel's code and stack are mapped the same way in both. */
    uint64_t old_ttbr1 = read_sysreg(TTBR1_EL1);
    dsb_ishst();
    write_sysreg(TTBR1_EL1, virt_to_phys(root));
    write_sysreg(TCR_EL1, read_sysreg(TCR_EL1) | (1ULL << 7));      /* EPD0 */
    isb();
    __asm__ volatile ("tlbi vmalle1" ::: "memory");
    dsb_ish();
    isb();
    pl011_regs = (volatile uint8_t *)phys_to_virt(PL011_PHYS);

    kprintf("  paging:        TTBR1 %p -> %p, %u translation tables (%u KiB), lower half off\n",
            (void *)old_ttbr1, (void *)virt_to_phys(root), pool_used, pool_used * 4);
    kprintf("                 kernel %p - %p (physical %p)\n", (void *)__kernel_start,
            (void *)(__kernel_end - 1), (void *)kernel_phys_base);
    kprintf("                 direct map of RAM up to %p; devices mapped on demand\n",
            (void *)(highest - 1));
    describe("NULL page", 0);
    describe("lower half", 0x40000000);
    describe("kernel code", (uint64_t)__text_start);
    describe("kernel rodata", (uint64_t)__rodata_start);
    describe("kernel data", (uint64_t)__data_start);
    describe("stack guard", (uint64_t)__stack_guard);
    describe("stack", (uint64_t)__stack_top - 8);
    describe("framebuffer", (uint64_t)fb->base);
    describe("serial port", (uint64_t)pl011_regs);
}

uint64_t paging_kernel_root(void)
{
    return virt_to_phys(root);
}

/* --- user address spaces (TTBR0) ---------------------------------------------- */

#define TCR_EPD0 (1ULL << 7)

uint64_t paging_user_create(void)
{
    uint64_t phys = pmm_alloc_page();
    if (phys)
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
    return phys;
}

/* (65e-38) Faults of one program run at once: a new table goes in only
 * where the entry is still empty, else the other core's is used and ours
 * freed. */
static uint64_t *user_next(uint64_t *table, unsigned index, bool create)
{
    uint64_t entry = __atomic_load_n(&table[index], __ATOMIC_ACQUIRE);
    if (!(entry & 1)) {
        uint64_t phys = create ? pmm_alloc_page() : 0;
        if (!phys)
            return 0;
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
        uint64_t mine = phys | DESC_TABLE;
        if (__atomic_compare_exchange_n(&table[index], &entry, mine, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            entry = mine;
        } else {
            pmm_free_page(phys);            /* (entry is the other core's now) */
        }
    }
    return phys_to_virt(entry & DESC_ADDRESS);
}

/* The level-3 entry for virt in space, or 0. */
static uint64_t *user_entry(uint64_t space, uint64_t virt, bool create)
{
    if (virt >= USER_TOP)
        return 0;
    uint64_t *table = phys_to_virt(space);
    for (int level = 0; level < 3 && table; level++)
        table = user_next(table, index_at(virt, level), create);
    return table ? &table[index_at(virt, 3)] : 0;
}

bool paging_user_map(uint64_t space, uint64_t virt, uint64_t phys, unsigned access)
{
    uint64_t *e = user_entry(space, virt, true);
    if (!e)
        return false;
    /* Normal memory, accessible at EL0, never executable by the kernel;
     * access 0 (22f: mprotect PROT_NONE over a page that exists): there, but
     * not EL0's to touch or run. (65e-38) Only over an empty entry, in one
     * step: another core's fault may put one at once. */
    uint64_t empty = 0;
    return __atomic_compare_exchange_n(e, &empty, phys | DESC_PAGE | DESC_ATTR(attr_normal) |
                                       DESC_SH_INNER | DESC_AF | DESC_NG |
                                       (access ? DESC_AP_EL0 : 0) | DESC_PXN |
                                       (access & USER_WRITE ? 0 : DESC_AP_RO) |
                                       (access & USER_EXEC ? 0 : DESC_UXN),
                                       false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

bool paging_user_query(uint64_t space, uint64_t virt, uint64_t *phys, unsigned *access)
{
    uint64_t *e = user_entry(space, virt, false);
    if (!e || (*e & 3) != DESC_PAGE)
        return false;
    *phys = (*e & DESC_ADDRESS) | (virt & (PAGE_SIZE - 1));
    *access = !(*e & DESC_AP_EL0) ? 0 : USER_READ | (*e & DESC_AP_RO ? 0 : USER_WRITE) |
              (*e & DESC_UXN ? 0 : USER_EXEC);
    return true;
}

void paging_user_unmap(uint64_t space, uint64_t virt)
{
    uint64_t *e = user_entry(space, virt, false);
    if (e && (*e & 1)) {
        *e = 0;                             /* the physical page is not ours to free */
        dsb_ishst();
        isb();
        /* No ASIDs: flush all, on every core (65c-2: inner shareable, so
         * another core running this program forgets the page too). */
        __asm__ volatile ("tlbi vmalle1is" ::: "memory");
        dsb_ish();
        isb();
    }
}

bool paging_user_take(uint64_t space, uint64_t virt, uint64_t *phys)
{
    uint64_t *e = user_entry(space, virt, false);
    if (!e || (*e & 3) != DESC_PAGE)
        return false;
    *phys = *e & DESC_ADDRESS;
    *e = 0;
    return true;
}

bool paging_user_protect(uint64_t space, uint64_t virt, unsigned access)
{
    uint64_t *e = user_entry(space, virt, false);
    if (!e || (*e & 3) != DESC_PAGE)
        return false;
    /* Only the permission bits change: no break-before-make needed. */
    *e = (*e & DESC_ADDRESS) | DESC_PAGE | DESC_ATTR(attr_normal) | DESC_SH_INNER | DESC_AF |
         DESC_NG | (access ? DESC_AP_EL0 : 0) | DESC_PXN | (access & USER_WRITE ? 0 : DESC_AP_RO) |
         (access & USER_EXEC ? 0 : DESC_UXN);
    return true;
}

void paging_user_flush(uint64_t space)
{
    (void)space;                            /* (no ASIDs: every core forgets everything) */
    dsb_ishst();
    __asm__ volatile ("tlbi vmalle1is" ::: "memory");
    dsb_ish();
    isb();
}

void paging_user_activate(uint64_t space)
{
    uint64_t tcr = read_sysreg(TCR_EL1);
    dsb_ishst();
    if (space) {
        write_sysreg(TTBR0_EL1, space);
        tcr &= ~TCR_EPD0;
    } else {
        tcr |= TCR_EPD0;
    }
    write_sysreg(TCR_EL1, tcr);
    isb();
    /* No address space IDs yet: forget all cached translations. */
    __asm__ volatile ("tlbi vmalle1" ::: "memory");
    dsb_ish();
    isb();
}

/* --- (35b) swap entries and the access flag ------------------------------------------ */

#define SWAP_ENTRY(slot) ((slot) << 12 | 2)    /* invalid: bit 0 clear, bit 1 set */

static bool is_swap_entry(uint64_t e)
{
    return (e & 3) == 2;
}

bool paging_user_evict(uint64_t space, uint64_t virt, uint64_t phys, uint64_t slot)
{
    uint64_t *e = user_entry(space, virt, false);
    uint64_t now = e ? __atomic_load_n(e, __ATOMIC_ACQUIRE) : 0;
    while (e && (now & 3) == DESC_PAGE && (now & DESC_ADDRESS) == phys)
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
    /* (An invalid entry becomes valid: no break-before-make needed.) */
    return e && __atomic_compare_exchange_n(e, &want, phys | DESC_PAGE | DESC_ATTR(attr_normal) |
                                            DESC_SH_INNER | DESC_AF | DESC_NG |
                                            (access ? DESC_AP_EL0 : 0) | DESC_PXN |
                                            (access & USER_WRITE ? 0 : DESC_AP_RO) |
                                            (access & USER_EXEC ? 0 : DESC_UXN),
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

/* No hardware access flag management here (TCR_EL1.HA is not set): a
 * cleared flag makes the next use fault (an access flag fault), which
 * paging_user_touch answers. Changing only the flag needs no
 * break-before-make. */
bool paging_user_young(uint64_t space, uint64_t virt)
{
    uint64_t *e = user_entry(space, virt, false);
    if (!e || (*e & 3) != DESC_PAGE)
        return false;
    return __atomic_fetch_and(e, ~DESC_AF, __ATOMIC_ACQ_REL) & DESC_AF;
}

bool paging_user_touch(uint64_t space, uint64_t virt)
{
    uint64_t *e = user_entry(space, virt, false);
    uint64_t now = e ? __atomic_load_n(e, __ATOMIC_ACQUIRE) : 0;
    while ((now & 3) == DESC_PAGE && !(now & DESC_AF))
        if (__atomic_compare_exchange_n(e, &now, now | DESC_AF, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return true;
    return false;
}

/* Free what the entries of a table at this level (0: top ... 3: pages)
 * point to: lower tables, and at the bottom the program's pages. */
static void free_below(uint64_t *table, int level)
{
    for (int i = 0; i < 512; i++) {
        if (!(table[i] & 1))
            continue;
        uint64_t phys = table[i] & DESC_ADDRESS;
        if (level < 3)
            free_below(phys_to_virt(phys), level + 1);
        pmm_free_page(phys);
    }
}

void paging_user_destroy(uint64_t space)
{
    free_below(phys_to_virt(space), 0);
    pmm_free_page(space);
}

uint64_t paging_firmware_enter(void)
{
    return 0;
}

void paging_firmware_leave(uint64_t previous)
{
    (void)previous;
}

/*
 * AArch64 parts of the bootloader: the kernel's first translation tables and
 * the jump to the kernel.
 *
 * ARM has two table roots: TTBR0_EL1 translates the lower half of the
 * address space, TTBR1_EL1 the upper half. The upper half gets the direct map
 * (HHDM_BASE) and the kernel image; the lower half a temporary identity map,
 * so that this code keeps running after the switch. Both share the level-1
 * table that maps physical memory.
 *
 * Unlike x86, ARM takes memory types only from the tables: RAM must be mapped
 * as Normal (cached) memory and everything else as Device memory. The
 * attribute indexes refer to MAIR_EL1, whose entries the firmware has already
 * defined; reusing them avoids changing MAIR while running.
 */
#include "boot_arch.h"
#include "console.h"
#include "cpu.h"
#include "string.h"
#include "vmlayout.h"
#include <stdbool.h>

#define GIB  (1ULL << 30)
#define MIB2 (2ULL << 20)

static UINT64 *pool;
static UINTN pool_pages, pool_used;
static UINT64 *ttbr0_root, *ttbr1_root;
static uint64_t tcr;
static unsigned normal_attr, device_attr;
static UINT64 kernel_start, kernel_bytes;

/* Index in MAIR_EL1 of an attribute encoding, or -1. */
static int mair_index(uint64_t mair, uint8_t a, uint8_t b)
{
    for (int i = 0; i < 8; i++) {
        uint8_t v = (uint8_t)(mair >> (8 * i));
        if (v == a || v == b)
            return i;
    }
    return -1;
}

EFI_STATUS boot_paging_alloc(EFI_BOOT_SERVICES *bs, UINT64 top, UINT64 kernel_size)
{
    if (current_el() != 1) {
        print(u"\r\n  this bootloader needs to run at EL1 (it runs at EL");
        print_dec(current_el(), 0);
        print(u")\r\n");
        return EFI_UNSUPPORTED;
    }
    uint64_t mair = read_sysreg(MAIR_EL1);
    int normal = mair_index(mair, MAIR_NORMAL_WB, MAIR_NORMAL_WB);
    int device = mair_index(mair, MAIR_DEVICE_NGNRE, MAIR_DEVICE_NGNRNE);
    if (normal < 0 || device < 0) {
        print(u"\r\n  the firmware's MAIR_EL1 lacks normal or device memory types\r\n");
        return EFI_UNSUPPORTED;
    }
    normal_attr = (unsigned)normal;
    device_attr = (unsigned)device;

    /* Two level-0 roots, one shared level-1 table, a level-2 table per GiB;
     * for the kernel a level-1, a level-2 and a level-3 table per 2 MiB; for
     * a framebuffer outside that a level-1 and two level-2 tables. */
    pool_pages = 2 + 1 + (UINTN)((top + GIB - 1) / GIB) + 2 + (UINTN)(kernel_size / MIB2) + 2 + 3;
    EFI_PHYSICAL_ADDRESS addr;
    EFI_STATUS status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pool_pages, &addr);
    if (!EFI_ERROR(status))
        pool = (UINT64 *)addr;
    return status;
}

static UINT64 *new_table(void)
{
    if (pool_used == pool_pages)
        return 0;
    UINT64 *t = pool + pool_used++ * 512;
    memset(t, 0, EFI_PAGE_SIZE);
    return t;
}

static UINT64 *next_table(UINT64 *table, UINTN index)
{
    if (!(table[index] & 1)) {
        UINT64 *t = new_table();
        if (!t)
            return 0;
        table[index] = (UINT64)t | DESC_TABLE;
    }
    return (UINT64 *)(table[index] & DESC_ADDRESS);
}

static unsigned index_at(UINT64 addr, int level)       /* level 0 = top */
{
    return (addr >> (39 - 9 * level)) & 511;
}

/* Does [start, start + size) overlap RAM, according to the memory map? */
static bool is_ram(const MEMORY_MAP *map, UINT64 start, UINT64 size)
{
    for (UINTN i = 0; i < MEMMAP_COUNT(map); i++) {
        EFI_MEMORY_DESCRIPTOR *d = MEMMAP_ENTRY(map, i);
        UINT64 end = d->PhysicalStart + d->NumberOfPages * EFI_PAGE_SIZE;
        bool ram = d->Type != EfiReservedMemoryType && d->Type != EfiUnusableMemory &&
                   d->Type != EfiMemoryMappedIO && d->Type != EfiMemoryMappedIOPortSpace;
        if (ram && d->PhysicalStart < start + size && end > start)
            return true;
    }
    return false;
}

UINT64 boot_paging_build(UINT64 top, UINT64 kernel_virt, UINT64 kernel_phys, UINT64 kernel_size,
                         const MEMORY_MAP *map)
{
    ttbr1_root = new_table();
    UINT64 *low = new_table();
    ttbr0_root = new_table();
    if (!ttbr1_root || !low || !ttbr0_root || top > 512 * GIB)
        return 0;
    kernel_start = kernel_phys;
    kernel_bytes = kernel_size;

    /* Physical memory in 2 MiB blocks, Normal or Device by the memory map. */
    ttbr0_root[index_at(0, 0)] = (UINT64)low | DESC_TABLE;
    ttbr1_root[index_at(HHDM_BASE, 0)] = (UINT64)low | DESC_TABLE;
    for (UINT64 gib = 0; gib * GIB < top; gib++) {
        UINT64 *l2 = new_table();
        if (!l2)
            return 0;
        low[gib] = (UINT64)l2 | DESC_TABLE;
        for (UINT64 i = 0; i < 512; i++) {
            UINT64 phys = gib * GIB + i * MIB2;
            l2[i] = phys | DESC_BLOCK | DESC_AF | DESC_UXN |
                    (is_ram(map, phys, MIB2) ? DESC_ATTR(normal_attr) | DESC_SH_INNER
                                             : DESC_ATTR(device_attr) | DESC_PXN);
        }
    }

    /* The kernel image in 4 KiB pages; the kernel sets finer permissions. */
    for (UINT64 off = 0; off < kernel_size; off += EFI_PAGE_SIZE) {
        UINT64 virt = kernel_virt + off;
        UINT64 *l1 = next_table(ttbr1_root, index_at(virt, 0));
        UINT64 *l2 = l1 ? next_table(l1, index_at(virt, 1)) : 0;
        UINT64 *l3 = l2 ? next_table(l2, index_at(virt, 2)) : 0;
        if (!l3)
            return 0;
        l3[index_at(virt, 3)] = (kernel_phys + off) | DESC_PAGE | DESC_AF | DESC_UXN |
                                DESC_ATTR(normal_attr) | DESC_SH_INNER;
    }

    /* 48-bit addresses in both halves, 4 KiB granule, table walks cached and
     * inner shareable, physical address size as the CPU reports it. */
    uint64_t pa_range = read_sysreg(ID_AA64MMFR0_EL1) & 0xF;
    tcr = 16ULL | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) |          /* TTBR0: T0SZ, cache, SH */
          (16ULL << 16) | (1ULL << 24) | (1ULL << 26) | (3ULL << 28) |  /* TTBR1 */
          (2ULL << 30) |                                                /* TG1: 4 KiB */
          (pa_range << 32);                                             /* IPS */
    return (UINT64)ttbr1_root;
}

bool boot_paging_map_framebuffer(UINT64 phys, UINT64 size)
{
    for (UINT64 a = phys & ~(MIB2 - 1); a < phys + size; a += MIB2) {
        UINT64 virt = HHDM_BASE + a;
        UINT64 *l1 = next_table(ttbr1_root, index_at(virt, 0));
        UINT64 *l2 = l1 ? next_table(l1, index_at(virt, 1)) : 0;
        if (!l2)
            return false;
        if (!(l2[index_at(virt, 2)] & 1))
            l2[index_at(virt, 2)] = a | DESC_BLOCK | DESC_AF | DESC_UXN | DESC_PXN |
                                    DESC_ATTR(device_attr);
    }
    return true;
}

UINT64 boot_paging_activate(UINT64 tables)
{
    (void)tables;
    return 0;
}

void boot_enter_kernel(UINT64 tables, UINT64 entry, struct boot_info *info)
{
    __asm__ volatile ("msr daifset, #0xf");        /* no interrupts until the kernel is ready */

    /* The kernel's code was written through the data cache: push it out and
     * discard stale instruction cache lines before running it. */
    uint64_t line = 4ULL << ((read_sysreg(CTR_EL0) >> 16) & 0xF);
    for (UINT64 a = kernel_start; a < kernel_start + kernel_bytes; a += line)
        __asm__ volatile ("dc cvau, %0" : : "r"(a) : "memory");
    dsb_ish();
    __asm__ volatile ("ic iallu" ::: "memory");
    dsb_ish();
    isb();

    /* Switch to the new tables. The identity map in TTBR0 keeps this code and
     * its stack where they are. */
    dsb_ishst();
    write_sysreg(TTBR0_EL1, (uint64_t)ttbr0_root);
    write_sysreg(TTBR1_EL1, tables);
    write_sysreg(TCR_EL1, tcr);
    isb();
    __asm__ volatile ("tlbi vmalle1" ::: "memory");
    dsb_ish();
    isb();

    kernel_entry_fn kernel = (kernel_entry_fn)entry;
    kernel(info);
}

void boot_halt(void)
{
    __asm__ volatile ("msr daifset, #0xf");        /* mask all interrupts */
    for (;;)
        __asm__ volatile ("wfe");
}

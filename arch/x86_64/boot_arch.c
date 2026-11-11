/* x86-64 parts of the bootloader: the kernel's first page tables, the jump. */
#include "boot_arch.h"
#include "string.h"
#include "vmlayout.h"

#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITABLE (1ULL << 1)
#define PTE_LARGE    (1ULL << 7)
#define PTE_ADDRESS  0x000FFFFFFFFFF000ULL

#define GIB (1ULL << 30)
#define MIB2 (2ULL << 20)

static UINT64 *pool;
static UINTN pool_pages, pool_used;
static UINT64 *pml4;

static UINTN tables_needed(UINT64 top, UINT64 kernel_size)
{
    UINTN gib = (UINTN)((top + GIB - 1) / GIB);
    /* PML4, one PDPT for the low 512 GiB, a PD per GiB; for the kernel a PDPT,
     * a PD and a PT per 2 MiB (+1 for an unaligned start); for a framebuffer
     * outside that a PDPT and two PDs; some slack. */
    return 1 + 1 + gib + 1 + 1 + (UINTN)(kernel_size / MIB2) + 2 + 3 + 4;
}

EFI_STATUS boot_paging_alloc(EFI_BOOT_SERVICES *bs, UINT64 top, UINT64 kernel_size)
{
    EFI_PHYSICAL_ADDRESS addr;
    pool_pages = tables_needed(top, kernel_size);
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
    if (!(table[index] & PTE_PRESENT)) {
        UINT64 *t = new_table();
        if (!t)
            return 0;
        table[index] = (UINT64)t | PTE_PRESENT | PTE_WRITABLE;
    }
    return (UINT64 *)(table[index] & PTE_ADDRESS);
}

static unsigned index_at(UINT64 addr, int level)
{
    return (addr >> (12 + 9 * level)) & 511;
}

UINT64 boot_paging_build(UINT64 top, UINT64 kernel_virt, UINT64 kernel_phys, UINT64 kernel_size,
                         const MEMORY_MAP *map)
{
    (void)map;              /* x86: memory types come from the firmware's MTRRs */
    pml4 = new_table();
    UINT64 *low = new_table();
    if (!pml4 || !low || top > 512 * GIB)
        return 0;

    /* Physical memory in 2 MiB pages. The same PDPT serves the identity map
     * (PML4 slot 0) and the direct map (slot 256 = HHDM_BASE). */
    pml4[index_at(0, 3)] = (UINT64)low | PTE_PRESENT | PTE_WRITABLE;
    pml4[index_at(HHDM_BASE, 3)] = (UINT64)low | PTE_PRESENT | PTE_WRITABLE;
    for (UINT64 gib = 0; gib * GIB < top; gib++) {
        UINT64 *pd = new_table();
        if (!pd)
            return 0;
        low[gib] = (UINT64)pd | PTE_PRESENT | PTE_WRITABLE;
        for (UINT64 i = 0; i < 512; i++)
            pd[i] = (gib * GIB + i * MIB2) | PTE_PRESENT | PTE_WRITABLE | PTE_LARGE;
    }

    /* The kernel image in 4 KiB pages; the kernel sets finer permissions. */
    for (UINT64 off = 0; off < kernel_size; off += EFI_PAGE_SIZE) {
        UINT64 virt = kernel_virt + off;
        UINT64 *pdpt = next_table(pml4, index_at(virt, 3));
        UINT64 *pd = pdpt ? next_table(pdpt, index_at(virt, 2)) : 0;
        UINT64 *pt = pd ? next_table(pd, index_at(virt, 1)) : 0;
        if (!pt)
            return 0;
        pt[index_at(virt, 0)] = (kernel_phys + off) | PTE_PRESENT | PTE_WRITABLE;
    }
    return (UINT64)pml4;
}

bool boot_paging_map_framebuffer(UINT64 phys, UINT64 size)
{
    for (UINT64 a = phys & ~(MIB2 - 1); a < phys + size; a += MIB2) {
        UINT64 virt = HHDM_BASE + a;
        UINT64 *pdpt = next_table(pml4, index_at(virt, 3));
        UINT64 *pd = pdpt ? next_table(pdpt, index_at(virt, 2)) : 0;
        if (!pd)
            return false;
        if (!(pd[index_at(virt, 1)] & PTE_PRESENT))
            pd[index_at(virt, 1)] = a | PTE_PRESENT | PTE_WRITABLE | PTE_LARGE;
    }
    return true;
}

UINT64 boot_paging_activate(UINT64 tables)
{
    __asm__ volatile ("cli; mov %0, %%cr3" : : "r"(tables) : "memory");
    return HHDM_BASE;
}

void boot_enter_kernel(UINT64 tables, UINT64 entry, struct boot_info *info)
{
    /* Interrupts stay off until the kernel has its own handlers. The identity
     * map in the new tables keeps this code running after the switch. */
    __asm__ volatile ("cli; mov %0, %%cr3" : : "r"(tables) : "memory");
    kernel_entry_fn kernel = (kernel_entry_fn)entry;
    kernel(info);
}

void boot_halt(void)
{
    for (;;)
        __asm__ volatile ("cli; hlt");
}

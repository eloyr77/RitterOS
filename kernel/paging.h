/*
 * Kernel page tables: the kernel image at KERNEL_VIRT_BASE with permissions
 * per section and an unmapped stack guard page, the direct map of physical
 * memory at HHDM_BASE, and nothing in the lower half (common/vmlayout.h).
 * Implemented per architecture in arch/<arch>/paging.c; both x86-64 and
 * AArch64 use 4-level tables with 4 KiB pages and 2 MiB large pages.
 */
#ifndef RITTEROS_PAGING_H
#define RITTEROS_PAGING_H

#include <stdbool.h>
#include <stdint.h>
#include "boot_info.h"

#define PAGE_SIZE       4096ULL
#define LARGE_PAGE_SIZE (2ULL * 1024 * 1024)

/* Build the kernel's page tables from the memory map and switch to them. */
void paging_init(const struct boot_info *info);

/*
 * Make device registers at physical [phys, phys + size) accessible through
 * the direct map, uncacheable, and return their virtual address. Pages that
 * are already mapped (everything below 4 GiB) are left as they are; the
 * firmware's memory type ranges make those uncacheable already.
 */
volatile void *paging_map_mmio(uint64_t phys, uint64_t size);

/*
 * Firmware calls (UEFI runtime services) run in their own address space:
 * the kernel's, plus physical memory at its own address in the lower half.
 * Firmware should only use the virtual addresses it was given by
 * SetVirtualAddressMap, but some keeps physical pointers (a real PC's AMI
 * firmware read a chipset register at 0xFED08004 in GetTime). Call with
 * interrupts disabled; enter returns what leave restores. ARM: nothing to
 * do so far (QEMU's firmware does not need it).
 */
/* (65b) The physical address of the kernel's top-level table. */
uint64_t paging_kernel_root(void);

/* (66b-3) Make the direct map of physical [phys, phys + size) write-
 * combining, as the boot does for the firmware's framebuffer: a mode set
 * later may need a larger one. (66f-3) What is not mapped yet is mapped
 * (2 MiB pages where they fit; tables from the page allocator). Every
 * core forgets its old translations.
 * False where the architecture does not (ARM: no driver sets modes there)
 * or a page table could not be had. */
bool paging_write_combining(uint64_t phys, uint64_t size);

uint64_t paging_firmware_enter(void);
void paging_firmware_leave(uint64_t previous);

/* Look up how addr is mapped: physical address and the leaf entry's
 * (architecture-specific) flags. Returns false if it is not mapped. */
bool paging_query(uint64_t addr, uint64_t *phys, uint64_t *flags);

/* (67a) Map the 4 KiB page at virt (a kernel module's, in MODULE_BASE's
 * range) to phys with its final access: PAGING_TEXT read + execute,
 * PAGING_RODATA read only, PAGING_DATA read + write. Its tables come from
 * the page allocator. False without memory. The module's code is written
 * before (through the direct map; ARM: arch_code_written after). */
enum { PAGING_TEXT, PAGING_RODATA, PAGING_DATA };
bool paging_map_kernel(uint64_t virt, uint64_t phys, int access);

/*
 * User address spaces: page tables for the lower half, one per program. A
 * space is named by the physical address of its top-level table. Its pages
 * belong to it: paging_user_destroy frees them with the tables.
 */
#define USER_READ   1
#define USER_WRITE  2
#define USER_EXEC   4
#define USER_COMBINE 8          /* (66d-2) write-combining: a screen buffer (kernel use only) */

/* A new, empty user address space; 0 if out of memory. */
uint64_t paging_user_create(void);

/* Map the 4 KiB page at phys to virt (lower half) with the given USER_*
 * access (0: the page is there but the program may not touch it).
 * False if out of memory or virt is already mapped. */
bool paging_user_map(uint64_t space, uint64_t virt, uint64_t phys, unsigned access);

/* Physical address and USER_* access of virt in space (access 0 for a page
 * the program may not touch); false if unmapped. */
bool paging_user_query(uint64_t space, uint64_t virt, uint64_t *phys, unsigned *access);

/* Remove the mapping of virt in space WITHOUT freeing the physical page (it
 * belongs to something else, e.g. a shared buffer). Flushes the TLB. */
void paging_user_unmap(uint64_t space, uint64_t virt);

/* (65e) Change a present page's access in place, without telling any core
 * (its old access may stay in their TLBs): a batch of these is followed by
 * paging_user_flush, which makes every core forget the space's
 * translations at once (fork: one announcement, not one per page). */
bool paging_user_protect(uint64_t space, uint64_t virt, unsigned access);
void paging_user_flush(uint64_t space);

/* (65e) Unmap a page without telling any core, as paging_user_protect: the
 * physical page may be freed only after the paging_user_flush that follows.
 * (65e-49) In one walk, giving the page that was there (until 65e-49 a
 * query, then a clear: two walks for every page munmap took out); false
 * when nothing was. */
bool paging_user_take(uint64_t space, uint64_t virt, uint64_t *phys);

/*
 * (35b) Swap. A page written to swap leaves a swap entry where it was
 * mapped: not present, holding its slot in the swap file; paging_user_map
 * never puts a page over one (only over an empty entry). None of these
 * tells the other cores: paging_user_flush after a batch, before the pages
 * taken out are used again or freed.
 */
/* Replace the present page phys at virt by the swap entry for slot (false
 * if phys is not what is there). */
bool paging_user_evict(uint64_t space, uint64_t virt, uint64_t phys, uint64_t slot);
/* The slot of the swap entry at virt; false if there is none. */
bool paging_user_swapped(uint64_t space, uint64_t virt, uint64_t *slot);
/* Put the page phys with access at virt in place of the swap entry for
 * slot (false if that is not what is there). */
bool paging_user_swap_in(uint64_t space, uint64_t virt, uint64_t slot, uint64_t phys,
                         unsigned access);
/* (35c) Put the swap entry for slot at virt, where nothing is (fork copies
 * one into the child); false without memory for a table or if virt is
 * taken. */
bool paging_user_put_swapped(uint64_t space, uint64_t virt, uint64_t slot);
/* Clear the swap entry at virt, giving its slot; false if there is none. */
bool paging_user_take_swapped(uint64_t space, uint64_t virt, uint64_t *slot);
/* Whether the page at virt was used since the last look, and clear that
 * (x86's accessed bit; ARM's access flag: the next use faults, and
 * paging_user_touch sets it again). False for no page. */
bool paging_user_young(uint64_t space, uint64_t virt);
/* (ARM) A fault on a present page whose access flag pageout cleared: set it,
 * true. False for anything else (x86's processor sets its bit itself). */
bool paging_user_touch(uint64_t space, uint64_t virt);

/* Make space the lower half of the running address space (0: none). */
void paging_user_activate(uint64_t space);

/* Free the tables and every page mapped in space. It must not be active. */
void paging_user_destroy(uint64_t space);

#endif

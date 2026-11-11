/*
 * Physical memory manager: hands out 4 KiB page frames of RAM.
 *
 * A bitmap holds one bit per frame (1 = in use). Only memory the boot memory
 * map calls usable is ever free; the first 1 MiB, the kernel image, boot data
 * and the bitmap itself stay reserved.
 */
#ifndef RITTEROS_PMM_H
#define RITTEROS_PMM_H

#include <stddef.h>
#include <stdint.h>
#include "boot_info.h"

#define PMM_PAGE_SIZE ((uint64_t)4096)

/* Build the bitmap from the memory map. Needs our page tables (paging_init),
 * since it frees the memory that held the firmware's. */
void pmm_init(const struct boot_info *info);

/* Allocate one page; returns its physical address, or 0 if memory is full
 * (address 0 is never handed out). Access it through phys_to_virt(). */
uint64_t pmm_alloc_page(void);

/* (35b) A page as high in memory as there is one: programs' pages while
 * swap is on. Paging out frees pages all over the memory programs use; kept
 * at the top, the kernel's runs of pages (stacks, large blocks: next fit
 * from the bottom) keep their room below. (35e) Through the core's own
 * `high` pages, as pmm_alloc_page through its cache. */
uint64_t pmm_alloc_page_high(void);
/* Allocate count physically contiguous pages; returns the first address or 0. */
uint64_t pmm_alloc_pages(size_t count);

/* The same, with the whole run below limit (for devices that DMA with 32-bit
 * addresses, like the MT7921's descriptor rings). */
uint64_t pmm_alloc_pages_below(size_t count, uint64_t limit);

/* (65b) A usable page in the first 640 KiB, never handed out (the first
 * 1 MiB is kept): where x86's other cores start, in real mode. 0 if none. */
uint64_t pmm_low_page(void);

/* Return pages. Freeing a page that is not allocated is a kernel bug: panic. */
void pmm_free_page(uint64_t addr);
void pmm_free_pages(uint64_t addr, size_t count);

/* Page counts. */
uint64_t pmm_free_count(void);
uint64_t pmm_usable_count(void);
/* (35b) The free count without the lock: about right, for watching how low
 * memory gets (a page fault looks every time). */
uint64_t pmm_free_estimate(void);

/* Allocate, write, verify and free 1000 pages; panics on any error. */
void pmm_self_test(void);

/* Allocate every free page, write its own address into it, verify all of
 * them, and free them again. Slow but thorough; panics on any error. */
void pmm_memtest(void);

#endif

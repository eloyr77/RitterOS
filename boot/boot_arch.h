/*
 * What each architecture provides to the bootloader (arch/<arch>/boot_arch.c).
 *
 * The page tables the kernel starts on (see common/vmlayout.h):
 *   - physical memory 0 .. top at HHDM_BASE (the direct map) and, temporarily,
 *     also at 0 (identity), so the bootloader keeps running after switching;
 *   - the kernel image at its link address, backed by its physical pages.
 * The kernel replaces these tables with its own, without the identity map.
 */
#ifndef RITTEROS_BOOT_ARCH_H
#define RITTEROS_BOOT_ARCH_H

#include <stdbool.h>
#include "efi.h"
#include "boot_info.h"
#include "memmap.h"

/* The kernel file on the RitterOS partition (RitterFS), one per architecture. */
#if defined(__x86_64__)
#define KERNEL_PATH "/system/x86_64/kernel.elf"
#elif defined(__aarch64__)
#define KERNEL_PATH "/system/aarch64/kernel.elf"
#endif

/* Allocate memory for the tables. Must be called before ExitBootServices. */
EFI_STATUS boot_paging_alloc(EFI_BOOT_SERVICES *bs, UINT64 top, UINT64 kernel_size);

/* Build the tables; returns the address of the top-level table (of the upper
 * half, on architectures with two). The memory map tells RAM from device
 * memory where the architecture needs it. Needs no firmware services. */
UINT64 boot_paging_build(UINT64 top, UINT64 kernel_virt, UINT64 kernel_phys, UINT64 kernel_size,
                         const MEMORY_MAP *map);

/* Add the framebuffer to the direct map if it lies outside 0 .. top (real
 * graphics cards often place it above 4 GiB or above all RAM), so that the
 * kernel can draw from its first instruction. False if out of tables. */
bool boot_paging_map_framebuffer(UINT64 phys, UINT64 size);

/*
 * Switch to the new tables already for SetVirtualAddressMap (after
 * ExitBootServices, interrupts off). They map memory both at its physical
 * address and in the direct map, so firmware that uses the new addresses
 * during the call (old AMI firmware hung there) finds its code at both;
 * Linux calls it the same way. ARM: nothing to do here, the switch happens
 * at the jump (QEMU's firmware handles the call with its own tables).
 * Returns the offset at which physical memory is reachable from now on
 * (x86: HHDM_BASE, as the direct map has everything, even the framebuffer
 * above 512 GiB; ARM: 0, the firmware's identity map).
 */
UINT64 boot_paging_activate(UINT64 tables);

/* Switch to the tables and jump to the kernel with info (a virtual address). */
__attribute__((noreturn))
void boot_enter_kernel(UINT64 tables, UINT64 entry, struct boot_info *info);

/* Stop for good after a fatal error (after ExitBootServices). */
__attribute__((noreturn)) void boot_halt(void);

#endif

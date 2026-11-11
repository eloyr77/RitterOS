/* Reading files from the boot partition; loading the kernel ELF file into memory. */
#ifndef RITTEROS_LOADER_H
#define RITTEROS_LOADER_H

#include "efi.h"

typedef struct {
    UINT64 entry;          /* virtual address of the kernel entry point */
    UINT64 virt_base;      /* virtual address of the image start (page-aligned) */
    UINT64 phys_base;      /* where the image was placed in physical memory */
    UINT64 size;           /* bytes reserved for the image (page-aligned) */
    UINTN file_size;
    UINTN segments;        /* number of PT_LOAD segments */
} LOADED_KERNEL;

/*
 * Read a whole file from the partition this bootloader was loaded from into
 * a pool allocation (free it with FreePool). On failure, *why says what went
 * wrong; nothing is printed.
 */
EFI_STATUS read_file(EFI_BOOT_SERVICES *bs, EFI_HANDLE image, const CHAR16 *path,
                     UINT8 **data, UINTN *size, const CHAR16 **why);

/*
 * Check that file (size bytes) is an ELF executable for this CPU, and copy
 * its segments into free physical memory, keeping their layout. The image
 * must then be mapped at virt_base (its link address) before it can run.
 * Prints the reason on failure.
 */
EFI_STATUS load_kernel(EFI_BOOT_SERVICES *bs, const UINT8 *file, UINTN size,
                       LOADED_KERNEL *kernel);

#endif

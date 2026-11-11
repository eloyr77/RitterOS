/* Reading the UEFI memory map and converting it for the kernel. */
#ifndef RITTEROS_MEMMAP_H
#define RITTEROS_MEMMAP_H

#include "efi.h"
#include "boot_info.h"

typedef struct {
    EFI_MEMORY_DESCRIPTOR *descriptors;  /* firmware-sized entries, see desc_size */
    UINTN size;                          /* bytes used in descriptors */
    UINTN capacity;                      /* bytes allocated for descriptors */
    UINTN key;                           /* identifies this map version for ExitBootServices */
    UINTN desc_size;
    UINT32 desc_version;
} MEMORY_MAP;

/* Fetch the current memory map into map (allocating its buffer as needed). */
EFI_STATUS memmap_get(EFI_BOOT_SERVICES *bs, MEMORY_MAP *map);

/* Sort the descriptors by physical address. */
void memmap_sort(MEMORY_MAP *map);

/* One line: descriptor count, free and usable-after-boot totals. */
void memmap_print_summary(const MEMORY_MAP *map);

/*
 * Convert a sorted map into kernel memory regions, merging neighbors of the
 * same kernel type. Uses no firmware services, so it is safe after
 * ExitBootServices. Returns the number of regions written.
 */
UINTN memmap_to_regions(const MEMORY_MAP *map, struct memory_region *out, UINTN capacity);

/* End of the highest RAM or firmware region (not MMIO or reserved), at least
 * 4 GiB so that everything below 4 GiB (devices, framebuffer) is covered. */
UINT64 memmap_ram_top(const MEMORY_MAP *map);

#define MEMMAP_ENTRY(map, i) \
    ((EFI_MEMORY_DESCRIPTOR *)((UINT8 *)(map)->descriptors + (i) * (map)->desc_size))
#define MEMMAP_COUNT(map) ((map)->size / (map)->desc_size)

#endif

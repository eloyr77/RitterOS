/*
 * UEFI device paths: a binary description of where a device sits, as a list
 * of nodes from the root, e.g. PCI root bridge -> PCI device -> NVMe
 * namespace -> partition.
 */
#ifndef RITTEROS_DEVPATH_H
#define RITTEROS_DEVPATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The partition node (UEFI "HD" node) of a device path. */
struct devpath_partition {
    uint32_t number;           /* 1-based partition number */
    uint64_t start_lba;
    uint64_t sector_count;
    uint8_t guid[16];          /* GPT unique partition GUID (on-disk byte order) */
    bool is_gpt;
};

/* Write the path in UEFI text notation, e.g.
 * "PciRoot(0x0)/Pci(0x2,0x0)/NVMe(0x1,...)/HD(1,GPT,<guid>,0x800,0x20000)". */
void devpath_to_text(const uint8_t *path, size_t size, char *out, size_t out_size);

/* Find the partition node; false if the path has none. */
bool devpath_partition(const uint8_t *path, size_t size, struct devpath_partition *part);

/* The Pci(device,function) nodes of the path, from the root bridge outwards.
 * Returns how many were stored (at most max). */
int devpath_pci_nodes(const uint8_t *path, size_t size, uint8_t *devices, uint8_t *functions,
                      int max);

/* Format a GUID stored in UEFI/GPT byte order as text (37 bytes needed). */
void guid_to_text(const uint8_t guid[16], char out[37]);

#endif

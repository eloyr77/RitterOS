/*
 * Block devices: storage read and written in fixed-size blocks (sectors).
 * Disk drivers provide one; partition tables and file systems use only this
 * interface, so they work with any kind of disk.
 */
#ifndef RITTEROS_BLOCKDEV_H
#define RITTEROS_BLOCKDEV_H

#include <stdbool.h>
#include <stdint.h>

struct block_device {
    char name[32];             /* e.g. "nvme0n1" */
    uint32_t block_size;       /* bytes per block, e.g. 512 */
    uint64_t block_count;
    /* Transfer count blocks starting at lba to or from buffer (any kernel
     * memory). Return false on an I/O error. */
    bool (*read)(struct block_device *dev, uint64_t lba, uint32_t count, void *buffer);
    bool (*write)(struct block_device *dev, uint64_t lba, uint32_t count, const void *buffer);
    /* Make all completed writes permanent (empty the device's write cache). */
    bool (*flush)(struct block_device *dev);
    /* Optional (0: the device cannot): count blocks from lba hold nothing
     * worth keeping; an SSD can erase them ahead of time (TRIM, NVMe
     * deallocate). Their content reads as anything afterwards. */
    bool (*discard)(struct block_device *dev, uint64_t lba, uint64_t count);
    void *driver_data;
    bool system;               /* holds the system partition (set by main.c) */
    const char *kind;          /* "NVMe", "SATA", "USB", "partition" */
    char model[48];            /* as the device names itself */
};

#endif

/*
 * Making file systems on a device opened for writing (/dev/NAME): the
 * writers that format and osinstall share (user/lib/mkfs*.c). Each takes
 * the open device, writes everything and returns false (errno set) when a
 * write fails. Sizes are the device's bytes; hidden is where the volume
 * begins on its disk, in the disk's blocks (FAT and exFAT keep it).
 */
#ifndef RITTEROS_MKFS_H
#define RITTEROS_MKFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MKFS_CLEAR          (64 * 1024)     /* cleared first: other file systems' marks */
#define EXFAT_LABEL_MAX     11              /* UTF-16 units */

/* RitterFS: 0 if one fits bytes, else why not ("is too small ..."). */
const char *mkfs_ritterfs_check(uint64_t bytes);
bool mkfs_ritterfs(int fd, const char *label, bool whole_disk, uint64_t device_bytes,
                   uint64_t now);

/* FAT32. */
struct fat_layout {
    uint32_t sector, cluster_sectors;   /* bytes per sector; sectors per cluster */
    uint64_t sectors;                   /* the whole volume */
    uint32_t fat_sectors, clusters;
};
/* The layout for bytes with sectors of sector bytes; false if FAT32 does
 * not fit (too small or too large). */
bool mkfs_fat_plan(uint64_t bytes, uint32_t sector, struct fat_layout *l);
/* Up to 11 letters, digits, spaces or !#$%&'()-@^_`{}~ (kept in upper case). */
bool mkfs_fat_label_valid(const char *label);
bool mkfs_fat32(int fd, const struct fat_layout *l, const char *label, uint64_t hidden,
                bool whole_disk, uint64_t device_bytes, uint64_t now);

/* exFAT. */
struct exfat_layout {
    uint32_t sector, sector_shift, cluster_shift, cluster;
    uint64_t sectors;                       /* the volume's length */
    uint32_t fat_offset, fat_length, heap_offset, clusters;
    uint32_t bitmap_clusters, upcase_clusters;  /* from cluster 2: bitmap, table, root */
};
bool mkfs_exfat_plan(uint64_t bytes, uint32_t sector, struct exfat_layout *l);
/* A label in UTF-16 (up to 11 characters of the Basic Multilingual Plane,
 * none of "*:<>?\|/ or control characters): the units, or -1. */
int mkfs_exfat_label(const char *label, uint16_t *out);
bool mkfs_exfat(int fd, const struct exfat_layout *l, const uint16_t *label, int label_units,
                uint64_t hidden, bool whole_disk, uint64_t device_bytes);

/* What the writers share. */
bool mkfs_write(int fd, uint64_t offset, const void *data, size_t n);
bool mkfs_zero(int fd, uint64_t offset, uint64_t n);
void mkfs_put16(uint8_t *p, uint32_t v);
void mkfs_put32(uint8_t *p, uint32_t v);
void mkfs_put64(uint8_t *p, uint64_t v);

#endif

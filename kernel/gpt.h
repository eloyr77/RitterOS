/*
 * GUID Partition Table. Sector 0 holds a "protective" MBR; sector 1 the GPT
 * header, which points to an array of partition entries. A backup header
 * and array sit at the end of the disk. Header and array carry CRC32
 * checksums.
 */
#ifndef RITTEROS_GPT_H
#define RITTEROS_GPT_H

#include <stdbool.h>
#include <stdint.h>
#include "blockdev.h"

/* Read and verify the partition table of dev and log it. */
bool gpt_report(struct block_device *dev);

/* GPT type GUID of RitterOS's own partition (also in scripts/mkdisk.py). */
#define GPT_TYPE_RITTEROS "B5CA1B3E-6278-48F0-BBEF-87907326A8B3"

/* GPT type GUID of the EFI system partition. */
#define GPT_TYPE_EFI "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"

/* What a partition table says about one partition. */
#define PARTITION_TABLE_NONE 0
#define PARTITION_TABLE_GPT  1
#define PARTITION_TABLE_MBR  2

struct partition_info {
    uint32_t number;                /* 1 for the first entry */
    uint64_t first_lba, block_count;
    uint8_t type_guid[16];          /* GPT; zero for MBR */
    uint8_t unique_guid[16];
    char name[37];                  /* GPT name (ASCII; others as '?') */
    uint8_t mbr_type;               /* MBR's type byte; 0 for GPT */
};

/* A disk's table: its kind and the blocks partitions may use (GPT: from
 * its header; MBR: 1 to the end, at most 2^32 - 1; none: 0 and 0). */
struct partition_table {
    int kind;                       /* PARTITION_TABLE_* */
    uint64_t first_usable, last_usable;
};

/* The disk's partitions, from a valid GPT (the copy at the disk's end if
 * the first is damaged) or else an MBR's four primary
 * entries (a boot sector of a file system on the whole disk, as on some
 * sticks, is not taken for an MBR). Returns how many (at most max), and
 * the table in *table. */
int partition_table_read(struct block_device *disk, struct partition_info *out, int max,
                         struct partition_table *table);

/* A partition as a block device named like "nvme0n1p2" or "usbdisk0p2":
 * block 0 is its first sector; accesses beyond its end fail. */
struct block_device *partition_open(struct block_device *disk, const struct partition_info *info);

/* A partition's disk; a device that is no partition is its own disk. */
struct block_device *partition_disk(struct block_device *dev);

/* A partition's first block on its disk; 0 for a disk. */
uint64_t partition_first_lba(struct block_device *dev);

/* The known name of a GPT type GUID (text form) or MBR type byte, or 0. */
const char *partition_type_name(const struct partition_info *info);

#endif

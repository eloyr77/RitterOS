/*
 * The devices (kernel/devices.c): every disk the drivers found, at the boot
 * or plugged in later, and every partition in its table, as /dev lists
 * them and the program disks shows them, with what each holds (a file
 * system found by its signature, its label).
 */
#ifndef RITTEROS_DEVICES_H
#define RITTEROS_DEVICES_H

#include <stdbool.h>
#include <stdint.h>
#include "blockdev.h"

struct sys_device;

/* A new disk: it and its partitions are listed from now on (each logged
 * with what it holds). */
void devices_add_disk(struct block_device *disk);

/* The disk is gone (unplugged): it and its partitions are no longer listed. */
void devices_remove_disk(struct block_device *disk);

/* A partition of disk with this unique GPT GUID (16 bytes), or else (guid
 * 0) the first with this GPT type GUID (text form); 0 if none. */
struct block_device *devices_find_partition(struct block_device *disk, const uint8_t *guid,
                                            const char *type);

/* Read the disk's partition table again (after a program wrote to
 * written, the disk or one of its partitions): partitions that are
 * unchanged keep their entries and mounts (what an unmounted one holds is
 * probed again), new ones are probed, the others are no longer listed; a
 * mounted one gone from the table stays listed until it is unmounted. New
 * partitions holding a RitterFS are mounted, and so is written if it holds
 * one now. */
void devices_rescan(struct block_device *disk, struct block_device *written);

/* Mount every RitterFS and FAT on disk (on the whole disk, or a
 * partition's), found by its content, that is not mounted yet: at the boot
 * and when a disk is plugged in. EFI system partitions are left alone. */
void devices_mount_volumes(struct block_device *disk);

/* Whether blocks [lba, lba + blocks) of dev (a disk or partition) hold a
 * mounted file system, wholly or in part: raw writes there are refused. */
bool devices_busy(struct block_device *dev, uint64_t lba, uint64_t blocks);

/* The device (disk or partition) named name, or 0. */
struct block_device *devices_by_name(const char *name);

/* Entry number index of the list (disks, each followed by its partitions);
 * false past the end. */
bool devices_info(int index, struct sys_device *out);

/* (68j) SYS_GROW: the RitterFS mounted on the partition named takes the
 * room its partition grew by: its block count now, or -ERR_*. */
int64_t devices_grow(const char *name);

/* The list as the program disks and the rescue console show it: a line
 * per disk and per partition (REPORT_DISKS). */
void devices_print(void);

/* Where the device's file system is mounted ("" and 0 for nowhere); its
 * label and generation are then the mounted file system's own. */
struct vfs;
void devices_set_mounted(struct block_device *dev, const char *path, struct vfs *fs);

#endif

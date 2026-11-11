/*
 * Mounted file systems besides the system's own at / (kernel/mounts.c):
 * RitterFS and FAT volumes on other partitions and disks, automatically at
 * /volumes/<label> when they are found (at the boot or plugged in), or
 * wherever root mounts them; the system disk's EFI partition at /boot.
 */
#ifndef RITTEROS_MOUNTS_H
#define RITTEROS_MOUNTS_H

#include <stdbool.h>
#include <stddef.h>
#include "blockdev.h"

struct vfs;

#define VOLUMES_DIR     "/volumes"
#define MOUNT_PATH_MAX  128

/* The system's file system, mounted at / by main.c: never mounted again. */
void mounts_set_root(struct block_device *dev, struct vfs *fs);
/* The root file system (0 before it is mounted), for kernel reads. */
struct vfs *mounts_root(void);

/* A partition (or disk) just found with a RitterFS or FAT: mount it (a
 * RitterFS checked, read only if the check finds problems) at
 * /volumes/<label>. Logs where. */
void mounts_auto(struct block_device *dev);

/* The same at path, and read only for good if read_only (the EFI system
 * partition at /boot). */
void mounts_auto_at(struct block_device *dev, const char *path, bool read_only);

/* Mount dev at path (0: /volumes/<label>); the path used goes to out.
 * Returns 0 or ERR_* (ERR_BUSY: dev or path is mounted already;
 * ERR_INVAL: nothing on dev that can be mounted). Its file system is mounted once and
 * kept: mounting it again finds it as it was. */
int mounts_add(struct block_device *dev, const char *path, char *out, size_t size);

/* The mount whose path is path, or whose device is named path; false if
 * none. */
bool mounts_get(const char *path, char *mount_path, size_t size, struct vfs **fs,
                struct block_device **dev);

/* Unmount the mount at mount_path (the caller made sure nothing uses it). */
void mounts_remove(const char *mount_path);

/* The file system that holds path (absolute, normalized), if a mount
 * covers it: the longest mount path that path is or lies in; *rest is the
 * path inside it ("/" for the mount point). */
bool mounts_find(const char *path, struct vfs **fs, const char **rest);

/* Entry number index of /volumes (the mounts there, in the order of their
 * names); false past the end. */
bool mounts_volume(int index, char *name, size_t size);

/* A number for fs (or a snapshot's view of it): 1 for the system's, then
 * one per volume mounted so far (stat's device); 0 if unknown. */
uint32_t mounts_fs_number(struct vfs *fs);

/* A program wrote to the disk: its file systems that are not mounted now
 * are read afresh when they are mounted again (the one kept from an
 * earlier mount, with its cache, may no longer match the disk). */
void mounts_forget(struct block_device *disk);

/* The disk is gone: its mounts end (logged). */
void mounts_disk_gone(struct block_device *disk);

#endif

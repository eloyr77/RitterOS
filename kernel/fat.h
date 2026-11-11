/*
 * FAT12, FAT16 and FAT32 (kernel/fat.c), read and written: the file system
 * of shop-bought sticks, memory cards and the EFI system partition. Long
 * file names (VFAT) are read and written (an 8.3 alias for each, as
 * Windows makes them); names are found without regard to case, as Windows
 * does. FAT has no owners: everything is root's, and everyone may
 * read and write (0777 directories, 0666 files; without write bits where
 * the read-only attribute is set).
 *
 * Objects for struct vfs: the root directory is ID 1; every other file or
 * directory is named by where its directory entry lies (its byte position
 * on the device / 32 + 2), and its inode keeps the first cluster in
 * root.address. Times are taken as UTC (FAT stores local time without a
 * zone).
 */
#ifndef RITTEROS_FAT_H
#define RITTEROS_FAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "blockdev.h"
#include "rfs_format.h"

struct fat;
struct vfs;

#define FAT_ROOT_ID 1

/* Mount the FAT on dev (its boot sector checked, the root directory read);
 * logs what it found. 0 if dev holds no FAT, or a damaged one (logged). */
struct fat *fat_mount(struct block_device *dev);

struct vfs *fat_vfs(struct fat *f);
const char *fat_type(struct fat *f);        /* "FAT12", "FAT16", "FAT32" */
struct block_device *fat_device(struct fat *f);
void fat_label(struct fat *f, char *out, size_t size);

bool fat_get_inode(struct fat *f, uint64_t id, struct rfs_inode *out);
bool fat_lookup(struct fat *f, const char *path, uint64_t *id, struct rfs_inode *inode);
int64_t fat_read(struct fat *f, uint64_t id, const struct rfs_inode *inode, uint64_t offset,
                 void *buf, uint64_t size);
int fat_dir_next(struct fat *f, uint64_t dir_id, const struct rfs_inode *dir, uint64_t *pos,
                 char *name, uint8_t *name_len, uint64_t *id, uint8_t *type);

/* Changes, as vfs.h describes them (RFS_OK or RFS_ERR_*). Data goes to the
 * device at once, the FAT at the latest with fat_commit; renaming gives a
 * file a new ID (its entry moves). chmod keeps only the owner's w, as the
 * read-only attribute; there are no owners to change. */
int fat_create(struct fat *f, const char *path, int type, uint64_t *id);
int fat_write(struct fat *f, uint64_t id, uint64_t offset, const void *data, uint64_t size);
int fat_truncate(struct fat *f, uint64_t id);
int fat_remove(struct fat *f, const char *path);
int fat_rename(struct fat *f, const char *from, const char *to);
int fat_chmod(struct fat *f, uint64_t id, unsigned mode);
int fat_set_time(struct fat *f, uint64_t id, uint64_t modified);     /* (63m) */
int fat_commit(struct fat *f);

/* Read only for good (the system's EFI partition at /boot). */
bool fat_read_only(struct fat *f);
void fat_set_read_only(struct fat *f);

#endif

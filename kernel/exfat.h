/*
 * exFAT (kernel/exfat.c, roadmap item 9): the file system of large sticks
 * and memory cards, shared with Windows, macOS and Linux. Read (46a) and
 * written (46b); names are found without regard to case through the
 * volume's own up-case table, as the other systems do. exFAT has no
 * owners: everything is root's, and everyone may read and write (0777
 * directories, 0666 files; without write bits where the read-only
 * attribute is set), as on FAT (the user's choice).
 *
 * Objects for struct vfs: the root directory is ID 1; every other file or
 * directory is named by where its file entry lies (its byte position on
 * the device / 32 + 2), with bit 62 set when the directory it is in is
 * one contiguous run of clusters (its entry set may go on into the next
 * cluster, found without the FAT then). Its inode keeps the first cluster
 * in root.address, EXFAT_CONTIGUOUS in flags when the object's own
 * clusters are one run, and its valid data length and data length in
 * inline_data (see exfat.c). Times are UTC (exFAT stores the zone).
 */
#ifndef RITTEROS_EXFAT_H
#define RITTEROS_EXFAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "blockdev.h"
#include "rfs_format.h"

struct exfat;
struct vfs;

#define EXFAT_ROOT_ID       1

/* Mount the exFAT on dev (its boot region checked, the up-case table and
 * the root directory read); logs what it found. 0 if dev holds no exFAT,
 * or a damaged one (logged). */
struct exfat *exfat_mount(struct block_device *dev);

struct vfs *exfat_vfs(struct exfat *x);
struct block_device *exfat_device(struct exfat *x);
void exfat_label(struct exfat *x, char *out, size_t size);
bool exfat_read_only(struct exfat *x);
void exfat_set_read_only(struct exfat *x);

bool exfat_get_inode(struct exfat *x, uint64_t id, struct rfs_inode *out);
bool exfat_lookup(struct exfat *x, const char *path, uint64_t *id, struct rfs_inode *inode);
int64_t exfat_read(struct exfat *x, uint64_t id, const struct rfs_inode *inode, uint64_t offset,
                   void *buf, uint64_t size);
int exfat_dir_next(struct exfat *x, uint64_t dir_id, const struct rfs_inode *dir, uint64_t *pos,
                   char *name, uint8_t *name_len, uint64_t *id, uint8_t *type);

/* Changes, as vfs.h describes them (RFS_OK or RFS_ERR_*). Data goes to the
 * device at once, the FAT and the bitmap at the latest with exfat_commit;
 * renaming gives an object a new ID (its entry set moves). chmod keeps
 * only the owner's w, as the read-only attribute; there are no owners. */
int exfat_create(struct exfat *x, const char *path, int type, uint64_t *id);
int exfat_write(struct exfat *x, uint64_t id, uint64_t offset, const void *data, uint64_t size);
int exfat_truncate(struct exfat *x, uint64_t id);
int exfat_remove(struct exfat *x, const char *path);
int exfat_rename(struct exfat *x, const char *from, const char *to);
int exfat_chmod(struct exfat *x, uint64_t id, unsigned mode);
int exfat_set_time(struct exfat *x, uint64_t id, uint64_t modified);  /* (63m) */
int exfat_commit(struct exfat *x);

#endif

/*
 * File systems behind one interface (kernel/vfs.c): the system calls
 * (process.c) and the mount table work with a struct vfs, whatever file
 * system is behind it. RitterFS (and each snapshot's view of it), FAT and
 * exFAT each embed one. An object is named by an ID (RitterFS: its object ID;
 * FAT: where its directory entry lies) and described by a struct
 * rfs_inode: type, size, times, owner and permissions are the same for
 * all; the rest of it belongs to the file system (FAT keeps the first
 * cluster in root.address).
 *
 * Snapshots, attributes and instant copies are RitterFS's own: callers get
 * the struct ritterfs with vfs_ritterfs() (0 for others).
 */
#ifndef RITTEROS_VFS_H
#define RITTEROS_VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "blockdev.h"
#include "rfs_format.h"

enum vfs_kind { VFS_RITTERFS = 1, VFS_FAT, VFS_EXFAT };

struct vfs {
    enum vfs_kind kind;
};

struct ritterfs;
struct fat;
struct exfat;

/* The RitterFS (or FAT) behind v, or 0 if it is another kind. */
struct ritterfs *vfs_ritterfs(struct vfs *v);
struct fat *vfs_fat(struct vfs *v);
struct exfat *vfs_exfat(struct vfs *v);

/* Mount what dev holds: RitterFS, FAT or exFAT (0 if none of them). A RitterFS is
 * checked first (read only if the check finds problems). */
struct vfs *vfs_mount(struct block_device *dev);

/* "RitterFS", "FAT32", "FAT16", "FAT12", "exFAT". */
const char *vfs_type(struct vfs *v);
struct block_device *vfs_device(struct vfs *v);
/* Changes are refused: a snapshot's view, a RitterFS whose check or commit
 * failed, the system's EFI partition (/boot). */
bool vfs_read_only(struct vfs *v);
void vfs_set_read_only(struct vfs *v);
/* A snapshot's view (read only, and its own kind of read only: EROFS). */
bool vfs_is_view(struct vfs *v);
void vfs_label(struct vfs *v, char *out, size_t size);
/* RitterFS: the current generation; 0 for others. */
uint64_t vfs_generation(struct vfs *v);

/* Reading. */
bool vfs_get_inode(struct vfs *v, uint64_t id, struct rfs_inode *out);
bool vfs_lookup(struct vfs *v, const char *path, uint64_t *id, struct rfs_inode *inode);
int64_t vfs_read(struct vfs *v, uint64_t id, const struct rfs_inode *inode, uint64_t offset,
                 void *buf, uint64_t size);

/* (65c-3) The same, with the file's inode as it is at the read (inode is
 * set): for reading an open file other threads may be changing. */
int64_t vfs_read_now(struct vfs *v, uint64_t id, struct rfs_inode *inode, uint64_t offset,
                     void *buf, uint64_t size);
/* One directory entry at a time from *pos (see rfs_dir_next): 1, 0 at the
 * end, -1 if the directory cannot be read. */
int vfs_dir_next(struct vfs *v, uint64_t dir_id, const struct rfs_inode *dir, uint64_t *pos,
                 char *name, uint8_t *name_len, uint64_t *id, uint8_t *type);

/* Changing (RFS_OK or RFS_ERR_*; RFS_ERR_READ_ONLY where it cannot). On
 * FAT, chown does nothing (no owners) and chmod keeps the owner's w as the
 * read-only attribute; a rename gives the object a new ID. */
int vfs_create(struct vfs *v, const char *path, int type, uint64_t *id);
int vfs_write(struct vfs *v, uint64_t id, uint64_t offset, const void *data, uint64_t size);
/* (65e-36) At the file's end (O_APPEND), *at: where it went. RitterFS finds
 * the end and writes in one hold of its lock; FAT and exFAT in two. */
int vfs_append(struct vfs *v, uint64_t id, const void *data, uint64_t size, uint64_t *at);
int vfs_truncate(struct vfs *v, uint64_t id);
int vfs_remove(struct vfs *v, const char *path);
int vfs_rename(struct vfs *v, const char *from, const char *to);
/* (57a) A symbolic link at path naming target; a hard link to (a new name for
 * the object at from). RFS_ERR_NOT_SUPPORTED on FAT and exFAT. */
int vfs_symlink(struct vfs *v, const char *path, const char *target, size_t length, uint64_t *id);
int vfs_link(struct vfs *v, const char *from, const char *to);
int vfs_chmod(struct vfs *v, uint64_t id, unsigned mode);
int vfs_chown(struct vfs *v, uint64_t id, uint32_t uid, uint32_t gid);
/* (63m) When it was last modified (nanoseconds since 1970): RitterFS to
 * the nanosecond, exFAT in 10 ms, FAT in 2-second steps. */
int vfs_set_time(struct vfs *v, uint64_t id, uint64_t modified);
int vfs_commit(struct vfs *v);
void vfs_abort(struct vfs *v);

#endif

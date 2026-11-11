/*
 * RitterFS, the RitterOS file system. The on-disk format is specified in
 * docs/ritterfs.md; scripts/ritterfs.py implements the same format on the host.
 *
 * Changes (create, write, remove) are collected in memory and become durable
 * with ritterfs_commit(); reads always see the latest, uncommitted state.
 * Reading itself is shared with the bootloader (common/rfs_read.h); the
 * functions here add the kernel's pending changes to it.
 */
#ifndef RITTEROS_RITTERFS_H
#define RITTEROS_RITTERFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "blockdev.h"
#include "rfs_cache.h"
#include "rfs_read.h"
#include "sync.h"
#include "vfs.h"

struct rfs_txn;                     /* uncommitted changes, see rfs_write.c */

struct ritterfs {
    /* vol.sb: the current state, on disk as of the last commit (vol.slot);
     * vol.bad_copies: damaged copies seen while reading. */
    struct rfs_volume vol;
    struct vfs vfs;                 /* what the system calls see (vfs.c) */
    struct block_device *dev;
    uint32_t sectors_per_block;
    struct rfs_txn *txn;            /* 0 while nothing has changed */
    bool read_only;                 /* set when a commit failed */
    /* (65e-36) Every public function takes it: readers (rfs_read_lock) many
     * at once, writers (rfs_lock) alone; measured. */
    struct rwlock lock;
    struct mutex index_lock;        /* (65e-36) "ritterfs index": the directory index and
                                     * the snapshots' views */
    const char *held_at;            /* (65e) where the outermost hold began (rfs_lock), or the
                                     * last reader in */
    uint64_t held_since;            /* arch_counter */
    struct rfs_cache cache;         /* blocks read through the shared reader */
    uint64_t discarded;             /* blocks reported free to the disk (TRIM) */
    struct rfs_dirindex *dirindex;  /* large directories' indexes (rfs_dirindex.c) */
    bool view;                      /* a read-only view of a snapshot */
    struct rfs_snapshots *snapshots;    /* views of snapshots (rfs_snapshot.c) */
    uint64_t next_free_id;          /* where the search for a free object ID starts */
    /* The last commit's superblock and next_free_id: ritterfs_abort goes
     * back to them. */
    struct rfs_superblock committed;
    uint64_t committed_next_free_id;
    uint64_t busy_object;           /* (35a) a fixed file in use (the swap file): not removed */
};

/* (65e) The lock, measured: where it was taken (the function), how often,
 * how long it was held (in all, at most) and how long others waited for it
 * there; `cpus` lists it (rfs_hold_report). */
void rfs_lock_at(struct ritterfs *fs, const char *site);
void rfs_unlock_at(struct ritterfs *fs);
void rfs_hold_report(void);
void rfs_commit_report(void);         /* (65e) a commit's phases (rfs_write.c) */
#define rfs_lock(fs) rfs_lock_at((fs), __func__)
#define rfs_unlock(fs) rfs_unlock_at(fs)
/* (65e-36) A hold to read: the time it began (0 when nested: not counted),
 * for the unlock. */
uint64_t rfs_read_lock_at(struct ritterfs *fs, const char *site);
void rfs_read_unlock_at(struct ritterfs *fs, const char *site, uint64_t since);
#define rfs_read_lock(fs) rfs_read_lock_at((fs), __func__)
#define rfs_read_unlock(fs, since) rfs_read_unlock_at((fs), __func__, (since))

/* --- mounting and reading ----------------------------------------------------- */

/* Mount the file system on dev: pick the newest valid superblock. Returns 0
 * if dev holds no valid RitterFS (the reason is logged). */
struct ritterfs *ritterfs_mount(struct block_device *dev);

/* Reading: the shared reader (common/rfs_read.c) under the file system's lock
 * (65e-36: held to read, by many threads at once). */
static inline bool ritterfs_get_inode(struct ritterfs *fs, uint64_t object_id,
                                      struct rfs_inode *out)
{
    uint64_t hold = rfs_read_lock(fs);
    bool ok = rfs_get_inode(&fs->vol, object_id, out);
    rfs_read_unlock(fs, hold);
    return ok;
}

static inline bool ritterfs_read_block(struct ritterfs *fs, uint64_t object_id,
                                       const struct rfs_inode *inode, uint64_t index, void *buf)
{
    uint64_t hold = rfs_read_lock(fs);
    bool ok = rfs_read_block(&fs->vol, object_id, inode, index, buf);
    rfs_read_unlock(fs, hold);
    return ok;
}

static inline int64_t ritterfs_read(struct ritterfs *fs, uint64_t object_id,
                                    const struct rfs_inode *inode, uint64_t offset, void *buf,
                                    uint64_t size)
{
    uint64_t hold = rfs_read_lock(fs);
    int64_t n = rfs_read(&fs->vol, object_id, inode, offset, buf, size);
    rfs_read_unlock(fs, hold);
    return n;
}

/* (65c-3) The object's inode as it is now, and a read with it, under one
 * hold of the lock: another thread's commit in between (its close) moves
 * the file's uncommitted data to its blocks, and a read with the inode from
 * before found nothing there (zeros), seen on several cores. 0 at the end
 * or if the object is gone. */
static inline int64_t ritterfs_read_now(struct ritterfs *fs, uint64_t object_id,
                                        struct rfs_inode *inode, uint64_t offset, void *buf,
                                        uint64_t size)
{
    uint64_t hold = rfs_read_lock(fs);
    int64_t n = rfs_get_inode(&fs->vol, object_id, inode) && inode->type != RFS_TYPE_FREE
                    ? rfs_read(&fs->vol, object_id, inode, offset, buf, size) : 0;
    rfs_read_unlock(fs, hold);
    return n;
}

/* The callback runs with the lock held; it may call these functions again. */
static inline bool ritterfs_iterate_dir(struct ritterfs *fs, uint64_t dir_id,
                                        const struct rfs_inode *dir, rfs_dir_callback cb,
                                        void *ctx)
{
    uint64_t hold = rfs_read_lock(fs);
    bool ok = rfs_iterate_dir(&fs->vol, dir_id, dir, cb, ctx);
    rfs_read_unlock(fs, hold);
    return ok;
}

static inline int ritterfs_dir_next(struct ritterfs *fs, uint64_t dir_id,
                                    const struct rfs_inode *dir, uint64_t *pos, char *name,
                                    uint8_t *name_len, uint64_t *object_id, uint8_t *type)
{
    uint64_t hold = rfs_read_lock(fs);
    int r = rfs_dir_next(&fs->vol, dir_id, dir, pos, name, name_len, object_id, type);
    rfs_read_unlock(fs, hold);
    return r;
}

static inline bool ritterfs_lookup(struct ritterfs *fs, const char *path, uint64_t *object_id,
                                   struct rfs_inode *inode)
{
    uint64_t hold = rfs_read_lock(fs);
    bool ok = rfs_lookup(&fs->vol, path, object_id, inode);
    rfs_read_unlock(fs, hold);
    return ok;
}

/* Full consistency check of the committed state, like scripts/ritterfs.py
 * check: every tree, both copies of all metadata, block ownership, the
 * bitmap, directory entries and link counts. Logs a summary. */
bool ritterfs_check(struct ritterfs *fs);

/* --- snapshots (rfs_write.c, rfs_snapshot.c) ----------------------------------- */

/* Take a snapshot of the committed state (pending changes are committed
 * first) under name (1-63 bytes, no '/'). From then on, blocks it sees are
 * not freed by changes. */
int ritterfs_snapshot_create(struct ritterfs *fs, const char *name);

/* Delete a snapshot; a sweep then frees the blocks nothing needs any more
 * (*freed: how many). */
int ritterfs_snapshot_delete(struct ritterfs *fs, const char *name, uint64_t *freed);

/* Free the blocks held back (for snapshots or copies) that nothing reaches
 * any more, and lower the pinned generation to what is still shared. A
 * commit sweeps by itself once enough frees were held back. */
int ritterfs_sweep(struct ritterfs *fs, uint64_t *freed);

/* Copy a file or a directory tree (absolute paths; `to` must not exist and
 * not lie inside `from`) without copying data: the copy shares the
 * committed blocks, which stay pinned while both use them. One commit; on
 * an error nothing is changed. */
int ritterfs_copy(struct ritterfs *fs, const char *from, const char *to);

/* The same with the source in another state of this file system (a
 * snapshot's view): restoring an old version shares its blocks too. With
 * an owner, every copied object belongs to it (0: the source's owners). */
struct rfs_owner {
    uint32_t uid, gid;
};
int ritterfs_copy_from(struct ritterfs *fs, struct ritterfs *source, const char *from,
                       const char *to, const struct rfs_owner *owner);

/* Each snapshot: name, generation, time; callback returns false to stop. */
typedef bool (*ritterfs_snapshot_callback)(void *ctx, const char *name, uint64_t generation,
                                           uint64_t created);
bool ritterfs_snapshot_list(struct ritterfs *fs, ritterfs_snapshot_callback cb, void *ctx);

/* A read-only view of snapshot `name` (kept for later calls), or 0. */
struct ritterfs *ritterfs_snapshot_view(struct ritterfs *fs, const char *name);

/* A read-only view with this object table at this generation. */
struct ritterfs *ritterfs_view(struct ritterfs *fs, const struct rfs_inode *table,
                               uint64_t generation);

/* Internal: mark every block reachable from the live state and every
 * snapshot in map (a bitmap like the free-space bitmap) and give the
 * generation that must stay pinned (*pin: the newest snapshot's, or the
 * newest birth of a block the live state uses twice); forget a deleted
 * snapshot's view. */
bool rfs_reachable(struct ritterfs *fs, uint8_t *map, uint64_t *pin);
void rfs_snapshot_forget(struct ritterfs *fs, const char *name);

/* --- attributes (rfs_attr.c, rfs_write.c) ------------------------------------- */

/* Each attribute of object (0: of every object); the callback returns false
 * to stop. The key is not NUL-terminated. Returns false on a read error. */
typedef bool (*ritterfs_attr_callback)(void *ctx, uint64_t object, const char *key,
                                       size_t key_length, const uint8_t *value,
                                       size_t value_length);
bool ritterfs_attr_each(struct ritterfs *fs, uint64_t object, ritterfs_attr_callback cb,
                        void *ctx);

/* One attribute's value into buf (at most size bytes); returns its full
 * length, or RFS_ERR_NOT_FOUND / RFS_ERR_IO. */
int64_t ritterfs_attr_get(struct ritterfs *fs, uint64_t object, const char *key, void *buf,
                          uint64_t size);

/* Set (create or replace) or remove an attribute of a file or directory;
 * keys are 1-63 bytes without '=', values up to 1024 bytes. Committed by
 * the next ritterfs_commit, like every change. */
int ritterfs_attr_set(struct ritterfs *fs, uint64_t object, const char *key, const void *value,
                      uint64_t length);
int ritterfs_attr_remove(struct ritterfs *fs, uint64_t object, const char *key);

/* Internal: a valid key; a table object's whole content (a 1-byte buffer
 * and size 0 for id 0); the attribute record at *at, if well-formed. */
bool rfs_attr_key_valid(const char *key, size_t len);
uint8_t *rfs_table_content(struct ritterfs *fs, uint64_t id, struct rfs_inode *ino,
                           uint64_t *size);
bool rfs_attr_next(const uint8_t *table, uint64_t size, uint64_t *at,
                   struct rfs_attr_record *r);

/* Log the cache's counters: block reads, how many came from the cache. */
void ritterfs_cache_report(struct ritterfs *fs);

/* --- changing (rfs_write.c) ------------------------------------------------------ */

/* Errors returned by the functions below (0 = success). */
enum {
    RFS_OK = 0,
    RFS_ERR_NOT_FOUND = -1,     /* a path component does not exist */
    RFS_ERR_EXISTS = -2,        /* the name is already taken */
    RFS_ERR_NOT_DIR = -3,       /* a path component is not a directory */
    RFS_ERR_IS_DIR = -4,        /* a directory where a file was expected */
    RFS_ERR_NOT_EMPTY = -5,     /* removing a directory that still has entries */
    RFS_ERR_BAD_NAME = -6,
    RFS_ERR_NO_SPACE = -7,
    RFS_ERR_IO = -8,
    RFS_ERR_READ_ONLY = -9,
    RFS_ERR_NOT_SUPPORTED = -10,    /* (57a) links on a file system without them (FAT) */
    RFS_ERR_BUSY = -11,             /* (35a) a fixed file: never written; not removed in use */
};

/* Create an empty file or directory (type RFS_TYPE_FILE or RFS_TYPE_DIR);
 * the parent directory must exist. */
int ritterfs_create(struct ritterfs *fs, const char *path, int type, uint64_t *object_id);

/* Write size bytes at offset of a file, extending it if needed. */
int ritterfs_write(struct ritterfs *fs, uint64_t object_id, uint64_t offset, const void *data,
                   uint64_t size);

/* (65e-36) Write size bytes at the file's end, found in the same hold of
 * the lock (O_APPEND: two appenders never write at one place); *at: where. */
int ritterfs_append(struct ritterfs *fs, uint64_t object_id, const void *data, uint64_t size,
                    uint64_t *at);

/* Remove a file, or an empty directory. */
int ritterfs_remove(struct ritterfs *fs, const char *path);

/* (57a) A symbolic link at path naming target (1-255 bytes, no NUL), and a
 * hard link: a second name, to, for the object at from (not a directory:
 * RFS_ERR_IS_DIR); its link count goes up by one. */
int ritterfs_symlink(struct ritterfs *fs, const char *path, const char *target, size_t length,
                     uint64_t *object_id);
int ritterfs_link(struct ritterfs *fs, const char *from, const char *to);

/* Permissions (rwxrwxrwx bits) and owner of a file or directory. */
int ritterfs_chmod(struct ritterfs *fs, uint64_t object_id, unsigned mode);
int ritterfs_set_time(struct ritterfs *fs, uint64_t object_id, uint64_t modified);  /* (63m) */
int ritterfs_chown(struct ritterfs *fs, uint64_t object_id, uint32_t uid, uint32_t gid);

/* (35a) Make a file fixed (RFS_FLAG_FIXED, committed at once): a file of
 * whole blocks without holes, none of them shared with a snapshot or a copy
 * (born after the pinned generation). From then on its blocks are written in
 * place by their owner (swap.c) and never through the file system: writes
 * and truncation are refused (RFS_ERR_BUSY). Already fixed: RFS_OK. */
int ritterfs_make_fixed(struct ritterfs *fs, uint64_t object_id);

/* (35a) A fixed file's data blocks in order, as runs of consecutive blocks:
 * run(ctx, first block of the content, its disk block, count, step) for
 * each, content block index + k at disk block block + k * step, step 1 or
 * -1 (a commit places a file's new blocks from the last one down); false
 * (and it stops) if it returns false or a block cannot be read. */
typedef bool (*ritterfs_run_callback)(void *ctx, uint64_t index, uint64_t block, uint64_t count,
                                      int step);
bool ritterfs_fixed_runs(struct ritterfs *fs, uint64_t object_id, ritterfs_run_callback run,
                         void *ctx);

/* Cut a file to 0 bytes (its old blocks are freed by the commit). */
int ritterfs_truncate(struct ritterfs *fs, uint64_t object_id);

/* Move or rename a file or directory (to must not exist; a directory not
 * into itself): only directory entries change. */
int ritterfs_rename(struct ritterfs *fs, const char *from, const char *to);

/* Make all changes durable (see docs/ritterfs.md, "Commits"). */
int ritterfs_commit(struct ritterfs *fs);

/* (68j) The device grew (devices.c raises a mounted partition's block
 * count): the file system takes its new blocks in one commit (its block
 * count and the bitmap longer, the new blocks free). RFS_OK with *blocks
 * the count now: unchanged when the device has no new room. */
int ritterfs_grow(struct ritterfs *fs, uint64_t *blocks);

/* Throw away every change since the last commit (a transaction that is
 * not wanted after all): the file system is as the last commit left it. */
void ritterfs_abort(struct ritterfs *fs);

const char *ritterfs_error(int error);

/* --- scrub and repair (rfs_scrub.c) ------------------------------------------- */

/* Verify both copies of every metadata block and every data block. A damaged
 * metadata copy is rewritten from the good one (in place, verified by reading
 * it back; if that fails, or with force_relocate, both copies move to new
 * places through a commit). Damaged file data, stored once, is reported with
 * its path. Returns the number of problems that could not be repaired. */
int ritterfs_scrub(struct ritterfs *fs, bool force_relocate);

/* Internal, shared by ritterfs.c and rfs_write.c. */
const uint8_t *rfs_dirty_block(struct ritterfs *fs, uint64_t object_id, uint64_t index);
bool rfs_has_changes(struct ritterfs *fs);
bool rfs_read_raw(struct ritterfs *fs, uint64_t block, void *buf);
bool rfs_write_raw(struct ritterfs *fs, uint64_t block, const void *data);
int rfs_mark_dirty(struct ritterfs *fs, uint64_t object_id, uint64_t index);

#endif

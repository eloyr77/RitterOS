/*
 * Reading RitterFS: the part of the file system that the bootloader and the
 * kernel share. It chooses the newest valid superblock, walks content trees
 * (verifying the checksum of every block and falling back to the second copy
 * of metadata), reads files and directories and looks up paths. Writing,
 * checking and scrubbing are kernel-only (kernel/ritterfs.h).
 *
 * The user of this code provides the block reads (struct rfs_volume) and
 * memory for 4 KiB buffers (rfs_alloc_block, rfs_free_block): the kernel
 * from its heap, the bootloader from the UEFI firmware.
 */
#ifndef RITTEROS_RFS_READ_H
#define RITTEROS_RFS_READ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rfs_format.h"

struct rfs_volume {
    /* Read 4 KiB block `block` (counted from the partition start) into buf. */
    bool (*read)(struct rfs_volume *vol, uint64_t block, void *buf);
    /* Optional: the uncommitted content of block `index` of an object, or 0.
     * It takes precedence over the disk (the kernel's pending changes). */
    const uint8_t *(*pending)(struct rfs_volume *vol, uint64_t object_id, uint64_t index);
    /* Optional (the kernel's block cache): copy block into buf if it is
     * known to match checksum already, so its CRC need not be computed
     * again; and the note that a block just read matched checksum. */
    bool (*read_verified)(struct rfs_volume *vol, uint64_t block, uint32_t checksum, void *buf);
    void (*verified)(struct rfs_volume *vol, uint64_t block, uint32_t checksum);
    /* Optional (the kernel's directory index): look name up in directory
     * dir_id; 1 and *id if found, 0 if not, -1 to search the entries. */
    int (*find_entry)(struct rfs_volume *vol, uint64_t dir_id, const struct rfs_inode *dir,
                      const char *name, size_t len, uint64_t *id);
    uint64_t capacity;              /* partition size in 4 KiB blocks */
    struct rfs_superblock sb;       /* the current state, chosen by rfs_open */
    int slot;                       /* its slot, RFS_SLOT_A or RFS_SLOT_B (0: none) */
    uint64_t bad_copies;            /* damaged copies seen while reading */
};

/* A 4 KiB buffer and its release; provided by the bootloader and the kernel. */
void *rfs_alloc_block(void);
void rfs_free_block(void *block);

/* Read both superblock slots and choose the valid one with the highest
 * generation. Set read and capacity first. False if neither is valid. */
bool rfs_open(struct rfs_volume *vol);

/* Magic, version, block size, size and checksum of a superblock. */
bool rfs_superblock_valid(const struct rfs_volume *vol, const struct rfs_superblock *sb);

/* Read the block a pointer refers to and verify its checksum; fall back to
 * the second copy if the first is damaged. A zero address reads as zeros. */
bool rfs_read_pointer(struct rfs_volume *vol, const struct rfs_pointer *p, void *buf);

/* Read block `index` of object `object_id` (whose current inode is `inode`)
 * into buf (4096 bytes). Pending changes, holes and inline content are
 * handled; checksums are verified, with fallback to the second copy. */
bool rfs_read_block(struct rfs_volume *vol, uint64_t object_id, const struct rfs_inode *inode,
                    uint64_t index, void *buf);

/* Read an object's current inode. */
bool rfs_get_inode(struct rfs_volume *vol, uint64_t object_id, struct rfs_inode *out);

/* Read up to size bytes at offset; returns the number of bytes read (less at
 * the end of the content), or -1 on an I/O error. */
int64_t rfs_read(struct rfs_volume *vol, uint64_t object_id, const struct rfs_inode *inode,
                 uint64_t offset, void *buf, uint64_t size);

/* Called for each entry of a directory; return false to stop. The name is
 * not NUL-terminated. */
typedef bool (*rfs_dir_callback)(void *ctx, const char *name, size_t name_len,
                                 uint64_t object_id, uint8_t type);

/* Visit the entries of a directory. False if it is malformed or unreadable. */
bool rfs_iterate_dir(struct rfs_volume *vol, uint64_t dir_id, const struct rfs_inode *dir,
                     rfs_dir_callback cb, void *ctx);

/* One entry at a time: the entry at byte *pos of the directory (deleted
 * entries are skipped), *pos moves past it. name gets *name_len bytes (not
 * zero-terminated, at most 255). Returns 1, 0 at the end, -1 if the
 * directory is malformed or unreadable. */
int rfs_dir_next(struct rfs_volume *vol, uint64_t dir_id, const struct rfs_inode *dir,
                 uint64_t *pos, char *name, uint8_t *name_len, uint64_t *object_id,
                 uint8_t *type);

/* Look up a path such as "/test/docs/readme.txt" from the root directory. */
bool rfs_lookup(struct rfs_volume *vol, const char *path, uint64_t *object_id,
                struct rfs_inode *inode);

#endif

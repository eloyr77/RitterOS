/*
 * RitterFS block cache: recently read blocks of one file system stay in RAM,
 * so that the reader does not fetch the same directory, index or inode
 * block from the disk again and again. 2048 blocks (8 MiB), the least
 * recently used one makes room; allocated on first use.
 *
 * It holds blocks as they are on the disk: checksums are still verified on
 * every read, so a damaged block behaves exactly as without the cache. Every
 * write of a block drops its cached copy (rfs_write.c). The check and scrub
 * read the disk itself (read_raw): their job is to test the medium.
 * (65e-36) Its own spinlock guards it ("ritterfs cache"): readers of the
 * file system use it at once. A block is copied out under it, never handed
 * out by pointer (a reader's insertion could replace it meanwhile).
 */
#ifndef RITTEROS_RFS_CACHE_H
#define RITTEROS_RFS_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include "sync.h"

#define RFS_CACHE_BLOCKS 2048

struct rfs_cache {
    struct spinlock lock;           /* (65e-36) guards the rest */
    uint8_t *data;                  /* RFS_CACHE_BLOCKS blocks, 0 until first use */
    uint64_t *block;                /* the disk block in each slot */
    uint64_t *last_use;             /* for choosing what to drop */
    uint32_t *checksum;             /* what each slot's block was verified against */
    bool *verified;
    int32_t *next;                  /* hash chains, -1 ends one */
    int32_t *bucket;
    uint32_t used;                  /* slots filled so far */
    uint64_t clock;
    uint64_t hits, misses, dropped;
};

/* The cached copy of block into buf (4 KiB); false if it is not there. */
bool rfs_cache_get(struct rfs_cache *c, uint64_t block, void *buf);

/* Keep a copy of block (just read from the disk). Does nothing if there is
 * no memory for the cache. */
void rfs_cache_put(struct rfs_cache *c, uint64_t block, const void *data);

/* The cached copy of block into buf if it was verified against checksum
 * before (its CRC need not be computed again); false if not. */
bool rfs_cache_get_verified(struct rfs_cache *c, uint64_t block, uint32_t checksum, void *buf);

/* The cached block matched checksum. */
void rfs_cache_verified(struct rfs_cache *c, uint64_t block, uint32_t checksum);

/* Forget block: it is about to be written. */
void rfs_cache_forget(struct rfs_cache *c, uint64_t block);

#endif

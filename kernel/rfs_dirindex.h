/*
 * Directory index: for directories larger than one block, a hash table
 * name -> object in RAM, built on the first lookup, so that finding a name
 * no longer means comparing it with every entry. Up to 8 directories per
 * file system; the least recently used one makes room. Any change to a
 * directory's content drops its index (rfs_write.c); the next lookup builds
 * it again from the entries as they are then. Under the file system's lock.
 */
#ifndef RITTEROS_RFS_DIRINDEX_H
#define RITTEROS_RFS_DIRINDEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rfs_read.h"

struct ritterfs;

/* The struct rfs_volume find_entry callback (common/rfs_read.h). */
int rfs_dirindex_find(struct rfs_volume *vol, uint64_t dir_id, const struct rfs_inode *dir,
                      const char *name, size_t len, uint64_t *id);

/* The directory's content changes: forget its index. */
void rfs_dirindex_forget(struct ritterfs *fs, uint64_t dir_id);

/* How many lookups the indexes answered, and how many were built. */
void rfs_dirindex_counts(struct ritterfs *fs, uint64_t *lookups, uint64_t *builds);

#endif

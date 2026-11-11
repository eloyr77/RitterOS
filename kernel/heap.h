/*
 * Kernel heap: memory blocks of any size.
 *
 * Requests up to 1024 bytes are rounded up to a size class (16, 32, ...,
 * 1024) and served from "slabs": 4 KiB pages divided into equal slots, with
 * a header at the start of the page. Larger requests get whole pages. All
 * blocks are 16-byte aligned and reached through the direct map. (65e)
 * Each core keeps up to 32 free blocks of each class for itself, taken
 * from and given back to the shared slabs 16 at a time.
 */
#ifndef RITTEROS_HEAP_H
#define RITTEROS_HEAP_H

#include <stddef.h>
#include <stdint.h>

/* Allocate size bytes; returns 0 if size is 0 or memory is exhausted. */
void *kmalloc(size_t size);

/* Like kmalloc, but the block is filled with zeros. */
void *kzalloc(size_t size);

/* Free a block from kmalloc. Freeing 0 does nothing; freeing anything else
 * that is not a live heap block is a kernel bug and panics. */
void kfree(void *ptr);

/* Return completely free slabs that are kept for reuse to the page allocator
 * (every core's kept blocks first: `mem` and the leak checks count pages). */
void heap_trim(void);

/* (65e) This core's kept blocks back to the slabs (cpus.c asks every core). */
void heap_cache_flush(void);

/* Allocate, fill, verify and free 2000 blocks of random sizes (from seed),
 * then check that every page went back to the page allocator. */
void heap_self_test(uint64_t seed);

#endif

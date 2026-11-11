/*
 * Memory mappings (SYS_MMAP, step 22a): a program's anonymous memory and its
 * mappings of files, in the mmap window of its address space. A read-only
 * mapping of a RitterFS file uses the page cache: one physical copy of each
 * page of one version of the file, shared by every program that maps it
 * (the base for shared libraries). Private writable mappings and anonymous
 * memory are the program's own pages. Since 22e anonymous pages are made
 * when first touched and a private writable page is copied at its first
 * write (mmap_fault, from the page fault and from copy_user); a file's pages
 * are still read when it is mapped (a version rewritten later may have its
 * blocks reused).
 */
#ifndef RITTEROS_MMAP_H
#define RITTEROS_MMAP_H

#include <stdbool.h>
#include <stdint.h>
#include "sync.h"

struct vfs;
struct mapping;

/* A process's mappings: a list sorted by address, under its own lock. (65e-38)
 * A readers-writer lock: page faults that only fill an empty entry hold it
 * to read, so a program's threads fault at once; every change of the list
 * or of a page in place holds it to write. */
struct mmap_state {
    struct mapping *maps;
    struct rwlock lock;
    uint64_t start;                 /* where mappings are placed from: random (22l) */
    /* (65e) copy_user's copies of pages already in place run without the
     * lock (mmap_copy_begin); whoever changes the tables stops them first
     * (mmap_change_begin, and every lock taken in mmap.c). */
    int copying;                    /* such copies under way */
    int changing;                   /* changes under way: no new copies */
    /* (65e-49) It forked, or is a fork's child: only then may it have pages
     * another process holds too (set under the lock to write, by fork). */
    bool forked;
    /* (35b) Its program is loaded (the kernel no longer fills its pages
     * through the direct map): pageout may write its pages to swap; from
     * pageout_at on next time (a clock over its pages). */
    bool pageable;
    uint64_t pageout_at;
};

void mmap_init(struct mmap_state *m);

/* (65e) For copy_user, interrupts off from begin to end: true if a page in
 * place may be copied now without the lock (looked at after begin: it
 * cannot change before end). end in either case. */
bool mmap_copy_begin(struct mmap_state *m);
void mmap_copy_end(struct mmap_state *m);

/* (65e) The lock, with the copies above stopped and waited for: around a
 * change of the program's pages outside mmap.c (a shared buffer's). */
void mmap_change_begin(struct mmap_state *m);
void mmap_change_end(struct mmap_state *m);

/* (65e-38) The lock to read: no page in place changes or goes until the
 * end (copy_user copies such a page under it). */
void mmap_read_begin(struct mmap_state *m);
void mmap_read_end(struct mmap_state *m);

/* What to map from a file (0 for anonymous memory). */
struct mmap_file {
    struct vfs *fs;
    uint64_t object_id;
    bool readable;
    bool writable;                  /* open for writing: MAP_SHARED may write it (22j) */
};

/* Map length bytes (MMAP_* access and flags, common/syscalls.h) into space:
 * the address, or -ERR_*. A MAP_SHARED mapping of a RitterFS file open for
 * writing may be writable (22j): the cache's pages, written back by
 * mmap_sync. */
int64_t mmap_map(struct mmap_state *m, uint64_t space, uint64_t addr, uint64_t length,
                 unsigned access, unsigned flags, const struct mmap_file *file, uint64_t offset);

/* A program touched addr (write: to write it, exec: to run it) and it was not
 * there with that access (22e): make the page -- anonymous memory's first
 * touch, a copy-on-write page's first write -- and return 0, so the program
 * goes on; else the signal that ends it (SIGSEGV, or SIGBUS past the end of
 * a mapped file). Sleeps (the mappings' lock): call with interrupts on. */
int mmap_fault(struct mmap_state *m, uint64_t space, uint64_t addr, bool write, bool exec);

/* Give the mappings in [addr, addr + length) the MMAP_* access (splitting
 * them at its ends; all of the range must be mapped, else -ERR_NOMEM): 0 or
 * -ERR_*. Never writable and executable (-ERR_NOTSUP); a shared file mapping
 * never writable (-ERR_NOTSUP, as at mmap); a file mapping made PROT_NONE
 * cannot be given access later (-ERR_NOTSUP). Pages keep their contents. */
int64_t mmap_protect(struct mmap_state *m, uint64_t space, uint64_t addr, uint64_t length,
                     unsigned access);

/* Write back to their files the pages written through the writable shared
 * mappings in [addr, addr + length) (msync, 22j; munmap and the process's end
 * do it too): 0 or -ERR_*. Takes the file systems' locks: not under any. */
int64_t mmap_sync(struct mmap_state *m, uint64_t addr, uint64_t length);

/* Remove the mappings in [addr, addr + length) (parts of them too); 0 or -ERR_*. */
int64_t mmap_unmap(struct mmap_state *m, uint64_t space, uint64_t addr, uint64_t length);

/* For the kernel's loader (22g): map a program's segment at vaddr -- the
 * page cache's pages of file from offset, private (copy-on-write when
 * writable), or anonymous memory for file 0 -- anywhere a program may be. */
int64_t mmap_map_segment(struct mmap_state *m, uint64_t space, uint64_t vaddr, uint64_t length,
                         unsigned access, const struct mmap_file *file, uint64_t offset);

/* Free every cached file version no mapping uses (the `mem` report, so its
 * count is exact; 22g). */
void mmap_cache_drop_idle(void);

/* One line: the files in the page cache, their pages (and how many of them
 * no mapping uses), and the pages read from files so far (REPORT_MEMORY). */
void mmap_print_cache(void);

/* fork (56a): give child (fresh, its space empty) a copy of m's mappings in
 * child_space: the cache's pages as they are, the process's own held by both
 * (copied at the first write), anonymous MAP_SHARED memory one for both.
 * 0, or -ERR_NOMEM (what was copied stays in child: release it). */
int64_t mmap_fork(struct mmap_state *m, uint64_t space, struct mmap_state *child,
                  uint64_t child_space);

/* (35b) For the thread pageout: up to want (at most 32) of the process's own
 * pages (anonymous memory, a copy-on-write mapping's copies) not used since
 * the last look written to swap and freed, the others' used bits cleared.
 * (35c) A page forked processes share goes out of this one's entry: written
 * once (the slot is remembered with the page; another holder's entry names
 * it too), freed with its last holder. A page in swap leaves a swap entry,
 * read back at its next fault (into a page of the process's own). How many
 * went; *looked: the pages looked at. */
uint64_t mmap_pageout(struct mmap_state *m, uint64_t space, uint64_t want, uint64_t *looked);

/* (35b) Every page of m in swap read back (swap off): 0, or -ERR_NOMEM /
 * -ERR_IO (what was read stays). */
int64_t mmap_swap_in_all(struct mmap_state *m, uint64_t space);

/* (35c-2) Shared anonymous memory: the pages mmap_pageout found unused,
 * taken out of every process that maps them and written to swap (pageout,
 * holding no mappings' lock): how many went; and every one in swap read
 * back (swap off): 0 or -ERR_*. */
uint64_t mmap_pageout_shared(void);
int64_t mmap_anon_swap_in_all(void);

/* Remove every mapping (the process ends): before its space is destroyed,
 * since shared pages are not the space's to free. */
void mmap_release_all(struct mmap_state *m, uint64_t space);

#endif

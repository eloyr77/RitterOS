/*
 * A user-space heap (malloc/free/calloc/realloc), K&R style: a free list of
 * blocks, sorted by address, coalesced on free. morecore() grows the heap by
 * an arena of 1 MiB of anonymous memory from the kernel (mmap, step 22a;
 * until then it was 16 MiB shared buffers, each also taking a descriptor).
 * Each arena begins with a header of its own, so a block never touches the
 * next arena and coalescing never crosses into it. Since 22h the heap is
 * safe to use from several threads at once (one lock around it) and gives
 * memory back: a request of 128 KiB or more is a mapping of its own, unmapped
 * when freed, and an arena that has become wholly free is unmapped, unless it
 * is the only free one (kept for the next requests, so a program that frees
 * and allocates in turn does not map and unmap at every call).
 *
 * Since 64i small blocks (up to SMALL_UNITS units, 2 KiB with the header)
 * never walk the free list: a freed one goes on a list of its size
 * (`bins`), and a new one comes from there or is cut from the end of a
 * chunk of 64 KiB taken from the arenas (`chunk`). The free list, walked at
 * every malloc and free, had grown to thousands of blocks in iBrowser (a
 * JavaScript engine's many small objects): one timer of www.apple.com took
 * 15 s on the J1900, 0.2 ms on the Mac with the Mac's malloc. Small blocks
 * stay on their lists and are never joined again, so an arena with one in
 * it is not given back; bigger blocks are as before. The lock is a ticket
 * lock: the threads get it in turn (a thread giving it back and taking it
 * again at once had starved the others, which slept a tick between tries).
 */
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "ritteros.h"

typedef union header {              /* blocks are an integer number of these */
    struct {
        union header *next;         /* free: the next free block (by address); in use: 0,
                                     * or BIG for a block that is a mapping of its own */
        size_t units;               /* this block's size, in header units */
    } s;
    long double align;              /* force the widest alignment */
} Header;

#define ARENA_UNITS  ((1024 * 1024) / sizeof(Header))           /* an arena, its header included */
#define BIG_BYTES    (128 * 1024)                               /* from here on: a mapping */
#define BIG          ((Header *)-1)
/* (22p) An aligned block that is a mapping of its own: its header is just
 * before the block, the mapping begins at that header's page, and s.units
 * holds the mapping's length in bytes. */
#define BIG_AT       ((Header *)-2)
#define PAGE         4096
#define SMALL_UNITS  ((2048) / sizeof(Header))                   /* (64i) the largest on a bin */
#define CHUNK_UNITS  ((64 * 1024) / sizeof(Header))              /* small blocks are cut from these */

static Header base;                 /* the empty list's anchor */
static Header *freep;               /* a point into the free list */
static Header *arenas;              /* every arena's header (s.next: the next arena) */
static Header *bins[SMALL_UNITS + 1];   /* (64i) freed small blocks by size (s.next: the next) */
static Header *chunk;               /* what is left of the chunk small blocks are cut from */
static size_t chunk_units;

/*
 * The lock (64i): a ticket lock. A thread takes the next ticket and waits
 * until it is served, sleeping on the futex of `serving` meanwhile (with one
 * processor spinning cannot help: the holder is not running). Giving it back
 * serves the next ticket and wakes the waiters only when there are some, so
 * a lock nobody waits for makes no system call. The turns are fair: a thread
 * that gives the lock back and wants it again at once waits behind the
 * others (until 64i a free-for-all with a tick's sleep between tries let one
 * busy thread keep it for seconds).
 */
static unsigned next_ticket, serving;

static void lock(void)
{
    unsigned mine = __atomic_fetch_add(&next_ticket, 1, __ATOMIC_ACQUIRE);
    for (;;) {
        unsigned now = __atomic_load_n(&serving, __ATOMIC_ACQUIRE);
        if (now == mine)
            return;
        syscall6(SYS_FUTEX, (int64_t)&serving, FUTEX_WAIT, now, 0, 0, 0);    /* (back at once if */
    }                                                                     /* it has moved on) */
}

static void unlock(void)
{
    unsigned next = __atomic_add_fetch(&serving, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&next_ticket, __ATOMIC_ACQUIRE) != next)
        syscall6(SYS_FUTEX, (int64_t)&serving, FUTEX_WAKE, INT_MAX, 0, 0, 0);
}

/* (56a) fork holds the lock across the call, so a child never finds it
 * taken by a thread it does not have. */
void __ritteros_malloc_lock(void)
{
    lock();
}

void __ritteros_malloc_unlock(void)
{
    unlock();
}

/* (64i) A fork's child: it has only the calling thread, so the tickets
 * other threads were waiting with are gone; the lock starts again, free. */
void __ritteros_malloc_child(void)
{
    __atomic_store_n(&next_ticket, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&serving, 0, __ATOMIC_RELEASE);
}

static void release(Header *bp);

/* A new arena from the kernel, put on the free list. Locked. */
static Header *morecore(void)
{
    size_t bytes = ARENA_UNITS * sizeof(Header);
    Header *arena = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena == MAP_FAILED) {
        errno = ENOMEM;
        return 0;
    }
    arena->s.next = arenas;
    arena->s.units = ARENA_UNITS;
    arenas = arena;
    Header *up = arena + 1;
    up->s.units = ARENA_UNITS - 1;
    release(up);                    /* fold the new space into the free list */
    return freep;
}

/* bp, a whole arena's block, is free (on the list after p): unmap the arena
 * if another arena is wholly free too. Locked. */
static void give_back(Header *bp)
{
    Header *spare = 0, *prev = 0;
    for (Header *p = base.s.next; p != &base && !(spare && prev); p = p->s.next) {
        if (p != bp && p->s.units == ARENA_UNITS - 1)
            spare = p;
        if (p->s.next == bp)
            prev = p;
    }
    if (base.s.next == bp)
        prev = &base;
    if (!spare || !prev)
        return;                     /* the only free arena: kept */
    prev->s.next = bp->s.next;
    freep = prev;
    Header *arena = bp - 1;
    for (Header **link = &arenas; *link; link = &(*link)->s.next)
        if (*link == arena) {
            *link = arena->s.next;
            break;
        }
    munmap(arena, ARENA_UNITS * sizeof(Header));
}

/* Put the arena block bp on the free list, joined to its neighbours. Locked. */
static void release(Header *bp)
{
    Header *p;
    for (p = freep; !(bp > p && bp < p->s.next); p = p->s.next)
        if (p >= p->s.next && (bp > p || bp < p->s.next))
            break;                  /* bp is before the start or after the end */
    if (bp + bp->s.units == p->s.next) {        /* join to the upper neighbour */
        bp->s.units += p->s.next->s.units;
        bp->s.next = p->s.next->s.next;
    } else {
        bp->s.next = p->s.next;
    }
    if (p != &base && p + p->s.units == bp) {   /* join to the lower neighbour */
        p->s.units += bp->s.units;
        p->s.next = bp->s.next;
        bp = p;
    } else {
        p->s.next = bp;
    }
    freep = p;
    if (bp->s.units == ARENA_UNITS - 1)         /* a whole arena is free */
        give_back(bp);
}

/* A block of `units` from the free list (0: no memory). Locked. */
static Header *from_list(size_t units)
{
    Header *prevp = freep;
    if (!prevp) {                   /* first call: an empty list */
        base.s.next = freep = prevp = &base;
        base.s.units = 0;
    }
    for (Header *p = prevp->s.next;; prevp = p, p = p->s.next) {
        if (p->s.units >= units) {                      /* big enough */
            if (p->s.units == units) {
                prevp->s.next = p->s.next;              /* exact fit: unlink it */
            } else {
                p->s.units -= units;                   /* split: tail stays free */
                p += p->s.units;
                p->s.units = units;
            }
            p->s.next = 0;                              /* in use (not BIG) */
            freep = prevp;
            return p;
        }
        if (p == freep && (p = morecore()) == 0)        /* wrapped: ask the kernel */
            return 0;
    }
}

/* (64i) A small block: its size's list's first, else cut from the chunk
 * (a new chunk when this one is too short; what was left of the old one
 * goes on the free list). Locked. */
static Header *small(size_t units)
{
    Header *p = bins[units];
    if (p) {
        bins[units] = p->s.next;
        p->s.next = 0;
        return p;
    }
    if (chunk_units < units) {
        if (chunk_units) {
            chunk->s.units = chunk_units;
            release(chunk);
        }
        chunk_units = 0;
        if (!(chunk = from_list(CHUNK_UNITS)))
            return 0;
        chunk_units = CHUNK_UNITS;
    }
    p = chunk;
    chunk += units;
    chunk_units -= units;
    p->s.units = units;
    p->s.next = 0;
    return p;
}

void free(void *ptr)
{
    if (!ptr)
        return;
    Header *bp = (Header *)ptr - 1; /* the block header */
    if (bp->s.next == BIG) {        /* a mapping of its own: back to the kernel */
        munmap(bp, bp->s.units * sizeof(Header));
        return;
    }
    if (bp->s.next == BIG_AT) {
        munmap((void *)((uintptr_t)bp & ~(uintptr_t)(PAGE - 1)), bp->s.units);
        return;
    }
    lock();
    if (bp->s.units <= SMALL_UNITS) {   /* (64i) a small one: on its size's list */
        bp->s.next = bins[bp->s.units];
        bins[bp->s.units] = bp;
    } else {
        release(bp);
    }
    unlock();
}

void *malloc(size_t size)
{
    if (size == 0)
        return 0;
    if (size > (size_t)-1 / 2) {
        errno = ENOMEM;
        return 0;
    }
    size_t units = (size + sizeof(Header) - 1) / sizeof(Header) + 1;     /* + the header */
    if (size >= BIG_BYTES) {
        Header *bp = mmap(0, units * sizeof(Header), PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (bp == MAP_FAILED) {
            errno = ENOMEM;
            return 0;
        }
        bp->s.next = BIG;
        bp->s.units = units;
        return (void *)(bp + 1);
    }
    lock();
    Header *p = units <= SMALL_UNITS ? small(units) : from_list(units);
    unlock();
    return p ? (void *)(p + 1) : 0;
}

void *calloc(size_t count, size_t size)
{
    if (count && size > (size_t)-1 / count) {           /* overflow */
        errno = ENOMEM;
        return 0;
    }
    size_t total = count * size;
    void *p = malloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr)
        return malloc(size);
    if (size == 0) {
        free(ptr);
        return 0;
    }
    Header *bp = (Header *)ptr - 1;
    size_t have = (bp->s.units - 1) * sizeof(Header);   /* usable bytes in the block */
    if (bp->s.next == BIG_AT)                           /* (to its mapping's end) */
        have = ((uintptr_t)bp & ~(uintptr_t)(PAGE - 1)) + bp->s.units - (uintptr_t)ptr;
    if (have >= size)
        return ptr;                                     /* already big enough */
    void *np = malloc(size);
    if (!np)
        return 0;
    memcpy(np, ptr, have);
    free(ptr);
    return np;
}

/* (22p) A block at a multiple of alignment. Small ones are malloc'd with
 * room to spare and the part before the aligned place goes back to the free
 * list as a block of its own; big ones are a mapping whose pages before the
 * block's header are unmapped again. */
void *aligned_alloc(size_t alignment, size_t size)
{
    if (alignment == 0 || (alignment & (alignment - 1)) || size == 0) {
        errno = EINVAL;
        return 0;
    }
    if (alignment <= sizeof(Header))
        return malloc(size);                            /* (every block is) */
    if (size > (size_t)-1 / 4 || alignment > (size_t)-1 / 4) {
        errno = ENOMEM;
        return 0;
    }
    if (size + alignment < BIG_BYTES) {
        char *p = malloc(size + alignment);
        if (!p)
            return 0;
        uintptr_t q = ((uintptr_t)p + alignment - 1) & ~(uintptr_t)(alignment - 1);
        if (q == (uintptr_t)p)
            return p;
        lock();
        Header *bp = (Header *)p - 1, *nb = (Header *)q - 1;
        size_t lead = (size_t)(nb - bp);
        nb->s.units = bp->s.units - lead;
        nb->s.next = 0;
        bp->s.units = lead;
        release(bp);
        unlock();
        return (void *)q;
    }
    size_t span = (size + sizeof(Header) + alignment + PAGE - 1) & ~(size_t)(PAGE - 1);
    char *m = mmap(0, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) {
        errno = ENOMEM;
        return 0;
    }
    uintptr_t q = ((uintptr_t)m + sizeof(Header) + alignment - 1) & ~(uintptr_t)(alignment - 1);
    uintptr_t start = (q - sizeof(Header)) & ~(uintptr_t)(PAGE - 1);
    if (start > (uintptr_t)m)
        munmap(m, start - (uintptr_t)m);
    Header *h = (Header *)q - 1;
    h->s.next = BIG_AT;
    h->s.units = span - (start - (uintptr_t)m);
    return (void *)q;
}

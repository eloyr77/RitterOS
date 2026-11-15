/*
 * A user-space heap (malloc/free/calloc/realloc). morecore() grows the heap
 * by an arena of 1 MiB of anonymous memory from the kernel (mmap, step 22a;
 * until then it was 16 MiB shared buffers, each also taking a descriptor).
 * Each arena begins with a header of its own and ends with a fence (a block
 * always in use), so a block never touches the next arena and joining never
 * crosses into it. Since 22h the heap is safe to use from several threads at
 * once (one lock around it) and gives memory back: a request of 128 KiB or
 * more is a mapping of its own, unmapped when freed, and an arena that has
 * become wholly free is unmapped, unless it is the only free one (kept for
 * the next requests, so a program that frees and allocates in turn does not
 * map and unmap at every call).
 *
 * Since 64i small blocks (up to SMALL_UNITS units, 2 KiB with the header)
 * never walk a list: a freed one goes on a list of its size (`bins`), and a
 * new one comes from there or is cut from the end of a chunk of 64 KiB taken
 * from the arenas (`chunk`). The free list had grown to thousands of blocks
 * in iBrowser (a JavaScript engine's many small objects): one timer of
 * www.apple.com took 15 s on the J1900, 0.2 ms on the Mac with the Mac's
 * malloc. Small blocks stay on their lists and are never joined again, so an
 * arena with one in it is not given back.
 *
 * Since 68o the arenas' blocks (the rest, up to 128 KiB) do not walk one
 * either. Until then they were one list sorted by address, walked by malloc
 * to find a block that fits and by free to find the neighbours to join: the
 * WebKit port's Gmail spent 43% of its main thread there (thousands of free
 * blocks of 2-64 KiB). Now each block's header says whether the block below
 * it is free, and a free block ends with its size (its footer), so free
 * finds both neighbours where they lie; free blocks are on lists by size
 * class (`lists`: 8 classes per power of two, `nonempty` a bit for each),
 * and malloc looks at a few blocks of the request's class, then takes the
 * first of the next class that is not empty (every block there fits). The
 * lock (68r) is a futex mutex that spins first (lock()).
 */
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "ritteros.h"

typedef union header {              /* blocks are an integer number of these */
    struct {
        union header *next;         /* in use: the kind of block (below); a free small
                                     * block: the next on its size's list */
        size_t units;               /* this block's size, in header units */
    } s;
    struct {                        /* (68o) a free arena block's second unit: its class's */
        union header *next, *prev;  /* list; an arena's header: the list of arenas */
    } l;
    long double align;              /* force the widest alignment */
} Header;

/* What s.next says of a block in use, and of a free arena block. */
#define PIECE        ((Header *)0)  /* a small block cut from a chunk */
#define BIG          ((Header *)-1) /* a mapping of its own */
/* (22p) An aligned block that is a mapping of its own: its header is just
 * before the block, the mapping begins at that header's page, and s.units
 * holds the mapping's length in bytes. */
#define BIG_AT       ((Header *)-2)
#define USED         ((Header *)16) /* (68o) an arena block, the block below it in use too */
#define USED_LOW_FREE ((Header *)32)    /* an arena block, the block below it free */
#define FREE         ((Header *)48) /* a free arena block (the one below it is in use: joined) */

#define ARENA_UNITS  ((1024 * 1024) / sizeof(Header))           /* an arena, its header included */
#define WHOLE        (ARENA_UNITS - 2)  /* (68o) an arena's blocks, between its header and fence */
#define MIN_FREE     3                  /* a free arena block: header, links, footer */
#define BIG_BYTES    (128 * 1024)                               /* from here on: a mapping */
#define PAGE         4096
#define SMALL_UNITS  ((2048) / sizeof(Header))                   /* (64i) the largest on a bin */
#define CHUNK_UNITS  ((64 * 1024) / sizeof(Header))              /* small blocks are cut from these */
#define CLASSES      73                 /* (68o) class_of(WHOLE) + 1 */
#define LOOK         8                  /* blocks of a request's own class looked at */

static Header *arenas;              /* every arena's header (l: the list) */
static Header *lists[CLASSES];      /* (68o) free arena blocks by class (l in their second unit) */
static uint64_t nonempty[2];        /* a bit for each class with a block */
static unsigned spare;              /* wholly free arenas (0 or 1: the one kept) */
static Header *bins[SMALL_UNITS + 1];   /* (64i) freed small blocks by size (s.next: the next) */
static Header *chunk_head;          /* the chunk small blocks are cut from (an arena block) */
static Header *chunk;               /* what is left of it, at its end */
static size_t chunk_units;


/*
 * The lock. (68r) A futex mutex as glibc's (Drepper's "Futexes are
 * tricky", mutex 3): `state` 0 free, 1 held, 2 held and maybe sleepers.
 * A waiter looks for SPIN_NS first (since 68o a hold is about 40 ns, and
 * the kernel's mutexes spin up to 20 us too, 65e-10), then marks it 2 and
 * sleeps on the futex; unlock wakes one sleeper, and only when it was 2.
 * Whoever finds it free takes it. Until 68r a ticket lock (64i) whose every
 * waiter slept at once, and whose every unlock with a ticket out woke them
 * all: on the VivoBook four threads took 1,056 ms for what this lock does
 * in 69 ms, and the WebKit port's Gmail spent 7% of its main thread in
 * those futex calls. A ticket lock that spins was measured too: fair turns
 * stall whenever the thread whose turn is next is not running (on the
 * 2-core VM it was slower than not spinning). 64i's starvation came from
 * waiters that slept a whole tick between tries; these sleep on the futex
 * and are woken at once (measured: on the VivoBook eight threads finished
 * within 77 ms of each other in 145).
 */
static int state;

#define SPIN_NS 20000

static void relax(void)
{
#if defined(__x86_64__)
    __asm__ volatile ("pause");
#else
    __asm__ volatile ("yield");
#endif
}

static uint64_t monotonic_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);     /* (68p: no system call) */
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

static void lock(void)
{
    int c = 0;
    if (__atomic_compare_exchange_n(&state, &c, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;
    uint64_t start = monotonic_ns();
    for (unsigned looks = 1;; looks++) {
        c = 0;
        if (__atomic_load_n(&state, __ATOMIC_RELAXED) == 0 &&
            __atomic_compare_exchange_n(&state, &c, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
        relax();
        if (looks % 32 == 0 && monotonic_ns() - start > SPIN_NS)
            break;
    }
    c = __atomic_exchange_n(&state, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {                        /* (the futex returns at once if it is no longer 2) */
        syscall6(SYS_FUTEX, (int64_t)&state, FUTEX_WAIT, 2, 0, 0, 0);
        c = __atomic_exchange_n(&state, 2, __ATOMIC_ACQUIRE);
    }
}

static void unlock(void)
{
    if (__atomic_fetch_sub(&state, 1, __ATOMIC_RELEASE) != 1) {
        __atomic_store_n(&state, 0, __ATOMIC_RELEASE);
        syscall6(SYS_FUTEX, (int64_t)&state, FUTEX_WAKE, 1, 0, 0, 0);
    }
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

/* (64i) A fork's child: it has only the calling thread (no other waits);
 * the lock starts again, free. */
void __ritteros_malloc_child(void)
{
    __atomic_store_n(&state, 0, __ATOMIC_RELEASE);
}


/* (68o) A free arena block's class: 0 for those no request asks for (up to
 * SMALL_UNITS: small requests are cut from chunks; these are only kept to
 * be joined), then 8 classes in each power of two of units, from 128 up. */
static unsigned class_of(size_t units)
{
    if (units <= SMALL_UNITS)
        return 0;
    unsigned top = 63 - (unsigned)__builtin_clzl(units);   /* 7 (128 units) .. 15 */
    return 1 + (top - 7) * 8 + (unsigned)((units >> (top - 3)) & 7);
}

/* Put the free block bp (FREE, its units set) first on its class's list.
 * Locked. */
static void list_add(Header *bp)
{
    unsigned c = class_of(bp->s.units);
    Header *first = lists[c];
    bp[1].l.next = first;
    bp[1].l.prev = 0;
    if (first)
        first[1].l.prev = bp;
    lists[c] = bp;
    nonempty[c / 64] |= (uint64_t)1 << (c % 64);
}

/* Take the free block bp off its list (its units still those it was put on
 * with). Locked. */
static void list_remove(Header *bp)
{
    Header *next = bp[1].l.next, *prev = bp[1].l.prev;
    if (next)
        next[1].l.prev = prev;
    if (prev) {
        prev[1].l.next = next;
    } else {
        unsigned c = class_of(bp->s.units);
        lists[c] = next;
        if (!next)
            nonempty[c / 64] &= ~((uint64_t)1 << (c % 64));
    }
}

/* Make bp a free block of `units` (the block below it in use, the one above
 * it told already) and list it. Locked. */
static void set_free(Header *bp, size_t units)
{
    bp->s.next = FREE;
    bp->s.units = units;
    ((size_t *)(bp + units))[-1] = units;      /* the footer, for the block above */
    list_add(bp);
}

/* A new arena from the kernel: one free block between its header and its
 * fence. Locked. */
static int morecore(void)
{
    Header *arena = mmap(0, ARENA_UNITS * sizeof(Header), PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena == MAP_FAILED) {
        errno = ENOMEM;
        return 0;
    }
    arena->l.next = arenas;
    arena->l.prev = 0;
    if (arenas)
        arenas->l.prev = arena;
    arenas = arena;
    Header *fence = arena + ARENA_UNITS - 1;
    fence->s.next = USED_LOW_FREE;
    fence->s.units = 1;
    set_free(arena + 1, WHOLE);
    spare++;
    return 1;
}

/* bp, a free block, is a whole arena: unmap the arena if another arena is
 * wholly free too. Locked. */
static void whole_free(Header *bp)
{
    if (!spare) {
        spare = 1;                  /* the only free arena: kept */
        return;
    }
    list_remove(bp);
    Header *arena = bp - 1;
    if (arena->l.prev)
        arena->l.prev->l.next = arena->l.next;
    else
        arenas = arena->l.next;
    if (arena->l.next)
        arena->l.next->l.prev = arena->l.prev;
    munmap(arena, ARENA_UNITS * sizeof(Header));
}

/* Free the arena block bp (USED or USED_LOW_FREE), joined to its free
 * neighbours. Locked. */
static void release(Header *bp)
{
    size_t units = bp->s.units;
    if (bp->s.next == USED_LOW_FREE) {          /* join to the lower neighbour */
        size_t low = ((size_t *)bp)[-1];
        bp -= low;
        list_remove(bp);
        units += low;
    }
    Header *up = bp + units;
    if (up->s.next == FREE) {                   /* join to the upper neighbour */
        list_remove(up);
        units += up->s.units;
    } else {
        up->s.next = USED_LOW_FREE;
    }
    set_free(bp, units);
    if (units == WHOLE)
        whole_free(bp);
}

/* The first block of the first class above c with one (all fit). Locked. */
static Header *above(unsigned c)
{
    for (unsigned w = (c + 1) / 64; w < 2; w++) {
        uint64_t bits = nonempty[w];
        if (w == (c + 1) / 64)
            bits &= ~(uint64_t)0 << ((c + 1) % 64);
        if (bits)
            return lists[w * 64 + (unsigned)__builtin_ctzll(bits)];
    }
    return 0;
}

/* An arena block of at least `units` (more than SMALL_UNITS; 0: no
 * memory): one of the first LOOK of its class that fits, else the next
 * class's first, else a new arena's. What is left over stays free above it.
 * Locked. */
static Header *from_list(size_t units)
{
    unsigned c = class_of(units);
    Header *p = lists[c];
    for (int look = 0; p && p->s.units < units; p = p[1].l.next)
        if (++look == LOOK) {
            p = 0;
            break;
        }
    if (!p && !(p = above(c))) {
        if (!morecore())
            return 0;
        p = lists[class_of(WHOLE)];
    }
    list_remove(p);
    if (p->s.units == WHOLE)
        spare--;
    size_t rest = p->s.units - units;
    if (rest >= MIN_FREE) {
        set_free(p + units, rest);              /* (the block above sees a free one below still) */
        p->s.units = units;
    } else {
        (p + p->s.units)->s.next = USED;        /* all of it: the block above, its lower in use */
    }
    p->s.next = USED;                           /* (below a free block, one in use) */
    return p;
}

/* (64i) A small block: its size's list's first, else cut from the chunk
 * (a new chunk when this one is too short; what was left of the old one
 * goes back to its arena). Locked. */
static Header *small(size_t units)
{
    Header *p = bins[units];
    if (p) {
        bins[units] = p->s.next;
        p->s.next = 0;
        return p;
    }
    if (chunk_units < units) {
        if (chunk_units >= MIN_FREE) {          /* (68o) the chunk's end, an arena block freed */
            chunk_head->s.units -= chunk_units;
            chunk->s.next = USED;
            chunk->s.units = chunk_units;
            release(chunk);
        }
        chunk_units = 0;
        if (!(chunk_head = from_list(CHUNK_UNITS)))
            return 0;
        chunk = chunk_head + 1;                 /* (its header stays the arena's block's) */
        chunk_units = chunk_head->s.units - 1;
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
    if (bp->s.next == PIECE) {      /* (64i) a small one: on its size's list */
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
    /* (68o) An arena block's part before the aligned place is freed, so it
     * is no part or at least MIN_FREE units (an alignment of 32 or more
     * moves it on); the block itself is at least that too. */
    if (size < MIN_FREE * sizeof(Header))
        size = MIN_FREE * sizeof(Header);
    if (size + alignment + MIN_FREE * sizeof(Header) < BIG_BYTES) {
        char *p = malloc(size + alignment + MIN_FREE * sizeof(Header));
        if (!p)
            return 0;
        uintptr_t q = ((uintptr_t)p + alignment - 1) & ~(uintptr_t)(alignment - 1);
        if (q == (uintptr_t)p)
            return p;
        lock();
        Header *bp = (Header *)p - 1;
        if (bp->s.next != PIECE && q - (uintptr_t)p < MIN_FREE * sizeof(Header))
            q += alignment;
        Header *nb = (Header *)q - 1;
        size_t lead = (size_t)(nb - bp);
        nb->s.units = bp->s.units - lead;
        bp->s.units = lead;
        if (bp->s.next == PIECE) {      /* a small one's part: a small block of its own */
            nb->s.next = PIECE;
            bp->s.next = bins[lead];
            bins[lead] = bp;
        } else {
            nb->s.next = USED;          /* (release tells it its lower is free) */
            release(bp);
        }
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

/* (43a) The bytes a block may hold (at least what was asked). */
size_t malloc_usable_size(void *ptr)
{
    if (!ptr)
        return 0;
    Header *bp = (Header *)ptr - 1;
    if (bp->s.next == BIG_AT)
        return ((uintptr_t)bp & ~(uintptr_t)(PAGE - 1)) + bp->s.units - (uintptr_t)ptr;
    return (bp->s.units - 1) * sizeof(Header);
}

/* (43a) POSIX's aligned allocation: an error number instead of errno, and
 * alignment a power of two of at least a pointer's size. */
int posix_memalign(void **out, size_t alignment, size_t size)
{
    if (alignment < sizeof(void *) || (alignment & (alignment - 1)))
        return EINVAL;
    if (size == 0) {
        *out = 0;
        return 0;
    }
    int saved = errno;
    void *p = aligned_alloc(alignment, size);
    int error = errno;
    errno = saved;
    if (!p)
        return error;
    *out = p;
    return 0;
}

void *memalign(size_t alignment, size_t size)
{
    return aligned_alloc(alignment, size);
}

void *valloc(size_t size)
{
    return aligned_alloc(PAGE, size);
}

void *pvalloc(size_t size)
{
    return aligned_alloc(PAGE, (size + PAGE - 1) & ~(size_t)(PAGE - 1));
}

void *reallocarray(void *ptr, size_t count, size_t size)
{
    if (count && size > (size_t)-1 / count) {
        errno = ENOMEM;
        return 0;
    }
    return realloc(ptr, count * size);
}

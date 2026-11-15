/*
 * (68o) user/lib/malloc.c on the Mac, its functions renamed heap_*, under
 * ASan and UBSan, with a checker that walks every arena after every few
 * calls: the blocks tile each arena from its header to its fence, every
 * block's mark says rightly whether the block below it is free, no two free
 * blocks are neighbours (they would have been joined), every free block has
 * its size as its footer and is on its class's list (and nothing else is),
 * the lists' links agree both ways, `nonempty` has a bit exactly for each
 * list with a block, `spare` counts the wholly free arenas (at most one),
 * and the chunk lies inside its arena block. Every block holds a pattern
 * made from its number, checked before it is freed (a block handed out
 * twice, or too short, shows there), and its address has its alignment.
 * The stub "ritteros.h" (a lock with no other threads) comes from
 * malloc_host_test. Prints a line per part; "malloc_host: all ok" last.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/mman.h>
#include <unistd.h>

#define malloc heap_malloc
#define free heap_free
#define calloc heap_calloc
#define realloc heap_realloc
#define aligned_alloc heap_aligned_alloc
#define malloc_usable_size heap_usable_size
#define posix_memalign heap_posix_memalign
#define memalign heap_memalign
#define valloc heap_valloc
#define pvalloc heap_pvalloc
#define reallocarray heap_reallocarray
#include "malloc.c"
#undef malloc
#undef free
#undef calloc
#undef realloc
#undef aligned_alloc

static int failed;

static void bad(const char *what, const void *where)
{
    if (failed++ < 5)
        printf("malloc_host: FAIL %s at %p\n", what, where);
}

static int by_address(const void *a, const void *b)
{
    uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
    return x < y ? -1 : x > y;
}

/* Every invariant of the arenas' blocks; returns the number of arenas. */
static int check_heap(void)
{
    size_t listed = 0, cap = 1024;
    uintptr_t *on_lists = malloc(cap * sizeof(uintptr_t));
    for (unsigned c = 0; c < CLASSES; c++) {
        int bit = (nonempty[c / 64] >> (c % 64)) & 1;
        if (bit != (lists[c] != 0))
            bad("a class's bit and its list disagree", lists[c]);
        Header *prev = 0;
        for (Header *p = lists[c]; p; prev = p, p = p[1].l.next) {
            if (p->s.next != FREE)
                bad("a listed block is not free", p);
            if (class_of(p->s.units) != c)
                bad("a block on another class's list", p);
            if (p[1].l.prev != prev)
                bad("a list's back link", p);
            if (listed == cap)
                on_lists = realloc(on_lists, (cap *= 2) * sizeof(uintptr_t));
            on_lists[listed++] = (uintptr_t)p;
            if (listed > 10000000) {
                bad("a list loops", p);
                return 0;
            }
        }
    }
    qsort(on_lists, listed, sizeof(uintptr_t), by_address);
    size_t free_blocks = 0;
    unsigned wholes = 0;
    int count = 0, chunk_seen = chunk_head == 0;
    Header *before = 0;
    for (Header *a = arenas; a; before = a, a = a->l.next, count++) {
        if (a->l.prev != before)
            bad("the arenas' back link", a);
        Header *fence = a + ARENA_UNITS - 1, *p = a + 1;
        int low_free = 0;
        while (p < fence) {
            size_t n = p->s.units;
            if (n == 0 || p + n > fence) {
                bad("a block past its arena's fence", p);
                break;
            }
            if (p == chunk_head) {
                chunk_seen = 1;
                if (chunk_units && chunk + chunk_units != p + n)
                    bad("the chunk's rest does not end with its block", p);
            }
            if (p->s.next == FREE) {
                if (low_free)
                    bad("two free blocks side by side", p);
                if (n < MIN_FREE || ((size_t *)(p + n))[-1] != n)
                    bad("a free block's footer", p);
                if (!bsearch(&(uintptr_t){ (uintptr_t)p }, on_lists, listed, sizeof(uintptr_t),
                             by_address))
                    bad("a free block on no list", p);
                free_blocks++;
                wholes += n == WHOLE;
                low_free = 1;
            } else {
                if (p->s.next != (low_free ? USED_LOW_FREE : USED))
                    bad("a block's mark of the one below", p);
                low_free = 0;
            }
            p += n;
        }
        if (p == fence && (fence->s.units != 1 || fence->s.next != (low_free ? USED_LOW_FREE : USED)))
            bad("an arena's fence", fence);
    }
    if (free_blocks != listed)
        bad("blocks on the lists that are not free in an arena", 0);
    if (wholes != spare || spare > 1)
        bad("the count of wholly free arenas", 0);
    if (!chunk_seen)
        bad("the chunk is in no arena", chunk_head);
    free(on_lists);
    return count;
}

/* The harness's own random numbers (the same on every machine). */
static uint64_t seed = 0x6865617036386fULL;

static uint64_t next(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
}

struct slot {
    unsigned char *p;
    size_t size, align;
    uint32_t mark;
};

static unsigned char pattern(uint32_t mark, size_t k)
{
    return (unsigned char)(mark * 131 + k * 7 + (k >> 8));
}

/* The first and last 32 bytes and every 256th: enough to see a block that
 * overlaps another, without filling megabytes at every call. */
static size_t step(size_t k, size_t size)
{
    if (k < 32 || k + 33 >= size)
        return k + 1;
    return k + 256 < size - 32 ? k + 256 : size - 32;
}

static void fill(struct slot *s)
{
    for (size_t k = 0; k < s->size; k = step(k, s->size))
        s->p[k] = pattern(s->mark, k);
}

static void verify(struct slot *s)
{
    for (size_t k = 0; k < s->size; k = step(k, s->size))
        if (s->p[k] != pattern(s->mark, k)) {
            bad("a block's contents changed", s->p + k);
            return;
        }
}

static size_t log_size(size_t lo, size_t hi)  /* lo..hi, as many of each power of two */
{
    unsigned a = 63 - (unsigned)__builtin_clzl(lo), b = 63 - (unsigned)__builtin_clzl(hi);
    unsigned e = a + (unsigned)(next() % (b - a + 1));
    size_t v = ((size_t)1 << e) + (size_t)(next() % ((size_t)1 << e));
    return v < lo ? lo : v > hi ? hi : v;
}

static void allocate(struct slot *s, int mixed, uint32_t mark)
{
    unsigned kind = (unsigned)(next() % 100);
    s->align = 16;
    if (!mixed) {
        s->size = log_size(2100, 120000);
        s->p = heap_malloc(s->size);
    } else if (kind < 35) {
        s->size = 1 + next() % 2100;
        s->p = heap_malloc(s->size);
    } else if (kind < 85) {
        s->size = log_size(2048, 131072);
        s->p = heap_malloc(s->size);
    } else if (kind < 90) {
        s->size = log_size(131072, 1 << 20);
        s->p = heap_malloc(s->size);
    } else if (kind < 95) {
        s->size = 1 + next() % 20000;
        s->align = (size_t)32 << (next() % 9);
        s->p = heap_aligned_alloc(s->align, s->size);
    } else {
        s->size = 1 + next() % 30000;
        s->p = heap_calloc(1, s->size);
        for (size_t k = 0; s->p && k < s->size; k++)
            if (s->p[k]) {
                bad("calloc's block is not zero", s->p + k);
                break;
            }
    }
    s->mark = mark;
    if (!s->p) {
        bad("no memory", 0);
        return;
    }
    if ((uintptr_t)s->p % s->align)
        bad("a block not at its alignment", s->p);
    if (heap_usable_size(s->p) < s->size)
        bad("a block shorter than asked", s->p);
    fill(s);
}

static void let_go(struct slot *s, int grow)
{
    if (!s->p)
        return;
    verify(s);
    if (grow && s->align == 16) {               /* realloc keeps what was there */
        unsigned char *q = heap_realloc(s->p, s->size + 1 + next() % 40000);
        if (!q) {
            bad("realloc", s->p);
            return;
        }
        s->p = q;
        verify(s);
    }
    heap_free(s->p);
    s->p = 0;
}

#define SLOTS 3000

int main(void)
{
    static struct slot slot[SLOTS];
    uint32_t mark = 1;

    /* Blocks of 2.1-120 KB only (no small ones: their chunks stay), freed in a random order: every arena comes
     * back but the one kept. */
    for (int i = 0; i < SLOTS; i++)
        allocate(&slot[i], 0, mark++);
    check_heap();
    for (int i = SLOTS - 1; i > 0; i--) {
        int j = (int)(next() % (uint64_t)(i + 1));
        struct slot t = slot[i];
        slot[i] = slot[j];
        slot[j] = t;
    }
    for (int i = 0; i < SLOTS; i++) {
        let_go(&slot[i], 0);
        if (i % 97 == 0)
            check_heap();
    }
    int left = check_heap();
    if (left != 1 || spare != 1)
        bad("arenas not given back", 0);
    printf("malloc_host: give back %s\n", failed ? "FAILED" : "ok");

    /* Every kind mixed: small, arena-sized, mappings, aligned, calloc and
     * realloc, a block at a time replaced in a random slot. */
    long rounds = 400000, checks = 0;
    for (long r = 0; r < rounds && failed < 5; r++) {
        struct slot *s = &slot[next() % SLOTS];
        let_go(s, next() % 8 == 0);
        allocate(s, 1, mark++);
        if (r % 1000 == 0) {
            check_heap();
            checks++;
        }
    }
    for (int i = 0; i < SLOTS; i++)
        let_go(&slot[i], 0);
    check_heap();
    printf("malloc_host: mixed %s (%ld calls, the heap checked %ld times)\n",
           failed ? "FAILED" : "ok", rounds, checks + 1);
    printf("malloc_host: %s\n", failed ? "FAILED" : "all ok");
    return failed != 0;
}

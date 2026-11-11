#include "heap.h"
#include "arch.h"
#include "cpus.h"
#include "kprintf.h"
#include "memory.h"
#include "panic.h"
#include "pmm.h"
#include "string.h"
#include "sync.h"
#include <stdbool.h>

#define SLAB_MAGIC   0x42414C53u                 /* "SLAB" */
#define LARGE_MAGIC  0x45475241u                 /* "ARGE" */
#define FREE_MAGIC   0xF4EEF4EEF4EEF4EEULL       /* marks a free slot */
#define KEPT_MAGIC   0xCAC4ECAC4ECAC4EULL        /* (65e) marks a block a core keeps */

#define CLASS_COUNT  HEAP_CLASSES                /* 16, 32, ..., 1024 bytes */
#define BATCH        (HEAP_CACHE / 2)            /* blocks moved at once, core <-> slabs */
#define MIN_CLASS    16
#define MAX_SMALL    1024
#define SLAB_HEADER  64                          /* keeps slots 16-byte aligned */
#define LARGE_HEADER 16

/* At the start of every slab page. */
struct slab {
    uint32_t magic;
    uint16_t class_index;
    uint16_t slot_size;
    uint32_t slots;
    uint32_t free_slots;
    struct slab *next;                           /* slabs of this class with free slots */
    struct slab *prev;
    void *free_list;                             /* first free slot */
    uint8_t pad[SLAB_HEADER - 40];
};

/* A free slot: the next free slot of the same slab, and a marker. */
struct free_slot {
    struct free_slot *next;
    uint64_t magic;
};

/* At the start of a large block's first page. */
struct large {
    uint32_t magic;
    uint32_t pages;
    uint64_t size;
};

_Static_assert(sizeof(struct slab) == SLAB_HEADER, "slab header size");
_Static_assert(sizeof(struct large) == LARGE_HEADER, "large header size");

static struct slab *partial[CLASS_COUNT];       /* slabs that have free slots */
static struct slab *cached_empty[CLASS_COUNT];  /* one empty slab kept per class */

static unsigned class_of(size_t size)
{
    unsigned c = 0;
    while ((size_t)(MIN_CLASS << c) < size)
        c++;
    return c;
}

static void list_remove(struct slab *s)
{
    if (s->prev)
        s->prev->next = s->next;
    else
        partial[s->class_index] = s->next;
    if (s->next)
        s->next->prev = s->prev;
    s->next = s->prev = 0;
}

static void list_push(struct slab *s)
{
    s->prev = 0;
    s->next = partial[s->class_index];
    if (s->next)
        s->next->prev = s;
    partial[s->class_index] = s;
}

static struct slab *new_slab(unsigned class_index)
{
    uint64_t page = pmm_alloc_page();
    if (!page)
        return 0;
    struct slab *s = phys_to_virt(page);
    s->magic = SLAB_MAGIC;
    s->class_index = (uint16_t)class_index;
    s->slot_size = (uint16_t)(MIN_CLASS << class_index);
    s->slots = (uint32_t)((PMM_PAGE_SIZE - SLAB_HEADER) / s->slot_size);
    s->free_slots = s->slots;
    s->next = s->prev = 0;

    /* Chain all slots into the free list in address order. */
    uint8_t *first = (uint8_t *)s + SLAB_HEADER;
    s->free_list = first;
    for (uint32_t i = 0; i < s->slots; i++) {
        struct free_slot *f = (struct free_slot *)(first + i * s->slot_size);
        f->next = i + 1 < s->slots ? (struct free_slot *)(first + (i + 1) * s->slot_size) : 0;
        f->magic = FREE_MAGIC;
    }
    return s;
}

/* A slot pointer must lie on a slot boundary inside the slab. */
static bool valid_slot(const struct slab *s, const void *p)
{
    uint64_t off = (uint64_t)p - (uint64_t)s;
    return off >= SLAB_HEADER && off < PMM_PAGE_SIZE &&
           (off - SLAB_HEADER) % s->slot_size == 0 &&
           (off - SLAB_HEADER) / s->slot_size < s->slots;
}

static void *alloc_small(size_t size)
{
    unsigned c = class_of(size);
    struct slab *s = partial[c];
    if (!s) {
        s = cached_empty[c] ? cached_empty[c] : new_slab(c);
        cached_empty[c] = 0;
        if (!s)
            return 0;
        list_push(s);
    }

    struct free_slot *f = s->free_list;
    /* A buffer overflow from the slot before would have damaged these. */
    if (f->magic != FREE_MAGIC || (f->next && !valid_slot(s, f->next)))
        panic("heap: corrupted free slot %p in slab %p (buffer overflow?)", (void *)f, (void *)s);
    s->free_list = f->next;
    f->magic = 0;
    if (--s->free_slots == 0)
        list_remove(s);
    return f;
}

static void free_small(struct slab *s, void *ptr)
{
    if (!valid_slot(s, ptr))
        panic("heap: kfree(%p) is not the start of a heap block", ptr);

    struct free_slot *f = ptr;
    if (f->magic == FREE_MAGIC) {
        /* Probably already free: confirm by searching the free list. */
        for (struct free_slot *x = s->free_list; x; x = x->next)
            if (x == f)
                panic("heap: double free of %p", ptr);
    }
    f->next = s->free_list;
    f->magic = FREE_MAGIC;
    s->free_list = f;

    if (s->free_slots++ == 0)
        list_push(s);                           /* it was full: has room again */
    if (s->free_slots == s->slots) {
        /* Completely free: keep one per class, return the rest. */
        list_remove(s);
        if (!cached_empty[s->class_index]) {
            cached_empty[s->class_index] = s;
        } else {
            s->magic = 0;
            pmm_free_page(virt_to_phys(s));
        }
    }
}

/* The slabs and their lists are shared by all threads: a spinlock guards
 * them (the page allocator below has its own; large blocks, whole pages,
 * need only that one). */
static struct spinlock lock = SPINLOCK_INIT("heap");

static void *allocate(size_t size)
{
    if (size == 0)
        return 0;
    if (size <= MAX_SMALL)
        return alloc_small(size);

    uint64_t pages = (size + LARGE_HEADER + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys)
        return 0;
    struct large *l = phys_to_virt(phys);
    l->magic = LARGE_MAGIC;
    l->pages = (uint32_t)pages;
    l->size = size;
    return (uint8_t *)l + LARGE_HEADER;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

static void release(void *ptr)
{
    if (!ptr)
        return;
    if ((uint64_t)ptr < HHDM_BASE || (uint64_t)ptr >= KERNEL_VIRT_BASE)
        panic("heap: kfree(%p) is not a heap address", ptr);

    void *page = (void *)((uint64_t)ptr & ~(PMM_PAGE_SIZE - 1));
    uint32_t magic = *(uint32_t *)page;
    if (magic == SLAB_MAGIC) {
        free_small(page, ptr);
    } else if (magic == LARGE_MAGIC && (uint8_t *)ptr == (uint8_t *)page + LARGE_HEADER) {
        struct large *l = page;
        l->magic = 0;
        pmm_free_pages(virt_to_phys(l), l->pages);
    } else {
        panic("heap: kfree(%p) is not a live heap block", ptr);
    }
}

static void trim(void)
{
    for (unsigned c = 0; c < CLASS_COUNT; c++) {
        if (cached_empty[c]) {
            cached_empty[c]->magic = 0;
            pmm_free_page(virt_to_phys(cached_empty[c]));
            cached_empty[c] = 0;
        }
    }
}

/* --- self-test ----------------------------------------------------------------- */

#define TEST_BLOCKS 2000

static uint64_t rng_state;

static uint64_t rng_next(void)        /* xorshift64 */
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static uint8_t pattern(int block, size_t offset)
{
    return (uint8_t)(block * 31 + offset * 7 + 1);
}

void heap_self_test(uint64_t seed)
{
    static uint8_t *blocks[TEST_BLOCKS];
    static size_t sizes[TEST_BLOCKS];
    static int order[TEST_BLOCKS];
    uint64_t pages_before = pmm_free_count();
    uint64_t total = 0;
    size_t largest = 0;

    rng_state = seed ? seed : 0x52495454455253ULL;
    for (int i = 0; i < TEST_BLOCKS; i++) {
        /* Mostly small blocks, some medium, a few spanning several pages. */
        uint64_t r = rng_next();
        size_t size = r % 10 < 7 ? 1 + r % 256 : r % 10 < 9 ? 1 + r % 2048 : 1 + r % 20000;
        blocks[i] = kmalloc(size);
        sizes[i] = size;
        if (!blocks[i] || (uint64_t)blocks[i] % 16 != 0)
            panic("heap self-test: kmalloc(%zu) returned %p", size, (void *)blocks[i]);
        for (size_t j = 0; j < size; j++)
            blocks[i][j] = pattern(i, j);
        total += size;
        if (size > largest)
            largest = size;
    }

    /* Overlapping blocks would have overwritten each other's patterns. */
    for (int i = 0; i < TEST_BLOCKS; i++)
        for (size_t j = 0; j < sizes[i]; j++)
            if (blocks[i][j] != pattern(i, j))
                panic("heap self-test: block %d (%p) damaged at byte %zu", i,
                      (void *)blocks[i], j);

    /* Free in random order (Fisher-Yates shuffle). */
    for (int i = 0; i < TEST_BLOCKS; i++)
        order[i] = i;
    for (int i = TEST_BLOCKS - 1; i > 0; i--) {
        int j = (int)(rng_next() % (uint64_t)(i + 1));
        int t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (int i = 0; i < TEST_BLOCKS; i++)
        kfree(blocks[order[i]]);
    heap_trim();

    if (pmm_free_count() != pages_before)
        panic("heap self-test: %lu pages free before, %lu after", pages_before,
              pmm_free_count());
    kprintf("  heap:          self-test passed: %d blocks (%lu KiB, 1-%zu bytes) "
            "allocated, verified, freed in random order\n",
            TEST_BLOCKS, total / 1024, largest);
}

/* (65e) Small blocks come from this core's kept ones; only taking a new
 * batch from the slabs or giving one back takes the lock (65e-4's
 * measurement: the cores waited longest for this spinlock). Interrupts are
 * off while a core works on its own list, so nothing else touches it. */

static void kept_set(struct cpu *c, unsigned k, int n)
{
    __atomic_store_n(&c->heap_cached[k], n, __ATOMIC_RELEASE);
}

/* Is this block among a core's kept ones? (Others' lists are only read: a
 * block kept now was put there after its count grew past it.) */
static bool kept_by(struct cpu *c, unsigned k, const void *p)
{
    int n = __atomic_load_n(&c->heap_cached[k], __ATOMIC_ACQUIRE);
    for (int i = 0; i < n && i < HEAP_CACHE; i++)
        if (__atomic_load_n(&c->heap_cache[k][i], __ATOMIC_RELAXED) == p)
            return true;
    return false;
}

void *kmalloc(size_t size)
{
    if (size == 0 || size > MAX_SMALL)
        return allocate(size);                  /* whole pages: the slabs are not touched */
    unsigned k = class_of(size);
    uint64_t irq = arch_irq_save();
    struct cpu *c = arch_this_cpu();
    int n = c->heap_cached[k];
    if (!n) {
        uint64_t state = spin_lock(&lock);
        while (n < BATCH) {
            struct free_slot *f = alloc_small(MIN_CLASS << k);
            if (!f)
                break;
            f->magic = KEPT_MAGIC;
            c->heap_cache[k][n++] = f;
        }
        spin_unlock(&lock, state);
        /* Given out in address order, as the slab would (the overflow
         * check below then sees the next block a write ran into). */
        for (int i = 0, j = n - 1; i < j; i++, j--) {
            void *t = c->heap_cache[k][i];
            c->heap_cache[k][i] = c->heap_cache[k][j];
            c->heap_cache[k][j] = t;
        }
        if (!n) {
            arch_irq_restore(irq);
            return 0;                           /* out of memory */
        }
    }
    struct free_slot *f = c->heap_cache[k][n - 1];
    if (f->magic != KEPT_MAGIC)
        panic("heap: corrupted free slot %p kept by core %d (buffer overflow?)", (void *)f,
              c->index);
    f->magic = 0;
    kept_set(c, k, n - 1);
    arch_irq_restore(irq);
    return f;
}

/* n of this core's kept blocks of class k back to the slabs, the lock held. */
static void give_back(struct cpu *c, unsigned k, int n)
{
    int left = c->heap_cached[k];
    while (n-- > 0 && left) {
        struct free_slot *f = c->heap_cache[k][--left];
        f->magic = 0;
        free_small((struct slab *)((uint64_t)f & ~(PMM_PAGE_SIZE - 1)), f);
    }
    kept_set(c, k, left);
}

void kfree(void *ptr)
{
    struct slab *s = (struct slab *)((uint64_t)ptr & ~(PMM_PAGE_SIZE - 1));
    if (!ptr || (uint64_t)ptr < HHDM_BASE || (uint64_t)ptr >= KERNEL_VIRT_BASE ||
        s->magic != SLAB_MAGIC) {
        release(ptr);                           /* a large block (pages only), or the panic */
        return;
    }
    if (!valid_slot(s, ptr))
        panic("heap: kfree(%p) is not the start of a heap block", ptr);
    struct free_slot *f = ptr;
    unsigned k = s->class_index;
    uint64_t irq = arch_irq_save();
    struct cpu *c = arch_this_cpu();
    if (f->magic == KEPT_MAGIC) {
        /* Probably kept already: confirm by searching the cores' lists. */
        int cores = cpus_online();
        for (int i = 0; i < cores; i++)
            if (kept_by(cpu_online_at(i), k, f))
                panic("heap: double free of %p", ptr);
    } else if (f->magic == FREE_MAGIC) {
        /* Probably free in its slab: the slab's own check confirms. */
        uint64_t state = spin_lock(&lock);
        free_small(s, ptr);
        spin_unlock(&lock, state);
        arch_irq_restore(irq);
        return;
    }
    int n = c->heap_cached[k];
    if (n == HEAP_CACHE) {
        uint64_t state = spin_lock(&lock);
        give_back(c, k, BATCH);
        spin_unlock(&lock, state);
        n = c->heap_cached[k];
    }
    f->magic = KEPT_MAGIC;
    c->heap_cache[k][n] = f;
    kept_set(c, k, n + 1);
    arch_irq_restore(irq);
}

void heap_cache_flush(void)
{
    uint64_t irq = arch_irq_save();
    struct cpu *c = arch_this_cpu();
    uint64_t state = spin_lock(&lock);
    for (unsigned k = 0; k < CLASS_COUNT; k++)
        give_back(c, k, HEAP_CACHE);
    spin_unlock(&lock, state);
    arch_irq_restore(irq);
}

void heap_trim(void)
{
    cpus_flush_heap_caches();
    uint64_t state = spin_lock(&lock);
    trim();
    spin_unlock(&lock, state);
}

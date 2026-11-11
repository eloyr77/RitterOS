#include "pmm.h"
#include "sync.h"
#include "cpus.h"
#include <stdbool.h>
#include "kprintf.h"
#include "memory.h"
#include "panic.h"
#include "string.h"
#include "cmdline.h"

#define LOW_MEMORY_END   0x100000ULL     /* first 1 MiB: kept for later use */
#define LOW_PAGE_END     0xA0000ULL      /* (65b) the end of conventional memory */
#define BITS_PER_WORD    64

static uint64_t *bitmap;                 /* bit set = frame in use (direct-map pointer) */
static uint64_t bitmap_phys;
static uint64_t frame_count;             /* frames covered by the bitmap */
static uint64_t word_count;
static uint64_t free_frames;
static uint64_t usable_frames;
static uint64_t next_word;               /* where the next search starts */
static uint64_t next_run;                /* (65e) where the next search for a run starts */
static uint64_t low_page;                /* (65b) see pmm_low_page */
static uint64_t high_hint;               /* (35b) no free frame in a word above it */

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static bool is_used(uint64_t frame)
{
    return bitmap[frame / BITS_PER_WORD] & (1ULL << (frame % BITS_PER_WORD));
}

static void set_used(uint64_t frame)
{
    bitmap[frame / BITS_PER_WORD] |= 1ULL << (frame % BITS_PER_WORD);
}

static void set_free(uint64_t frame)
{
    bitmap[frame / BITS_PER_WORD] &= ~(1ULL << (frame % BITS_PER_WORD));
}

void pmm_init(const struct boot_info *info)
{
    /* The bitmap must cover everything up to the end of the highest usable RAM. */
    uint64_t top = 0;
    for (uint64_t i = 0; i < info->memory_map_count; i++) {
        const struct memory_region *r = &info->memory_map[i];
        if (r->type == MEMORY_USABLE && r->base + r->length > top)
            top = r->base + r->length;
    }
    frame_count = top / PMM_PAGE_SIZE;
    word_count = (frame_count + BITS_PER_WORD - 1) / BITS_PER_WORD;
    uint64_t bitmap_bytes = align_up(word_count * sizeof(uint64_t), PMM_PAGE_SIZE);

    /* (65b) A page below 640 KiB for x86's core start-up (not page 0). */
    for (uint64_t i = 0; i < info->memory_map_count && !low_page; i++) {
        const struct memory_region *r = &info->memory_map[i];
        uint64_t start = r->base < PMM_PAGE_SIZE ? PMM_PAGE_SIZE : align_up(r->base, PMM_PAGE_SIZE);
        if (r->type == MEMORY_USABLE && start + PMM_PAGE_SIZE <= r->base + r->length &&
            start + PMM_PAGE_SIZE <= LOW_PAGE_END)
            low_page = start;
    }

    /* Place the bitmap in the first usable region above 1 MiB that fits. */
    for (uint64_t i = 0; i < info->memory_map_count && !bitmap; i++) {
        const struct memory_region *r = &info->memory_map[i];
        uint64_t start = r->base < LOW_MEMORY_END ? LOW_MEMORY_END : r->base;
        if (r->type == MEMORY_USABLE && start + bitmap_bytes <= r->base + r->length) {
            bitmap_phys = start;
            bitmap = phys_to_virt(start);
        }
    }
    if (!bitmap)
        panic("pmm: no room for a %lu KiB bitmap", bitmap_bytes / 1024);

    /* (35b) `memory=N`: only the first N MiB of usable memory are given out
     * (to see swap at work on a machine with plenty). */
    char limit_text[16];
    uint64_t limit = ~0ULL;
    if (cmdline_get(info->cmdline, "memory", limit_text, sizeof(limit_text))) {
        uint64_t mib = 0;
        for (const char *c = limit_text; *c >= '0' && *c <= '9'; c++)
            mib = mib * 10 + (uint64_t)(*c - '0');
        if (mib >= 64)
            limit = mib * 1024 * 1024 / PMM_PAGE_SIZE;
    }

    /* Everything starts as used; then free the usable regions. */
    memset(bitmap, 0xFF, word_count * sizeof(uint64_t));
    for (uint64_t i = 0; i < info->memory_map_count; i++) {
        const struct memory_region *r = &info->memory_map[i];
        if (r->type != MEMORY_USABLE)
            continue;
        uint64_t start = r->base < LOW_MEMORY_END ? LOW_MEMORY_END : r->base;
        for (uint64_t a = start; a < r->base + r->length && free_frames < limit;
             a += PMM_PAGE_SIZE) {
            set_free(a / PMM_PAGE_SIZE);
            free_frames++;
        }
    }

    /* The bitmap's own pages are in use. */
    for (uint64_t a = bitmap_phys; a < bitmap_phys + bitmap_bytes; a += PMM_PAGE_SIZE) {
        if (!is_used(a / PMM_PAGE_SIZE))
            free_frames--;
        set_used(a / PMM_PAGE_SIZE);
    }
    usable_frames = free_frames;
    high_hint = word_count - 1;

    kprintf("  memory:        %lu MiB free (%lu pages of 4 KiB), bitmap %p (%lu KiB, %lu frames)%s\n",
            free_frames * PMM_PAGE_SIZE / (1024 * 1024), free_frames, (void *)bitmap_phys,
            bitmap_bytes / 1024, frame_count, limit != ~0ULL ? " (memory=: the rest unused)" : "");
}

uint64_t pmm_low_page(void)
{
    return low_page;
}

/* The bitmap is shared by all threads: a spinlock guards it. */
static struct spinlock lock = SPINLOCK_INIT("pmm");

static uint64_t alloc_page(void)
{
    /* Next fit: continue where the last allocation found a free page. */
    for (uint64_t n = 0; n < word_count; n++) {
        uint64_t w = (next_word + n) % word_count;
        if (bitmap[w] == ~0ULL)
            continue;                          /* all 64 frames in use */
        uint64_t bit = (uint64_t)__builtin_ctzll(~bitmap[w]);
        uint64_t frame = w * BITS_PER_WORD + bit;
        if (frame >= frame_count)
            continue;
        set_used(frame);
        free_frames--;
        next_word = w;
        return frame * PMM_PAGE_SIZE;
    }
    return 0;
}

/* The first of count free frames in a row between from and to, or to. A
 * word of 64 frames all in use or all free is passed at once (61d: frame
 * by frame, the search held interrupts off for milliseconds on a 5 GiB
 * map, and pings waited for it). */
static uint64_t find_run(uint64_t from, uint64_t to, size_t count)
{
    uint64_t run = 0;
    for (uint64_t frame = from; frame < to; frame++) {
        if (frame % BITS_PER_WORD == 0 && frame + BITS_PER_WORD <= to) {
            uint64_t w = bitmap[frame / BITS_PER_WORD];
            if (w == ~0ULL) {
                run = 0;
                frame += BITS_PER_WORD - 1;
                continue;
            }
            if (w == 0) {
                if (run + BITS_PER_WORD >= count)
                    return frame - run;
                run += BITS_PER_WORD;
                frame += BITS_PER_WORD - 1;
                continue;
            }
        }
        if (is_used(frame)) {
            run = 0;
            continue;
        }
        if (++run == count)
            return frame + 1 - count;
    }
    return to;
}

static uint64_t alloc_pages(size_t count)
{
    if (count == 0)
        return 0;
    if (count == 1)
        return alloc_page();

    /* (65e) Next fit, as for single pages: from where the last run was
     * found, then from the start. Until 65e every search began at frame 0
     * and went through the fragmented low memory again: under smpstress on
     * 4 cores each took 50-120 us (up to 3 ms), every kernel stack and
     * large heap block one, and the cores waited ~18 s for it in 20 s. */
    uint64_t first = next_run < frame_count ? find_run(next_run, frame_count, count) : frame_count;
    if (first == frame_count)
        first = find_run(0, frame_count, count);
    if (first == frame_count)
        return 0;
    for (uint64_t f = first; f < first + count; f++)
        set_used(f);
    free_frames -= count;
    next_run = first + count;
    return first * PMM_PAGE_SIZE;
}

/* (35b) The highest free page (see pmm_alloc_page_high): from the hint down,
 * lowering it past words found full. */
static uint64_t alloc_page_high(void)
{
    for (uint64_t w = high_hint + 1; w-- > 0;) {
        uint64_t free = ~bitmap[w];
        if (w == word_count - 1 && frame_count % BITS_PER_WORD)
            free &= (1ULL << (frame_count % BITS_PER_WORD)) - 1;
        if (!free) {
            high_hint = w ? w - 1 : 0;
            continue;
        }
        uint64_t frame = w * BITS_PER_WORD + 63 - (uint64_t)__builtin_clzll(free);
        set_used(frame);
        free_frames--;
        high_hint = w;
        return frame * PMM_PAGE_SIZE;
    }
    return 0;
}

/* Contiguous pages whose whole run lies below limit (for 32-bit DMA). */
static uint64_t alloc_pages_below(size_t count, uint64_t limit)
{
    if (count == 0)
        return 0;
    uint64_t run = 0, max_frame = limit / PMM_PAGE_SIZE;
    for (uint64_t frame = 0; frame < frame_count && frame < max_frame; frame++) {
        if (frame % BITS_PER_WORD == 0 && bitmap[frame / BITS_PER_WORD] == ~0ULL) {
            run = 0;
            frame += BITS_PER_WORD - 1;         /* (as alloc_pages) */
            continue;
        }
        if (is_used(frame)) {
            run = 0;
            continue;
        }
        if (++run == count) {
            uint64_t first = frame + 1 - count;
            for (uint64_t f = first; f <= frame; f++)
                set_used(f);
            free_frames -= count;
            return first * PMM_PAGE_SIZE;
        }
    }
    return 0;
}

static void free_page(uint64_t addr)
{
    uint64_t frame = addr / PMM_PAGE_SIZE;
    if (addr % PMM_PAGE_SIZE != 0 || addr < LOW_MEMORY_END || frame >= frame_count)
        panic("pmm: freeing invalid page address %p", (void *)addr);
    if (!is_used(frame))
        panic("pmm: double free of page %p", (void *)addr);
    set_free(frame);
    free_frames++;
    if (frame / BITS_PER_WORD > high_hint)
        high_hint = frame / BITS_PER_WORD;
}

/* --- (65e-47) the cores' own pages ------------------------------------------ */

/* Each core keeps up to PMM_CACHE free pages (struct cpu's pages, under its
 * page_lock, "pmm cache"), taken from the bitmap and given back to it
 * PMM_BATCH at a time in one hold of the bitmap's lock: until 65e-47 every
 * page took that lock (on the laptop eight threads faulting fresh pages in
 * and unmapping them spun 3.56 s of 80 on it, 12.9 million times in 10 s).
 * The pages kept count as free; when the bitmap has none left, the cores'
 * caches are emptied into it (one at a time) before an allocation fails.
 * The last page given back is the first given out (still in the caches). */
#define PMM_BATCH (PMM_CACHE / 2)

/* In the cores' caches. A batch moved between a cache and the bitmap
 * changes it and free_frames in one hold of the lock, and the count of free
 * pages reads both under it, so that no page is missed or counted twice
 * meanwhile (65e-47's first try changed it after the lock: ARM's memtest
 * found 24 pages more than counted). A page alone, in or out of a cache,
 * changes only it. */
static uint64_t kept_frames;

/* This core's cache, locked (interrupts off from the look on: the block is
 * this core's until the unlock). */
static struct cpu *cache_lock(uint64_t *irq)
{
    *irq = arch_irq_save();
    struct cpu *c = arch_this_cpu();
    spin_lock(&c->page_lock);
    return c;
}

static void cache_unlock(struct cpu *c, uint64_t irq)
{
    spin_unlock_only(&c->page_lock);
    arch_irq_restore(irq);
}

/* Every core's cache back into the bitmap, one at a time (never two cache
 * locks at once). */
static void caches_empty(void)
{
    for (int i = 0; i < cpus_online(); i++) {
        struct cpu *c = cpu_online_at(i);
        uint64_t state = spin_lock(&c->page_lock);
        spin_lock(&lock);
        for (int k = 0; k < c->pages_kept; k++)
            free_page(c->pages[k]);
        for (int k = 0; k < c->high_kept; k++)
            free_page(c->high[k]);
        __atomic_sub_fetch(&kept_frames, (uint64_t)(c->pages_kept + c->high_kept),
                           __ATOMIC_RELAXED);
        c->pages_kept = 0;
        c->high_kept = 0;
        spin_unlock_only(&lock);
        spin_unlock(&c->page_lock, state);
    }
}

uint64_t pmm_alloc_page(void)
{
    uint64_t irq;
    struct cpu *c = cache_lock(&irq);
    if (!c->pages_kept) {
        uint64_t state = spin_lock(&lock);
        uint64_t page;
        while (c->pages_kept < PMM_BATCH && (page = alloc_page()))
            c->pages[c->pages_kept++] = page;
        __atomic_add_fetch(&kept_frames, (uint64_t)c->pages_kept, __ATOMIC_RELAXED);
        spin_unlock(&lock, state);
    }
    uint64_t page = 0;
    if (c->pages_kept) {
        page = c->pages[--c->pages_kept];
        __atomic_sub_fetch(&kept_frames, 1, __ATOMIC_RELAXED);
    }
    cache_unlock(c, irq);
    if (!page) {                        /* the bitmap is empty: the others' caches */
        caches_empty();
        uint64_t state = spin_lock(&lock);
        page = alloc_page();
        spin_unlock(&lock, state);
    }
    return page;
}

/* (35e) Programs' pages while swap is on come through the core too: its
 * `high` pages, taken PMM_HIGH at a time from the top of memory. Until 35e
 * each one took the bitmap's lock: with swap on but nothing in it, 16
 * programs faulting in 1 GiB each on the laptop waited for it 3.7 million
 * times (5.6 s; 0.12 million and 0.36 s with swap off) and took 8% longer.
 * Pages given back go to the core's other cache, never among these: only
 * pages from the top are given out here. */
uint64_t pmm_alloc_page_high(void)
{
    uint64_t irq;
    struct cpu *c = cache_lock(&irq);
    if (!c->high_kept) {
        uint64_t state = spin_lock(&lock);
        uint64_t page;
        while (c->high_kept < PMM_HIGH && (page = alloc_page_high()))
            c->high[c->high_kept++] = page;
        __atomic_add_fetch(&kept_frames, (uint64_t)c->high_kept, __ATOMIC_RELAXED);
        spin_unlock(&lock, state);
    }
    uint64_t page = 0;
    if (c->high_kept) {
        page = c->high[--c->high_kept];
        __atomic_sub_fetch(&kept_frames, 1, __ATOMIC_RELAXED);
    }
    cache_unlock(c, irq);
    return page ? page : pmm_alloc_page();          /* (the cores' caches) */
}

uint64_t pmm_alloc_pages(size_t count)
{
    for (int round = 0;; round++) {
        uint64_t state = spin_lock(&lock);
        uint64_t pages = alloc_pages(count);
        spin_unlock(&lock, state);
        if (pages || round)
            return pages;
        caches_empty();                 /* (their pages may make the run) */
    }
}

uint64_t pmm_alloc_pages_below(size_t count, uint64_t limit)
{
    for (int round = 0;; round++) {
        uint64_t state = spin_lock(&lock);
        uint64_t pages = alloc_pages_below(count, limit);
        spin_unlock(&lock, state);
        if (pages || round)
            return pages;
        caches_empty();
    }
}

void pmm_free_page(uint64_t addr)
{
    uint64_t frame = addr / PMM_PAGE_SIZE;
    if (addr % PMM_PAGE_SIZE != 0 || addr < LOW_MEMORY_END || frame >= frame_count)
        panic("pmm: freeing invalid page address %p", (void *)addr);
    uint64_t irq;
    struct cpu *c = cache_lock(&irq);
    /* A page given back twice: free in the bitmap already, or in this
     * core's caches (in another core's: found when that one is emptied). */
    bool twice = !(__atomic_load_n(&bitmap[frame / BITS_PER_WORD], __ATOMIC_RELAXED) &
                   (1ULL << (frame % BITS_PER_WORD)));
    for (int k = 0; k < c->pages_kept && !twice; k++)
        twice = c->pages[k] == addr;
    for (int k = 0; k < c->high_kept && !twice; k++)
        twice = c->high[k] == addr;
    if (twice) {
        cache_unlock(c, irq);
        panic("pmm: double free of page %p", (void *)addr);
    }
    if (c->pages_kept == PMM_CACHE) {   /* the oldest half back to the bitmap */
        uint64_t state = spin_lock(&lock);
        for (int k = 0; k < PMM_BATCH; k++)
            free_page(c->pages[k]);
        __atomic_sub_fetch(&kept_frames, (uint64_t)PMM_BATCH, __ATOMIC_RELAXED);
        spin_unlock(&lock, state);
        memmove(c->pages, c->pages + PMM_BATCH, (PMM_CACHE - PMM_BATCH) * sizeof(uint64_t));
        c->pages_kept -= PMM_BATCH;
    }
    c->pages[c->pages_kept++] = addr;
    __atomic_add_fetch(&kept_frames, 1, __ATOMIC_RELAXED);
    cache_unlock(c, irq);
}

void pmm_free_pages(uint64_t addr, size_t count)
{
    uint64_t state = spin_lock(&lock);
    for (size_t i = 0; i < count; i++)
        free_page(addr + i * PMM_PAGE_SIZE);
    spin_unlock(&lock, state);
}

uint64_t pmm_free_count(void)
{
    uint64_t state = spin_lock(&lock);
    uint64_t n = free_frames + __atomic_load_n(&kept_frames, __ATOMIC_RELAXED);
    spin_unlock(&lock, state);
    return n;
}

uint64_t pmm_free_estimate(void)
{
    return __atomic_load_n(&free_frames, __ATOMIC_RELAXED) +
           __atomic_load_n(&kept_frames, __ATOMIC_RELAXED);
}

uint64_t pmm_usable_count(void)
{
    return usable_frames;
}

/* --- tests ------------------------------------------------------------------ */

#define SELF_TEST_PAGES 1000

void pmm_self_test(void)
{
    static uint64_t pages[SELF_TEST_PAGES];
    uint64_t before = pmm_free_count();

    for (int i = 0; i < SELF_TEST_PAGES; i++) {
        pages[i] = pmm_alloc_page();
        if (pages[i] == 0 || pages[i] % PMM_PAGE_SIZE != 0 || pages[i] < LOW_MEMORY_END)
            panic("pmm self-test: bad page %p", (void *)pages[i]);
        /* Mark the first and last word with the page's own address. */
        volatile uint64_t *p = phys_to_virt(pages[i]);
        p[0] = pages[i];
        p[PMM_PAGE_SIZE / 8 - 1] = ~pages[i];
    }
    /* If two allocations overlapped, a later write would have overwritten an
     * earlier one. */
    for (int i = 0; i < SELF_TEST_PAGES; i++) {
        volatile uint64_t *p = phys_to_virt(pages[i]);
        if (p[0] != pages[i] || p[PMM_PAGE_SIZE / 8 - 1] != ~pages[i])
            panic("pmm self-test: page %p was overwritten", (void *)pages[i]);
    }
    for (int i = 0; i < SELF_TEST_PAGES; i++)
        pmm_free_page(pages[i]);

    uint64_t run = pmm_alloc_pages(16);
    if (run == 0)
        panic("pmm self-test: no 16 contiguous pages");
    pmm_free_pages(run, 16);

    if (pmm_free_count() != before)
        panic("pmm self-test: %lu pages free before, %lu after", before, pmm_free_count());
    kprintf("  memory:        self-test passed: %d pages allocated, written, verified, freed\n",
            SELF_TEST_PAGES);
}

void pmm_memtest(void)
{
    uint64_t before = pmm_free_count();
    kprintf("  memtest:       allocating and writing all %lu free pages...\n", before);

    /* Chain every page into a list: each page stores its own address and the
     * address of the previously allocated page. */
    uint64_t head = 0, count = 0, page;
    while ((page = pmm_alloc_page()) != 0) {
        volatile uint64_t *p = phys_to_virt(page);
        p[0] = page;
        p[1] = head;
        head = page;
        count++;
    }
    if (count != before)
        panic("memtest: allocated %lu pages, expected %lu", count, before);

    /* Walk the chain: every page must still hold what was written to it. */
    uint64_t checked = 0;
    while (head != 0) {
        volatile uint64_t *p = phys_to_virt(head);
        if (p[0] != head)
            panic("memtest: page %p holds %p", (void *)head, (void *)p[0]);
        uint64_t next = p[1];
        pmm_free_page(head);
        head = next;
        checked++;
    }
    if (checked != count || pmm_free_count() != before)
        panic("memtest: %lu of %lu pages verified, %lu free", checked, count, pmm_free_count());
    kprintf("  memtest:       passed: %lu pages (%lu MiB) written, verified and freed\n",
            count, count * PMM_PAGE_SIZE / (1024 * 1024));
}

/*
 * Memory mappings (see mmap.h). Each process keeps a sorted list of its
 * mappings in the mmap window (MMAP_WINDOW_BASE..TOP): a mapping is either
 * the process's own pages (anonymous memory, or a private copy of a file)
 * or pages of the page cache. Since 22e anonymous pages are made when first
 * touched (mmap_fault), and a private writable mapping of a RitterFS file
 * shows the cache's pages read-only until a page's first write copies it. The cache holds, per version of a RitterFS
 * file -- its file system, object, inode generation, modification time and
 * size -- the pages read so far; mappings hold references on it, and the
 * last one going frees its pages. A file rewritten later is another version,
 * so programs that mapped the old one keep its pages (a library replaced
 * under a running program goes on working).
 *
 * fork (56a) copies a process's mappings: the cache's pages are mapped in
 * both as they are; the process's own pages are then held by both (a count
 * per page, `shared_pages`), read-only in each where they were writable,
 * and a write copies the page if another still holds it (or just makes it
 * writable again if not). Anonymous MAP_SHARED memory stays one memory: its
 * pages are all made at the fork and both keep them writable.
 */
#include "mmap.h"
#include "arch.h"
#include "heap.h"
#include "kprintf.h"
#include "memory.h"
#include "paging.h"
#include "panic.h"
#include "pmm.h"
#include "process.h"
#include "random.h"
#include "ritterfs.h"
#include "string.h"
#include "swap.h"
#include "timer.h"
#include "syscalls.h"
#include "vfs.h"
#include "vmlayout.h"

struct fpages {                     /* one version of a file in the page cache */
    struct vfs *fs;
    uint64_t object_id, generation, modified, size;
    uint64_t count;                 /* pages the file has */
    uint64_t *phys;                 /* page i's physical address, 0: not read yet */
    uint64_t loaded;                /* pages read so far */
    unsigned refs;                  /* mappings that use it */
    uint64_t idle_since;            /* refs 0: kept, in the order it went idle (22g) */
    uint8_t *written;               /* a bit per page written through a shared mapping (22j;
                                     * 0: none yet), so msync writes it back */
    struct fpages *next;
};

struct mapping {
    uint64_t start, end;            /* [start, end), whole pages */
    unsigned access;                /* USER_* (0: a reservation, nothing mapped) */
    struct fpages *file;            /* the page cache's pages, or 0: the process's own */
    uint64_t first;                 /* the file page at start (file mappings) */
    uint64_t file_pages;            /* the file's pages: touching one past them is SIGBUS */
    bool anonymous;                 /* zeroed memory, each page made when first touched */
    bool cow;                       /* private and writable over the cache: a page is
                                     * copied at its first write */
    bool unread;                    /* a file mapping made PROT_NONE: its pages were
                                     * never read, so it cannot get access later */
    bool shared;                    /* MAP_SHARED of a file */
    bool shared_write;              /* ... of a RitterFS file open for writing (22j): writes go
                                     * to the cache's pages, msync writes them back */
    bool shared_anon;               /* anonymous MAP_SHARED (56a): one memory after a fork */
    struct anon_obj *anon;          /* (35c-2) its pages (shared_anon), from page `first` */
    struct mapping *next;           /* sorted by start */
};

static struct fpages *cache;
static struct mutex cache_lock = MUTEX_INIT("page cache");

/* A version no mapping uses any more stays cached (22g: a program started
 * again, a library loaded again, reads nothing), up to this many pages in
 * all; then the one idle the longest goes. */
#define CACHE_KEEP_PAGES 8192           /* 32 MiB, at most an eighth of the memory */
static uint64_t idle_pages, idle_clock, pages_read;

/* The process's own pages that fork shares (56a), with how many address
 * spaces hold each; a page not listed has one owner. Short sections under a
 * spinlock: the nodes are allocated and freed outside it. (35c) A page with
 * a slot that holds its content stays listed with one holder too, read-only
 * (a page read back from a slot other entries name: going out again, it
 * names that slot without being written; page_own before a write). */
struct shared_page {
    uint64_t phys;
    uint32_t holders;
    /* (35c) A slot already holding its content (pageout wrote it for one
     * holder; read-only in every holder while shared, so it cannot change),
     * held once for this: the next holder's entry names it too. */
    uint64_t slot;
    struct shared_page *next;
};
#define SHARED_BUCKETS 1024
static struct shared_page *shared_pages[SHARED_BUCKETS];
static struct spinlock shared_lock = SPINLOCK_INIT("forked pages");

static struct shared_page **shared_find(uint64_t phys)
{
    struct shared_page **link = &shared_pages[(phys / PAGE_SIZE) % SHARED_BUCKETS];
    while (*link && (*link)->phys != phys)
        link = &(*link)->next;
    return link;
}

/* One holder more (a fork); false if out of memory. */
static bool page_share(uint64_t phys)
{
    struct shared_page *n = kmalloc(sizeof(*n));
    if (!n)
        return false;
    uint64_t state = spin_lock(&shared_lock);
    struct shared_page **link = shared_find(phys);
    if (*link) {
        (*link)->holders++;
    } else {
        n->phys = phys;
        n->holders = 2;
        n->slot = SWAP_NONE;
        n->next = 0;
        *link = n;
        n = 0;
    }
    spin_unlock(&shared_lock, state);
    kfree(n);
    return true;
}

static unsigned page_holders(uint64_t phys)
{
    uint64_t state = spin_lock(&shared_lock);
    struct shared_page *e = *shared_find(phys);
    unsigned holders = e ? e->holders : 1;
    spin_unlock(&shared_lock, state);
    return holders;
}

/* One holder fewer of the listed page at *link (shared_lock held): taken
 * out of the list when one holder is left without a slot (one owner again)
 * or none is (35c); *last then if none is. The node to free after the lock
 * (its slot given back), or 0. */
static struct shared_page *one_fewer(struct shared_page **link, bool *last)
{
    struct shared_page *e = *link;
    if (--e->holders > 1 || (e->holders == 1 && e->slot != SWAP_NONE))
        return 0;
    *last = e->holders == 0;
    *link = e->next;
    return e;
}

static void node_free(struct shared_page *gone)
{
    if (gone && gone->slot != SWAP_NONE)
        swap_slot_give(gone->slot);             /* (35c: it may change again from now on) */
    kfree(gone);
}

/* One holder fewer: true if it was the last, so the caller frees the page. */
static bool page_drop(uint64_t phys)
{
    uint64_t state = spin_lock(&shared_lock);
    struct shared_page **link = shared_find(phys), *gone = 0;
    bool last = !*link;
    if (*link)
        gone = one_fewer(link, &last);
    spin_unlock(&shared_lock, state);
    node_free(gone);
    return last;
}

/* (35c) Before its one holder writes the page in place: it is the process's
 * own from now on (no longer what a slot holds). */
static void page_own(uint64_t phys)
{
    uint64_t state = spin_lock(&shared_lock);
    struct shared_page **link = shared_find(phys), *gone = *link;
    if (gone)
        *link = gone->next;
    spin_unlock(&shared_lock, state);
    node_free(gone);
}

/* (35c) A page just read back from slot, which other entries still name:
 * listed with its one holder and the slot (held once more), so it goes out
 * again without a write. False without memory (then it is not listed). */
static bool page_attach(uint64_t phys, uint64_t slot)
{
    struct shared_page *n = kmalloc(sizeof(*n));
    if (!n || !swap_slot_hold(slot)) {
        kfree(n);
        return false;
    }
    n->phys = phys;
    n->holders = 1;
    n->slot = slot;
    uint64_t state = spin_lock(&shared_lock);
    struct shared_page **link = shared_find(phys);
    n->next = *link;
    *link = n;
    spin_unlock(&shared_lock, state);
    return true;
}

/* (35c) The slot already holding a shared page's content, held once more
 * for one more entry to name it; SWAP_NONE if there is none. */
static uint64_t page_slot(uint64_t phys)
{
    uint64_t state = spin_lock(&shared_lock);
    struct shared_page *e = *shared_find(phys);
    uint64_t slot = e && e->slot != SWAP_NONE && swap_slot_hold(e->slot) ? e->slot : SWAP_NONE;
    spin_unlock(&shared_lock, state);
    return slot;
}

/* (35c) One holder's entry names slot now instead of the page (pageout):
 * the page has a holder fewer, and the slot is remembered for the others.
 * True if that was the last holder: the page is free. */
static bool page_out(uint64_t phys, uint64_t slot)
{
    uint64_t state = spin_lock(&shared_lock);
    struct shared_page **link = shared_find(phys), *gone = 0;
    bool last = !*link;
    if (*link) {
        if ((*link)->slot == SWAP_NONE && swap_slot_hold(slot))
            (*link)->slot = slot;
        gone = one_fewer(link, &last);
    }
    spin_unlock(&shared_lock, state);
    node_free(gone);
    return last;
}

/* --- (35c-2) shared anonymous memory ----------------------------------------------------
 *
 * The pages of a MAP_SHARED | MAP_ANONYMOUS mapping belong to an object that
 * every mapping of it holds (a fork's child's too): a page-table entry is a
 * view of the object's page, made at a fault. So pageout can take a page out
 * of every process at once: it marks the page leaving (a fault on it waits,
 * without the mappings' lock), clears its entry in each process that maps
 * the object, one process at a time (process_each_space), and writes it to
 * swap unless one of them used it meanwhile. Lock order: a mappings' lock,
 * then an object's; pageout holds no object's while it takes the others. */
#define ANON_SWAPPED 1                      /* page[i]: slot << 12 | ANON_SWAPPED */
#define ANON_LEAVING 2                      /* page[i]: phys | ANON_LEAVING */

struct anon_obj {
    struct mutex lock;                      /* "shared memory": page[] */
    unsigned refs;                          /* mappings and lookers holding it (atomic) */
    uint64_t count;                         /* pages */
    uint64_t *page;                         /* 0: not made yet; a page; or in swap */
    struct wait_queue leave;                /* faults wait here for a page leaving */
    struct anon_obj *next;                  /* every object (swap off) */
};
static struct anon_obj *anon_objs;
static struct spinlock anon_list = SPINLOCK_INIT("shared memories");

static struct anon_obj *anon_new(uint64_t count)
{
    struct anon_obj *o = kzalloc(sizeof(*o));
    uint64_t *page = kzalloc(count * sizeof(uint64_t));
    if (!o || !page) {
        kfree(o);
        kfree(page);
        return 0;
    }
    struct mutex init = MUTEX_INIT("shared memory");
    o->lock = init;
    o->refs = 1;
    o->count = count;
    o->page = page;
    uint64_t state = spin_lock(&anon_list);
    o->next = anon_objs;
    anon_objs = o;
    spin_unlock(&anon_list, state);
    return o;
}

static void anon_ref(struct anon_obj *o)
{
    __atomic_add_fetch(&o->refs, 1, __ATOMIC_RELAXED);
}

/* The last holder frees the pages and slots: only once no page-table entry
 * shows them any more (a mapping's entries are announced gone first). */
static void anon_unref(struct anon_obj *o)
{
    if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL))
        return;
    uint64_t state = spin_lock(&anon_list);
    for (struct anon_obj **link = &anon_objs; *link; link = &(*link)->next)
        if (*link == o) {
            *link = o->next;
            break;
        }
    spin_unlock(&anon_list, state);
    for (uint64_t i = 0; i < o->count; i++) {
        if (o->page[i] & ANON_SWAPPED)
            swap_slot_give(o->page[i] >> 12);
        else if (o->page[i])
            pmm_free_page(o->page[i]);
    }
    wait_queue_detach(&o->leave);
    kfree(o->page);
    kfree(o);
}

/* A fault on a page of shared anonymous memory with no entry: the object's
 * page (made zeroed, or read back from swap) shown at va. FAULT_ANON_WAIT
 * while it is leaving (*wait: the object, held for the waiter). */
#define FAULT_ANON_WAIT (-3)
static int anon_fault(struct anon_obj *o, uint64_t index, uint64_t space, uint64_t va,
                      unsigned access, unsigned need, bool desperate, struct anon_obj **wait);

/* Mappings placed by the kernel keep an unmapped page between them, so a
 * program running off the end of one faults instead of reaching the next. */
#define GAP PAGE_SIZE

/* (65e) The mappings' lock, taken by everything here that looks at or
 * changes the tables: copy_user's copies without the lock are stopped and
 * waited for while it is held (until 65e copy_user took it for every page:
 * 5 million times in 20 s of smpstress on 4 cores, a queue of waiters behind
 * holders preempted with it). Such a copy runs with interrupts off for one
 * page: the wait is microseconds. */
static void copies_stop(struct mmap_state *m)
{
    __atomic_add_fetch(&m->changing, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&m->copying, __ATOMIC_SEQ_CST))
        arch_cpu_relax();
}

static void copies_go(struct mmap_state *m)
{
    __atomic_sub_fetch(&m->changing, 1, __ATOMIC_RELEASE);
}

static void mm_lock(struct mmap_state *m)
{
    rwlock_write(&m->lock);
    copies_stop(m);
}

static void mm_unlock(struct mmap_state *m)
{
    copies_go(m);
    rwlock_unlock(&m->lock);
}

bool mmap_copy_begin(struct mmap_state *m)
{
    __atomic_add_fetch(&m->copying, 1, __ATOMIC_SEQ_CST);
    return !__atomic_load_n(&m->changing, __ATOMIC_SEQ_CST);
}

void mmap_copy_end(struct mmap_state *m)
{
    __atomic_sub_fetch(&m->copying, 1, __ATOMIC_RELEASE);
}

void mmap_change_begin(struct mmap_state *m)
{
    mm_lock(m);
}

void mmap_change_end(struct mmap_state *m)
{
    mm_unlock(m);
}

/* (65e-38) Readers change no page in place (a fault under it only fills an
 * empty entry), so copy_user's copies without the lock go on. */
void mmap_read_begin(struct mmap_state *m)
{
    rwlock_read(&m->lock);
}

void mmap_read_end(struct mmap_state *m)
{
    rwlock_unlock(&m->lock);
}

void mmap_init(struct mmap_state *m)
{
    struct rwlock init = RWLOCK_INIT("mappings");
    m->maps = 0;
    m->lock = init;
    m->copying = m->changing = 0;
    m->forked = false;
    m->pageable = false;
    m->pageout_at = 0;
    /* Address-space randomization (22l): this process's mappings (libraries,
     * the heap, files) start at a random page in the window's first half. */
    uint64_t r;
    random_bytes(&r, sizeof(r));
    m->start = MMAP_WINDOW_BASE + (r % ((MMAP_WINDOW_TOP - MMAP_WINDOW_BASE) / 2 / PAGE_SIZE)) * PAGE_SIZE;
}

/* The page-table access for MMAP_* access: writing or running needs
 * reading too (on both CPUs a written or executed page is readable). */
static unsigned user_access(unsigned access)
{
    unsigned a = 0;
    if (access)
        a |= USER_READ;
    if (access & MMAP_WRITE)
        a |= USER_WRITE;
    if (access & MMAP_EXEC)
        a |= USER_EXEC;
    return a;
}

/* A new page holding the file's bytes of page i (zeros past its end) in
 * *out; 0 or -ERR_*. Code is made visible to instruction fetch (ARM). */
static int64_t read_page(struct vfs *fs, uint64_t id, const struct rfs_inode *inode, uint64_t i,
                         uint64_t *out)
{
    uint64_t page = pmm_alloc_page();
    if (!page)
        return -ERR_NOMEM;
    uint8_t *v = phys_to_virt(page);
    memset(v, 0, PAGE_SIZE);
    uint64_t off = i * PAGE_SIZE;
    if (off < inode->size) {
        uint64_t n = inode->size - off < PAGE_SIZE ? inode->size - off : PAGE_SIZE;
        if (vfs_read(fs, id, inode, off, v, n) != (int64_t)n) {
            pmm_free_page(page);
            return -ERR_IO;
        }
    }
    arch_code_written(v, PAGE_SIZE);
    *out = page;
    return 0;
}

/* The cache's entry for this version of the file, with one more reference;
 * 0 if out of memory. cache_lock held. */
static void cache_free(struct fpages *f);

static struct fpages *cache_get(struct vfs *fs, uint64_t id, const struct rfs_inode *inode)
{
    for (struct fpages **link = &cache; *link;) {
        struct fpages *f = *link;
        bool same_file = f->fs == fs && f->object_id == id;
        if (same_file && f->generation == inode->generation && f->modified == inode->modified &&
            f->size == inode->size) {
            if (f->refs++ == 0)
                idle_pages -= f->loaded;            /* kept, and used again */
            return f;
        }
        if (same_file && f->refs == 0) {            /* an older version: never used again */
            *link = f->next;
            idle_pages -= f->loaded;
            cache_free(f);
            continue;
        }
        link = &f->next;
    }
    struct fpages *f = kzalloc(sizeof(*f));
    if (!f)
        return 0;
    f->count = (inode->size + PAGE_SIZE - 1) / PAGE_SIZE;
    f->phys = kzalloc(f->count * sizeof(uint64_t));
    if (!f->phys) {
        kfree(f);
        return 0;
    }
    f->fs = fs;
    f->object_id = id;
    f->generation = inode->generation;
    f->modified = inode->modified;
    f->size = inode->size;
    f->refs = 1;
    f->next = cache;
    cache = f;
    return f;
}

static void cache_free(struct fpages *f)
{
    kfree(f->written);
    for (uint64_t i = 0; i < f->count; i++)
        if (f->phys[i])
            pmm_free_page(f->phys[i]);
    kfree(f->phys);
    kfree(f);
}

/* Free idle versions, the longest idle first, until at most keep pages are
 * idle. cache_lock held. */
static void cache_trim(uint64_t keep)
{
    while (idle_pages > keep) {
        struct fpages **oldest = 0;
        for (struct fpages **link = &cache; *link; link = &(*link)->next)
            if ((*link)->refs == 0 && (!oldest || (*link)->idle_since < (*oldest)->idle_since))
                oldest = link;
        if (!oldest)
            break;
        struct fpages *f = *oldest;
        *oldest = f->next;
        idle_pages -= f->loaded;
        cache_free(f);
    }
}

/* One reference fewer; after the last the version is kept, idle, while the
 * budget allows (22g). cache_lock held. */
static void cache_put(struct fpages *f)
{
    if (--f->refs)
        return;
    f->idle_since = ++idle_clock;
    idle_pages += f->loaded;
    uint64_t keep = pmm_usable_count() / 8;
    cache_trim(keep < CACHE_KEEP_PAGES ? keep : CACHE_KEEP_PAGES);
}

void mmap_cache_drop_idle(void)
{
    mutex_lock(&cache_lock);
    cache_trim(0);
    mutex_unlock(&cache_lock);
}

void mmap_print_cache(void)
{
    uint64_t files = 0, pages = 0;
    mutex_lock(&cache_lock);
    for (struct fpages *f = cache; f; f = f->next) {
        files++;
        pages += f->loaded;
    }
    kprintf("page cache: %lu file(s), %lu page(s) (%lu kept unused), %lu page(s) read\n",
            files, pages, idle_pages, pages_read);
    mutex_unlock(&cache_lock);
}

static void file_ref(struct fpages *f)
{
    mutex_lock(&cache_lock);
    f->refs++;
    mutex_unlock(&cache_lock);
}

static void file_unref(struct fpages *f)
{
    mutex_lock(&cache_lock);
    cache_put(f);
    mutex_unlock(&cache_lock);
}

/* Unmap the pages of [s, e), a part of mapping g; the process's own pages
 * are freed, the cache's stay (a copy-on-write mapping has both: a page is
 * the cache's while it is the one the cache holds for that place). */
/* (65e) Pages taken out are announced to the other cores once per batch,
 * not once each (a 1 MiB arena freed was 256 rounds of interrupts to every
 * core running the program); a page goes back to the allocator only after
 * that, when no core can still reach it. (65e-46) A batch is a page from
 * the allocator (511 addresses, the stack's 64 for a short range), and the
 * last batch goes back after the lock is given back: on the laptop munmap
 * of 1 MiB held it 15.8 us, 6.7 of them announcing four batches and 3.2
 * giving the pages back, while seven threads of its program waited. */
#define DROP_BATCH 64
#define DROP_PAGE (PAGE_SIZE / sizeof(uint64_t))

struct drop {
    struct mmap_state *m;
    uint64_t space;
    uint64_t *freed;
    unsigned size, count;
    bool cleared;                       /* entries cleared since the last announcement */
    uint64_t page;                      /* freed's page from the allocator, or 0 (own) */
    uint64_t own[DROP_BATCH];
    /* (35c-2) Shared memory objects let go of: after the announcement too
     * (the last one frees its pages). */
    struct anon_obj *anons[8];
    unsigned anon_count;
};

/* For taking out up to `pages` pages of the space (more is fine: batches). */
static void drop_begin(struct drop *d, struct mmap_state *m, uint64_t space, uint64_t pages)
{
    d->m = m;
    d->space = space;
    d->page = pages > DROP_BATCH ? pmm_alloc_page() : 0;
    d->freed = d->page ? phys_to_virt(d->page) : d->own;
    d->size = d->page ? DROP_PAGE : DROP_BATCH;
    d->count = 0;
    d->cleared = false;
    d->anon_count = 0;
}

static void drop_announce(struct drop *d)
{
    if (d->cleared)
        paging_user_flush(d->space);
    d->cleared = false;
}

static void drop_free(struct drop *d)
{
    drop_announce(d);
    for (unsigned i = 0; i < d->count; i++)
        pmm_free_page(d->freed[i]);
    d->count = 0;
    for (unsigned i = 0; i < d->anon_count; i++)
        anon_unref(d->anons[i]);
    d->anon_count = 0;
}

/* (35c-2) A mapping's hold on its object, given up through d. */
static void drop_anon(struct drop *d, struct anon_obj *o)
{
    if (d->anon_count == sizeof(d->anons) / sizeof(d->anons[0]))
        drop_free(d);
    d->anons[d->anon_count++] = o;
}

/* After the lock is given back: the pages, then the batch's own page. */
static void drop_end(struct drop *d)
{
    drop_free(d);
    if (d->page)
        pmm_free_page(d->page);
}

/* The lock given back after the other cores were told, then the pages. */
static void mm_unlock_dropped(struct mmap_state *m, struct drop *d)
{
    drop_announce(d);
    mm_unlock(m);
    drop_end(d);
}

/* Unmap the pages of [s, e), a part of mapping g; the process's own pages
 * are freed, the cache's stay (a copy-on-write mapping has both: a page is
 * the cache's while it is the one the cache holds for that place). */
static void drop_pages(struct drop *d, const struct mapping *g, uint64_t s, uint64_t e)
{
    /* (Also without access: mprotect PROT_NONE keeps the pages that exist.) */
    for (uint64_t va = s; va < e; va += PAGE_SIZE) {
        uint64_t phys, slot;
        if (!paging_user_take(d->space, va, &phys)) {
            if (paging_user_take_swapped(d->space, va, &slot))
                swap_slot_give(slot);               /* (35b) in swap: its slot is free again */
            continue;                               /* never touched, or past the file */
        }
        d->cleared = true;
        if (g->anon)
            continue;                               /* (35c-2) the object's page */
        uint64_t index = g->first + (va - g->start) / PAGE_SIZE;
        /* Unless a forked process still has it. (65e-49) Only a program that
         * forked looks: the forked pages' lock is every process's, and was
         * taken for every page given back. (m->forked changes only under
         * the lock to write: not while this hold lasts.) */
        if (!(g->file && index < g->file->count && g->file->phys[index] == phys) &&
            (!d->m->forked || page_drop(phys))) {
            if (d->count == d->size)
                drop_free(d);
            d->freed[d->count++] = phys;
        }
    }
}

/* (65e-48) What remove_range took out, its pages still mapped: mappings
 * taken out whole (chained by next, their files still held), and up to
 * two cut down (copies as they were, for [from, to): the first and the last
 * mapping a range overlaps; one cut inside a mapping splits it). Their
 * pages go with drop_detached, holding the lock to write or (munmap) only
 * to read: a fault finds no mapping there any more, and a cut mapping and
 * its file stay as they are until the hold ends. */
struct detached {
    struct mapping *whole;
    int parts;
    struct mapping part[2];
    uint64_t from[2], to[2];
};

static void detach_part(struct detached *t, const struct mapping *g, uint64_t s, uint64_t e)
{
    if (t->parts == 2)
        panic("mmap: a range cut more than two mappings");
    t->part[t->parts] = *g;
    t->from[t->parts] = s;
    t->to[t->parts] = e;
    t->parts++;
}

static void drop_detached(struct drop *d, struct detached *t)
{
    for (int i = 0; i < t->parts; i++)
        drop_pages(d, &t->part[i], t->from[i], t->to[i]);
    t->parts = 0;
    while (t->whole) {
        struct mapping *g = t->whole;
        t->whole = g->next;
        drop_pages(d, g, g->start, g->end);
        if (g->file)
            file_unref(g->file);
        if (g->anon)
            drop_anon(d, g->anon);
        kfree(g);
    }
}

/* Take [a, b) out of the mappings into t (its pages: drop_detached); a
 * mapping overlapping it in part shrinks, or splits in two. 0, or
 * -ERR_NOMEM (for the second half of a split; nothing has changed then).
 * m->lock held to write. */
static int64_t remove_range(struct mmap_state *m, struct detached *t, uint64_t a, uint64_t b)
{
    struct mapping **link = &m->maps;
    while (*link) {
        struct mapping *g = *link;
        if (g->end <= a) {
            link = &g->next;
            continue;
        }
        if (g->start >= b)
            break;
        uint64_t s = g->start > a ? g->start : a, e = g->end < b ? g->end : b;
        if (s > g->start && e < g->end) {           /* inside it: split it in two */
            struct mapping *right = kzalloc(sizeof(*right));
            if (!right)
                return -ERR_NOMEM;
            *right = *g;
            right->start = e;
            right->first = g->first + (e - g->start) / PAGE_SIZE;
            if (g->file)
                file_ref(g->file);
            if (g->anon)
                anon_ref(g->anon);
            detach_part(t, g, s, e);
            g->end = s;
            g->next = right;
            return 0;                               /* nothing else can overlap */
        }
        if (s == g->start && e == g->end) {         /* all of it */
            *link = g->next;
            g->next = t->whole;
            t->whole = g;
            continue;
        }
        detach_part(t, g, s, e);
        if (s == g->start) {                        /* its start */
            g->first += (e - g->start) / PAGE_SIZE;
            g->start = e;
        } else {                                    /* its end */
            g->end = s;
        }
        link = &g->next;
    }
    return 0;
}

/* The lowest place in the window for length bytes with a gap on each
 * side; 0 if there is none. m->lock held. */
static uint64_t find_room_from(struct mmap_state *m, uint64_t at, uint64_t length)
{
    for (struct mapping *g = m->maps; g; g = g->next) {
        if (at + length + GAP <= g->start)
            break;
        if (g->end + GAP > at)
            at = g->end + GAP;
    }
    return at <= MMAP_WINDOW_TOP - length ? at : 0;
}

/* From the process's random start up (22l), else from the window's start. */
static uint64_t find_room(struct mmap_state *m, uint64_t length)
{
    uint64_t at = find_room_from(m, m->start, length);
    return at ? at : find_room_from(m, MMAP_WINDOW_BASE, length);
}

static void insert(struct mmap_state *m, struct mapping *g)
{
    struct mapping **link = &m->maps;
    while (*link && (*link)->start < g->start)
        link = &(*link)->next;
    g->next = *link;
    *link = g;
}

static int64_t map(struct mmap_state *m, uint64_t space, uint64_t addr, uint64_t length,
                   unsigned access, unsigned flags, const struct mmap_file *file, uint64_t offset,
                   bool anywhere);

int64_t mmap_map(struct mmap_state *m, uint64_t space, uint64_t addr, uint64_t length,
                 unsigned access, unsigned flags, const struct mmap_file *file, uint64_t offset)
{
    return map(m, space, addr, length, access, flags, file, offset, false);
}

int64_t mmap_map_segment(struct mmap_state *m, uint64_t space, uint64_t vaddr, uint64_t length,
                         unsigned access, const struct mmap_file *file, uint64_t offset)
{
    unsigned flags = MMAP_PRIVATE | MMAP_FIXED | (file ? 0 : MMAP_ANONYMOUS);
    return map(m, space, vaddr, length, access, flags, file, offset, true);
}

static int64_t map(struct mmap_state *m, uint64_t space, uint64_t addr, uint64_t length,
                   unsigned access, unsigned flags, const struct mmap_file *file, uint64_t offset,
                   bool anywhere)
{
    unsigned kind = flags & (MMAP_SHARED | MMAP_PRIVATE);
    if ((flags & ~(MMAP_SHARED | MMAP_PRIVATE | MMAP_FIXED | MMAP_ANONYMOUS)) ||
        (kind != MMAP_SHARED && kind != MMAP_PRIVATE) ||
        (access & ~(MMAP_READ | MMAP_WRITE | MMAP_EXEC)))
        return -ERR_INVAL;
    if ((access & MMAP_WRITE) && (access & MMAP_EXEC))
        return -ERR_NOTSUP;                         /* never writable and executable at once */
    if (length == 0 || length > MMAP_WINDOW_TOP - MMAP_WINDOW_BASE)
        return -ERR_INVAL;
    length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if ((flags & MMAP_FIXED) && (addr % PAGE_SIZE ||
                                 (anywhere ? addr < USER_LOAD_MIN || addr > USER_TOP - length
                                           : addr < MMAP_WINDOW_BASE || addr > MMAP_WINDOW_TOP - length)))
        return -ERR_INVAL;                          /* programs: only inside the mmap window */
    bool anonymous = flags & MMAP_ANONYMOUS;
    uint64_t pages = length / PAGE_SIZE, first = 0, count = 0, backed = 0;
    struct rfs_inode inode;
    bool cached = false, cow = false, shared_write = false;
    if (!anonymous) {
        if (!file)
            return -ERR_BADF;
        if (offset % PAGE_SIZE)
            return -ERR_INVAL;
        if (!file->readable)
            return -ERR_ACCES;
        if ((access & MMAP_WRITE) && kind == MMAP_SHARED && !file->writable)
            return -ERR_ACCES;                      /* (a descriptor open for reading) */
        if (!vfs_get_inode(file->fs, file->object_id, &inode) || inode.type != RFS_TYPE_FILE)
            return -ERR_ACCES;
        count = (inode.size + PAGE_SIZE - 1) / PAGE_SIZE;
        first = offset / PAGE_SIZE;
        /* The pages past the file's end are mapped too, but touching one is
         * SIGBUS (22e); the ones the file covers: */
        backed = first >= count ? 0 : count - first < pages ? count - first : pages;
        /* The page cache for RitterFS (whose inodes tell its versions apart):
         * read-only, the same pages for everyone; private and writable, the
         * same until a page is written. Other file systems: a copy. */
        cached = vfs_ritterfs(file->fs) != 0;
        if ((access & MMAP_WRITE) && kind == MMAP_SHARED && !cached)
            return -ERR_NOTSUP;                     /* writing back other file systems: not yet */
        cow = cached && (access & MMAP_WRITE) && kind == MMAP_PRIVATE;
        shared_write = cached && kind == MMAP_SHARED && file->writable;
    }
    /* Read the file's part before taking the mappings' lock: a program's
     * read() may hold the file system's lock while copy_user makes a page
     * of one of its mappings (mmap_fault), so never the other way round. File
     * pages are read now, not when touched: a version of a file rewritten
     * later may have its blocks freed and reused. */
    struct fpages *fp = 0;
    uint64_t *copies = 0;
    int64_t r = 0;
    if (backed && access) {
        if (cached) {
            mutex_lock(&cache_lock);
            fp = cache_get(file->fs, file->object_id, &inode);
            r = fp ? 0 : -ERR_NOMEM;
            if (!r && shared_write && !fp->written &&
                !(fp->written = kzalloc((fp->count + 7) / 8 + 1)))
                r = -ERR_NOMEM;
            for (uint64_t i = 0; !r && i < backed; i++)
                if (!fp->phys[first + i] &&
                    !(r = read_page(file->fs, file->object_id, &inode, first + i, &fp->phys[first + i]))) {
                    fp->loaded++;
                    pages_read++;
                }
            mutex_unlock(&cache_lock);
        } else {
            copies = kzalloc(backed * sizeof(uint64_t));
            r = copies ? 0 : -ERR_NOMEM;
            for (uint64_t i = 0; !r && i < backed; i++)
                r = read_page(file->fs, file->object_id, &inode, first + i, &copies[i]);
        }
    }
    struct mapping *g = r ? 0 : kzalloc(sizeof(*g));
    if (!r && !g)
        r = -ERR_NOMEM;
    /* (35c-2) Shared anonymous memory: its object. */
    struct anon_obj *anon = !r && anonymous && kind == MMAP_SHARED ? anon_new(pages) : 0;
    if (!r && anonymous && kind == MMAP_SHARED && !anon)
        r = -ERR_NOMEM;
    uint64_t at = addr;
    struct drop d;
    if (!r) {
        drop_begin(&d, m, space, flags & MMAP_FIXED ? pages : backed);
        mm_lock(m);
        struct detached t = { 0 };
        if (flags & MMAP_FIXED) {
            r = remove_range(m, &t, addr, addr + length);
            drop_detached(&d, &t);                  /* (emptied before the new pages go in) */
        } else if (!(at = find_room(m, length)))
            r = -ERR_NOMEM;
        if (r)
            mm_unlock_dropped(m, &d);
    }
    if (r) {                                        /* nothing was mapped yet */
        if (anon)
            anon_unref(anon);
        if (fp)
            file_unref(fp);
        for (uint64_t i = 0; copies && i < backed; i++)
            if (copies[i])
                pmm_free_page(copies[i]);
        kfree(copies);
        kfree(g);
        return r;
    }
    g->start = at;
    g->end = at + length;
    g->access = user_access(access);
    g->first = first;
    g->file_pages = count;
    g->anonymous = anonymous;
    g->cow = cow;
    g->shared = !anonymous && kind == MMAP_SHARED;
    g->shared_write = shared_write;
    g->shared_anon = anonymous && kind == MMAP_SHARED;
    g->anon = anon;
    g->unread = !anonymous && !access && backed;
    g->file = fp;
    /* Anonymous memory and reservations: nothing now. A file's pages: the
     * cache's (read-only while copy-on-write) or the copies. */
    uint64_t i = 0;
    for (; g->access && i < backed; i++) {
        uint64_t page = fp ? fp->phys[first + i] : copies[i];
        bool first_write = cow || shared_write;         /* (read-only until written) */
        if (!paging_user_map(space, at + i * PAGE_SIZE, page,
                             first_write ? g->access & ~USER_WRITE : g->access)) {
            r = -ERR_NOMEM;
            break;
        }
    }
    if (r) {                                        /* undo what was mapped */
        drop_pages(&d, g, g->start, g->end);
        for (; copies && i < backed; i++)           /* the copies not mapped yet */
            pmm_free_page(copies[i]);
        if (fp)
            file_unref(fp);
        mm_unlock_dropped(m, &d);
        kfree(copies);
        kfree(g);
        return r;
    }
    insert(m, g);
    mm_unlock_dropped(m, &d);
    kfree(copies);
    return (int64_t)at;
}

/* (65e-38) A fault's page put where the entry was empty, with the lock held
 * to read: another thread of the program may have put one there first
 * (paging_user_map puts it only on an empty entry). 0 when a page with the
 * access is there now; the page the fault made (ours) is then freed. */
static int place(uint64_t space, uint64_t va, uint64_t page, unsigned access, unsigned need,
                 bool ours)
{
    if (paging_user_map(space, va, page, access))
        return 0;
    uint64_t phys;
    unsigned have;
    bool there = paging_user_query(space, va, &phys, &have) && (have & need) == need;
    if (ours)
        pmm_free_page(page);
    return there ? 0 : SIGSEGV;             /* (else out of memory for its tables) */
}

#define FAULT_WRITER (-1)                   /* the fault changes a page in place */
#define FAULT_NOMEM (-2)                    /* (35b) no page: wait for pageout, then again */
#define FAULT_WAITS 60                      /* seconds without a page freed before the reserve */

/* (35b) A page for the program: none while swap is on and free memory is
 * down to its reserve (the fault waits for pageout instead), unless it has
 * waited enough (desperate); with swap on, from the top of memory (see
 * pmm_alloc_page_high). */
static uint64_t user_page(bool desperate)
{
    if (!desperate && swap_short())
        return 0;
    uint64_t page = swap_on_now() ? pmm_alloc_page_high() : pmm_alloc_page();
    swap_note();
    return page;
}

/* The fault with the lock held: to read (alone false), when it only fills
 * an empty entry, else FAULT_WRITER; to write (alone true), all of it. */
static int fault(struct mmap_state *m, uint64_t space, uint64_t va, bool write, bool exec,
                 bool alone, bool desperate, struct anon_obj **wait)
{
    unsigned need = USER_READ | (write ? USER_WRITE : 0) | (exec ? USER_EXEC : 0);
    struct mapping *g = m->maps;
    while (g && !(va >= g->start && va < g->end))
        g = g->next;
    if (!g || (g->access & need) != need)
        return SIGSEGV;                             /* not mapped, or not allowed */
    uint64_t index = g->first + (va - g->start) / PAGE_SIZE;
    if (!g->anonymous && index >= g->file_pages)
        return SIGBUS;                              /* past the end of the file */
    uint64_t phys;
    unsigned have;
    bool present = paging_user_query(space, va, &phys, &have);
    if (present && (have & need) == need)
        return 0;                                   /* another thread made it first */
    if (present && !alone)
        return FAULT_WRITER;                        /* every change of a page in place */
    if (g->anon && !present)                        /* (35c-2) shared memory: the object's */
        return anon_fault(g->anon, index, space, va, g->access, need, desperate, wait);
    uint64_t slot;
    if (!present && paging_user_swapped(space, va, &slot)) {
        /* (35b) A page pageout wrote to swap: read back, holding the lock to
         * write (pageout writes holding it too: the two never meet). */
        if (!alone)
            return FAULT_WRITER;
        uint64_t page = user_page(desperate);
        if (!page)
            return FAULT_NOMEM;
        if (!swap_read(slot, phys_to_virt(page))) {
            kprintf("  %-15sslot %lu could not be read back: the program ends\n", "swap:", slot);
            pmm_free_page(page);
            return SIGSEGV;
        }
        /* (35c) A slot other entries name (a fork's): the page stays what
         * the slot holds, read-only until it is written (page_own). */
        unsigned access = g->access;
        bool kept = m->forked && swap_slot_refs(slot) > 1 && page_attach(page, slot);
        if (kept)
            access &= ~USER_WRITE;
        if (!paging_user_swap_in(space, va, slot, page, access)) {
            if (!kept || page_drop(page))
                pmm_free_page(page);
            return SIGSEGV;
        }
        swap_slot_give(slot);
        return 0;
    }
    int signal = SIGSEGV;
    bool cache_page = present && g->file && index < g->file->count && g->file->phys[index] == phys;
    if (present && write && !cache_page && !g->shared_write) {
        /* A page of its own made read-only by a fork (56a): copied at its
         * first write while another process still holds it, else this
         * one's alone again. Shared anonymous memory stays one page. */
        if (!g->shared_anon && page_holders(phys) > 1) {
            uint64_t page = user_page(desperate);
            if (!page)
                signal = FAULT_NOMEM;
            if (page) {
                memcpy(phys_to_virt(page), phys_to_virt(phys), PAGE_SIZE);
                paging_user_unmap(space, va);
                if (paging_user_map(space, va, page, g->access)) {
                    signal = 0;
                    if (page_drop(phys))
                        pmm_free_page(phys);        /* (the others ended meanwhile) */
                } else {
                    paging_user_map(space, va, phys, have);
                    pmm_free_page(page);
                }
            }
        } else {
            if (m->forked && !g->shared_anon)
                page_own(phys);                     /* (35c: no longer what a slot holds) */
            paging_user_unmap(space, va);
            if (paging_user_map(space, va, phys, g->access))
                signal = 0;
        }
    } else if (g->anonymous && !present) {          /* its first touch: a zeroed page */
        uint64_t page = user_page(desperate);
        if (!page)
            signal = FAULT_NOMEM;
        if (page) {
            memset(phys_to_virt(page), 0, PAGE_SIZE);
            signal = place(space, va, page, g->access, need, true);
        }
    } else if (g->shared_write && g->file && index < g->file->count && g->file->phys[index]) {
        /* A shared page: the cache's own, also when written (22j); the
         * first write marks it to be written back. */
        uint64_t page = g->file->phys[index];
        unsigned now = write ? g->access : g->access & ~USER_WRITE;
        if (write) {
            mutex_lock(&cache_lock);
            g->file->written[index / 8] |= (uint8_t)(1u << (index % 8));
            mutex_unlock(&cache_lock);
        }
        if (present) {
            paging_user_unmap(space, va);
            if (paging_user_map(space, va, page, now))
                signal = 0;
        } else {
            signal = place(space, va, page, now, need, false);
        }
    } else if (g->cow && g->file && index < g->file->count && g->file->phys[index]) {
        uint64_t source = g->file->phys[index];
        if (!write && !present) {                   /* (mapped read-only at mmap) */
            signal = place(space, va, source, g->access & ~USER_WRITE, need, false);
        } else if (write) {                         /* its first write: a copy of its own */
            uint64_t page = user_page(desperate);
            if (!page)
                signal = FAULT_NOMEM;
            if (page) {
                memcpy(phys_to_virt(page), phys_to_virt(source), PAGE_SIZE);
                if (present) {
                    paging_user_unmap(space, va);
                    if (paging_user_map(space, va, page, g->access))
                        signal = 0;
                    else
                        pmm_free_page(page);
                } else {
                    signal = place(space, va, page, g->access, need, true);
                }
            }
        }
    }
    return signal;
}

static int anon_fault(struct anon_obj *o, uint64_t index, uint64_t space, uint64_t va,
                      unsigned access, unsigned need, bool desperate, struct anon_obj **wait)
{
    mutex_lock(&o->lock);
    uint64_t v = o->page[index], page = v;
    if (v & ANON_LEAVING) {
        anon_ref(o);                                /* (for the waiter) */
        *wait = o;
        mutex_unlock(&o->lock);
        return FAULT_ANON_WAIT;
    }
    if (!v || (v & ANON_SWAPPED)) {
        if (!(page = user_page(desperate))) {
            mutex_unlock(&o->lock);
            return FAULT_NOMEM;
        }
        if (!v) {
            memset(phys_to_virt(page), 0, PAGE_SIZE);
        } else if (!swap_read(v >> 12, phys_to_virt(page))) {
            kprintf("  %-15sslot %lu could not be read back: the program ends\n", "swap:", v >> 12);
            mutex_unlock(&o->lock);
            pmm_free_page(page);
            return SIGSEGV;
        } else {
            swap_slot_give(v >> 12);
        }
        o->page[index] = page;
    }
    int signal = place(space, va, page, access, need, false);
    mutex_unlock(&o->lock);
    return signal;
}

/* (65e-38) Faults of a program's threads run at once: the lock to read, and
 * only a fault that changes a page already there (copy-on-write) takes it
 * again to write and looks again. Until 65e-38 every fault held the lock
 * alone: on the laptop 8 threads faulting fresh pages made 306,000 a second
 * in all, one alone 1.69 million (they slept 49.6 s of 10 on the lock). */
int mmap_fault(struct mmap_state *m, uint64_t space, uint64_t addr, bool write, bool exec)
{
    uint64_t va = addr & ~(PAGE_SIZE - 1);
    if (paging_user_touch(space, va))
        return 0;                                   /* (35b, ARM) used again: pageout sees it */
    for (int waited = 0;; waited++) {
        bool desperate = waited >= FAULT_WAITS;
        struct anon_obj *wait = 0;
        rwlock_read(&m->lock);
        int signal = fault(m, space, va, write, exec, false, desperate, &wait);
        rwlock_unlock(&m->lock);
        if (signal == FAULT_WRITER) {
            mm_lock(m);
            signal = fault(m, space, va, write, exec, true, desperate, &wait);
            mm_unlock(m);
        }
        if (signal == FAULT_ANON_WAIT) {
            /* (35c-2) Its page is leaving (pageout takes it out of every
             * process): wait for that without the lock, then again. */
            uint64_t index = 0;
            rwlock_read(&m->lock);
            for (struct mapping *g = m->maps; g; g = g->next)
                if (va >= g->start && va < g->end && g->anon == wait)
                    index = g->first + (va - g->start) / PAGE_SIZE;
            rwlock_unlock(&m->lock);
            wait_event_until(&wait->leave, !(__atomic_load_n(&wait->page[index], __ATOMIC_ACQUIRE) &
                                             ANON_LEAVING), timer_ticks() + 10);
            anon_unref(wait);
            waited--;
            continue;
        }
        if (signal != FAULT_NOMEM)
            return signal;
        /* (35b) No page: wait for pageout without the lock (it takes the
         * lock to write), then again, as long as it frees pages (35c: while
         * pages forked processes share go out, a holder at a time, memory
         * comes free slowly); when it has nothing to write, or nothing came
         * free for a minute, from the reserve; then out of memory, as
         * without swap. */
        if (desperate)
            return SIGSEGV;
        int got = swap_wait_memory();
        if (got < 0)
            waited = FAULT_WAITS - 1;
        else if (got > 0)
            waited = -1;                            /* (the loop's ++ makes it 0) */
    }
}

/* Split the mapping that has x strictly inside it at x. m->lock held. */
static int64_t split_at(struct mmap_state *m, uint64_t x)
{
    for (struct mapping *g = m->maps; g; g = g->next) {
        if (x <= g->start || x >= g->end)
            continue;
        struct mapping *right = kzalloc(sizeof(*right));
        if (!right)
            return -ERR_NOMEM;
        *right = *g;
        right->start = x;
        right->first = g->first + (x - g->start) / PAGE_SIZE;
        if (g->file)
            file_ref(g->file);
        if (g->anon)
            anon_ref(g->anon);
        g->end = x;
        g->next = right;
        break;
    }
    return 0;
}

int64_t mmap_protect(struct mmap_state *m, uint64_t space, uint64_t addr, uint64_t length,
                     unsigned access)
{
    if (addr % PAGE_SIZE || length == 0 || length > USER_TOP || addr > USER_TOP - length ||
        (access & ~(MMAP_READ | MMAP_WRITE | MMAP_EXEC)))
        return -ERR_INVAL;
    if ((access & MMAP_WRITE) && (access & MMAP_EXEC))
        return -ERR_NOTSUP;                         /* never writable and executable at once */
    length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t end = addr + length, at = addr;
    unsigned want = user_access(access);
    int64_t r = 0;
    mm_lock(m);
    /* All of the range mapped, and the access allowed for each mapping in it. */
    for (struct mapping *g = m->maps; g && at < end && !r; g = g->next) {
        if (g->end <= at)
            continue;
        if (g->start > at)
            break;                                  /* a hole */
        if ((want & USER_WRITE) && g->file && !g->cow && !g->shared_write)
            r = g->shared ? -ERR_ACCES : -ERR_NOTSUP;   /* a file open for reading; a read-only
                                                     * private mapping of the cache's pages */
        if (want && g->unread)
            r = -ERR_NOTSUP;
        at = g->end;
    }
    if (!r && at < end)
        r = -ERR_NOMEM;                             /* part of the range is not mapped */
    if (!r)
        r = split_at(m, addr);
    if (!r)
        r = split_at(m, end);
    for (struct mapping *g = m->maps; g && !r; g = g->next) {
        if (g->start < addr || g->end > end)
            continue;
        for (uint64_t va = g->start; va < g->end; va += PAGE_SIZE) {
            uint64_t phys;
            unsigned have;
            if (!paging_user_query(space, va, &phys, &have))
                continue;                           /* not made yet: made with the new access */
            uint64_t index = g->first + (va - g->start) / PAGE_SIZE;
            bool cached = g->file && index < g->file->count && g->file->phys[index] == phys;
            unsigned now = cached ? want & ~USER_WRITE : want;    /* copy-on-write stays */
            if (!cached && !g->shared_anon && page_holders(phys) > 1)
                now = want & ~USER_WRITE;           /* (a page a fork shares: the same, 56a) */
            if (now == have)
                continue;
            if ((now & USER_WRITE) && !(have & USER_WRITE) && !cached && m->forked &&
                !g->shared_anon)
                page_own(phys);                     /* (35c: no longer what a slot holds) */
            if ((now & USER_EXEC) && !(have & USER_EXEC) && !cached)
                arch_code_written(phys_to_virt(phys), PAGE_SIZE);  /* code it may have written */
            paging_user_unmap(space, va);
            paging_user_map(space, va, phys, now);
        }
        g->access = want;
    }
    mm_unlock(m);
    return r;
}

/* Write the written pages of f in [from, to) (page numbers) back to its file,
 * commit, and give f the file's new version as its key, so later mappings of
 * the file share these pages (22j). 0 or -ERR_IO. */
static int64_t write_back(struct fpages *f, uint64_t from, uint64_t to)
{
    bool any = false;
    int rc = RFS_OK;
    for (uint64_t i = from; i < to && i < f->count && rc == RFS_OK; i++) {
        if (!f->written || !(f->written[i / 8] & (1u << (i % 8))) || !f->phys[i])
            continue;
        uint64_t off = i * PAGE_SIZE;
        if (off >= f->size)
            break;
        uint64_t n = f->size - off < PAGE_SIZE ? f->size - off : PAGE_SIZE;
        rc = vfs_write(f->fs, f->object_id, off, phys_to_virt(f->phys[i]), n);
        any = true;
    }
    if (any && rc == RFS_OK)
        rc = vfs_commit(f->fs);
    struct rfs_inode inode;
    if (any && rc == RFS_OK && vfs_get_inode(f->fs, f->object_id, &inode) && inode.size == f->size) {
        mutex_lock(&cache_lock);
        f->generation = inode.generation;
        f->modified = inode.modified;
        mutex_unlock(&cache_lock);
    }
    return rc == RFS_OK ? 0 : -ERR_IO;
}

/* Write back what was written through the shared writable mappings in
 * [a, b). The file system takes its own locks (and a read() may hold one into
 * mmap_fault), so not under m->lock: the files are held meanwhile. */
static int64_t sync_range(struct mmap_state *m, uint64_t a, uint64_t b)
{
    struct { struct fpages *f; uint64_t from, to; } work[16];
    int64_t result = 0;
    while (a < b) {
        int n = 0;
        uint64_t next = b;
        mmap_read_begin(m);                         /* (65e-46: it only looks) */
        for (struct mapping *g = m->maps; g && n < 16; g = g->next) {
            if (g->end <= a || g->start >= b || !g->shared_write || !g->file)
                continue;
            uint64_t s0 = g->start > a ? g->start : a, e = g->end < b ? g->end : b;
            file_ref(g->file);
            work[n].f = g->file;
            work[n].from = g->first + (s0 - g->start) / PAGE_SIZE;
            work[n].to = g->first + (e - g->start) / PAGE_SIZE;
            n++;
            next = e;
        }
        mmap_read_end(m);
        for (int i = 0; i < n; i++) {
            int64_t r = write_back(work[i].f, work[i].from, work[i].to);
            if (r && !result)
                result = r;
            file_unref(work[i].f);
        }
        a = n == 16 ? next : b;                     /* (more than 16: from where it stopped) */
    }
    return result;
}

int64_t mmap_sync(struct mmap_state *m, uint64_t addr, uint64_t length)
{
    if (addr % PAGE_SIZE || length > USER_TOP || addr > USER_TOP - length)
        return -ERR_INVAL;
    return sync_range(m, addr, addr + ((length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)));
}

int64_t mmap_unmap(struct mmap_state *m, uint64_t space, uint64_t addr, uint64_t length)
{
    if (addr % PAGE_SIZE || length == 0 || length > USER_TOP || addr > USER_TOP - length)
        return -ERR_INVAL;
    length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    sync_range(m, addr, addr + length);             /* (what was written reaches the file) */
    struct drop d;
    struct detached t = { 0 };
    drop_begin(&d, m, space, length / PAGE_SIZE);
    mm_lock(m);
    int64_t r = remove_range(m, &t, addr, addr + length);
    /* (65e-48) The pages taken out holding the lock only to read, as Linux
     * does since 4.20: the program's faults elsewhere go on meanwhile (on
     * the laptop eight faulting threads spun 9.4 s of 80 waiting for munmap's
     * 8.5 us holds to write). Copies without the lock stay stopped until
     * the other cores were told (an ARM core with interrupts off is not
     * waited for). */
    rwlock_downgrade(&m->lock);
    drop_detached(&d, &t);
    drop_announce(&d);
    copies_go(m);
    rwlock_unlock(&m->lock);
    drop_end(&d);
    return r;
}

int64_t mmap_fork(struct mmap_state *m, uint64_t space, struct mmap_state *child,
                  uint64_t child_space)
{
    int64_t r = 0;
    bool protected = false;
    mm_lock(m);
    m->forked = child->forked = true;           /* (65e-49) both may share pages from now on */
    child->pageable = m->pageable;              /* (35c) the same program, loaded */
    child->start = m->start;
    struct mapping **tail = &child->maps;
    for (struct mapping *g = m->maps; g && !r; g = g->next) {
        struct mapping *c = kzalloc(sizeof(*c));
        if (!c) {
            r = -ERR_NOMEM;
            break;
        }
        *c = *g;
        c->next = 0;
        if (g->file)
            file_ref(g->file);
        *tail = c;
        tail = &c->next;
        if (g->anon) {
            anon_ref(g->anon);              /* (35c-2) the same object: the child's faults show it */
            continue;
        }
        for (uint64_t va = g->start; va < g->end; va += PAGE_SIZE) {
            uint64_t phys, slot;
            unsigned have;
            bool present = paging_user_query(space, va, &phys, &have);
            if (!present && paging_user_swapped(space, va, &slot)) {
                /* (35c) In swap: the child's entry names the same slot.
                 * (35b: read back first; still so if the slot cannot be
                 * held once more.) */
                if (swap_slot_hold(slot)) {
                    if (paging_user_put_swapped(child_space, va, slot))
                        continue;
                    swap_slot_give(slot);
                    r = -ERR_NOMEM;
                    break;
                }
                phys = pmm_alloc_page();
                if (!phys || !swap_read(slot, phys_to_virt(phys)) ||
                    !paging_user_swap_in(space, va, slot, phys, g->access)) {
                    if (phys)
                        pmm_free_page(phys);
                    r = -ERR_NOMEM;
                    break;
                }
                swap_slot_give(slot);
                have = g->access;
                present = true;
            }
            if (!present)
                continue;
            uint64_t index = g->first + (va - g->start) / PAGE_SIZE;
            bool cache_page = g->file && index < g->file->count && g->file->phys[index] == phys;
            unsigned give = have;
            if (!cache_page) {
                if (!page_share(phys)) {
                    r = -ERR_NOMEM;
                    break;
                }
                if (!g->shared_anon && (have & USER_WRITE)) {
                    give = have & ~USER_WRITE;      /* copy-on-write from now on, in both */
                    paging_user_protect(space, va, give);
                    protected = true;
                }
            }
            if (!paging_user_map(child_space, va, phys, give)) {
                if (!cache_page)
                    page_drop(phys);                /* (the parent's again alone) */
                r = -ERR_NOMEM;
                break;
            }
        }
    }
    /* (65e) One announcement for every page made read-only, before the
     * child can run (it starts once fork returns): until then the parent's
     * other threads may still write through what their cores kept, as
     * during the call itself. */
    if (protected)
        paging_user_flush(space);
    mm_unlock(m);
    return r;
}

/* (35b) Whose pages pageout may write: the process's own (not the cache's,
 * not memory shared), and none it may not touch (PROT_NONE). */
static bool pageable_mapping(const struct mapping *g)
{
    return g->access && !g->shared && (g->anon || (!g->shared_anon && (g->anonymous || g->cow)));
}

#define PAGEOUT_LOOKS 2048                  /* pages looked at in one call, at most */

/* (35c-2) Shared memory pages pageout found unused in a process: taken out
 * of every process afterwards (mmap_pageout_shared), with no mappings' lock
 * held. Only pageout's thread uses them. */
#define SHARED_BATCH 8
static struct { struct anon_obj *o; uint64_t index; } shared_out[SHARED_BATCH];
static unsigned shared_count;

struct anon_evict {
    struct anon_obj *o;
    uint64_t index, phys;
    bool used;                              /* a process used it since the last look */
};

/* For process_each_space: the page's entries in this process cleared (the
 * other cores told), and whether it was used. */
static void anon_unmap_in(struct mmap_state *m, uint64_t space, void *arg)
{
    struct anon_evict *e = arg;
    bool cleared = false;
    mm_lock(m);
    for (struct mapping *g = m->maps; g; g = g->next) {
        uint64_t pages = (g->end - g->start) / PAGE_SIZE, phys;
        if (g->anon != e->o || e->index < g->first || e->index >= g->first + pages)
            continue;
        uint64_t va = g->start + (e->index - g->first) * PAGE_SIZE;
        if (paging_user_young(space, va))
            e->used = true;
        if (paging_user_take(space, va, &phys))
            cleared = true;
    }
    if (cleared)
        paging_user_flush(space);
    mm_unlock(m);
}

/* Page index of o out of every process, then to swap unless one used it
 * (its entries are gone either way: the next use faults it back in). */
static bool anon_evict(struct anon_obj *o, uint64_t index)
{
    mutex_lock(&o->lock);
    uint64_t v = o->page[index];
    bool there = v && !(v & (ANON_SWAPPED | ANON_LEAVING));
    if (there)
        __atomic_store_n(&o->page[index], v | ANON_LEAVING, __ATOMIC_RELEASE);
    mutex_unlock(&o->lock);
    if (!there)
        return false;
    struct anon_evict e = { o, index, v, false };
    process_each_space(anon_unmap_in, &e);
    uint64_t slot = e.used ? SWAP_NONE : swap_slot_take();
    bool out = slot != SWAP_NONE && swap_write(slot, phys_to_virt(v));
    if (!out && slot != SWAP_NONE)
        swap_slot_give(slot);
    mutex_lock(&o->lock);
    __atomic_store_n(&o->page[index], out ? slot << 12 | ANON_SWAPPED : v, __ATOMIC_RELEASE);
    mutex_unlock(&o->lock);
    wait_queue_wake_all(&o->leave);
    if (out)
        pmm_free_page(v);
    return out;
}

uint64_t mmap_pageout_shared(void)
{
    uint64_t written = 0;
    for (unsigned i = 0; i < shared_count; i++) {
        written += anon_evict(shared_out[i].o, shared_out[i].index);
        anon_unref(shared_out[i].o);
    }
    shared_count = 0;
    return written;
}

uint64_t mmap_pageout(struct mmap_state *m, uint64_t space, uint64_t want, uint64_t *seen)
{
    struct { uint64_t va, phys, slot; unsigned access; bool write; } out[32];
    unsigned n = 0;
    uint64_t looked = 0, written = 0;
    bool full = false, cleared = false;
    if (want > 32)
        want = 32;
    mm_lock(m);
    *seen = 0;
    if (!m->pageable) {
        mm_unlock(m);
        return 0;
    }
    uint64_t start = m->pageout_at;
    for (int pass = 0; pass < 2 && n < want && looked < PAGEOUT_LOOKS && !full; pass++) {
        uint64_t from = pass ? 0 : start, to = pass ? start : USER_TOP;
        for (struct mapping *g = m->maps; g && n < want && looked < PAGEOUT_LOOKS && !full;
             g = g->next) {
            if (g->end <= from || g->start >= to || !pageable_mapping(g))
                continue;
            uint64_t end = g->end < to ? g->end : to;
            for (uint64_t va = g->start > from ? g->start : from;
                 va < end && n < want && looked < PAGEOUT_LOOKS; va += PAGE_SIZE) {
                uint64_t phys;
                unsigned have;
                m->pageout_at = va + PAGE_SIZE;
                if (!paging_user_query(space, va, &phys, &have))
                    continue;                       /* not made yet, or in swap */
                looked++;
                uint64_t index = g->first + (va - g->start) / PAGE_SIZE;
                if (g->file && index < g->file->count && g->file->phys[index] == phys)
                    continue;                       /* the cache's: not the process's own */
                if (paging_user_young(space, va)) {
                    cleared = true;                 /* used: looked at again next time */
                    continue;
                }
                if (g->anon) {                      /* (35c-2) out of every process, after */
                    if (shared_count < SHARED_BATCH) {
                        anon_ref(g->anon);
                        shared_out[shared_count].o = g->anon;
                        shared_out[shared_count++].index = index;
                    }
                    continue;
                }
                /* (35c) A page forked processes share: in swap already for
                 * another holder, or written once for all of them. */
                uint64_t slot = m->forked ? page_slot(phys) : SWAP_NONE;
                bool write = slot == SWAP_NONE;
                if (write && (slot = swap_slot_take()) == SWAP_NONE) {
                    full = true;                    /* swap is full (or off) */
                    break;
                }
                if (!paging_user_evict(space, va, phys, slot)) {
                    swap_slot_give(slot);
                    continue;
                }
                out[n].va = va, out[n].phys = phys, out[n].slot = slot, out[n].access = have;
                out[n].write = write;
                n++;
            }
        }
        if (pass == 0 && n < want && looked < PAGEOUT_LOOKS && !full)
            m->pageout_at = 0;                      /* to the end: from the start again */
    }
    /* Every core forgets the pages taken out before their content is read
     * (a write through a translation a core kept would be lost), and the
     * cleared bits, so a use sets them again. */
    if (n || cleared)
        paging_user_flush(space);
    for (unsigned i = 0; i < n; i++) {
        if (!out[i].write || swap_write(out[i].slot, phys_to_virt(out[i].phys))) {
            if (!m->forked || page_out(out[i].phys, out[i].slot))
                pmm_free_page(out[i].phys);         /* (else other holders keep it) */
            written++;
        } else {                                    /* (not written: the page stays) */
            paging_user_swap_in(space, out[i].va, out[i].slot, out[i].phys, out[i].access);
            swap_slot_give(out[i].slot);
        }
    }
    mm_unlock(m);
    *seen = looked;
    return written;
}

int64_t mmap_swap_in_all(struct mmap_state *m, uint64_t space)
{
    int64_t r = 0;
    mm_lock(m);
    for (struct mapping *g = m->maps; g && !r; g = g->next) {
        for (uint64_t va = g->start; va < g->end && !r; va += PAGE_SIZE) {
            uint64_t slot, page;
            if (!paging_user_swapped(space, va, &slot))
                continue;
            if (!(page = pmm_alloc_page_high()))
                r = -ERR_NOMEM;
            else if (!swap_read(slot, phys_to_virt(page)) ||
                     !paging_user_swap_in(space, va, slot, page, g->access))
                r = -ERR_IO;
            if (r && page)
                pmm_free_page(page);
            if (!r)
                swap_slot_give(slot);
        }
    }
    mm_unlock(m);
    return r;
}

int64_t mmap_anon_swap_in_all(void)
{
    int64_t r = 0;
    for (;;) {
        struct anon_obj *o = 0;
        uint64_t state = spin_lock(&anon_list);
        for (struct anon_obj *q = anon_objs; q && !o; q = q->next) {
            for (uint64_t i = 0; i < q->count && !o; i++)
                if (__atomic_load_n(&q->page[i], __ATOMIC_ACQUIRE) & ANON_SWAPPED)
                    o = q;
        }
        if (o)
            anon_ref(o);
        spin_unlock(&anon_list, state);
        if (!o || r)
            break;
        mutex_lock(&o->lock);
        for (uint64_t i = 0; i < o->count && !r; i++) {
            uint64_t v = o->page[i], page;
            if (!(v & ANON_SWAPPED))
                continue;
            if (!(page = pmm_alloc_page_high()))
                r = -ERR_NOMEM;
            else if (!swap_read(v >> 12, phys_to_virt(page)))
                r = -ERR_IO;
            if (r && page)
                pmm_free_page(page);
            if (!r) {
                swap_slot_give(v >> 12);
                o->page[i] = page;
            }
        }
        mutex_unlock(&o->lock);
        anon_unref(o);
    }
    return r;
}

void mmap_release_all(struct mmap_state *m, uint64_t space)
{
    sync_range(m, 0, USER_TOP);                     /* the program's writes reach their files */
    struct drop d;
    drop_begin(&d, m, space, DROP_PAGE);
    mm_lock(m);
    while (m->maps) {
        struct mapping *g = m->maps;
        m->maps = g->next;
        drop_pages(&d, g, g->start, g->end);
        if (g->file)
            file_unref(g->file);
        if (g->anon)
            drop_anon(&d, g->anon);
        kfree(g);
    }
    mm_unlock_dropped(m, &d);
}

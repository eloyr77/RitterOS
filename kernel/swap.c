/*
 * Swap (see swap.h). While on, the swap file's data blocks are known as runs
 * of consecutive disk blocks (read once from its tree, which never changes:
 * the file is fixed), slot s being content block s; a bit per slot says
 * whether it holds a page. Pages go to the disk through the partition's own
 * block device, past RitterFS (its cache never holds these blocks: they are
 * forgotten when swap goes on, and nothing reads them through it).
 */
#include "swap.h"
#include "heap.h"
#include "kprintf.h"
#include "mounts.h"
#include "paging.h"
#include "pmm.h"
#include "process.h"
#include "sched.h"
#include "timer.h"
#include "random.h"
#include "ritterfs.h"
#include "string.h"
#include "sync.h"
#include "syscalls.h"
#include "vfs.h"
#include "xts.h"

_Static_assert(PAGE_SIZE == RFS_BLOCK_SIZE, "a slot is a page and a block");

#define SETTINGS_MAX 1024

struct run {
    uint64_t slot, block, count;    /* slots [slot, slot + count): slot + k at block + k * step */
    int64_t step;                   /* 1, or -1 (see ritterfs_fixed_runs) */
};

static struct {
    struct ritterfs *fs;            /* 0: off */
    uint64_t object;
    uint64_t pages;                 /* slots */
    struct run *runs;
    uint64_t run_count, run_room;
    uint64_t *used;                 /* a bit per slot */
    uint16_t *refs;                 /* (35c) entries naming each slot (forks share them) */
    uint64_t used_count, cursor;    /* under slot_lock (cursor: a word) */
    bool draining;                  /* (35b) swap off reads pages back: no slot given out */
    struct xts xts;                 /* this time's key */
    uint64_t written, read;         /* pages since the boot */
} swap;

static struct mutex swap_lock = MUTEX_INIT("swap");             /* on, off */
static struct spinlock slot_lock = SPINLOCK_INIT("swap slots");
/* (35b) Writes go through one page made when swap first goes on (pageout
 * writes when memory is short: no allocation then); swap_off takes it too,
 * so no write is under way while the runs go. */
static struct mutex write_lock = MUTEX_INIT("swap write");
static uint8_t *bounce;
static bool self_testing;
static uint64_t low_mark, high_mark, reserve;   /* (35b) pages: see set_marks */
static void set_marks(void);
static void pageout(void *arg);
static struct thread *pageout_thread;

static bool add_run(void *ctx, uint64_t index, uint64_t block, uint64_t count, int step)
{
    (void)ctx;
    if (swap.run_count == swap.run_room) {
        uint64_t room = swap.run_room ? swap.run_room * 2 : 16;
        struct run *grown = kmalloc(room * sizeof(*grown));
        if (!grown)
            return false;
        if (swap.run_count)
            memcpy(grown, swap.runs, swap.run_count * sizeof(*grown));
        kfree(swap.runs);
        swap.runs = grown;
        swap.run_room = room;
    }
    swap.runs[swap.run_count++] = (struct run){ index, block, count, step };
    return true;
}

/* The disk block of a slot (the runs are in slot order). */
static uint64_t slot_block(uint64_t slot)
{
    uint64_t low = 0, high = swap.run_count;
    while (high - low > 1) {
        uint64_t mid = (low + high) / 2;
        if (swap.runs[mid].slot <= slot)
            low = mid;
        else
            high = mid;
    }
    const struct run *r = &swap.runs[low];
    return r->block + (uint64_t)((int64_t)(slot - r->slot) * r->step);
}

static bool block_io(uint64_t block, void *buf, bool write)
{
    struct ritterfs *fs = swap.fs;
    uint64_t lba = block * fs->sectors_per_block;
    return write ? fs->dev->write(fs->dev, lba, fs->sectors_per_block, buf)
                 : fs->dev->read(fs->dev, lba, fs->sectors_per_block, buf);
}

bool swap_write(uint64_t slot, const void *page)
{
    mutex_lock(&write_lock);
    bool ok = swap.fs && slot < swap.pages;
    if (ok) {
        xts_encrypt(&swap.xts, slot, page, bounce, PAGE_SIZE);
        ok = block_io(slot_block(slot), bounce, true);
    }
    mutex_unlock(&write_lock);
    if (ok)
        __atomic_fetch_add(&swap.written, 1, __ATOMIC_RELAXED);
    return ok;
}

bool swap_read(uint64_t slot, void *page)
{
    if (!swap.fs || slot >= swap.pages || !block_io(slot_block(slot), page, false))
        return false;
    xts_decrypt(&swap.xts, slot, page, page, PAGE_SIZE);
    __atomic_fetch_add(&swap.read, 1, __ATOMIC_RELAXED);
    return true;
}

uint64_t swap_slot_take(void)
{
    uint64_t flags = spin_lock(&slot_lock);
    uint64_t slot = SWAP_NONE, words = (swap.pages + 63) / 64;
    if (swap.fs && !swap.draining && swap.used_count < swap.pages) {
        for (uint64_t k = 0; k < words; k++) {          /* next fit, a word at a time */
            uint64_t w = (swap.cursor + k) % words, free = ~swap.used[w];
            if (w == words - 1 && swap.pages % 64)
                free &= (1ULL << (swap.pages % 64)) - 1;    /* (past the last slot) */
            if (free) {
                uint64_t bit = (uint64_t)__builtin_ctzll(free);
                swap.used[w] |= 1ULL << bit;
                swap.refs[w * 64 + bit] = 1;
                swap.used_count++;
                swap.cursor = w;
                slot = w * 64 + bit;
                break;
            }
        }
    }
    spin_unlock(&slot_lock, flags);
    return slot;
}

void swap_slot_give(uint64_t slot)
{
    uint64_t flags = spin_lock(&slot_lock);
    if (swap.fs && slot < swap.pages && (swap.used[slot / 64] & (1ULL << (slot % 64))) &&
        --swap.refs[slot] == 0) {
        swap.used[slot / 64] &= ~(1ULL << (slot % 64));
        swap.used_count--;
    }
    spin_unlock(&slot_lock, flags);
}

unsigned swap_slot_refs(uint64_t slot)
{
    uint64_t flags = spin_lock(&slot_lock);
    unsigned refs = swap.fs && slot < swap.pages ? swap.refs[slot] : 0;
    spin_unlock(&slot_lock, flags);
    return refs;
}

bool swap_slot_hold(uint64_t slot)
{
    uint64_t flags = spin_lock(&slot_lock);
    bool ok = swap.fs && slot < swap.pages && (swap.used[slot / 64] & (1ULL << (slot % 64))) &&
              swap.refs[slot] < UINT16_MAX;
    if (ok)
        swap.refs[slot]++;
    spin_unlock(&slot_lock, flags);
    return ok;
}

/* A slot's own test content: no two pages alike. */
static void pattern(uint64_t *words, uint64_t slot)
{
    for (uint64_t i = 0; i < PAGE_SIZE / 8; i++)
        words[i] = (slot + 1) * 0x9E3779B97F4A7C15ULL ^ i * 0xD1B54A32D192ED03ULL;
}

/* (`selftest`) Every slot written with its own content and read back; on
 * the disk none may hold what was written (it is encrypted). The writes
 * also show that the runs are the file's: the tests' host check then finds
 * every block of /system/swap changed and every other block intact. */
static void self_test(void)
{
    uint64_t *page = kmalloc(PAGE_SIZE), *back = kmalloc(PAGE_SIZE);
    uint64_t good = 0, plain = 0, failed = 0;
    if (!page || !back) {
        kfree(page);
        kfree(back);
        return;
    }
    for (uint64_t s = 0; s < swap.pages; s++) {
        pattern(page, s);
        if (!swap_write(s, page))
            failed++;
    }
    for (uint64_t s = 0; s < swap.pages; s++) {
        pattern(page, s);
        if (block_io(slot_block(s), back, false) && !memcmp(back, page, PAGE_SIZE))
            plain++;
        if (swap_read(s, back) && !memcmp(back, page, PAGE_SIZE))
            good++;
    }
    kfree(page);
    kfree(back);
    kprintf("  %-15sself-test: %lu of %lu pages read back as written (%lu writes failed), "
            "%lu readable on the disk\n", "swap:", good, swap.pages, failed, plain);
}

static void forget_runs(void)
{
    kfree(swap.runs);
    kfree(swap.used);
    kfree(swap.refs);
    swap.refs = 0;
    swap.runs = 0;
    swap.used = 0;
    swap.run_count = swap.run_room = 0;
}

int64_t swap_on(void)
{
    mutex_lock(&swap_lock);
    int64_t error = 0;
    struct vfs *root = mounts_root();
    struct ritterfs *fs = root ? vfs_ritterfs(root) : 0;
    uint64_t id;
    struct rfs_inode ino;
    if (swap.fs)
        goto done;                                  /* (on already) */
    if (!fs || !ritterfs_lookup(fs, SWAP_FILE, &id, &ino)) {
        error = -ERR_NOENT;
        goto done;
    }
    if (ino.type != RFS_TYPE_FILE) {
        error = -ERR_INVAL;
        goto done;
    }
    int r = ritterfs_make_fixed(fs, id);
    if (r != RFS_OK || !ritterfs_get_inode(fs, id, &ino)) {
        /* NOT_SUPPORTED: holes, a part block, or blocks a snapshot or a
         * copy shares (written again, the other would change too). */
        error = r == RFS_ERR_NOT_SUPPORTED ? -ERR_INVAL : r == RFS_ERR_READ_ONLY ? -ERR_ROFS
                                                                                 : -ERR_IO;
        kprintf("  %-15s%s cannot be the swap file: %s\n", "swap:", SWAP_FILE,
                r == RFS_ERR_NOT_SUPPORTED ? "holes, a part block, or blocks a snapshot or a "
                                             "copy shares" : ritterfs_error(r));
        goto done;
    }
    swap.pages = ino.size / PAGE_SIZE;
    swap.used = kzalloc((swap.pages + 63) / 64 * 8);
    swap.refs = kzalloc(swap.pages * sizeof(uint16_t));
    if (!bounce)
        bounce = kmalloc(PAGE_SIZE);                /* (kept from then on) */
    if (!swap.used || !swap.refs || !bounce || !ritterfs_fixed_runs(fs, id, add_run, 0)) {
        forget_runs();
        error = -ERR_IO;
        goto done;
    }
    for (uint64_t s = 0; s < swap.pages; s++)
        rfs_cache_forget(&fs->cache, slot_block(s));
    uint8_t key[64];
    random_bytes(key, sizeof(key));
    xts_init(&swap.xts, key);
    memset(key, 0, sizeof(key));
    rfs_lock(fs);
    fs->busy_object = id;                           /* never removed while on */
    rfs_unlock(fs);
    swap.object = id;
    swap.used_count = swap.cursor = 0;
    set_marks();
    swap.fs = fs;
    kprintf("  %-15son, %s: %lu MiB (%lu pages in %lu run(s)), XTS-AES-256 with a new key\n",
            "swap:", SWAP_FILE, (uint64_t)(swap.pages * PAGE_SIZE >> 20), swap.pages, swap.run_count);
    if (self_testing)
        self_test();
    if (!pageout_thread && !(pageout_thread = thread_create("pageout", pageout, 0)))
        kprintf("  %-15sno thread pageout: nothing will be paged out\n", "swap:");
done:
    mutex_unlock(&swap_lock);
    return error;
}

int64_t swap_off(void)
{
    mutex_lock(&swap_lock);
    int64_t error = 0;
    struct ritterfs *fs = swap.fs;
    /* (35b) Pages in swap come back first, if memory has room for them (and
     * its reserve twice): no slot is given out meanwhile. A process being
     * made or ending may still hold some: a few times, then refused. */
    uint64_t used = __atomic_load_n(&swap.used_count, __ATOMIC_RELAXED);
    if (fs && used && used + 2 * reserve > pmm_free_estimate()) {
        mutex_unlock(&swap_lock);
        return -ERR_NOMEM;
    }
    if (fs && used) {
        uint64_t flags = spin_lock(&slot_lock);
        swap.draining = true;
        spin_unlock(&slot_lock, flags);
        for (int i = 0; i < 4 && __atomic_load_n(&swap.used_count, __ATOMIC_RELAXED) && !error;
             i++)
            error = process_swap_in_all();
        kprintf("  %-15s%lu pages read back from swap%s\n", "swap:",
                used - __atomic_load_n(&swap.used_count, __ATOMIC_RELAXED),
                error ? ": not all (out of memory, or a read failed)" : "");
    }
    mutex_lock(&write_lock);                        /* (no write under way) */
    uint64_t flags = spin_lock(&slot_lock);
    if (fs && (error || swap.used_count))
        error = error ? error : -ERR_BUSY;          /* pages are in it still */
    else
        swap.fs = 0;                                /* (no slot is taken from now on) */
    swap.draining = false;
    spin_unlock(&slot_lock, flags);
    mutex_unlock(&write_lock);
    if (fs && !error) {
        rfs_lock(fs);
        fs->busy_object = 0;
        rfs_unlock(fs);
        forget_runs();
        memset(&swap.xts, 0, sizeof(swap.xts));     /* the key goes */
        swap.pages = 0;
        kprintf("  %-15soff\n", "swap:");
    }
    mutex_unlock(&swap_lock);
    return error;
}

/* --- (35b) paging out ----------------------------------------------------------- */

/* Free pages: below low, pageout writes programs' pages to swap until high
 * again; below reserve, a program's fault waits for it (the kernel's own
 * allocations keep the reserve). An 64th of memory, between 1 and 64 MiB. */
static struct wait_queue pageout_wake;          /* pageout waits here */
static struct wait_queue pages_freed;           /* faults wait here for pageout */
static uint64_t pageout_stuck;                  /* pageout found nothing it could write */

#define PAGEOUT_BATCH 32                        /* pages written in one look at a process */

static void set_marks(void)
{
    uint64_t usable = pmm_usable_count(), low = usable / 64;
    low = low < 256 ? 256 : low > 16384 ? 16384 : low;
    low_mark = low;
    high_mark = 2 * low;
    reserve = low / 4;
}

bool swap_on_now(void)
{
    return swap.fs != 0;
}

bool swap_short(void)
{
    return swap.fs && pmm_free_estimate() < reserve;
}

void swap_note(void)
{
    if (swap.fs && pmm_free_estimate() < low_mark)
        wait_queue_wake_all(&pageout_wake);
}

int swap_wait_memory(void)
{
    if (!swap.fs)
        return -1;
    uint64_t stuck = __atomic_load_n(&pageout_stuck, __ATOMIC_ACQUIRE);
    wait_queue_wake_all(&pageout_wake);
    wait_event_until(&pages_freed, pmm_free_estimate() >= reserve ||
                                   __atomic_load_n(&pageout_stuck, __ATOMIC_ACQUIRE) != stuck,
                     timer_ticks() + 100);
    if (__atomic_load_n(&pageout_stuck, __ATOMIC_ACQUIRE) != stuck)
        return -1;
    return pmm_free_estimate() >= reserve ? 1 : 0;
}

/* The thread pageout: while free memory is below the low mark, a batch of
 * one process's pages at a time (process_pageout), process after process,
 * until the high mark. Having looked at twice as many pages as the machine
 * has without writing one (a first look only clears a page's used bit), or
 * finding no process, it says it is stuck: the faults waiting for it take
 * the reserve, then fail as without swap. */
static void pageout(void *arg)
{
    (void)arg;
    for (;;) {
        wait_event_until(&pageout_wake, swap.fs && pmm_free_estimate() < low_mark,
                         timer_ticks() + 100);
        uint64_t idle_looks = 0, limit = 2 * pmm_usable_count();
        unsigned empty = 0;
        bool stuck = false;
        while (swap.fs && pmm_free_estimate() < high_mark && !stuck) {
            uint64_t looked;
            uint64_t n = process_pageout(PAGEOUT_BATCH, &looked);
            if (n) {
                idle_looks = empty = 0;
                wait_queue_wake_all(&pages_freed);
            } else {
                idle_looks += looked;
                empty = looked ? 0 : empty + 1;
            }
            stuck = idle_looks > limit || empty > 1024;
        }
        if (stuck) {
            __atomic_fetch_add(&pageout_stuck, 1, __ATOMIC_RELEASE);
            wait_queue_wake_all(&pages_freed);
            wait_event_until(&pageout_wake, false, timer_ticks() + 10);   /* (a breath) */
        }
    }
}

void swap_status(struct sys_swap *out)
{
    mutex_lock(&swap_lock);
    uint64_t flags = spin_lock(&slot_lock);
    out->on = swap.fs != 0;
    out->pages = swap.pages;
    out->used = swap.used_count;
    spin_unlock(&slot_lock, flags);
    out->written = __atomic_load_n(&swap.written, __ATOMIC_RELAXED);
    out->read = __atomic_load_n(&swap.read, __ATOMIC_RELAXED);
    mutex_unlock(&swap_lock);
}

void swap_print(void)
{
    struct sys_swap s;
    swap_status(&s);
    if (!s.on)
        kprintf("swap: off\n");
    else
        kprintf("swap: %lu of %lu MiB used (%lu of %lu pages), %lu pages written, %lu read\n",
                (uint64_t)(s.used * PAGE_SIZE >> 20), (uint64_t)(s.pages * PAGE_SIZE >> 20), s.used, s.pages, s.written,
                s.read);
}

static bool blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* Whether text (key = value lines, # to the line's end a comment) says
 * on = yes; the last `on` counts. */
static bool settings_on(const char *text, size_t n)
{
    bool on = false;
    for (size_t at = 0; at < n;) {
        size_t end = at;
        while (end < n && text[end] != '\n')
            end++;
        size_t i = at;
        while (i < end && blank(text[i]))
            i++;
        if (end - i > 2 && !memcmp(text + i, "on", 2) && (blank(text[i + 2]) ||
                                                          text[i + 2] == '=')) {
            i += 2;
            while (i < end && blank(text[i]))
                i++;
            if (i < end && text[i] == '=') {
                i++;
                while (i < end && blank(text[i]))
                    i++;
                size_t stop = i;
                while (stop < end && !blank(text[stop]) && text[stop] != '#')
                    stop++;
                on = stop - i == 3 && !memcmp(text + i, "yes", 3);
            }
        }
        at = end + 1;
    }
    return on;
}

void swap_settings(bool selftest)
{
    static char text[SETTINGS_MAX];
    self_testing = selftest;
    struct vfs *fs = mounts_root();
    uint64_t id;
    struct rfs_inode inode;
    int64_t got = fs && vfs_lookup(fs, SWAP_SETTINGS, &id, &inode)
                  ? vfs_read(fs, id, &inode, 0, text, sizeof(text)) : -1;
    if (got <= 0 || !settings_on(text, (size_t)got))
        return;                                     /* off: what it always is first */
    int64_t error = swap_on();
    if (error)
        kprintf("  %-15snot on (%s says on = yes): %s %s\n", "swap:", SWAP_SETTINGS, SWAP_FILE,
                error == -ERR_NOENT ? "is missing" : "cannot be used");
}

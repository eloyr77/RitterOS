#include "rfs_dirindex.h"
#include "heap.h"
#include "ritterfs.h"
#include "string.h"

#define INDEXES     8
#define MIN_SIZE    4096            /* smaller directories: one block, searched */

struct ientry {
    uint64_t id;
    uint32_t hash;
    uint32_t name;                  /* offset in names */
    uint8_t len;
    int32_t next;                   /* in the hash chain, -1 ends it */
};

struct dir_index {
    uint64_t dir_id;                /* 0: unused */
    uint64_t last_use;
    uint32_t count, buckets;
    struct ientry *entries;
    int32_t *heads;
    char *names;
    uint32_t names_used, capacity, names_size;
};

struct rfs_dirindex {
    struct dir_index dirs[INDEXES];
    uint64_t clock, lookups, builds;
};

static uint32_t hash(const char *name, size_t len)
{
    uint32_t h = 2166136261u;       /* FNV-1a */
    for (size_t i = 0; i < len; i++)
        h = (h ^ (uint8_t)name[i]) * 16777619u;
    return h;
}

static void release(struct dir_index *d)
{
    kfree(d->entries);
    kfree(d->heads);
    kfree(d->names);
    memset(d, 0, sizeof(*d));
}

static bool add(void *ctx, const char *name, size_t len, uint64_t id, uint8_t type)
{
    struct dir_index *d = ctx;
    (void)type;
    if (d->count == d->capacity || d->names_used + len > d->names_size)
        return false;               /* cannot happen: sized from the directory */
    struct ientry *e = &d->entries[d->count];
    e->id = id;
    e->hash = hash(name, len);
    e->name = d->names_used;
    e->len = (uint8_t)len;
    memcpy(d->names + d->names_used, name, len);
    d->names_used += (uint32_t)len;
    uint32_t b = e->hash % d->buckets;
    e->next = d->heads[b];
    d->heads[b] = (int32_t)d->count++;
    return true;
}

/* Read the directory's entries into d; false if out of memory or the
 * directory cannot be read. Entries take at least 16 bytes each, names at
 * most the directory's size. */
static bool build(struct ritterfs *fs, struct dir_index *d, uint64_t dir_id,
                  const struct rfs_inode *dir)
{
    d->capacity = (uint32_t)(dir->size / 16 + 1);
    d->names_size = (uint32_t)dir->size;
    d->buckets = d->capacity * 2 + 1;
    d->entries = kmalloc(d->capacity * sizeof(struct ientry));
    d->heads = kmalloc(d->buckets * sizeof(int32_t));
    d->names = kmalloc(d->names_size ? d->names_size : 1);
    if (!d->entries || !d->heads || !d->names) {
        release(d);
        return false;
    }
    for (uint32_t i = 0; i < d->buckets; i++)
        d->heads[i] = -1;
    if (!rfs_iterate_dir(&fs->vol, dir_id, dir, add, d)) {
        release(d);
        return false;
    }
    d->dir_id = dir_id;
    return true;
}

static int find_locked(struct ritterfs *fs, uint64_t dir_id, const struct rfs_inode *dir,
                       const char *name, size_t len, uint64_t *id)
{
    if (!fs->dirindex && !(fs->dirindex = kzalloc(sizeof(*fs->dirindex))))
        return -1;
    struct rfs_dirindex *x = fs->dirindex;
    struct dir_index *d = 0, *oldest = &x->dirs[0];
    for (int i = 0; i < INDEXES && !d; i++) {
        if (x->dirs[i].dir_id == dir_id)
            d = &x->dirs[i];
        else if (x->dirs[i].last_use < oldest->last_use)
            oldest = &x->dirs[i];
    }
    if (!d) {
        release(oldest);
        if (!build(fs, oldest, dir_id, dir))
            return -1;
        d = oldest;
        x->builds++;
    }
    d->last_use = ++x->clock;
    x->lookups++;
    uint32_t h = hash(name, len);
    for (int32_t i = d->heads[h % d->buckets]; i >= 0; i = d->entries[i].next) {
        const struct ientry *e = &d->entries[i];
        if (e->hash == h && e->len == len && memcmp(d->names + e->name, name, len) == 0) {
            *id = e->id;
            return 1;
        }
    }
    *id = 0;
    return 0;
}

/* (65e-36) Under its own mutex ("ritterfs index"): lookups of several
 * readers at once (a build reads the directory: a mutex, not a spinlock). */
int rfs_dirindex_find(struct rfs_volume *vol, uint64_t dir_id, const struct rfs_inode *dir,
                      const char *name, size_t len, uint64_t *id)
{
    struct ritterfs *fs = (struct ritterfs *)vol;
    if (dir->size < MIN_SIZE)
        return -1;
    mutex_lock(&fs->index_lock);
    int r = find_locked(fs, dir_id, dir, name, len, id);
    mutex_unlock(&fs->index_lock);
    return r;
}

void rfs_dirindex_forget(struct ritterfs *fs, uint64_t dir_id)
{
    mutex_lock(&fs->index_lock);
    for (int i = 0; fs->dirindex && i < INDEXES; i++)
        if (fs->dirindex->dirs[i].dir_id == dir_id)
            release(&fs->dirindex->dirs[i]);
    mutex_unlock(&fs->index_lock);
}

void rfs_dirindex_counts(struct ritterfs *fs, uint64_t *lookups, uint64_t *builds)
{
    *lookups = fs->dirindex ? fs->dirindex->lookups : 0;
    *builds = fs->dirindex ? fs->dirindex->builds : 0;
}

#include "rfs_cache.h"
#include "heap.h"
#include "rfs_format.h"
#include "string.h"

#define BUCKETS (RFS_CACHE_BLOCKS * 2)

static uint32_t hash(uint64_t block)
{
    return (uint32_t)((block * 0x9E3779B97F4A7C15ULL) >> 32) % BUCKETS;
}

/* The cache's memory, made at its first use: allocated outside the lock
 * (8 MiB), put in place under it unless another thread was first. */
static bool ensure(struct rfs_cache *c)
{
    if (__atomic_load_n(&c->data, __ATOMIC_ACQUIRE))
        return true;
    uint8_t *data = kmalloc((uint64_t)RFS_CACHE_BLOCKS * RFS_BLOCK_SIZE);
    uint64_t *block = kmalloc(RFS_CACHE_BLOCKS * sizeof(uint64_t));
    uint64_t *last_use = kmalloc(RFS_CACHE_BLOCKS * sizeof(uint64_t));
    uint32_t *checksum = kmalloc(RFS_CACHE_BLOCKS * sizeof(uint32_t));
    bool *verified = kmalloc(RFS_CACHE_BLOCKS * sizeof(bool));
    int32_t *next = kmalloc(RFS_CACHE_BLOCKS * sizeof(int32_t));
    int32_t *bucket = kmalloc(BUCKETS * sizeof(int32_t));
    bool ok = data && block && last_use && checksum && verified && next && bucket;
    if (ok)
        for (int i = 0; i < BUCKETS; i++)
            bucket[i] = -1;
    uint64_t state = spin_lock(&c->lock);
    bool mine = ok && !c->data;
    if (mine) {
        c->block = block;
        c->last_use = last_use;
        c->checksum = checksum;
        c->verified = verified;
        c->next = next;
        c->bucket = bucket;
        c->used = 0;
        __atomic_store_n(&c->data, data, __ATOMIC_RELEASE);
    }
    bool have = c->data != 0;
    spin_unlock(&c->lock, state);
    if (!mine) {
        kfree(data);
        kfree(block);
        kfree(last_use);
        kfree(checksum);
        kfree(verified);
        kfree(next);
        kfree(bucket);
    }
    return have;
}

static int32_t find(const struct rfs_cache *c, uint64_t block)
{
    for (int32_t i = c->bucket[hash(block)]; i >= 0; i = c->next[i])
        if (c->block[i] == block)
            return i;
    return -1;
}

static void unlink(struct rfs_cache *c, int32_t slot)
{
    int32_t *link = &c->bucket[hash(c->block[slot])];
    while (*link != slot)
        link = &c->next[*link];
    *link = c->next[slot];
}

bool rfs_cache_get(struct rfs_cache *c, uint64_t block, void *buf)
{
    uint64_t state = spin_lock(&c->lock);
    int32_t slot = c->data ? find(c, block) : -1;
    if (slot < 0) {
        c->misses++;
        spin_unlock(&c->lock, state);
        return false;
    }
    c->hits++;
    c->last_use[slot] = ++c->clock;
    memcpy(buf, c->data + (uint64_t)slot * RFS_BLOCK_SIZE, RFS_BLOCK_SIZE);
    spin_unlock(&c->lock, state);
    return true;
}

void rfs_cache_put(struct rfs_cache *c, uint64_t block, const void *data)
{
    if (!ensure(c))
        return;
    uint64_t state = spin_lock(&c->lock);
    if (find(c, block) >= 0) {                      /* (another reader put it meanwhile) */
        spin_unlock(&c->lock, state);
        return;
    }
    int32_t slot;
    if (c->used < RFS_CACHE_BLOCKS) {
        slot = (int32_t)c->used++;
    } else {
        /* Full: drop the least recently used block. A linear search, but
         * only on a miss, which costs a disk read anyway. */
        slot = 0;
        for (int32_t i = 1; i < RFS_CACHE_BLOCKS; i++)
            if (c->last_use[i] < c->last_use[slot])
                slot = i;
        if (c->block[slot] != UINT64_MAX) {     /* not a forgotten, free slot */
            unlink(c, slot);
            c->dropped++;
        }
    }
    c->block[slot] = block;
    c->last_use[slot] = ++c->clock;
    c->verified[slot] = false;
    memcpy(c->data + (uint64_t)slot * RFS_BLOCK_SIZE, data, RFS_BLOCK_SIZE);
    uint32_t h = hash(block);
    c->next[slot] = c->bucket[h];
    c->bucket[h] = slot;
    spin_unlock(&c->lock, state);
}

bool rfs_cache_get_verified(struct rfs_cache *c, uint64_t block, uint32_t checksum, void *buf)
{
    uint64_t state = spin_lock(&c->lock);
    int32_t slot = c->data ? find(c, block) : -1;
    bool ok = slot >= 0 && c->verified[slot] && c->checksum[slot] == checksum;
    if (ok) {
        c->hits++;
        c->last_use[slot] = ++c->clock;
        memcpy(buf, c->data + (uint64_t)slot * RFS_BLOCK_SIZE, RFS_BLOCK_SIZE);
    }
    spin_unlock(&c->lock, state);
    return ok;
}

void rfs_cache_verified(struct rfs_cache *c, uint64_t block, uint32_t checksum)
{
    uint64_t state = spin_lock(&c->lock);
    int32_t slot = c->data ? find(c, block) : -1;
    if (slot >= 0) {
        c->verified[slot] = true;
        c->checksum[slot] = checksum;
    }
    spin_unlock(&c->lock, state);
}

void rfs_cache_forget(struct rfs_cache *c, uint64_t block)
{
    uint64_t state = spin_lock(&c->lock);
    int32_t slot = c->data ? find(c, block) : -1;
    if (slot >= 0) {
        unlink(c, slot);
        c->last_use[slot] = 0;      /* reused first */
        c->block[slot] = UINT64_MAX;
    }
    spin_unlock(&c->lock, state);
}

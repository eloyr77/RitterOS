#include "ritterfs.h"
#include "rfs_dirindex.h"
#include "arch.h"
#include "crc32.h"
#include "devpath.h"
#include "heap.h"
#include "kprintf.h"
#include "string.h"
#include <stdarg.h>
#include <stddef.h>

#define FIRST_ALLOCATABLE RFS_FIRST_ALLOCATABLE

/* The shared reader's buffers come from the heap. */
void *rfs_alloc_block(void)
{
    return kmalloc(RFS_BLOCK_SIZE);
}

void rfs_free_block(void *block)
{
    kfree(block);
}

static bool read_raw(struct ritterfs *fs, uint64_t block, void *buf)
{
    return fs->dev->read(fs->dev, block * fs->sectors_per_block, fs->sectors_per_block, buf);
}

/* struct rfs_volume callbacks: the volume is the first member of struct
 * ritterfs. Normal reading goes through the cache; the check and scrub call
 * read_raw themselves. */
static bool volume_read(struct rfs_volume *vol, uint64_t block, void *buf)
{
    struct ritterfs *fs = (struct ritterfs *)vol;
    if (fs->view)
        return read_raw(fs, block, buf);            /* snapshot views: no cache */
    if (rfs_cache_get(&fs->cache, block, buf))
        return true;
    if (!read_raw(fs, block, buf))
        return false;
    rfs_cache_put(&fs->cache, block, buf);
    return true;
}

static bool volume_read_verified(struct rfs_volume *vol, uint64_t block, uint32_t checksum,
                                 void *buf)
{
    return rfs_cache_get_verified(&((struct ritterfs *)vol)->cache, block, checksum, buf);
}

static void volume_verified(struct rfs_volume *vol, uint64_t block, uint32_t checksum)
{
    rfs_cache_verified(&((struct ritterfs *)vol)->cache, block, checksum);
}

void ritterfs_cache_report(struct ritterfs *fs)
{
    const struct rfs_cache *c = &fs->cache;
    uint64_t reads = c->hits + c->misses;
    kprintf("  RitterFS cache: %lu block reads, %lu from the cache (%lu%%), %lu from the disk; "
            "%u of %u blocks held, %lu dropped\n", reads, c->hits,
            reads ? c->hits * 100 / reads : 0, c->misses, c->used, RFS_CACHE_BLOCKS,
            c->dropped);
}

static const uint8_t *volume_pending(struct rfs_volume *vol, uint64_t object_id, uint64_t index)
{
    return rfs_dirty_block((struct ritterfs *)vol, object_id, index);
}

/* Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm). */
static void format_time(uint64_t ns, char *out, size_t size)
{
    int64_t secs = (int64_t)(ns / 1000000000ULL);
    int64_t days = secs / 86400, rem = secs % 86400;
    int64_t z = days + 719468, era = z / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    int64_t y = yoe + era * 400 + (m <= 2);
    ksnprintf(out, size, "%04ld-%02ld-%02ld %02ld:%02ld:%02ld UTC", y, m, d, rem / 3600,
              rem / 60 % 60, rem % 60);
}

struct ritterfs *ritterfs_mount(struct block_device *dev)
{
    if (crc32c("123456789", 9) != 0xE3069283u) {
        kprintf("  RitterFS:      CRC-32C self-test failed\n");
        return 0;
    }
    if (dev->block_size > RFS_BLOCK_SIZE || RFS_BLOCK_SIZE % dev->block_size) {
        kprintf("  RitterFS:      %s has unsupported %u-byte blocks\n", dev->name,
                dev->block_size);
        return 0;
    }
    struct ritterfs *fs = kzalloc(sizeof(*fs));
    if (!fs)
        return 0;
    fs->vfs.kind = VFS_RITTERFS;
    fs->dev = dev;
    fs->sectors_per_block = RFS_BLOCK_SIZE / dev->block_size;
    fs->lock = (struct rwlock)RWLOCK_INIT("ritterfs");
    fs->index_lock = (struct mutex)MUTEX_INIT("ritterfs index");
    fs->cache.lock = (struct spinlock)SPINLOCK_INIT("ritterfs cache");
    fs->vol.read = volume_read;
    fs->vol.pending = volume_pending;
    fs->vol.read_verified = volume_read_verified;
    fs->vol.find_entry = rfs_dirindex_find;
    fs->next_free_id = RFS_OBJ_ROOT + 1;
    fs->vol.verified = volume_verified;
    fs->vol.capacity = dev->block_count / fs->sectors_per_block;
    if (!rfs_open(&fs->vol)) {
        kprintf("  RitterFS:      %s holds no valid RitterFS superblock\n", dev->name);
        kfree(fs);
        return 0;
    }

    fs->committed = fs->vol.sb;
    fs->committed_next_free_id = fs->next_free_id;
    char uuid[37], created[32], committed[32];
    guid_to_text(fs->vol.sb.uuid, uuid);
    format_time(fs->vol.sb.created_time, created, sizeof(created));
    format_time(fs->vol.sb.commit_time, committed, sizeof(committed));
    kprintf("  RitterFS:      %s '%.64s', UUID %s\n", dev->name, fs->vol.sb.label, uuid);
    kprintf("                 generation %lu (slot %c), %lu blocks of 4 KiB (%lu MiB), "
            "%lu used\n", fs->vol.sb.generation, fs->vol.slot == RFS_SLOT_A ? 'A' : 'B',
            fs->vol.sb.block_count, fs->vol.sb.block_count / 256, fs->vol.sb.used_blocks);
    kprintf("                 created %s, last commit %s\n", created, committed);
    return fs;
}

/* --- consistency check ------------------------------------------------------- */

struct check {
    struct ritterfs *fs;            /* the state being walked: live, or a snapshot's view */
    uint8_t *seen;                  /* one bit per block: reached from the superblock */
    uint8_t *state_seen;            /* the same for the state being walked: a block used
                                     * twice in one state is an error, by two states not,
                                     * nor by two copies (born up to the pinned generation) */
    uint64_t pinned;                /* the live file system's pinned generation */
    struct ritterfs *live;
    uint64_t shared;                /* blocks the live state uses more than once (copies) */
    uint64_t blocks;
    unsigned errors;
    uint8_t *types;                 /* per object: inode type */
    uint32_t *links;                /* per object: link_count from the inode */
    uint32_t *refs;                 /* per object: directory entries referring to it */
    uint64_t objects;               /* size of the three arrays */
    uint64_t dir;                   /* directory being checked */
    uint64_t fixed_blocks;          /* (35a) data blocks of the fixed file being walked */
};

__attribute__((format(printf, 2, 3)))
static void problem(struct check *c, const char *fmt, ...)
{
    if (c->errors++ < 8) {
        char message[160];
        va_list ap;
        va_start(ap, fmt);
        kvsnprintf(message, sizeof(message), fmt, ap);
        va_end(ap);
        kprintf("                 problem: %s\n", message);
    }
}

static bool count_entry(void *ctx, const char *name, size_t len, uint64_t id, uint8_t type)
{
    (void)name, (void)len, (void)id, (void)type;
    (*(uint64_t *)ctx)++;
    return true;
}

static bool check_entry(void *ctx, const char *name, size_t len, uint64_t id, uint8_t type)
{
    struct check *c = ctx;
    bool bad_name = (len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.');
    for (size_t i = 0; i < len; i++)
        bad_name |= name[i] == '/' || name[i] == 0;
    if (bad_name)
        problem(c, "directory %lu: invalid name of length %lu", c->dir, len);
    if (id >= c->objects || c->types[id] == RFS_TYPE_FREE || id == RFS_OBJ_ROOT ||
        id <= RFS_OBJ_BITMAP)
        problem(c, "directory %lu refers to object %lu", c->dir, id);
    else if (c->types[id] != type)
        problem(c, "directory %lu: entry for object %lu has the wrong type", c->dir, id);
    else
        c->refs[id]++;
    return true;
}


static void claim(struct check *c, uint64_t block, uint64_t object, uint64_t birth)
{
    if (block < FIRST_ALLOCATABLE || block >= c->fs->vol.sb.block_count) {
        problem(c, "object %lu uses block %lu outside the allocatable area", object, block);
        return;
    }
    uint8_t bitmask = (uint8_t)(1 << (block % 8));
    if (c->state_seen[block / 8] & bitmask) {
        if (!c->pinned || birth > c->pinned)
            problem(c, "block %lu is used twice (again by object %lu)", block, object);
        else if (c->fs == c->live)
            c->shared++;
    }
    c->state_seen[block / 8] |= bitmask;
    if (!(c->seen[block / 8] & bitmask))
        c->blocks++;
    c->seen[block / 8] |= bitmask;
}

/* Verify one pointer: claim its blocks and check every copy. (35a) A fixed
 * file's data block (fixed) is only claimed: it is written in place. */
static bool check_pointer(struct check *c, const struct rfs_pointer *p, bool meta, bool fixed,
                          uint64_t object, void *buf)
{
    if (meta && !p->address2)
        problem(c, "object %lu: metadata block %lu has no second copy", object, p->address);
    if (p->birth > c->fs->vol.sb.generation)
        problem(c, "object %lu: block born in a future generation %lu", object, p->birth);
    if (fixed) {
        claim(c, p->address, object, p->birth);
        if (p->address2)
            problem(c, "object %lu: fixed data block %lu has a second copy", object, p->address);
        c->fixed_blocks++;
        return true;
    }
    bool good = false;
    uint64_t copies[2] = { p->address, p->address2 };
    for (int i = 0; i < 2 && copies[i]; i++) {
        claim(c, copies[i], object, p->birth);
        if (read_raw(c->fs, copies[i], buf) && crc32c(buf, RFS_BLOCK_SIZE) == p->checksum)
            good = true;
        else
            problem(c, "object %lu: copy at block %lu has a bad checksum", object, copies[i]);
    }
    return good && rfs_read_pointer(&c->fs->vol, p, buf);
}

static void check_tree(struct check *c, const struct rfs_pointer *p, int level, bool meta,
                       bool fixed, uint64_t object)
{
    if (p->address == 0) {
        if (meta)
            problem(c, "object %lu: hole in metadata (level %lu)", object, (uint64_t)level);
        return;
    }
    uint8_t *buf = kmalloc(RFS_BLOCK_SIZE);
    if (!buf || !check_pointer(c, p, meta || level > 0, fixed && level == 0, object, buf)) {
        kfree(buf);
        return;
    }
    if (level > 0) {
        for (int i = 0; i < RFS_POINTERS_PER_BLOCK; i++) {
            struct rfs_pointer child;
            memcpy(&child, buf + i * 32, sizeof(child));
            if (child.address || child.address2)
                check_tree(c, &child, level - 1, meta, fixed, object);
        }
    }
    kfree(buf);
}

static void check_object(struct check *c, const struct rfs_inode *ino, uint64_t object)
{
    bool fixed = ino->flags & RFS_FLAG_FIXED;
    if (fixed && (ino->type != RFS_TYPE_FILE || (ino->flags & RFS_FLAG_INLINE) ||
                  ino->size == 0 || ino->size % RFS_BLOCK_SIZE)) {
        problem(c, "object %lu: fixed, but not a file of whole blocks", object);
        fixed = false;
    }
    c->fixed_blocks = 0;
    if (!(ino->flags & RFS_FLAG_INLINE))
        check_tree(c, &ino->root, ino->height, rfs_is_metadata(ino->type), fixed, object);
    if (fixed && c->fixed_blocks != ino->size / RFS_BLOCK_SIZE)
        problem(c, "object %lu: fixed file with holes (%lu of %lu blocks)", object,
                c->fixed_blocks, ino->size / RFS_BLOCK_SIZE);
}

/* (35a) The fixed file's leaves in order, joined into runs. */
struct run_walk {
    ritterfs_run_callback run;
    void *ctx;
    uint64_t index, block, count;   /* the run so far */
    int step;                       /* 1 or -1 once it has two blocks */
    bool ok;
};

static void run_leaf(struct run_walk *w, uint64_t index, uint64_t block)
{
    if (w->count && index == w->index + w->count) {
        uint64_t last = w->block + (w->count - 1) * (uint64_t)(int64_t)w->step;
        int step = block == last + 1 ? 1 : block == last - 1 ? -1 : 0;
        if (step && (w->count == 1 || step == w->step)) {
            w->step = step;
            w->count++;
            return;
        }
    }
    if (w->count && w->ok)
        w->ok = w->run(w->ctx, w->index, w->block, w->count, w->step);
    w->index = index;
    w->block = block;
    w->count = 1;
    w->step = 1;
}

static void run_tree(struct ritterfs *fs, struct run_walk *w, const struct rfs_pointer *p,
                     int level, uint64_t base, uint64_t count)
{
    if (!w->ok || !p->address) {
        w->ok = false;                              /* (a fixed file has no holes) */
        return;
    }
    if (level == 0) {
        run_leaf(w, base, p->address);
        return;
    }
    uint8_t *buf = kmalloc(RFS_BLOCK_SIZE);
    if (!buf || !rfs_read_pointer(&fs->vol, p, buf)) {
        w->ok = false;
        kfree(buf);
        return;
    }
    uint64_t under = 1ULL << (7 * (level - 1));
    for (uint64_t i = 0; w->ok && i < RFS_POINTERS_PER_BLOCK && base + i * under < count; i++) {
        struct rfs_pointer child;
        memcpy(&child, buf + i * 32, sizeof(child));
        run_tree(fs, w, &child, level - 1, base + i * under, count);
    }
    kfree(buf);
}

bool ritterfs_fixed_runs(struct ritterfs *fs, uint64_t object_id, ritterfs_run_callback run,
                         void *ctx)
{
    struct run_walk w = { run, ctx, 0, 0, 0, 1, true };
    uint64_t hold = rfs_read_lock(fs);
    struct rfs_inode ino;
    if (!rfs_get_inode(&fs->vol, object_id, &ino) || !(ino.flags & RFS_FLAG_FIXED))
        w.ok = false;
    else
        run_tree(fs, &w, &ino.root, ino.height, 0, ino.size / RFS_BLOCK_SIZE);
    rfs_read_unlock(fs, hold);
    if (w.ok && w.count)
        w.ok = run(ctx, w.index, w.block, w.count, w.step);
    return w.ok;
}

bool rfs_read_raw(struct ritterfs *fs, uint64_t block, void *buf)
{
    return read_raw(fs, block, buf);
}

struct snapshot_walk {
    struct check *c;
    uint64_t snapshots;
};

/* Walk one snapshot's state like the live one (trees, checksums, copies). */
static bool check_snapshot(void *ctx, const char *name, uint64_t generation, uint64_t created)
{
    struct snapshot_walk *w = ctx;
    struct check *c = w->c;
    (void)created;
    struct ritterfs *live = c->fs, *view = ritterfs_snapshot_view(live, name);
    if (!view) {
        problem(c, "snapshot '%s' (generation %lu) cannot be read", name, generation);
        return true;
    }
    w->snapshots++;
    memset(c->state_seen, 0, (live->vol.sb.block_count + 7) / 8);
    c->fs = view;
    check_object(c, &view->vol.sb.table, RFS_OBJ_TABLE);
    uint64_t slots = view->vol.sb.table.size / sizeof(struct rfs_inode);
    for (uint64_t id = 2; id < slots; id++) {
        struct rfs_inode ino;
        if (rfs_get_inode(&view->vol, id, &ino) && ino.type != RFS_TYPE_FREE)
            check_object(c, &ino, id);
    }
    c->fs = live;
    return true;
}

static void check_attributes(struct check *c, uint64_t *records, uint64_t *objects)
{
    struct ritterfs *fs = c->fs;
    uint64_t id = fs->vol.sb.attribute_object, size;
    struct rfs_inode ino;
    if (id >= c->objects || c->types[id] != RFS_TYPE_TABLE) {
        problem(c, "attribute table (object %lu) is missing", id);
        return;
    }
    uint8_t *table = rfs_table_content(fs, id, &ino, &size);
    uint8_t *has = kzalloc(c->objects);
    if (!table || !has) {
        problem(c, "attribute table (object %lu) cannot be read", id);
        kfree(table);
        kfree(has);
        return;
    }
    struct rfs_attr_record r;
    uint64_t at = 0;
    for (; rfs_attr_next(table, size, &at, &r); at += r.size) {
        if (!r.object_id)
            continue;
        const char *key = (const char *)table + at + sizeof(r);
        if (sizeof(r) + r.key_length + r.value_length > r.size ||
            !rfs_attr_key_valid(key, r.key_length) || r.value_length > RFS_ATTR_VALUE_MAX) {
            problem(c, "attribute record at byte %lu is malformed", at);
            continue;
        }
        if (r.object_id >= c->objects || (c->types[r.object_id] != RFS_TYPE_FILE &&
                                           c->types[r.object_id] != RFS_TYPE_DIR)) {
            problem(c, "attribute '%.*s' of object %lu, which does not exist", (int)r.key_length,
                    key, r.object_id);
            continue;
        }
        struct rfs_attr_record o;
        for (uint64_t k = 0; rfs_attr_next(table, size, &k, &o) && k < at; k += o.size)
            if (o.object_id == r.object_id && o.key_length == r.key_length &&
                memcmp(table + k + sizeof(o), key, r.key_length) == 0)
                problem(c, "object %lu has attribute '%.*s' twice", r.object_id,
                        (int)r.key_length, key);
        (*records)++;
        if (!has[r.object_id]) {
            has[r.object_id] = 1;
            (*objects)++;
        }
    }
    if (at != size)
        problem(c, "attribute table is malformed at byte %lu (of %lu)", at, size);
    kfree(table);
    kfree(has);
}

static bool ritterfs_check_locked(struct ritterfs *fs)
{
    if (rfs_has_changes(fs)) {
        kprintf("                 check skipped: uncommitted changes\n");
        return false;
    }
    struct check c = {0};
    c.fs = c.live = fs;
    c.pinned = fs->vol.sb.pinned_generation;
    uint64_t bitmap_bytes = (fs->vol.sb.block_count + 7) / 8;
    uint64_t slots = fs->vol.sb.table.size / sizeof(struct rfs_inode);
    c.objects = slots;
    c.seen = kzalloc(bitmap_bytes);
    c.state_seen = kzalloc(bitmap_bytes);
    c.types = kzalloc(slots);
    c.links = kzalloc(slots * sizeof(uint32_t));
    c.refs = kzalloc(slots * sizeof(uint32_t));
    uint8_t *buf = kmalloc(RFS_BLOCK_SIZE);
    if (!c.seen || !c.state_seen || !c.types || !c.links || !c.refs || !buf) {
        kfree(c.seen);
        kfree(c.state_seen);
        kfree(c.types);
        kfree(c.links);
        kfree(c.refs);
        kfree(buf);
        return false;
    }

    /* 1. Every object's tree, starting with the object table itself. */
    uint64_t objects = 1, root_entries = 0;
    check_object(&c, &fs->vol.sb.table, RFS_OBJ_TABLE);
    struct rfs_inode ino, bitmap = {0}, root = {0};
    for (uint64_t id = 2; id < slots; id++) {
        if (!ritterfs_get_inode(fs, id, &ino) || ino.type == RFS_TYPE_FREE)
            continue;
        c.types[id] = (uint8_t)ino.type;
        c.links[id] = ino.link_count;
        if (id >= fs->vol.sb.next_object_id)
            problem(&c, "object %lu is in use but not below next_object_id %lu", id,
                    fs->vol.sb.next_object_id);
        if (ino.mode && (!(ino.mode & RFS_MODE_SET) ||
                         (ino.mode & ~(RFS_MODE_SET | RFS_MODE_BITS))))
            problem(&c, "object %lu: invalid mode 0x%lX", id, (uint64_t)ino.mode);
        if (ino.type == RFS_TYPE_SYMLINK && (ino.size == 0 || ino.size > 255))
            problem(&c, "object %lu: a symbolic link of %lu bytes", id, ino.size);
        if (ino.type == RFS_TYPE_DIR && id != RFS_OBJ_ROOT && ino.link_count != 1)
            problem(&c, "directory %lu: link count %u (a directory has one name)", id,
                    ino.link_count);
        objects++;
        check_object(&c, &ino, id);
        if (id == RFS_OBJ_BITMAP)
            bitmap = ino;
        if (id == RFS_OBJ_ROOT)
            root = ino;
    }
    if (bitmap.type != RFS_TYPE_TABLE || bitmap.size != bitmap_bytes)
        problem(&c, "bitmap (object %d) is missing or has the wrong size %lu", RFS_OBJ_BITMAP,
                bitmap.size);
    if (root.type != RFS_TYPE_DIR)
        problem(&c, "root directory (object %d) is missing, type %u", RFS_OBJ_ROOT, root.type);

    /* 1b. Every snapshot's state: its object table and objects, through a
     *     view; blocks it shares with others are fine. */
    struct snapshot_walk walk = { &c, 0 };
    uint64_t live_blocks = c.blocks;
    ritterfs_snapshot_list(fs, check_snapshot, &walk);
    uint64_t kept = c.blocks - live_blocks;         /* reached only through snapshots */

    /* 2. The bitmap must mark exactly the reserved and the reached blocks. */
    for (uint64_t b = 0; b < FIRST_ALLOCATABLE; b++)
        c.seen[b / 8] |= (uint8_t)(1 << (b % 8));
    /* With snapshots (a pinned generation), blocks whose free was held back
     *    stay marked until a sweep: extra marks are allowed, missing ones not. */
    bool pinned = fs->vol.sb.pinned_generation != 0;
    uint64_t marked = 0, mismatched = 0, unreached = 0;
    for (uint64_t i = 0; bitmap.type == RFS_TYPE_TABLE && i < bitmap_bytes; i++) {
        if (i % RFS_BLOCK_SIZE == 0 &&
            !ritterfs_read_block(fs, RFS_OBJ_BITMAP, &bitmap, i / RFS_BLOCK_SIZE, buf))
            break;
        uint8_t on_disk = buf[i % RFS_BLOCK_SIZE];
        for (int bit = 0; bit < 8; bit++) {
            marked += (on_disk >> bit) & 1;
            unreached += (on_disk & ~c.seen[i]) >> bit & 1;
        }
        if (pinned ? (c.seen[i] & ~on_disk) != 0 : on_disk != c.seen[i])
            mismatched++;
    }
    if (mismatched)
        problem(&c, "bitmap differs from the blocks in use in %lu bytes (of %lu)", mismatched,
                bitmap_bytes);
    if (marked != fs->vol.sb.used_blocks)
        problem(&c, "bitmap marks %lu blocks, superblock says %lu", marked, fs->vol.sb.used_blocks);

    /* 3. Directories: valid entries, and link counts that match them. */
    for (uint64_t id = RFS_OBJ_ROOT; id < slots; id++) {
        if (c.types[id] != RFS_TYPE_DIR || !ritterfs_get_inode(fs, id, &ino))
            continue;
        c.dir = id;
        if (!ritterfs_iterate_dir(fs, id, &ino, check_entry, &c))
            problem(&c, "directory %lu is malformed or unreadable", id);
    }
    for (uint64_t id = RFS_OBJ_ROOT; id < slots; id++) {
        if (c.types[id] == RFS_TYPE_FREE)
            continue;
        uint32_t want = id == RFS_OBJ_ROOT ? 0 : c.links[id];
        if (c.types[id] == RFS_TYPE_TABLE)
            continue;                               /* internal: the snapshot table */
        if (id != RFS_OBJ_ROOT && c.refs[id] == 0)
            problem(&c, "object %lu is not in any directory (orphan)", id);
        else if (c.refs[id] != want)
            problem(&c, "object %lu: link count %u, but %u directory entries", id, want,
                    c.refs[id]);
    }
    if (root.type == RFS_TYPE_DIR)
        ritterfs_iterate_dir(fs, RFS_OBJ_ROOT, &root, count_entry, &root_entries);

    /* 4. Attributes: well-formed records of files and directories that
     *    exist, no key twice for one object. */
    uint64_t attributes = 0, attributed = 0;
    if (fs->vol.sb.attribute_object)
        check_attributes(&c, &attributes, &attributed);

    kprintf("                 check: %lu objects, %lu blocks in use, root directory %lu entries: "
            "%s\n", objects, marked, root_entries,
            c.errors ? "PROBLEMS FOUND" : "all checksums ok (both copies), bitmap consistent");
    if (walk.snapshots)
        kprintf("                 snapshots: %lu, their blocks checked; %lu block(s) kept only "
                "for them, %lu awaiting a sweep\n", walk.snapshots, kept, unreached);
    if (fs->vol.sb.attribute_object)
        kprintf("                 attributes: %lu on %lu object(s)\n", attributes, attributed);
    if (c.shared || (pinned && !walk.snapshots))
        kprintf("                 copies: %lu block(s) shared, generations up to %lu pinned, "
                "%lu awaiting a sweep\n", c.shared, c.pinned, unreached);
    kfree(buf);
    kfree(c.seen);
    kfree(c.state_seen);
    kfree(c.types);
    kfree(c.links);
    kfree(c.refs);
    return c.errors == 0;
}

bool ritterfs_check(struct ritterfs *fs)
{
    uint64_t hold = rfs_read_lock(fs);              /* (65e-36: it only reads) */
    bool result = ritterfs_check_locked(fs);
    rfs_read_unlock(fs, hold);
    return result;
}

/* A read-only view of the file system as it was: the object table `table`,
 * at `generation`; same disk, no cache, no changes. */
struct ritterfs *ritterfs_view(struct ritterfs *fs, const struct rfs_inode *table,
                               uint64_t generation)
{
    struct ritterfs *v = kzalloc(sizeof(*v));
    if (!v)
        return 0;
    v->vfs.kind = VFS_RITTERFS;
    v->dev = fs->dev;
    v->sectors_per_block = fs->sectors_per_block;
    v->lock = (struct rwlock)RWLOCK_INIT("ritterfs view");
    v->index_lock = (struct mutex)MUTEX_INIT("ritterfs index");
    v->vol.read = volume_read;
    v->vol.find_entry = rfs_dirindex_find;
    v->vol.capacity = fs->vol.capacity;
    v->vol.sb = fs->vol.sb;
    v->vol.sb.table = *table;
    v->vol.sb.generation = generation;
    v->vol.slot = fs->vol.slot;
    v->view = true;
    v->read_only = true;
    return v;
}

/* --- (65e) the lock, measured -------------------------------------------------------- */

#define HOLD_SITES 48
static struct hold_site {
    const char *site;
    uint64_t count, held, longest;      /* holds; arch_counter counts */
    uint64_t made_wait, waited;         /* others waited while it held; it waited */
} hold_sites[HOLD_SITES];
static struct spinlock hold_lock = SPINLOCK_INIT("ritterfs holds");

/* (by name: an inline function of ritterfs.h has its __func__ in each file) */
static struct hold_site *hold_site(const char *site)
{
    for (int i = 0; i < HOLD_SITES; i++)
        if (!hold_sites[i].site || hold_sites[i].site == site || !strcmp(hold_sites[i].site, site)) {
            hold_sites[i].site = site;
            return &hold_sites[i];
        }
    return 0;
}

/* A wait over 1 us at site, while holder held the lock. */
static void count_wait(const char *site, const char *holder, uint64_t waited)
{
    if (!holder || waited * 1000000 <= arch_counter_hz())
        return;
    uint64_t state = spin_lock(&hold_lock);
    struct hold_site *me = hold_site(site), *them = hold_site(holder);
    if (me)
        me->waited += waited;
    if (them)
        them->made_wait += waited;
    spin_unlock(&hold_lock, state);
}

static void count_hold(const char *site, uint64_t held)
{
    uint64_t state = spin_lock(&hold_lock);
    struct hold_site *h = hold_site(site);
    if (h) {
        h->count++;
        h->held += held;
        if (held > h->longest)
            h->longest = held;
    }
    spin_unlock(&hold_lock, state);
}

void rfs_lock_at(struct ritterfs *fs, const char *site)
{
    uint64_t t0 = arch_counter();
    const char *holder = __atomic_load_n(&fs->held_at, __ATOMIC_RELAXED);   /* (who made us wait) */
    rwlock_write(&fs->lock);
    uint64_t now = arch_counter();
    if (fs->lock.write_depth != 1)
        return;                             /* (inside a hold of our own) */
    count_wait(site, holder, now - t0);
    __atomic_store_n(&fs->held_at, site, __ATOMIC_RELAXED);
    fs->held_since = now;
}

void rfs_unlock_at(struct ritterfs *fs)
{
    if (fs->lock.write_depth == 1) {
        const char *site = fs->held_at;
        __atomic_store_n(&fs->held_at, (const char *)0, __ATOMIC_RELAXED);
        count_hold(site, arch_counter() - fs->held_since);
    }
    rwlock_unlock(&fs->lock);
}

/* (65e-36) Reads: many at once, each counted at its own site (a nested one,
 * or the writer's read, not again). held_at is the last reader in: whom a
 * writer that waits waited for. */
uint64_t rfs_read_lock_at(struct ritterfs *fs, const char *site)
{
    uint64_t t0 = arch_counter();
    const char *holder = __atomic_load_n(&fs->held_at, __ATOMIC_RELAXED);
    if (rwlock_read(&fs->lock) != 1)
        return 0;
    uint64_t now = arch_counter();
    count_wait(site, holder, now - t0);
    __atomic_store_n(&fs->held_at, site, __ATOMIC_RELAXED);
    return now ? now : 1;
}

void rfs_read_unlock_at(struct ritterfs *fs, const char *site, uint64_t since)
{
    if (since) {
        const char *mine = site;
        __atomic_compare_exchange_n(&fs->held_at, &mine, (const char *)0, false, __ATOMIC_RELAXED,
                                    __ATOMIC_RELAXED);
        count_hold(site, arch_counter() - since);
    }
    rwlock_unlock(&fs->lock);
}

void rfs_hold_report(void)
{
    static struct hold_site copy[HOLD_SITES];   /* (cpus's caller is one at a time: kprintf) */
    uint64_t state = spin_lock(&hold_lock);
    memcpy(copy, hold_sites, sizeof(copy));
    spin_unlock(&hold_lock, state);
    int n = 0;
    while (n < HOLD_SITES && copy[n].site)
        n++;
    for (int i = 1; i < n; i++)                 /* the longest held first */
        for (int k = i; k > 0 && copy[k].held > copy[k - 1].held; k--) {
            struct hold_site t = copy[k];
            copy[k] = copy[k - 1];
            copy[k - 1] = t;
        }
    uint64_t hz = arch_counter_hz() ? arch_counter_hz() : 1;
    bool locked = kprintf_lock();
    kprintf("  the file systems' lock (ritterfs) by where it was taken, the longest held first:\n");
    for (int i = 0; i < n && i < 16; i++)
        kprintf("    %-24s %8lu times, held %lu.%03lu ms in all, at most %lu.%03lu ms; others "
                "waited %lu.%03lu ms, it waited %lu.%03lu ms\n", copy[i].site, copy[i].count,
                copy[i].held * 1000 / hz, copy[i].held * 1000000 / hz % 1000,
                copy[i].longest * 1000 / hz, copy[i].longest * 1000000 / hz % 1000,
                copy[i].made_wait * 1000 / hz, copy[i].made_wait * 1000000 / hz % 1000,
                copy[i].waited * 1000 / hz, copy[i].waited * 1000000 / hz % 1000);
    kprintf_unlock(locked);
    rfs_commit_report();
}

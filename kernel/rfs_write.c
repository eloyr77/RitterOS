/*
 * RitterFS: changing the file system (copy-on-write).
 *
 * Changes live in memory until ritterfs_commit():
 *   - content changes as "dirty blocks" per object (files, directories, and
 *     the object table, which holds every inode);
 *   - allocation state as two bitmaps: `bitmap` is the state being built,
 *     `committed` the state of the last commit. A block may only be allocated
 *     if it is free in both: blocks freed since the last commit still belong
 *     to the tree the last superblock points to and must survive a crash.
 *
 * A commit rewrites every dirty block, and every indirect block above one, to
 * newly allocated places, children before parents, so that each parent
 * records its children's checksums. The order breaks the circle between the
 * object table (which holds the bitmap's inode) and the bitmap (which must
 * list the table's new blocks):
 *   1. files and directories       (may allocate)
 *   2. reserve the table's blocks  (may allocate)
 *   3. reserve the bitmap's blocks (may allocate; repeat until stable)
 *   4. write the bitmap, 5. write the table, 6. flush, superblock, flush.
 */
#include "ritterfs.h"
#include "arch.h"
#include "crc32.h"
#include "rfs_dirindex.h"
#include "heap.h"
#include "kprintf.h"
#include "string.h"
#include "uefi.h"

#define BLOCK RFS_BLOCK_SIZE
#define PPB   RFS_POINTERS_PER_BLOCK

struct dirty_block {
    uint64_t index;
    uint8_t *data;
    struct dirty_block *next;
};

struct plan_node {                  /* a block to be (re)written at commit */
    int level;                      /* 0 = data, > 0 = indirect */
    uint64_t base;                  /* first content block it covers */
    uint64_t address, address2;     /* its new place */
};

struct dirty_object {
    uint64_t id;
    struct rfs_pointer old_root;    /* the committed tree */
    uint8_t old_height;
    bool removed;                   /* free the whole committed tree */
    /* A committed tree truncated away: freed whole at the commit, and none
     * of its blocks is kept (old_root is cleared then). */
    struct rfs_pointer dropped_root;
    uint8_t dropped_height;
    struct dirty_block *blocks;
    struct plan_node *plan;
    uint64_t plan_count, plan_capacity;
    struct dirty_object *next;
};

struct rfs_txn {
    struct dirty_object *objects;
    uint8_t *bitmap;                /* allocation state being built: 1 = used */
    uint8_t *committed;             /* state of the last commit */
    uint8_t *bitmap_dirty;          /* per bitmap block: changed since the last commit */
    uint8_t *bitmap_loaded;         /* (65e) per bitmap block: read in (load_bitmap) */
    struct rfs_inode bitmap_inode;  /* (65e) the bitmap of the last commit: blocks not read in */
    uint64_t bitmap_bytes, bitmap_blocks;
    uint64_t cursor[2];             /* next-fit positions for first and second copies */
    uint64_t used;
    bool failed;                    /* an allocation or write failed during a commit */
};

static uint64_t now_ns(void)
{
    return uefi_clock_ns();
}

/* --- (65e) where a commit's time goes ---------------------------------------------------- */

/* Each phase: how often, in all, at most (arch_counter counts); under the
 * file system's lock. */
enum { PH_BEGIN, PH_LOAD, PH_WRITE, PH_FLUSH, PH_SUPER, PH_DISCARD, PH_COPY, PHASES };
static const char *const phase_names[PHASES] = {
    "a transaction begun", "a bitmap block read in (4 KiB)", "the new blocks written",
    "the first flush",
    "the superblock and its flush", "freed blocks found and TRIMmed", "the bitmap copied",
};
static struct { uint64_t count, all, longest; } phases[PHASES];
static uint64_t bitmap_bytes_seen;

static uint64_t phase_end(int phase, uint64_t since)
{
    uint64_t now = arch_counter(), took = now - since;
    phases[phase].count++;
    phases[phase].all += took;
    if (took > phases[phase].longest)
        phases[phase].longest = took;
    return now;
}

void rfs_commit_report(void)
{
    uint64_t hz = arch_counter_hz() ? arch_counter_hz() : 1;
    bool locked = kprintf_lock();
    kprintf("  RitterFS commits by phase (a bitmap of %lu KiB):\n", bitmap_bytes_seen / 1024);
    for (int i = 0; i < PHASES; i++)
        kprintf("    %-40s %6lu times, %lu.%03lu ms in all, at most %lu.%03lu ms\n", phase_names[i],
                phases[i].count, phases[i].all * 1000 / hz, phases[i].all * 1000000 / hz % 1000,
                phases[i].longest * 1000 / hz, phases[i].longest * 1000000 / hz % 1000);
    kprintf_unlock(locked);
}

/* --- transaction state ------------------------------------------------------------ */

static struct rfs_txn *txn(struct ritterfs *fs)
{
    if (fs->txn)
        return fs->txn;
    uint64_t began = arch_counter();
    struct rfs_txn *t = kzalloc(sizeof(*t));
    struct rfs_inode bitmap;
    if (!t || !ritterfs_get_inode(fs, RFS_OBJ_BITMAP, &bitmap))
        goto fail;
    t->bitmap_bytes = (fs->vol.sb.block_count + 7) / 8;
    t->bitmap_blocks = (t->bitmap_bytes + BLOCK - 1) / BLOCK;
    t->bitmap = kmalloc(t->bitmap_bytes);
    t->committed = kmalloc(t->bitmap_bytes);
    t->bitmap_dirty = kzalloc(t->bitmap_blocks);
    t->bitmap_loaded = kzalloc(t->bitmap_blocks);   /* (65e) read in as needed */
    if (!t->bitmap || !t->committed || !t->bitmap_dirty || !t->bitmap_loaded)
        goto fail;
    t->bitmap_inode = bitmap;
    t->cursor[0] = RFS_FIRST_ALLOCATABLE;
    t->cursor[1] = fs->vol.sb.block_count / 2;
    t->used = fs->vol.sb.used_blocks;
    fs->txn = t;
    bitmap_bytes_seen = t->bitmap_bytes;
    phase_end(PH_BEGIN, began);
    return t;
fail:
    if (t) {
        kfree(t->bitmap);
        kfree(t->committed);
        kfree(t->bitmap_dirty);
        kfree(t->bitmap_loaded);
        kfree(t);
    }
    return 0;
}

bool rfs_has_changes(struct ritterfs *fs)
{
    if (!fs->txn)
        return false;
    if (fs->txn->objects)
        return true;
    for (uint64_t i = 0; i < fs->txn->bitmap_blocks; i++)
        if (fs->txn->bitmap_dirty[i])
            return true;
    return false;
}

/* (65e) Bitmap block i (BLOCK bytes, 32,768 blocks' bits) in memory: read
 * from the last commit's bitmap the first time it is needed, into both
 * copies (unchanged since then, they agree). Until 65e a transaction read
 * all of it as it began: 14.9 MB on the J1900's 465 GiB disk, the lock held
 * 315 ms. False if it cannot be read. */
static bool load_bitmap(struct ritterfs *fs, uint64_t i)
{
    struct rfs_txn *t = fs->txn;
    if (t->bitmap_loaded[i])
        return true;
    uint64_t began = arch_counter();
    uint64_t from = i * BLOCK, n = t->bitmap_bytes - from < BLOCK ? t->bitmap_bytes - from : BLOCK;
    if (ritterfs_read(fs, RFS_OBJ_BITMAP, &t->bitmap_inode, from, t->bitmap + from, n) !=
        (int64_t)n)
        return false;
    memcpy(t->committed + from, t->bitmap + from, n);
    t->bitmap_loaded[i] = 1;
    phase_end(PH_LOAD, began);
    return true;
}

static bool bit(const uint8_t *map, uint64_t b)
{
    return map[b / 8] & (1 << (b % 8));
}

static void set_bit(struct rfs_txn *t, uint64_t b, bool on)
{
    if (on)
        t->bitmap[b / 8] |= (uint8_t)(1 << (b % 8));
    else
        t->bitmap[b / 8] &= (uint8_t)~(1 << (b % 8));
    t->bitmap_dirty[b / 8 / BLOCK] = 1;
}

/* Next free block from cursor `which`, wrapping around once. */
static uint64_t alloc_block(struct ritterfs *fs, int which)
{
    struct rfs_txn *t = fs->txn;
    uint64_t count = fs->vol.sb.block_count, start = t->cursor[which];
    for (uint64_t n = 0; n < count; n++) {
        uint64_t b = start + n;
        if (b >= count)
            b = RFS_FIRST_ALLOCATABLE + (b - count) % (count - RFS_FIRST_ALLOCATABLE);
        if (!load_bitmap(fs, b / 8 / BLOCK))
            break;
        if (!bit(t->bitmap, b) && !bit(t->committed, b)) {
            set_bit(t, b, true);
            t->used++;
            t->cursor[which] = b + 1;
            return b;
        }
    }
    t->failed = true;
    return 0;
}

/* Free block b, born in generation `birth` (0: unknown, free it). Blocks
 * born up to the pinned generation are shared with a snapshot: they stay,
 * counted, until a sweep finds them unused. */
static void free_block(struct ritterfs *fs, uint64_t b, uint64_t birth)
{
    struct rfs_txn *t = fs->txn;
    if (birth && birth <= fs->vol.sb.pinned_generation) {
        fs->vol.sb.deferred_blocks++;
        return;
    }
    if (b < RFS_FIRST_ALLOCATABLE || b >= fs->vol.sb.block_count || !load_bitmap(fs, b / 8 / BLOCK) ||
        !bit(t->bitmap, b)) {
        kprintf("  RitterFS: freeing block %lu that is not in use\n", b);
        t->failed = true;
        return;
    }
    set_bit(t, b, false);
    t->used--;
}

/* --- dirty objects and blocks --------------------------------------------------- */

static struct dirty_object *find_object(struct ritterfs *fs, uint64_t id)
{
    for (struct dirty_object *o = fs->txn ? fs->txn->objects : 0; o; o = o->next)
        if (o->id == id)
            return o;
    return 0;
}

/* The dirty-object record for id, created on first use: it remembers the
 * committed tree, which the commit replaces and partly frees. */
static struct dirty_object *dirty_object(struct ritterfs *fs, uint64_t id)
{
    struct dirty_object *o = find_object(fs, id);
    if (o)
        return o;
    struct rfs_inode ino;
    if (!ritterfs_get_inode(fs, id, &ino) || !(o = kzalloc(sizeof(*o))))
        return 0;
    o->id = id;
    if (!(ino.flags & RFS_FLAG_INLINE)) {
        o->old_root = ino.root;
        o->old_height = ino.height;
    }
    o->next = fs->txn->objects;
    fs->txn->objects = o;
    return o;
}

static struct dirty_block *find_block(struct dirty_object *o, uint64_t index)
{
    for (struct dirty_block *b = o->blocks; b; b = b->next)
        if (b->index == index)
            return b;
    return 0;
}

const uint8_t *rfs_dirty_block(struct ritterfs *fs, uint64_t object_id, uint64_t index)
{
    struct dirty_object *o = find_object(fs, object_id);
    struct dirty_block *b = o ? find_block(o, index) : 0;
    return b ? b->data : 0;
}

/* The dirty copy of a block, made from its current content on first use. */
static uint8_t *block_for_write(struct ritterfs *fs, struct dirty_object *o,
                                const struct rfs_inode *ino, uint64_t index)
{
    struct dirty_block *b = find_block(o, index);
    if (b)
        return b->data;
    b = kzalloc(sizeof(*b));
    uint8_t *data = kmalloc(BLOCK);
    if (!b || !data || !ritterfs_read_block(fs, o->id, ino, index, data)) {
        kfree(b);
        kfree(data);
        return 0;
    }
    b->index = index;
    b->data = data;
    b->next = o->blocks;
    o->blocks = b;
    return data;
}

static void drop_blocks(struct dirty_object *o)
{
    while (o->blocks) {
        struct dirty_block *b = o->blocks;
        o->blocks = b->next;
        kfree(b->data);
        kfree(b);
    }
}

/*
 * Change content: write size bytes at offset of object id, whose current
 * inode is *ino (updated: size, inline flag). Content of up to 160 bytes
 * stays inline in the inode; beyond that it moves into blocks.
 */
static int write_content(struct ritterfs *fs, uint64_t id, struct rfs_inode *ino, uint64_t offset,
                         const void *data, uint64_t size)
{
    if (ino->type == RFS_TYPE_DIR)
        rfs_dirindex_forget(fs, id);            /* its entries change */
    uint64_t end = offset + size;
    uint64_t new_size = end > ino->size ? end : ino->size;

    if (ino->flags & RFS_FLAG_INLINE) {
        if (new_size <= RFS_INLINE_MAX && ino->type != RFS_TYPE_TABLE) {
            memcpy(ino->inline_data + offset, data, size);
            ino->size = new_size;
            return RFS_OK;
        }
        /* Too large for the inode: block 0 takes over the inline content. */
        struct dirty_object *o = dirty_object(fs, id);
        uint8_t *first = o ? block_for_write(fs, o, ino, 0) : 0;
        if (!first)
            return RFS_ERR_IO;
        ino->flags &= (uint16_t)~RFS_FLAG_INLINE;
        memset(ino->inline_data, 0, RFS_INLINE_MAX);
        memset(&ino->root, 0, sizeof(ino->root));
        ino->height = 0;
    }

    struct dirty_object *o = dirty_object(fs, id);
    if (!o)
        return RFS_ERR_IO;
    for (uint64_t pos = offset; pos < end;) {
        uint64_t in_block = pos % BLOCK;
        uint64_t n = BLOCK - in_block < end - pos ? BLOCK - in_block : end - pos;
        uint8_t *block = block_for_write(fs, o, ino, pos / BLOCK);
        if (!block)
            return RFS_ERR_IO;
        memcpy(block + in_block, (const uint8_t *)data + (pos - offset), n);
        pos += n;
    }
    ino->size = new_size;
    return RFS_OK;
}

/* Store an inode in the object table (the table's own inode lives in the
 * superblock). */
static int put_inode(struct ritterfs *fs, uint64_t id, const struct rfs_inode *ino)
{
    if (id == RFS_OBJ_TABLE) {
        fs->vol.sb.table = *ino;
        return RFS_OK;
    }
    struct rfs_inode table = fs->vol.sb.table;
    int r = write_content(fs, RFS_OBJ_TABLE, &table, id * sizeof(*ino), ino, sizeof(*ino));
    fs->vol.sb.table = table;
    return r;
}

/* --- trees: planning and writing ------------------------------------------------- */

static uint64_t span(int level)             /* content blocks under a node */
{
    return 1ULL << (7 * level);
}

static int height_for(uint64_t size)
{
    uint64_t blocks = (size + BLOCK - 1) / BLOCK;
    int h = 0;
    while (span(h) < blocks)
        h++;
    return h;
}

static struct plan_node *plan_find(struct dirty_object *o, int level, uint64_t base)
{
    for (uint64_t i = 0; i < o->plan_count; i++)
        if (o->plan[i].level == level && o->plan[i].base == base)
            return &o->plan[i];
    return 0;
}

/* The committed tree's pointer at (level, base), or a zero pointer. */
static struct rfs_pointer old_pointer(struct ritterfs *fs, struct dirty_object *o, int level,
                                      uint64_t base)
{
    struct rfs_pointer p = {0};
    if (!o->old_root.address || level > o->old_height ||
        (level == o->old_height && base != 0) || base >= span(o->old_height))
        return p;
    p = o->old_root;
    uint8_t *buf = kmalloc(BLOCK);
    for (int l = o->old_height; l > level && p.address; l--) {
        if (!buf || !rfs_read_pointer(&fs->vol, &p, buf)) {
            fs->txn->failed = true;
            memset(&p, 0, sizeof(p));
            break;
        }
        memcpy(&p, buf + (base / span(l - 1)) % PPB * 32, sizeof(p));
    }
    kfree(buf);
    return p;
}

/* Give the node (level, base) a new place; the block it replaces is freed. */
static void plan_add(struct ritterfs *fs, struct dirty_object *o, int level, uint64_t base,
                     bool meta)
{
    if (plan_find(o, level, base))
        return;
    if (o->plan_count == o->plan_capacity) {
        uint64_t cap = o->plan_capacity ? o->plan_capacity * 2 : 16;
        struct plan_node *grown = kmalloc(cap * sizeof(*grown));
        if (!grown) {
            fs->txn->failed = true;
            return;
        }
        if (o->plan)
            memcpy(grown, o->plan, o->plan_count * sizeof(*grown));
        kfree(o->plan);
        o->plan = grown;
        o->plan_capacity = cap;
    }
    struct plan_node *n = &o->plan[o->plan_count++];
    n->level = level;
    n->base = base;
    n->address = alloc_block(fs, 0);
    n->address2 = meta || level > 0 ? alloc_block(fs, 1) : 0;

    struct rfs_pointer old = old_pointer(fs, o, level, base);
    if (old.address)
        free_block(fs, old.address, old.birth);
    if (old.address2)
        free_block(fs, old.address2, old.birth);
}

/* Plan content block `index` and every indirect block above it. */
static void plan_index(struct ritterfs *fs, struct dirty_object *o, uint64_t index, int height,
                       bool meta)
{
    for (int level = 0; level <= height; level++)
        plan_add(fs, o, level, index - index % span(level), meta);
}

static void plan_object(struct ritterfs *fs, struct dirty_object *o, uint64_t size, bool meta,
                        int *height)
{
    int h = height_for(size);
    if (h < o->old_height)
        h = o->old_height;                   /* trees never shrink */
    /* A taller tree keeps the old one as its first subtree. */
    if (o->old_root.address)
        for (int level = o->old_height + 1; level <= h; level++)
            plan_add(fs, o, level, 0, true);
    for (struct dirty_block *b = o->blocks; b; b = b->next)
        plan_index(fs, o, b->index, h, meta);
    *height = h;
}

static bool write_raw(struct ritterfs *fs, uint64_t block, const void *data)
{
    rfs_cache_forget(&fs->cache, block);
    return fs->dev->write(fs->dev, block * fs->sectors_per_block, fs->sectors_per_block, data);
}

bool rfs_write_raw(struct ritterfs *fs, uint64_t block, const void *data)
{
    return write_raw(fs, block, data);
}

/* Make block `index` of an object dirty without changing it, so that the next
 * commit writes it (and the indirect blocks above it) to new places. */
int rfs_mark_dirty(struct ritterfs *fs, uint64_t object_id, uint64_t index)
{
    struct rfs_inode ino;
    if (fs->read_only)
        return RFS_ERR_READ_ONLY;
    if (!txn(fs) || !ritterfs_get_inode(fs, object_id, &ino))
        return RFS_ERR_IO;
    if (object_id == RFS_OBJ_BITMAP) {
        if (index >= fs->txn->bitmap_blocks || !load_bitmap(fs, index))
            return RFS_ERR_IO;                  /* (65e: the commit writes it from memory) */
        fs->txn->bitmap_dirty[index] = 1;       /* the commit rewrites flagged blocks */
        return RFS_OK;
    }
    struct dirty_object *o = dirty_object(fs, object_id);
    return o && block_for_write(fs, o, &ino, index) ? RFS_OK : RFS_ERR_IO;
}

static struct rfs_pointer write_block(struct ritterfs *fs, const struct plan_node *n,
                                      const uint8_t *data, uint64_t generation)
{
    struct rfs_pointer p = { n->address, n->address2, crc32c(data, BLOCK), 0, generation };
    if (!n->address || !write_raw(fs, n->address, data) ||
        (n->address2 && !write_raw(fs, n->address2, data)))
        fs->txn->failed = true;
    return p;
}

/* Write node (level, base) and the planned nodes below it; unplanned subtrees
 * are kept as they are. Returns the pointer for the parent. */
static struct rfs_pointer write_node(struct ritterfs *fs, struct dirty_object *o, int level,
                                     uint64_t base, uint64_t generation)
{
    const struct plan_node *n = plan_find(o, level, base);
    if (!n)
        return old_pointer(fs, o, level, base);
    if (level == 0) {
        struct dirty_block *b = find_block(o, base);
        if (!b) {
            fs->txn->failed = true;
            struct rfs_pointer none = {0};
            return none;
        }
        return write_block(fs, n, b->data, generation);
    }

    uint8_t *data = kzalloc(BLOCK);
    if (!data) {
        fs->txn->failed = true;
        return (struct rfs_pointer){0};
    }
    struct rfs_pointer old = old_pointer(fs, o, level, base);
    if (old.address && !rfs_read_pointer(&fs->vol, &old, data))
        fs->txn->failed = true;
    for (uint64_t c = 0; c < PPB; c++) {
        uint64_t child = base + c * span(level - 1);
        struct rfs_pointer p;
        if (plan_find(o, level - 1, child))
            p = write_node(fs, o, level - 1, child, generation);
        else if (!old.address)
            p = old_pointer(fs, o, level - 1, child);    /* new level: the old root */
        else
            continue;                                     /* unchanged child */
        memcpy(data + c * 32, &p, sizeof(p));
    }
    struct rfs_pointer p = write_block(fs, n, data, generation);
    kfree(data);
    return p;
}

/* Free every block of a committed tree. */
static void free_tree(struct ritterfs *fs, const struct rfs_pointer *p, int level)
{
    if (!p->address)
        return;
    if (level > 0) {
        uint8_t *buf = kmalloc(BLOCK);
        if (!buf || !rfs_read_pointer(&fs->vol, p, buf)) {
            fs->txn->failed = true;
        } else {
            for (int i = 0; i < PPB; i++) {
                struct rfs_pointer child;
                memcpy(&child, buf + i * 32, sizeof(child));
                free_tree(fs, &child, level - 1);
            }
        }
        kfree(buf);
    }
    free_block(fs, p->address, p->birth);
    if (p->address2)
        free_block(fs, p->address2, p->birth);
}

/* --- operations ---------------------------------------------------------------- */

const char *ritterfs_error(int error)
{
    switch (error) {
    case RFS_OK:            return "ok";
    case RFS_ERR_NOT_FOUND: return "not found";
    case RFS_ERR_EXISTS:    return "already exists";
    case RFS_ERR_NOT_DIR:   return "not a directory";
    case RFS_ERR_IS_DIR:    return "is a directory";
    case RFS_ERR_NOT_EMPTY: return "directory not empty";
    case RFS_ERR_BAD_NAME:  return "invalid name";
    case RFS_ERR_NO_SPACE:  return "no space left";
    case RFS_ERR_IO:        return "I/O error";
    case RFS_ERR_READ_ONLY: return "file system is read-only";
    case RFS_ERR_BUSY:      return "in use";
    default:                return "unknown error";
    }
}

/* Split "/a/b/name" into the parent path and the last component. */
static int split_path(const char *path, char *parent, size_t parent_size, const char **name,
                      size_t *name_len)
{
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        len--;
    size_t slash = len;
    while (slash > 0 && path[slash - 1] != '/')
        slash--;
    *name = path + slash;
    *name_len = len - slash;
    if (*name_len == 0 || *name_len > RFS_NAME_MAX || slash >= parent_size ||
        (*name_len == 1 && **name == '.') ||
        (*name_len == 2 && (*name)[0] == '.' && (*name)[1] == '.'))
        return RFS_ERR_BAD_NAME;
    memcpy(parent, path, slash);
    parent[slash] = 0;
    return RFS_OK;
}

/* A whole directory's content, read into a new heap buffer. */
static uint8_t *read_dir(struct ritterfs *fs, uint64_t id, const struct rfs_inode *dir)
{
    uint8_t *data = kmalloc(dir->size ? dir->size : 1);
    if (data && ritterfs_read(fs, id, dir, 0, data, dir->size) != (int64_t)dir->size) {
        kfree(data);
        return 0;
    }
    return data;
}

/* Offset of the live entry called name, or -1. */
static int64_t find_entry(const uint8_t *data, uint64_t size, const char *name, size_t len,
                          uint64_t *object_id)
{
    for (uint64_t pos = 0; pos + 12 <= size;) {
        uint64_t id;
        uint16_t entry_size;
        memcpy(&id, data + pos, 8);
        memcpy(&entry_size, data + pos + 8, 2);
        if (entry_size < 12)
            break;
        if (id && data[pos + 10] == len && memcmp(data + pos + 12, name, len) == 0) {
            if (object_id)
                *object_id = id;
            return (int64_t)pos;
        }
        pos += entry_size;
    }
    return -1;
}

static int add_entry(struct ritterfs *fs, uint64_t dir_id, struct rfs_inode *dir, const char *name,
                     size_t len, uint64_t child, uint8_t type)
{
    uint8_t entry[12 + RFS_NAME_MAX + 8] = {0};
    uint16_t entry_size = (uint16_t)((12 + len + 7) / 8 * 8);
    memcpy(entry, &child, 8);
    memcpy(entry + 8, &entry_size, 2);
    entry[10] = (uint8_t)len;
    entry[11] = type;
    memcpy(entry + 12, name, len);

    /* A deleted entry that is large enough: the new one takes its place
     * and keeps its size (the rest stays padding). */
    uint8_t *old = read_dir(fs, dir_id, dir);
    if (!old)
        return RFS_ERR_IO;
    for (uint64_t at = 0; at + 12 <= dir->size;) {
        uint64_t id;
        uint16_t size;
        memcpy(&id, old + at, 8);
        memcpy(&size, old + at + 8, 2);
        if (size < 12)
            break;
        if (!id && size >= entry_size) {
            kfree(old);
            memcpy(entry + 8, &size, 2);
            return write_content(fs, dir_id, dir, at, entry, 12 + len);
        }
        at += size;
    }
    kfree(old);

    uint64_t pos = dir->size;
    uint64_t room = BLOCK - pos % BLOCK;
    if (pos % BLOCK && entry_size > room) {
        /* Pad the block's last entry to the block end; start a new block. */
        uint8_t *data = read_dir(fs, dir_id, dir);
        if (!data)
            return RFS_ERR_IO;
        uint64_t last = pos - pos % BLOCK, size;
        for (;;) {
            uint16_t s16;
            memcpy(&s16, data + last + 8, 2);
            if (s16 < 12) {                          /* corrupt: would loop forever */
                kfree(data);
                return RFS_ERR_IO;
            }
            if (last + s16 >= pos)
                break;
            last += s16;
        }
        uint16_t old_size;
        memcpy(&old_size, data + last + 8, 2);
        kfree(data);
        size = old_size + room;
        uint16_t padded = (uint16_t)size;
        uint8_t zeros[BLOCK] = {0};
        int r = write_content(fs, dir_id, dir, last + 8, &padded, 2);
        if (r == RFS_OK)
            r = write_content(fs, dir_id, dir, pos, zeros, room);
        if (r != RFS_OK)
            return r;
        pos += room;
    }
    return write_content(fs, dir_id, dir, pos, entry, entry_size);
}

/* An object ID for a new object: the lowest one freed by an earlier commit
 * (its inode is all zero), else a new one. IDs changed in this transaction
 * are skipped: a removed object's blocks are only freed at the commit. */
static uint64_t free_object_id(struct ritterfs *fs)
{
    for (uint64_t id = fs->next_free_id; id < fs->vol.sb.next_object_id; id++) {
        struct rfs_inode ino;
        if (!find_object(fs, id) && ritterfs_get_inode(fs, id, &ino) &&
            ino.type == RFS_TYPE_FREE) {
            fs->next_free_id = id + 1;
            return id;
        }
    }
    fs->next_free_id = fs->vol.sb.next_object_id + 1;
    return fs->vol.sb.next_object_id++;
}

static int ritterfs_create_locked(struct ritterfs *fs, const char *path, int type,
                                  uint64_t *object_id)
{
    char parent[256];
    const char *name;
    size_t len;
    uint64_t dir_id, id;
    struct rfs_inode dir;
    if (fs->read_only)
        return RFS_ERR_READ_ONLY;
    if (type != RFS_TYPE_FILE && type != RFS_TYPE_DIR && type != RFS_TYPE_SYMLINK)
        return RFS_ERR_BAD_NAME;
    int r = split_path(path, parent, sizeof(parent), &name, &len);
    if (r != RFS_OK)
        return r;
    for (size_t i = 0; i < len; i++)
        if (name[i] == 0)
            return RFS_ERR_BAD_NAME;
    if (!ritterfs_lookup(fs, parent[0] ? parent : "/", &dir_id, &dir))
        return RFS_ERR_NOT_FOUND;
    if (dir.type != RFS_TYPE_DIR)
        return RFS_ERR_NOT_DIR;
    if (!txn(fs))
        return RFS_ERR_IO;
    uint8_t *data = read_dir(fs, dir_id, &dir);
    if (!data)
        return RFS_ERR_IO;
    bool exists = find_entry(data, dir.size, name, len, 0) >= 0;
    kfree(data);
    if (exists)
        return RFS_ERR_EXISTS;

    uint64_t now = now_ns();
    struct rfs_inode ino = {0};
    ino.type = (uint16_t)type;
    ino.flags = RFS_FLAG_INLINE;
    ino.link_count = 1;
    ino.created = ino.modified = ino.changed = now;
    ino.generation = fs->vol.sb.generation + 1;
    id = free_object_id(fs);
    if ((r = put_inode(fs, id, &ino)) != RFS_OK)
        return r;

    if ((r = add_entry(fs, dir_id, &dir, name, len, id, (uint8_t)type)) != RFS_OK)
        return r;
    dir.modified = dir.changed = now;
    dir.generation = fs->vol.sb.generation + 1;
    if ((r = put_inode(fs, dir_id, &dir)) != RFS_OK)
        return r;
    if (object_id)
        *object_id = id;
    return RFS_OK;
}

static int ritterfs_write_locked(struct ritterfs *fs, uint64_t object_id, uint64_t offset,
                                 const void *data, uint64_t size)
{
    struct rfs_inode ino;
    if (fs->read_only)
        return RFS_ERR_READ_ONLY;
    if (!ritterfs_get_inode(fs, object_id, &ino) || ino.type == RFS_TYPE_FREE)
        return RFS_ERR_NOT_FOUND;
    if (ino.type != RFS_TYPE_FILE)
        return RFS_ERR_IS_DIR;
    if (ino.flags & RFS_FLAG_FIXED)
        return RFS_ERR_BUSY;                    /* (35a) written in place by its owner only */
    if (!txn(fs))
        return RFS_ERR_IO;
    int r = write_content(fs, object_id, &ino, offset, data, size);
    if (r != RFS_OK)
        return r;
    ino.modified = ino.changed = now_ns();
    ino.generation = fs->vol.sb.generation + 1;
    return put_inode(fs, object_id, &ino);
}

static int attr_remove(struct ritterfs *fs, uint64_t object, const char *key);

/* Cut a file to 0 bytes. Its committed tree is set aside (freed at the
 * commit), so later writes start from nothing: a block that is not
 * written again is a hole, never the old content. */
static int ritterfs_truncate_locked(struct ritterfs *fs, uint64_t object_id)
{
    struct rfs_inode ino;
    if (fs->read_only)
        return RFS_ERR_READ_ONLY;
    if (!ritterfs_get_inode(fs, object_id, &ino) || ino.type == RFS_TYPE_FREE)
        return RFS_ERR_NOT_FOUND;
    if (ino.type != RFS_TYPE_FILE)
        return RFS_ERR_IS_DIR;
    if (ino.flags & RFS_FLAG_FIXED)
        return RFS_ERR_BUSY;                    /* (35a) removed whole, never cut */
    if (!txn(fs))
        return RFS_ERR_IO;
    struct dirty_object *o = dirty_object(fs, object_id);
    if (!o)
        return RFS_ERR_IO;
    if (o->old_root.address) {
        o->dropped_root = o->old_root;
        o->dropped_height = o->old_height;
        memset(&o->old_root, 0, sizeof(o->old_root));
        o->old_height = 0;
    }
    drop_blocks(o);
    ino.flags = RFS_FLAG_INLINE;
    ino.size = 0;
    ino.height = 0;
    memset(&ino.root, 0, sizeof(ino.root));
    memset(ino.inline_data, 0, sizeof(ino.inline_data));
    ino.modified = ino.changed = now_ns();
    ino.generation = fs->vol.sb.generation + 1;
    return put_inode(fs, object_id, &ino);
}

static int ritterfs_commit_locked(struct ritterfs *fs);

/* (35a) Every block of a tree under p (base: its first content block, count:
 * the content's blocks), indirect ones too: there (no hole) and born after
 * pin, so no snapshot or copy shares it. */
static bool unshared(struct ritterfs *fs, const struct rfs_pointer *p, int level, uint64_t base,
                     uint64_t count, uint64_t pin)
{
    if (!p->address || p->birth <= pin)
        return false;
    if (level == 0)
        return true;
    uint8_t *buf = kmalloc(BLOCK);
    bool ok = buf && rfs_read_pointer(&fs->vol, p, buf);
    uint64_t under = span(level - 1);
    for (uint64_t i = 0; ok && i < PPB && base + i * under < count; i++) {
        struct rfs_pointer child;
        memcpy(&child, buf + i * 32, sizeof(child));
        ok = unshared(fs, &child, level - 1, base + i * under, count, pin);
    }
    kfree(buf);
    return ok;
}

static int ritterfs_make_fixed_locked(struct ritterfs *fs, uint64_t object_id)
{
    struct rfs_inode ino;
    if (fs->read_only)
        return RFS_ERR_READ_ONLY;
    int r = ritterfs_commit_locked(fs);         /* its blocks in the places they keep */
    if (r != RFS_OK)
        return r;
    if (!ritterfs_get_inode(fs, object_id, &ino) || ino.type == RFS_TYPE_FREE)
        return RFS_ERR_NOT_FOUND;
    if (ino.type != RFS_TYPE_FILE)
        return RFS_ERR_IS_DIR;
    if (ino.flags & RFS_FLAG_FIXED)
        return RFS_OK;
    if ((ino.flags & RFS_FLAG_INLINE) || ino.size == 0 || ino.size % BLOCK ||
        !unshared(fs, &ino.root, ino.height, 0, ino.size / BLOCK, fs->vol.sb.pinned_generation))
        return RFS_ERR_NOT_SUPPORTED;
    if (!txn(fs))
        return RFS_ERR_IO;
    ino.flags |= RFS_FLAG_FIXED;
    ino.changed = now_ns();
    ino.generation = fs->vol.sb.generation + 1;
    if ((r = put_inode(fs, object_id, &ino)) == RFS_OK)
        r = ritterfs_commit_locked(fs);
    return r;
}

int ritterfs_make_fixed(struct ritterfs *fs, uint64_t object_id)
{
    rfs_lock(fs);
    int r = ritterfs_make_fixed_locked(fs, object_id);
    rfs_unlock(fs);
    return r;
}

int ritterfs_truncate(struct ritterfs *fs, uint64_t object_id)
{
    rfs_lock(fs);
    int r = ritterfs_truncate_locked(fs, object_id);
    rfs_unlock(fs);
    return r;
}

/* Owner and permissions of an object. */
static int set_owner_locked(struct ritterfs *fs, uint64_t object_id, const struct rfs_owner *owner,
                            int mode)
{
    struct rfs_inode ino;
    if (fs->read_only)
        return RFS_ERR_READ_ONLY;
    if (!ritterfs_get_inode(fs, object_id, &ino) ||
        (ino.type != RFS_TYPE_FILE && ino.type != RFS_TYPE_DIR && ino.type != RFS_TYPE_SYMLINK))
        return RFS_ERR_NOT_FOUND;
    if (!txn(fs))
        return RFS_ERR_IO;
    if (owner) {
        ino.uid = owner->uid;
        ino.gid = owner->gid;
    }
    if (mode >= 0)
        ino.mode = (uint16_t)(RFS_MODE_SET | (mode & RFS_MODE_BITS));
    ino.changed = now_ns();
    ino.generation = fs->vol.sb.generation + 1;
    return put_inode(fs, object_id, &ino);
}

int ritterfs_chmod(struct ritterfs *fs, uint64_t object_id, unsigned mode)
{
    rfs_lock(fs);
    int r = set_owner_locked(fs, object_id, 0, (int)(mode & RFS_MODE_BITS));
    rfs_unlock(fs);
    return r;
}

/* (63m) The time it was last modified (nanoseconds since 1970); the time
 * its inode changed becomes now. */
int ritterfs_set_time(struct ritterfs *fs, uint64_t object_id, uint64_t modified)
{
    rfs_lock(fs);
    struct rfs_inode ino;
    int r = RFS_OK;
    if (fs->read_only)
        r = RFS_ERR_READ_ONLY;
    else if (!ritterfs_get_inode(fs, object_id, &ino) ||
             (ino.type != RFS_TYPE_FILE && ino.type != RFS_TYPE_DIR && ino.type != RFS_TYPE_SYMLINK))
        r = RFS_ERR_NOT_FOUND;
    else if (!txn(fs))
        r = RFS_ERR_IO;
    if (r == RFS_OK) {
        ino.modified = modified;
        ino.changed = now_ns();
        ino.generation = fs->vol.sb.generation + 1;
        r = put_inode(fs, object_id, &ino);
    }
    rfs_unlock(fs);
    return r;
}

int ritterfs_chown(struct ritterfs *fs, uint64_t object_id, uint32_t uid, uint32_t gid)
{
    struct rfs_owner owner = { uid, gid };
    rfs_lock(fs);
    int r = set_owner_locked(fs, object_id, &owner, -1);
    rfs_unlock(fs);
    return r;
}

static bool any_entry(void *ctx, const char *name, size_t len, uint64_t id, uint8_t type)
{
    (void)name, (void)len, (void)id, (void)type;
    *(bool *)ctx = true;
    return false;
}

static int ritterfs_remove_locked(struct ritterfs *fs, const char *path)
{
    char parent[256];
    const char *name;
    size_t len;
    uint64_t dir_id, id;
    struct rfs_inode dir, ino;
    if (fs->read_only)
        return RFS_ERR_READ_ONLY;
    int r = split_path(path, parent, sizeof(parent), &name, &len);
    if (r != RFS_OK)
        return r;
    if (!ritterfs_lookup(fs, parent[0] ? parent : "/", &dir_id, &dir))
        return RFS_ERR_NOT_FOUND;
    if (dir.type != RFS_TYPE_DIR)
        return RFS_ERR_NOT_DIR;
    if (!txn(fs))
        return RFS_ERR_IO;
    uint8_t *data = read_dir(fs, dir_id, &dir);
    if (!data)
        return RFS_ERR_IO;
    int64_t pos = find_entry(data, dir.size, name, len, &id);
    kfree(data);
    if (pos < 0 || !ritterfs_get_inode(fs, id, &ino))
        return RFS_ERR_NOT_FOUND;
    if (id == fs->busy_object && ino.link_count == 1)
        return RFS_ERR_BUSY;                    /* (35a) the swap file while swap is on */
    if (ino.type == RFS_TYPE_DIR) {
        bool has_entries = false;
        ritterfs_iterate_dir(fs, id, &ino, any_entry, &has_entries);
        if (has_entries)
            return RFS_ERR_NOT_EMPTY;
    }

    /* The entry stays as free space: its object ID becomes 0. */
    uint64_t zero = 0;
    if ((r = write_content(fs, dir_id, &dir, (uint64_t)pos, &zero, 8)) != RFS_OK)
        return r;
    uint64_t now = now_ns();
    dir.modified = dir.changed = now;
    dir.generation = fs->vol.sb.generation + 1;
    if ((r = put_inode(fs, dir_id, &dir)) != RFS_OK)
        return r;

    if (--ino.link_count > 0) {
        ino.changed = now;
        return put_inode(fs, id, &ino);
    }
    /* Last link gone: its attributes go now (its object ID may be reused),
     * its blocks are freed at commit. */
    if ((r = attr_remove(fs, id, 0)) != RFS_OK)
        return r;
    struct dirty_object *o = dirty_object(fs, id);
    if (!o)
        return RFS_ERR_IO;
    o->removed = true;
    drop_blocks(o);
    struct rfs_inode none = {0};
    return put_inode(fs, id, &none);
}

/* A path's parent directory and the entry's name in it (checked). */
static int parent_of(struct ritterfs *fs, const char *path, char *parent, const char **name,
                     size_t *len, uint64_t *dir_id, struct rfs_inode *dir)
{
    int r = split_path(path, parent, 256, name, len);
    if (r != RFS_OK)
        return r;
    for (size_t i = 0; i < *len; i++)
        if ((*name)[i] == 0)
            return RFS_ERR_BAD_NAME;
    if (!ritterfs_lookup(fs, parent[0] ? parent : "/", dir_id, dir))
        return RFS_ERR_NOT_FOUND;
    return dir->type == RFS_TYPE_DIR ? RFS_OK : RFS_ERR_NOT_DIR;
}

/* Move an entry: everything is checked first, then the new entry is added
 * and the old one freed; the object itself (ID, links, content) stays.
 * (68a) An entry already at to is replaced, as POSIX's rename: a file by a
 * file, an empty directory by a directory; its removal and the move are
 * one commit, so a reader finds the old object or the new one, never
 * none. Two names of one object: nothing changes. */
static int ritterfs_rename_locked(struct ritterfs *fs, const char *from, const char *to)
{
    char from_parent[256], to_parent[256];
    const char *from_name, *to_name;
    size_t from_len, to_len;
    uint64_t from_dir_id, to_dir_id, id, other;
    struct rfs_inode from_dir, to_dir, ino, target;
    if (fs->read_only)
        return RFS_ERR_READ_ONLY;
    int r = parent_of(fs, from, from_parent, &from_name, &from_len, &from_dir_id, &from_dir);
    if (r == RFS_OK)
        r = parent_of(fs, to, to_parent, &to_name, &to_len, &to_dir_id, &to_dir);
    if (r != RFS_OK)
        return r;
    if (!txn(fs))
        return RFS_ERR_IO;
    uint8_t *data = read_dir(fs, from_dir_id, &from_dir);
    if (!data)
        return RFS_ERR_IO;
    int64_t pos = find_entry(data, from_dir.size, from_name, from_len, &id);
    kfree(data);
    if (pos < 0 || !ritterfs_get_inode(fs, id, &ino))
        return RFS_ERR_NOT_FOUND;
    if (!(data = read_dir(fs, to_dir_id, &to_dir)))
        return RFS_ERR_IO;
    bool exists = find_entry(data, to_dir.size, to_name, to_len, &other) >= 0;
    kfree(data);
    if (exists && other == id)
        return RFS_OK;
    /* A directory cannot move into itself (it would leave the tree). */
    size_t n = strlen(from);
    while (n > 1 && from[n - 1] == '/')
        n--;
    if (ino.type == RFS_TYPE_DIR && strncmp(to, from, n) == 0 && (to[n] == '/' || to[n] == 0))
        return RFS_ERR_BAD_NAME;
    if (exists) {
        if (!ritterfs_get_inode(fs, other, &target))
            return RFS_ERR_IO;
        if (ino.type == RFS_TYPE_DIR && target.type != RFS_TYPE_DIR)
            return RFS_ERR_NOT_DIR;
        if (ino.type != RFS_TYPE_DIR && target.type == RFS_TYPE_DIR)
            return RFS_ERR_IS_DIR;
        /* (it checks the rest: not empty, the swap file) and changes the
         * directories: read them again after it */
        if ((r = ritterfs_remove_locked(fs, to)) != RFS_OK)
            return r;
        r = parent_of(fs, from, from_parent, &from_name, &from_len, &from_dir_id, &from_dir);
        if (r == RFS_OK)
            r = parent_of(fs, to, to_parent, &to_name, &to_len, &to_dir_id, &to_dir);
        if (r != RFS_OK)
            return RFS_ERR_IO;                  /* (the target is gone: thrown away) */
    }

    uint64_t now = now_ns(), gen = fs->vol.sb.generation + 1;
    if ((r = add_entry(fs, to_dir_id, &to_dir, to_name, to_len, id, (uint8_t)ino.type)) != RFS_OK)
        return r;
    to_dir.modified = to_dir.changed = now;
    to_dir.generation = gen;
    if ((r = put_inode(fs, to_dir_id, &to_dir)) != RFS_OK)
        return r;
    if (from_dir_id == to_dir_id)
        from_dir = to_dir;          /* the same directory: the entry kept its place */
    uint64_t zero = 0;
    if ((r = write_content(fs, from_dir_id, &from_dir, (uint64_t)pos, &zero, 8)) != RFS_OK)
        return r;
    from_dir.modified = from_dir.changed = now;
    from_dir.generation = gen;
    if ((r = put_inode(fs, from_dir_id, &from_dir)) != RFS_OK)
        return r;
    ino.changed = now;
    return put_inode(fs, id, &ino);
}

int ritterfs_rename(struct ritterfs *fs, const char *from, const char *to)
{
    rfs_lock(fs);
    int r = ritterfs_rename_locked(fs, from, to);
    rfs_unlock(fs);
    return r;
}

/* --- commit -------------------------------------------------------------------- */

static int ritterfs_sweep_locked(struct ritterfs *fs, uint64_t *freed);

static void free_txn_objects(struct rfs_txn *t)
{
    while (t->objects) {
        struct dirty_object *o = t->objects;
        t->objects = o->next;
        drop_blocks(o);
        kfree(o->plan);
        kfree(o);
    }
}

static bool write_superblock(struct ritterfs *fs)
{
    /* The oldest version that describes the file system: plain file
     * systems stay 1, older readers keep working until a feature is used. */
    struct rfs_superblock *sb = &fs->vol.sb;
    sb->version = sb->attribute_object                         ? 3
                : sb->pinned_generation || sb->snapshot_object ? 2 : 1;
    int slot = fs->vol.sb.generation % 2 == 0 ? 1 : 2;       /* even: A, odd: B */
    fs->vol.sb.checksum = crc32c(&fs->vol.sb, offsetof(struct rfs_superblock, checksum));
    if (!write_raw(fs, (uint64_t)slot, &fs->vol.sb))
        return false;
    fs->vol.slot = slot;
    return true;
}

/* Tell the disk which blocks this commit freed: in use before (committed),
 * free now (bitmap), as runs of neighbouring blocks. Bytes without a freed
 * block are skipped whole. */
static void discard_run(struct ritterfs *fs, uint64_t start, uint64_t run)
{
    if (run && fs->dev->discard(fs->dev, start * fs->sectors_per_block,
                                run * fs->sectors_per_block))
        fs->discarded += run;
}

/* Blocks the commit freed: set in `committed`, clear now. (65e) Only the
 * bitmap blocks this transaction changed can hold one (every change goes
 * through set_bit, which marks its block dirty): a pass over the whole
 * bitmap (14.9 MB on the J1900's 465 GiB disk) held the lock up to 105 ms
 * per commit. */
static void discard_freed(struct ritterfs *fs)
{
    struct rfs_txn *t = fs->txn;
    if (!fs->dev->discard)
        return;
    uint64_t count = fs->vol.sb.block_count, run = 0, start = 0;
    for (uint64_t i = RFS_FIRST_ALLOCATABLE / 8; i < t->bitmap_bytes; i++) {
        if (!t->bitmap_dirty[i / BLOCK]) {
            discard_run(fs, start, run);        /* (a run ends where the changes do) */
            run = 0;
            i = (i / BLOCK + 1) * BLOCK - 1;    /* the next bitmap block */
            continue;
        }
        uint8_t freed = t->committed[i] & (uint8_t)~t->bitmap[i];
        if (!freed && !run)
            continue;
        for (int k = 0; k < 8; k++) {
            uint64_t b = i * 8 + (uint64_t)k;
            if (b < RFS_FIRST_ALLOCATABLE || b >= count)
                continue;
            if (freed & (1u << k)) {
                if (!run)
                    start = b;
                run++;
            } else if (run) {
                discard_run(fs, start, run);
                run = 0;
            }
        }
    }
    discard_run(fs, start, run);
}

static int ritterfs_commit_locked(struct ritterfs *fs)
{
    if (fs->read_only)
        return RFS_ERR_READ_ONLY;
    if (!rfs_has_changes(fs))
        return RFS_OK;
    struct rfs_txn *t = fs->txn;
    uint64_t gen = fs->vol.sb.generation + 1;
    struct rfs_inode ino;
    t->failed = false;
    uint64_t at = arch_counter();               /* (65e: the phases' times) */

    /* 1. Files and directories: new trees; removed objects free theirs (and
     *    their IDs, for objects created after this commit). */
    uint64_t lowest_removed = fs->next_free_id;
    for (struct dirty_object *o = t->objects; o; o = o->next) {
        if (o->id == RFS_OBJ_TABLE || o->id == RFS_OBJ_BITMAP)
            continue;
        if (o->dropped_root.address)
            free_tree(fs, &o->dropped_root, o->dropped_height);
        if (o->removed) {
            free_tree(fs, &o->old_root, o->old_height);
            if (o->id < lowest_removed)
                lowest_removed = o->id;
            continue;
        }
        if (!ritterfs_get_inode(fs, o->id, &ino) || (ino.flags & RFS_FLAG_INLINE))
            continue;
        int height;
        plan_object(fs, o, ino.size, rfs_is_metadata(ino.type), &height);
        ino.root = write_node(fs, o, height, 0, gen);
        ino.height = (uint8_t)height;
        if (put_inode(fs, o->id, &ino) != RFS_OK)
            t->failed = true;
    }

    /* 2. The object table: its block holding the bitmap's inode will change
     *    too, so make it dirty now, then reserve the table's new blocks. */
    struct rfs_inode bitmap;
    if (!ritterfs_get_inode(fs, RFS_OBJ_BITMAP, &bitmap) ||
        put_inode(fs, RFS_OBJ_BITMAP, &bitmap) != RFS_OK)
        t->failed = true;
    struct dirty_object *table = dirty_object(fs, RFS_OBJ_TABLE);
    int table_height = 0;
    if (!table)
        t->failed = true;
    else
        plan_object(fs, table, fs->vol.sb.table.size, true, &table_height);

    /* 3. The bitmap: every changed bitmap block gets a new place, which may
     *    change further bitmap blocks; repeat until nothing new changes. */
    struct dirty_object *bm = dirty_object(fs, RFS_OBJ_BITMAP);
    if (!bm) {
        t->failed = true;
    } else {
        /* (68j) A grown file system's bitmap is longer: its tree grows as
         * a file's does, the old tree its first subtree (read as it is:
         * its blocks are found with the old height). */
        int h = height_for(t->bitmap_bytes);
        if (h > bitmap.height)
            for (int level = bitmap.height + 1; level <= h; level++)
                plan_add(fs, bm, level, 0, true);
        else
            h = bitmap.height;
        for (bool more = true; more && !t->failed;) {
            more = false;
            for (uint64_t i = 0; i < t->bitmap_blocks; i++) {
                if (!t->bitmap_dirty[i] || find_block(bm, i))
                    continue;
                if (!block_for_write(fs, bm, &bitmap, i)) {
                    t->failed = true;
                    break;
                }
                plan_index(fs, bm, i, h, true);
                more = true;
            }
        }
        bitmap.height = (uint8_t)h;
        bitmap.size = t->bitmap_bytes;
    }

    /* 4. Write the bitmap, now final, and record its new root. */
    if (!t->failed) {
        for (struct dirty_block *b = bm->blocks; b; b = b->next) {
            uint64_t start = b->index * BLOCK;
            uint64_t n = t->bitmap_bytes - start < BLOCK ? t->bitmap_bytes - start : BLOCK;
            memset(b->data, 0, BLOCK);
            memcpy(b->data, t->bitmap + start, n);
        }
        bitmap.root = write_node(fs, bm, bitmap.height, 0, gen);
        bitmap.generation = gen;
        if (put_inode(fs, RFS_OBJ_BITMAP, &bitmap) != RFS_OK)
            t->failed = true;
    }

    /* 5. Write the object table into the places reserved in step 2. */
    if (!t->failed) {
        struct rfs_inode tab = fs->vol.sb.table;
        tab.root = write_node(fs, table, table_height, 0, gen);
        tab.height = (uint8_t)table_height;
        tab.generation = gen;
        fs->vol.sb.table = tab;
    }

    /* 6. Everything reaches the disk before the superblock that points to it. */
    at = phase_end(PH_WRITE, at);
    bool flushed = !t->failed && fs->dev->flush(fs->dev);
    at = phase_end(PH_FLUSH, at);
    if (!flushed) {
        kprintf("  RitterFS: commit failed; file system is now read-only\n");
        fs->read_only = true;
        return RFS_ERR_IO;
    }
    fs->vol.sb.generation = gen;
    fs->vol.sb.commit_time = now_ns();
    fs->vol.sb.used_blocks = t->used;
    bool super = write_superblock(fs) && fs->dev->flush(fs->dev);
    at = phase_end(PH_SUPER, at);
    if (!super) {
        kprintf("  RitterFS: writing the superblock failed; file system is now read-only\n");
        fs->read_only = true;
        return RFS_ERR_IO;
    }
    t->bitmap_inode = bitmap;                   /* (65e) where unread blocks are now */

    /* 7. The new state is durable: blocks freed by this commit are reusable,
     *    and the disk may forget them (TRIM): no superblock refers to them. */
    discard_freed(fs);
    at = phase_end(PH_DISCARD, at);
    fs->next_free_id = lowest_removed;
    fs->committed = fs->vol.sb;
    fs->committed_next_free_id = fs->next_free_id;
    for (uint64_t i = 0; i < t->bitmap_blocks; i++)   /* (65e: only what changed) */
        if (t->bitmap_dirty[i]) {
            uint64_t from = i * BLOCK;
            memcpy(t->committed + from, t->bitmap + from,
                   t->bitmap_bytes - from < BLOCK ? t->bitmap_bytes - from : BLOCK);
        }
    memset(t->bitmap_dirty, 0, t->bitmap_blocks);
    free_txn_objects(t);
    phase_end(PH_COPY, at);
    return RFS_OK;
}

int ritterfs_create(struct ritterfs *fs, const char *path, int type, uint64_t *object_id)
{
    rfs_lock(fs);
    int result = ritterfs_create_locked(fs, path, type, object_id);
    rfs_unlock(fs);
    return result;
}

int ritterfs_write(struct ritterfs *fs, uint64_t object_id, uint64_t offset, const void *data,
                   uint64_t size)
{
    rfs_lock(fs);
    int result = ritterfs_write_locked(fs, object_id, offset, data, size);
    rfs_unlock(fs);
    return result;
}

int ritterfs_append(struct ritterfs *fs, uint64_t object_id, const void *data, uint64_t size,
                    uint64_t *at)
{
    rfs_lock(fs);
    struct rfs_inode inode;
    int result = RFS_ERR_NOT_FOUND;
    if (rfs_get_inode(&fs->vol, object_id, &inode) && inode.type != RFS_TYPE_FREE) {
        *at = inode.size;
        result = ritterfs_write_locked(fs, object_id, inode.size, data, size);
    }
    rfs_unlock(fs);
    return result;
}

/* (57a) A symbolic link at path naming target (1-255 bytes, no NUL). */
int ritterfs_symlink(struct ritterfs *fs, const char *path, const char *target, size_t length,
                     uint64_t *object_id)
{
    if (length == 0 || length > 255 || memchr(target, 0, length))
        return RFS_ERR_BAD_NAME;
    rfs_lock(fs);
    uint64_t id;
    int r = ritterfs_create_locked(fs, path, RFS_TYPE_SYMLINK, &id);
    struct rfs_inode ino;
    if (r == RFS_OK && !ritterfs_get_inode(fs, id, &ino))
        r = RFS_ERR_IO;
    if (r == RFS_OK)
        r = write_content(fs, id, &ino, 0, target, length);
    if (r == RFS_OK)
        r = put_inode(fs, id, &ino);
    rfs_unlock(fs);
    if (r == RFS_OK && object_id)
        *object_id = id;
    return r;
}

/* (57a) A second name, to, for the object at from (not a directory). */
int ritterfs_link(struct ritterfs *fs, const char *from, const char *to)
{
    char parent[256];
    const char *name;
    size_t len;
    uint64_t dir_id, id;
    struct rfs_inode dir, ino;
    rfs_lock(fs);
    int r = fs->read_only ? RFS_ERR_READ_ONLY : RFS_OK;
    if (r == RFS_OK && !ritterfs_lookup(fs, from, &id, &ino))
        r = RFS_ERR_NOT_FOUND;
    if (r == RFS_OK && ino.type == RFS_TYPE_DIR)
        r = RFS_ERR_IS_DIR;
    if (r == RFS_OK && id < RFS_OBJ_ROOT)
        r = RFS_ERR_BAD_NAME;
    if (r == RFS_OK)
        r = parent_of(fs, to, parent, &name, &len, &dir_id, &dir);
    if (r == RFS_OK && !txn(fs))
        r = RFS_ERR_IO;
    if (r == RFS_OK) {
        uint8_t *data = read_dir(fs, dir_id, &dir);
        if (!data)
            r = RFS_ERR_IO;
        else if (find_entry(data, dir.size, name, len, 0) >= 0)
            r = RFS_ERR_EXISTS;
        kfree(data);
    }
    if (r == RFS_OK)
        r = add_entry(fs, dir_id, &dir, name, len, id, (uint8_t)ino.type);
    uint64_t now = now_ns();
    if (r == RFS_OK) {
        dir.modified = dir.changed = now;
        dir.generation = fs->vol.sb.generation + 1;
        r = put_inode(fs, dir_id, &dir);
    }
    if (r == RFS_OK && !ritterfs_get_inode(fs, id, &ino))   /* (dir may be its parent too) */
        r = RFS_ERR_IO;
    if (r == RFS_OK) {
        ino.link_count++;
        ino.changed = now;
        ino.generation = fs->vol.sb.generation + 1;
        r = put_inode(fs, id, &ino);
    }
    rfs_unlock(fs);
    return r;
}

int ritterfs_remove(struct ritterfs *fs, const char *path)
{
    rfs_lock(fs);
    int result = ritterfs_remove_locked(fs, path);
    rfs_unlock(fs);
    return result;
}

/* Frees held back since the last sweep that make the next commit sweep:
 * an eighth of the blocks in use, at least 256 (a sweep walks every tree). */
static bool sweep_due(struct ritterfs *fs)
{
    uint64_t limit = fs->vol.sb.used_blocks / 8;
    return fs->vol.sb.deferred_blocks >= (limit > 256 ? limit : 256);
}

/* (68j) The bitmap's arrays at bytes for blocks: what was there kept, the
 * new blocks free, read in and changed (the commit writes them). */
static bool longer_bitmap(struct ritterfs *fs, uint64_t blocks)
{
    struct rfs_txn *t = fs->txn;
    uint64_t bytes = (blocks + 7) / 8, n = (bytes + BLOCK - 1) / BLOCK, last = t->bitmap_blocks - 1;
    uint8_t *bitmap = kzalloc(bytes), *committed = kzalloc(bytes);
    uint8_t *dirty = kzalloc(n), *loaded = kzalloc(n);
    bool ok = bitmap && committed && dirty && loaded && load_bitmap(fs, last);
    if (ok) {
        memcpy(bitmap, t->bitmap, t->bitmap_bytes);
        memcpy(committed, t->committed, t->bitmap_bytes);
        memcpy(dirty, t->bitmap_dirty, t->bitmap_blocks);
        memcpy(loaded, t->bitmap_loaded, t->bitmap_blocks);
        for (uint64_t i = last; i < n; i++)
            loaded[i] = dirty[i] = 1;
        uint8_t *swap[4] = { t->bitmap, t->committed, t->bitmap_dirty, t->bitmap_loaded };
        t->bitmap = bitmap, t->committed = committed, t->bitmap_dirty = dirty;
        t->bitmap_loaded = loaded;
        bitmap = swap[0], committed = swap[1], dirty = swap[2], loaded = swap[3];
        t->bitmap_bytes = bytes;
        t->bitmap_blocks = n;
        bitmap_bytes_seen = bytes;
    }
    kfree(bitmap);
    kfree(committed);
    kfree(dirty);
    kfree(loaded);
    return ok;
}

int ritterfs_grow(struct ritterfs *fs, uint64_t *blocks)
{
    rfs_lock(fs);
    uint64_t now = fs->dev->block_count / fs->sectors_per_block;
    int r = fs->read_only ? RFS_ERR_READ_ONLY : RFS_OK;
    if (r == RFS_OK && now > fs->vol.sb.block_count) {
        r = ritterfs_commit_locked(fs);         /* what was pending, on its own */
        if (r == RFS_OK && (!txn(fs) || !longer_bitmap(fs, now)))
            r = RFS_ERR_NO_SPACE;
        if (r == RFS_OK) {
            uint64_t before = fs->vol.sb.block_count;
            fs->vol.capacity = now;
            fs->vol.sb.block_count = now;
            r = ritterfs_commit_locked(fs);
            if (r == RFS_OK)
                kprintf("  RitterFS:      %s grown from %lu to %lu blocks of 4 KiB (%lu MiB)\n",
                        fs->dev->name, before, now, now / 256);
            else
                fs->vol.sb.block_count = before;    /* (read-only now: as on the disk) */
        }
    }
    *blocks = fs->vol.sb.block_count;
    rfs_unlock(fs);
    return r;
}

int ritterfs_commit(struct ritterfs *fs)
{
    rfs_lock(fs);
    int result = ritterfs_commit_locked(fs);
    uint64_t freed;
    if (result == RFS_OK && sweep_due(fs))
        result = ritterfs_sweep_locked(fs, &freed);
    rfs_unlock(fs);
    return result;
}

/* --- snapshots and the sweep ---------------------------------------------------- */

static bool valid_snapshot_name(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len > RFS_SNAPSHOT_NAME_MAX || strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0)
        return false;
    for (size_t i = 0; i < len; i++)
        if (name[i] == '/')
            return false;
    return true;
}

/* The snapshot table's content (0 bytes if there is none yet). */
static uint8_t *snapshot_table(struct ritterfs *fs, struct rfs_inode *ino, uint64_t *size)
{
    *size = 0;
    memset(ino, 0, sizeof(*ino));
    if (!fs->vol.sb.snapshot_object)
        return kmalloc(1);
    if (!ritterfs_get_inode(fs, fs->vol.sb.snapshot_object, ino) || ino->type != RFS_TYPE_TABLE)
        return 0;
    uint8_t *data = kmalloc(ino->size ? ino->size : 1);
    if (data && ritterfs_read(fs, fs->vol.sb.snapshot_object, ino, 0, data, ino->size) !=
                    (int64_t)ino->size) {
        kfree(data);
        return 0;
    }
    *size = ino->size;
    return data;
}

static int snapshot_find(const uint8_t *table, uint64_t size, const char *name)
{
    for (uint64_t i = 0; (i + 1) * sizeof(struct rfs_snapshot) <= size; i++) {
        const struct rfs_snapshot *s = (const void *)(table + i * sizeof(struct rfs_snapshot));
        if (s->generation && strncmp(s->name, name, sizeof(s->name)) == 0)
            return (int)i;
    }
    return -1;
}

static int ritterfs_snapshot_create_locked(struct ritterfs *fs, const char *name)
{
    if (fs->read_only)
        return RFS_ERR_READ_ONLY;
    if (!valid_snapshot_name(name))
        return RFS_ERR_BAD_NAME;
    int r = ritterfs_commit_locked(fs);             /* the snapshot shows committed data */
    if (r != RFS_OK)
        return r;
    struct rfs_inode ino;
    uint64_t size;
    uint8_t *table = snapshot_table(fs, &ino, &size);
    if (!table)
        return RFS_ERR_IO;
    int found = snapshot_find(table, size, name);
    uint64_t slot = size / sizeof(struct rfs_snapshot);
    for (uint64_t i = 0; i < size / sizeof(struct rfs_snapshot); i++)
        if (!((const struct rfs_snapshot *)(table + i * sizeof(struct rfs_snapshot)))->generation) {
            slot = i;
            break;
        }
    kfree(table);
    if (found >= 0)
        return RFS_ERR_EXISTS;
    if (!txn(fs))
        return RFS_ERR_IO;

    struct rfs_snapshot s = {0};
    memcpy(s.name, name, strlen(name));
    s.generation = fs->vol.sb.generation;
    s.created = now_ns();
    s.table = fs->vol.sb.table;
    uint64_t id = fs->vol.sb.snapshot_object;
    if (!id) {                                      /* the first snapshot: a new table */
        id = free_object_id(fs);
        memset(&ino, 0, sizeof(ino));
        ino.type = RFS_TYPE_TABLE;
        ino.flags = RFS_FLAG_INLINE;
        ino.created = ino.modified = ino.changed = s.created;
        ino.generation = fs->vol.sb.generation + 1;
        /* dirty_object() reads the inode: it must exist before the content. */
        if ((r = put_inode(fs, id, &ino)) != RFS_OK)
            return r;
        fs->vol.sb.snapshot_object = id;
    }
    ino.generation = fs->vol.sb.generation + 1;
    /* Everything the snapshot sees was born by now: keep it from here on. */
    fs->vol.sb.pinned_generation = fs->vol.sb.generation;
    if ((r = write_content(fs, id, &ino, slot * sizeof(s), &s, sizeof(s))) != RFS_OK ||
        (r = put_inode(fs, id, &ino)) != RFS_OK)
        return r;
    return ritterfs_commit_locked(fs);
}

/* Mark every block reachable from the live file system and every snapshot
 * (rfs_snapshot.c), then free the blocks marked in use that none of them
 * reaches: the frees held back for snapshots that are gone. */
static int ritterfs_sweep_locked(struct ritterfs *fs, uint64_t *freed)
{
    *freed = 0;
    int r = ritterfs_commit_locked(fs);
    if (r != RFS_OK)
        return r;
    struct rfs_txn *t = txn(fs);
    uint8_t *reachable = t ? kzalloc(t->bitmap_bytes) : 0;
    if (!reachable)
        return RFS_ERR_IO;
    uint64_t pin;
    if (!rfs_reachable(fs, reachable, &pin)) {
        kfree(reachable);
        return RFS_ERR_IO;
    }
    /* What must stay pinned now: the newest snapshot, or blocks that copies
     * still share. Lowering it is safe at once: the disk keeps the higher
     * value until the next commit, which only holds back more. */
    fs->vol.sb.pinned_generation = pin;
    for (uint64_t i = 0; i < t->bitmap_blocks; i++)  /* (65e: it looks at every block) */
        if (!load_bitmap(fs, i)) {
            kfree(reachable);
            return RFS_ERR_IO;
        }
    for (uint64_t b = RFS_FIRST_ALLOCATABLE; b < fs->vol.sb.block_count; b++) {
        if (bit(t->bitmap, b) && !bit(reachable, b)) {
            set_bit(t, b, false);
            t->used--;
            (*freed)++;
        }
    }
    kfree(reachable);
    fs->vol.sb.deferred_blocks = 0;
    return *freed ? ritterfs_commit_locked(fs) : RFS_OK;
}

static int ritterfs_snapshot_delete_locked(struct ritterfs *fs, const char *name,
                                           uint64_t *freed)
{
    *freed = 0;
    if (fs->read_only)
        return RFS_ERR_READ_ONLY;
    int r = ritterfs_commit_locked(fs);
    if (r != RFS_OK)
        return r;
    struct rfs_inode ino;
    uint64_t size;
    uint8_t *table = snapshot_table(fs, &ino, &size);
    if (!table)
        return RFS_ERR_IO;
    int found = snapshot_find(table, size, name);
    kfree(table);
    if (found < 0)
        return RFS_ERR_NOT_FOUND;
    if (!txn(fs))
        return RFS_ERR_IO;
    struct rfs_snapshot none = {0};
    ino.generation = fs->vol.sb.generation + 1;
    ino.modified = ino.changed = now_ns();
    if ((r = write_content(fs, fs->vol.sb.snapshot_object, &ino,
                           (uint64_t)found * sizeof(none), &none, sizeof(none))) != RFS_OK ||
        (r = put_inode(fs, fs->vol.sb.snapshot_object, &ino)) != RFS_OK ||
        (r = ritterfs_commit_locked(fs)) != RFS_OK)
        return r;
    rfs_snapshot_forget(fs, name);
    /* The sweep takes back what was held for it and lowers the pinned
     * generation to what the rest still needs (0: nothing shared). */
    return ritterfs_sweep_locked(fs, freed);
}

/* --- attributes ------------------------------------------------------------------- */

#define ATTR_HEADER sizeof(struct rfs_attr_record)

static uint16_t attr_size(size_t key_length, size_t value_length)
{
    return (uint16_t)((ATTR_HEADER + key_length + value_length + RFS_ATTR_ALIGN - 1) /
                      RFS_ATTR_ALIGN * RFS_ATTR_ALIGN);
}

/* Where object's record for key is (-1: none), and the first free record
 * of at least `need` bytes (*free_at -1: none). */
static int64_t attr_find(const uint8_t *table, uint64_t size, uint64_t object, const char *key,
                         size_t key_length, uint16_t need, int64_t *free_at,
                         uint16_t *free_size, uint16_t *found_size)
{
    struct rfs_attr_record r;
    *free_at = -1;
    for (uint64_t at = 0; rfs_attr_next(table, size, &at, &r); at += r.size) {
        if (r.object_id == object && r.key_length == key_length &&
            memcmp(table + at + ATTR_HEADER, key, key_length) == 0) {
            *found_size = r.size;
            return (int64_t)at;
        }
        if (!r.object_id && r.size >= need && *free_at < 0) {
            *free_at = (int64_t)at;
            *free_size = r.size;
        }
    }
    return -1;
}

/* The attribute table, created on first use (version 3 from then on). */
static int attr_table(struct ritterfs *fs, uint64_t *id, struct rfs_inode *ino)
{
    if ((*id = fs->vol.sb.attribute_object)) {
        return ritterfs_get_inode(fs, *id, ino) && ino->type == RFS_TYPE_TABLE ? RFS_OK
                                                                               : RFS_ERR_IO;
    }
    *id = free_object_id(fs);
    memset(ino, 0, sizeof(*ino));
    ino->type = RFS_TYPE_TABLE;
    ino->flags = RFS_FLAG_INLINE;
    ino->created = ino->modified = ino->changed = now_ns();
    ino->generation = fs->vol.sb.generation + 1;
    int r = put_inode(fs, *id, ino);        /* before any content: dirty_object reads it */
    if (r == RFS_OK)
        fs->vol.sb.attribute_object = *id;
    return r;
}

static int ritterfs_attr_set_locked(struct ritterfs *fs, uint64_t object, const char *key,
                                    const void *value, uint64_t length)
{
    size_t key_length = strlen(key);
    struct rfs_inode obj, ino;
    if (fs->read_only)
        return RFS_ERR_READ_ONLY;
    if (!rfs_attr_key_valid(key, key_length) || length > RFS_ATTR_VALUE_MAX)
        return RFS_ERR_BAD_NAME;
    if (!ritterfs_get_inode(fs, object, &obj) ||
        (obj.type != RFS_TYPE_FILE && obj.type != RFS_TYPE_DIR))
        return RFS_ERR_NOT_FOUND;
    if (!txn(fs))
        return RFS_ERR_IO;
    uint64_t id, size;
    int r = attr_table(fs, &id, &ino);
    uint8_t *table = r == RFS_OK ? rfs_table_content(fs, id, &ino, &size) : 0;
    uint8_t *record = table ? kzalloc(BLOCK) : 0;
    if (!record) {
        kfree(table);
        return r != RFS_OK ? r : RFS_ERR_IO;
    }
    uint16_t need = attr_size(key_length, length), free_size = 0, found_size = 0;
    int64_t free_at, at = attr_find(table, size, object, key, key_length, need, &free_at,
                                    &free_size, &found_size);
    kfree(table);

    /* The old record if it is large enough, else a free one, else the end
     * (a free filler record first if it would cross into the next block). */
    uint64_t pos = size, end = size;            /* end: the table's end so far */
    uint16_t record_size = need;
    if (at >= 0 && found_size >= need) {
        pos = (uint64_t)at;
        record_size = found_size;
    } else {
        uint64_t zero = 0;
        if (at >= 0)
            r = write_content(fs, id, &ino, (uint64_t)at, &zero, 8);
        if (free_at >= 0) {
            pos = (uint64_t)free_at;
            record_size = free_size;
        } else if (pos % BLOCK && need > BLOCK - pos % BLOCK && r == RFS_OK) {
            struct rfs_attr_record filler = { 0, (uint16_t)(BLOCK - pos % BLOCK), 0, 0, 0, 0 };
            memcpy(record, &filler, ATTR_HEADER);
            r = write_content(fs, id, &ino, pos, record, filler.size);
            memset(record, 0, ATTR_HEADER);
            pos = end = pos + filler.size;
        }
    }
    struct rfs_attr_record head = { object, record_size, (uint8_t)key_length, 0,
                                    (uint16_t)length, 0 };
    memcpy(record, &head, ATTR_HEADER);
    memcpy(record + ATTR_HEADER, key, key_length);
    memcpy(record + ATTR_HEADER + key_length, value, length);
    /* A new record is written whole (the padding extends the table); a
     * reused one up to its value: what follows inside it is ignored. */
    uint64_t write = pos == end ? record_size : ATTR_HEADER + key_length + length;
    if (r == RFS_OK)
        r = write_content(fs, id, &ino, pos, record, write);
    kfree(record);
    ino.modified = ino.changed = now_ns();
    ino.generation = fs->vol.sb.generation + 1;
    return r == RFS_OK ? put_inode(fs, id, &ino) : r;
}

/* Remove object's attribute key, or all of its attributes (key 0). */
static int attr_remove(struct ritterfs *fs, uint64_t object, const char *key)
{
    struct rfs_inode ino;
    uint64_t id = fs->vol.sb.attribute_object, size;
    if (!id)
        return key ? RFS_ERR_NOT_FOUND : RFS_OK;
    if (!txn(fs))
        return RFS_ERR_IO;
    uint8_t *table = rfs_table_content(fs, id, &ino, &size);
    if (!table)
        return RFS_ERR_IO;
    size_t key_length = key ? strlen(key) : 0;
    int r = RFS_OK, found = 0;
    struct rfs_attr_record rec;
    for (uint64_t at = 0; r == RFS_OK && rfs_attr_next(table, size, &at, &rec); at += rec.size) {
        if (rec.object_id != object || (key && (rec.key_length != key_length ||
                                                memcmp(table + at + ATTR_HEADER, key,
                                                       key_length) != 0)))
            continue;
        uint64_t zero = 0;
        r = write_content(fs, id, &ino, at, &zero, 8);
        found++;
    }
    kfree(table);
    if (r != RFS_OK || !found)
        return r != RFS_OK ? r : key ? RFS_ERR_NOT_FOUND : RFS_OK;
    ino.modified = ino.changed = now_ns();
    ino.generation = fs->vol.sb.generation + 1;
    return put_inode(fs, id, &ino);
}

static int ritterfs_attr_remove_locked(struct ritterfs *fs, uint64_t object, const char *key)
{
    if (fs->read_only)
        return RFS_ERR_READ_ONLY;
    return attr_remove(fs, object, key);
}

struct attr_copy {
    struct ritterfs *fs;
    uint64_t to;
    int result;
};

static bool copy_attr(void *ctx, uint64_t object, const char *key, size_t key_length,
                      const uint8_t *value, size_t value_length)
{
    struct attr_copy *c = ctx;
    char name[RFS_ATTR_KEY_MAX + 1];
    (void)object;
    memcpy(name, key, key_length);
    name[key_length] = 0;
    c->result = ritterfs_attr_set_locked(c->fs, c->to, name, value, value_length);
    return c->result == RFS_OK;
}

/* Give object `to` the attributes of `from` in source (the same file system
 * or a snapshot's view; instant copies keep them). The callback reads a
 * copy of the table, so it may change the table. */
static int attr_copy(struct ritterfs *fs, struct ritterfs *source, uint64_t from, uint64_t to)
{
    struct attr_copy c = { fs, to, RFS_OK };
    if (!source->vol.sb.attribute_object)
        return RFS_OK;
    if (!ritterfs_attr_each(source, from, copy_attr, &c))
        return RFS_ERR_IO;
    return c.result;
}

int ritterfs_attr_set(struct ritterfs *fs, uint64_t object, const char *key, const void *value,
                      uint64_t length)
{
    rfs_lock(fs);
    int r = ritterfs_attr_set_locked(fs, object, key, value, length);
    rfs_unlock(fs);
    return r;
}

int ritterfs_attr_remove(struct ritterfs *fs, uint64_t object, const char *key)
{
    rfs_lock(fs);
    int r = ritterfs_attr_remove_locked(fs, object, key);
    rfs_unlock(fs);
    return r;
}

/* --- instant copies ------------------------------------------------------------ */

#define COPY_PATH_MAX 256           /* as split_path's parent */

/* Throw the uncommitted changes away: back to the last commit. Only
 * between commits: the bitmap then still equals the committed one. The
 * pinned generation stays as it is now (a sweep may have lowered it, a
 * copy raised it: both are safe to keep). */
static void discard_changes(struct ritterfs *fs)
{
    const struct rfs_superblock *committed = &fs->committed;
    struct rfs_txn *t = fs->txn;
    if (t) {
        for (struct dirty_object *o = t->objects; o; o = o->next)
            rfs_dirindex_forget(fs, o->id);         /* may index uncommitted entries */
        free_txn_objects(t);
        memcpy(t->bitmap, t->committed, t->bitmap_bytes);
        memset(t->bitmap_dirty, 0, t->bitmap_blocks);
        t->used = committed->used_blocks;
    }
    uint64_t pinned = fs->vol.sb.pinned_generation;
    fs->vol.sb = *committed;
    fs->vol.sb.pinned_generation = pinned;
    fs->next_free_id = fs->committed_next_free_id;
}

void ritterfs_abort(struct ritterfs *fs)
{
    rfs_lock(fs);
    discard_changes(fs);
    rfs_unlock(fs);
}

struct copy_entry {
    char name[RFS_NAME_MAX + 1];
};

struct copy_list {
    struct copy_entry *e;
    uint64_t count, room;
    bool failed;
};

static bool collect_entry(void *ctx, const char *name, size_t len, uint64_t id, uint8_t type)
{
    struct copy_list *l = ctx;
    (void)id, (void)type;
    if (l->count == l->room) {
        uint64_t room = l->room ? l->room * 2 : 16;
        struct copy_entry *e = kmalloc(room * sizeof(*e));
        if (!e) {
            l->failed = true;
            return false;
        }
        if (l->count)
            memcpy(e, l->e, l->count * sizeof(*e));
        kfree(l->e);
        l->e = e;
        l->room = room;
    }
    memcpy(l->e[l->count].name, name, len);
    l->e[l->count].name[len] = 0;
    l->count++;
    return true;
}

/* Copy one object to `to` (a new name): a file's inode takes the source's
 * tree as it is; a directory gets new entries, each copied the same way.
 * (35a) A fixed file is not copied (its blocks are written in place): asked
 * for itself (top) it is refused, inside a directory left out. */
static int copy_object(struct ritterfs *fs, struct ritterfs *source, const char *from,
                       const char *to, uint64_t now, const struct rfs_owner *owner, bool top)
{
    uint64_t src_id, id;
    struct rfs_inode src;
    if (!ritterfs_lookup(source, from, &src_id, &src))
        return RFS_ERR_NOT_FOUND;
    if (src.flags & RFS_FLAG_FIXED)
        return top ? RFS_ERR_NOT_SUPPORTED : RFS_OK;
    int r = ritterfs_create_locked(fs, to, src.type, &id);
    if (r == RFS_OK)
        r = attr_copy(fs, source, src_id, id);
    if (r != RFS_OK)
        return r;
    if (src.type == RFS_TYPE_FILE || src.type == RFS_TYPE_SYMLINK) {
        struct rfs_inode ino = src;
        ino.link_count = 1;
        ino.created = ino.changed = now;            /* modified: the content's time */
        ino.generation = fs->vol.sb.generation + 1;
        if (owner) {
            ino.uid = owner->uid;
            ino.gid = owner->gid;
            ino.mode &= ~RFS_MODE_SETUID;           /* a copy is its maker's: never as
                                                     * another's rights (49k, like chown) */
        }
        return put_inode(fs, id, &ino);
    }
    /* A directory: the new one gets the source's mode (and owner). */
    struct rfs_inode dir;
    if (!ritterfs_get_inode(fs, id, &dir))
        return RFS_ERR_IO;
    dir.mode = src.mode;
    dir.uid = owner ? owner->uid : src.uid;
    dir.gid = owner ? owner->gid : src.gid;
    if ((r = put_inode(fs, id, &dir)) != RFS_OK)
        return r;
    struct copy_list l = { 0, 0, 0, false };
    if (!ritterfs_iterate_dir(source, src_id, &src, collect_entry, &l) || l.failed) {
        kfree(l.e);
        return RFS_ERR_IO;
    }
    char *a = kmalloc(2 * COPY_PATH_MAX), *b = a + COPY_PATH_MAX;
    r = a ? RFS_OK : RFS_ERR_IO;
    for (uint64_t i = 0; i < l.count && r == RFS_OK; i++) {
        size_t n = strlen(l.e[i].name);
        if (strlen(from) + n + 2 > COPY_PATH_MAX || strlen(to) + n + 2 > COPY_PATH_MAX) {
            r = RFS_ERR_BAD_NAME;                   /* the path got too long */
            break;
        }
        bool root = from[0] == '/' && from[1] == 0;    /* a snapshot's "/" */
        ksnprintf(a, COPY_PATH_MAX, "%s%s%s", from, root ? "" : "/", l.e[i].name);
        ksnprintf(b, COPY_PATH_MAX, "%s/%s", to, l.e[i].name);
        r = copy_object(fs, source, a, b, now, owner, false);
    }
    kfree(a);
    kfree(l.e);
    return r;
}

static int ritterfs_copy_locked(struct ritterfs *fs, struct ritterfs *source, const char *from,
                                const char *to, const struct rfs_owner *owner)
{
    if (fs->read_only)
        return RFS_ERR_READ_ONLY;
    size_t n = strlen(from);
    if (source == fs && strncmp(to, from, n) == 0 && (to[n] == '/' || to[n] == 0 || n == 1))
        return RFS_ERR_BAD_NAME;                    /* into itself */
    /* The copy shares committed blocks: commit first; once the copy is made,
     * pin everything born so far, so neither side frees a block the other
     * still uses (frees only happen at the commit). */
    int r = ritterfs_commit_locked(fs);
    if (r != RFS_OK)
        return r;
    r = copy_object(fs, source, from, to, now_ns(), owner, true);
    if (r != RFS_OK)
        discard_changes(fs);                        /* no half copy */
    else if (fs->vol.sb.pinned_generation < fs->vol.sb.generation)
        fs->vol.sb.pinned_generation = fs->vol.sb.generation;
    return r != RFS_OK ? r : ritterfs_commit_locked(fs);
}

int ritterfs_copy_from(struct ritterfs *fs, struct ritterfs *source, const char *from,
                       const char *to, const struct rfs_owner *owner)
{
    rfs_lock(fs);
    int r = ritterfs_copy_locked(fs, source, from, to, owner);
    rfs_unlock(fs);
    return r;
}

int ritterfs_copy(struct ritterfs *fs, const char *from, const char *to)
{
    return ritterfs_copy_from(fs, fs, from, to, 0);
}

int ritterfs_snapshot_create(struct ritterfs *fs, const char *name)
{
    rfs_lock(fs);
    int r = ritterfs_snapshot_create_locked(fs, name);
    rfs_unlock(fs);
    return r;
}

int ritterfs_snapshot_delete(struct ritterfs *fs, const char *name, uint64_t *freed)
{
    rfs_lock(fs);
    int r = ritterfs_snapshot_delete_locked(fs, name, freed);
    rfs_unlock(fs);
    return r;
}

int ritterfs_sweep(struct ritterfs *fs, uint64_t *freed)
{
    rfs_lock(fs);
    int r = ritterfs_sweep_locked(fs, freed);
    rfs_unlock(fs);
    return r;
}

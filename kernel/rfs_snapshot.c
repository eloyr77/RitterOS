/*
 * RitterFS snapshots, the read side: the list, read-only views, and the
 * blocks reachable from the live state and every snapshot (for the sweep
 * in rfs_write.c, which creates and deletes snapshots).
 */
#include "ritterfs.h"
#include "heap.h"
#include "string.h"

#define MAX_VIEWS 16

struct rfs_snapshots {
    struct {
        char name[64];
        uint64_t generation;
        struct ritterfs *view;      /* kept: open files may use it */
    } views[MAX_VIEWS];
};

/* The snapshot table's entries, read through fs (0 if there are none). */
static struct rfs_snapshot *entries(struct ritterfs *fs, uint64_t *count)
{
    *count = 0;
    uint64_t id = fs->vol.sb.snapshot_object;
    struct rfs_inode ino;
    if (!id || !ritterfs_get_inode(fs, id, &ino) || ino.type != RFS_TYPE_TABLE || !ino.size)
        return 0;
    struct rfs_snapshot *e = kmalloc(ino.size);
    if (e && ritterfs_read(fs, id, &ino, 0, e, ino.size) != (int64_t)ino.size) {
        kfree(e);
        return 0;
    }
    *count = e ? ino.size / sizeof(*e) : 0;
    return e;
}

bool ritterfs_snapshot_list(struct ritterfs *fs, ritterfs_snapshot_callback cb, void *ctx)
{
    uint64_t count;
    struct rfs_snapshot *e = entries(fs, &count);
    for (uint64_t i = 0; i < count; i++) {
        char name[64];
        memcpy(name, e[i].name, 63);
        name[63] = 0;
        if (e[i].generation && !cb(ctx, name, e[i].generation, e[i].created))
            break;
    }
    kfree(e);
    return true;
}

/* Programs look snapshots up concurrently: the list of views is under the
 * file system's lock (recursive, so the reads below may take it again). */
static struct ritterfs *find_view(struct ritterfs *fs, const char *name)
{
    if (!fs->snapshots && !(fs->snapshots = kzalloc(sizeof(*fs->snapshots))))
        return 0;
    struct rfs_snapshots *s = fs->snapshots;
    for (int i = 0; i < MAX_VIEWS; i++)
        if (s->views[i].view && strcmp(s->views[i].name, name) == 0)
            return s->views[i].view;
    uint64_t count;
    struct rfs_snapshot *e = entries(fs, &count);
    struct ritterfs *view = 0;
    for (uint64_t i = 0; i < count && !view; i++) {
        if (!e[i].generation || strncmp(e[i].name, name, sizeof(e[i].name)) != 0)
            continue;
        for (int k = 0; k < MAX_VIEWS && !view; k++) {
            if (s->views[k].view)
                continue;
            view = ritterfs_view(fs, &e[i].table, e[i].generation);
            if (view) {
                s->views[k].view = view;
                s->views[k].generation = e[i].generation;
                memcpy(s->views[k].name, e[i].name, 63);
            }
        }
    }
    kfree(e);
    return view;
}

/* (65e-36) A reader: the check walks every snapshot through its view under
 * its read hold. The list of views is "ritterfs index"'s (which guards the
 * directory index too): readers make views at once. */
struct ritterfs *ritterfs_snapshot_view(struct ritterfs *fs, const char *name)
{
    uint64_t hold = rfs_read_lock(fs);
    mutex_lock(&fs->index_lock);
    struct ritterfs *view = find_view(fs, name);
    mutex_unlock(&fs->index_lock);
    rfs_read_unlock(fs, hold);
    return view;
}

void rfs_snapshot_forget(struct ritterfs *fs, const char *name)
{
    /* The view stays allocated (an open file may still read through it):
     * it only leaves the list, so the name can be used again. */
    mutex_lock(&fs->index_lock);                    /* (65e-36: the views' list) */
    for (int i = 0; fs->snapshots && i < MAX_VIEWS; i++)
        if (fs->snapshots->views[i].view && strcmp(fs->snapshots->views[i].name, name) == 0)
            fs->snapshots->views[i].view = 0;
    mutex_unlock(&fs->index_lock);
}

static void mark(uint8_t *map, uint64_t block)
{
    if (block)
        map[block / 8] |= (uint8_t)(1 << (block % 8));
}

/* Mark a tree's blocks (indirect blocks are read through vol). While the
 * live state is marked (shared != 0), a block found marked already is used
 * twice, by copies: *shared becomes the newest birth of such blocks. */
static bool mark_tree(struct rfs_volume *vol, const struct rfs_pointer *p, int level,
                      uint8_t *map, uint8_t *buf, uint64_t *shared)
{
    if (!p->address)
        return true;
    if (shared && (map[p->address / 8] >> (p->address % 8) & 1) && p->birth > *shared)
        *shared = p->birth;
    mark(map, p->address);
    mark(map, p->address2);
    if (level == 0)
        return true;
    if (!rfs_read_pointer(vol, p, buf))
        return false;
    uint8_t *copy = kmalloc(RFS_BLOCK_SIZE);
    if (!copy)
        return false;
    memcpy(copy, buf, RFS_BLOCK_SIZE);
    bool ok = true;
    for (int i = 0; i < RFS_POINTERS_PER_BLOCK && ok; i++) {
        struct rfs_pointer child;
        memcpy(&child, copy + i * 32, sizeof(child));
        ok = mark_tree(vol, &child, level - 1, map, buf, shared);
    }
    kfree(copy);
    return ok;
}

/* Every object of one state (its object table, then each inode). */
static bool mark_state(struct ritterfs *fs, uint8_t *map, uint8_t *buf, uint64_t *shared)
{
    const struct rfs_inode *table = &fs->vol.sb.table;
    if (!mark_tree(&fs->vol, &table->root, table->height, map, buf, shared))
        return false;
    uint64_t slots = table->size / sizeof(struct rfs_inode);
    for (uint64_t id = 2; id < slots; id++) {
        struct rfs_inode ino;
        if (!rfs_get_inode(&fs->vol, id, &ino))
            return false;
        if (ino.type != RFS_TYPE_FREE && !(ino.flags & RFS_FLAG_INLINE) &&
            !mark_tree(&fs->vol, &ino.root, ino.height, map, buf, shared))
            return false;
    }
    return true;
}

bool rfs_reachable(struct ritterfs *fs, uint8_t *map, uint64_t *pin)
{
    *pin = 0;
    uint8_t *buf = kmalloc(RFS_BLOCK_SIZE);
    if (!buf)
        return false;
    for (uint64_t b = 0; b < RFS_FIRST_ALLOCATABLE; b++)
        mark(map, b);
    map[0] |= 1;
    bool ok = mark_state(fs, map, buf, pin);
    uint64_t count;
    struct rfs_snapshot *e = ok ? entries(fs, &count) : 0;
    for (uint64_t i = 0; ok && e && i < count; i++) {
        if (!e[i].generation)
            continue;
        if (e[i].generation > *pin)
            *pin = e[i].generation;
        struct ritterfs *view = ritterfs_view(fs, &e[i].table, e[i].generation);
        ok = view && mark_state(view, map, buf, 0);
        kfree(view);
    }
    kfree(e);
    kfree(buf);
    return ok;
}

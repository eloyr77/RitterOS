/*
 * RitterFS scrub: read every block of every tree, verify every copy, repair
 * damaged metadata copies from the intact one.
 *
 * Rewriting a damaged copy in place does not break copy-on-write safety: its
 * new content is identical to the good copy (same checksum, same tree), and
 * the good copy is never touched, so a crash during the repair leaves at
 * worst the same damaged copy. If the rewritten copy still reads back wrong
 * (a failing sector), the block is relocated instead: marked dirty, so that
 * the next commit writes both copies to new places.
 */
#include "ritterfs.h"
#include "crc32.h"
#include "heap.h"
#include "kprintf.h"
#include "string.h"

#define BLOCK RFS_BLOCK_SIZE

struct relocation {
    uint64_t object_id;
    uint64_t index;                 /* a content block; its ancestors move with it */
};

struct scrub {
    struct ritterfs *fs;
    bool force_relocate;
    uint64_t metadata_blocks, data_blocks;
    uint64_t repaired, lost;
    struct relocation *relocations;
    uint64_t relocation_count, relocation_capacity;
    bool fixed;                     /* (35a) the object is a fixed file: its data is
                                     * written in place (no checksums), it never moves */
};

static const char *object_name(uint64_t id)
{
    switch (id) {
    case RFS_OBJ_TABLE:  return " table";
    case RFS_OBJ_BITMAP: return " bitmap";
    case RFS_OBJ_ROOT:   return " root directory";
    default:             return "";
    }
}

/* --- finding a file's path for the report ------------------------------------ */

struct path_search {
    struct ritterfs *fs;
    uint64_t wanted;
    char path[256];
    size_t len;
    bool found;
};

static bool search_entry(void *ctx, const char *name, size_t len, uint64_t id, uint8_t type)
{
    struct path_search *s = ctx;
    size_t saved = s->len;
    if (s->len + 1 + len >= sizeof(s->path))
        return true;
    s->path[s->len++] = '/';
    memcpy(s->path + s->len, name, len);
    s->len += len;
    s->path[s->len] = 0;
    if (id == s->wanted) {
        s->found = true;
        return false;
    }
    struct rfs_inode ino;
    if (type == RFS_TYPE_DIR && ritterfs_get_inode(s->fs, id, &ino)) {
        ritterfs_iterate_dir(s->fs, id, &ino, search_entry, s);
        if (s->found)
            return false;
    }
    s->len = saved;
    s->path[s->len] = 0;
    return true;
}

static const char *path_of(struct ritterfs *fs, uint64_t id, struct path_search *s)
{
    struct rfs_inode root;
    memset(s, 0, sizeof(*s));
    s->fs = fs;
    s->wanted = id;
    if (ritterfs_get_inode(fs, RFS_OBJ_ROOT, &root))
        ritterfs_iterate_dir(fs, RFS_OBJ_ROOT, &root, search_entry, s);
    return s->found ? s->path : "(not in any directory)";
}

/* --- walking and repairing ------------------------------------------------------ */

static void relocate(struct scrub *s, uint64_t object_id, uint64_t index)
{
    if (s->relocation_count == s->relocation_capacity) {
        uint64_t cap = s->relocation_capacity ? s->relocation_capacity * 2 : 8;
        struct relocation *grown = kmalloc(cap * sizeof(*grown));
        if (!grown)
            return;
        if (s->relocations)
            memcpy(grown, s->relocations, s->relocation_count * sizeof(*grown));
        kfree(s->relocations);
        s->relocations = grown;
        s->relocation_capacity = cap;
    }
    s->relocations[s->relocation_count++] = (struct relocation){ object_id, index };
}

static bool copy_ok(struct scrub *s, uint64_t address, uint32_t checksum, uint8_t *buf)
{
    return rfs_read_raw(s->fs, address, buf) && crc32c(buf, BLOCK) == checksum;
}

/*
 * Verify the block a pointer refers to (both copies for metadata), repair a
 * damaged copy, and leave the good content in `good`. `base` is the first
 * content block under the pointer. Returns false if no copy is intact.
 */
static bool scrub_pointer(struct scrub *s, const struct rfs_pointer *p, bool meta, int level,
                          uint64_t object_id, uint64_t base, uint8_t *good)
{
    uint8_t *other = kmalloc(BLOCK);
    if (!other)
        return false;
    bool ok1 = copy_ok(s, p->address, p->checksum, good);
    bool ok2 = p->address2 && copy_ok(s, p->address2, p->checksum, ok1 ? other : good);
    bool result = ok1 || ok2;

    if (meta || level > 0)
        s->metadata_blocks++;
    else
        s->data_blocks++;

    if (!result) {
        struct path_search ps;
        s->lost++;
        if (level == 0 && !p->address2)
            kprintf("                 damaged data: %s block %lu (disk block %lu): "
                    "stored once, not repairable\n", path_of(s->fs, object_id, &ps), base,
                    p->address);
        else
            kprintf("                 LOST: object %lu%s level %d block %lu: no intact copy\n",
                    object_id, object_name(object_id), level, base);
    } else if (p->address2 && ok1 != ok2) {
        uint64_t bad = ok1 ? p->address2 : p->address;
        bool fixed = false;
        if (!s->force_relocate && rfs_write_raw(s->fs, bad, good) &&
            copy_ok(s, bad, p->checksum, other)) {
            fixed = true;
        } else if (s->fixed) {
            s->lost++;                              /* (35a) its blocks stay where they are */
        } else {
            relocate(s, object_id, base);
        }
        s->repaired++;
        struct path_search ps;
        char where[48];
        if (level)
            ksnprintf(where, sizeof(where), "level-%d indirect block above block %lu", level, base);
        else
            ksnprintf(where, sizeof(where), "block %lu", base);
        kprintf("                 repaired: %s%s %s, copy at disk block %lu: %s\n",
                object_id > RFS_OBJ_ROOT ? path_of(s->fs, object_id, &ps) : "object",
                object_id > RFS_OBJ_ROOT ? "" : object_name(object_id), where, bad,
                fixed ? "rewritten in place" : "will move to new places");
    }
    kfree(other);
    return result;
}

static void scrub_tree(struct scrub *s, const struct rfs_pointer *p, int level, bool meta,
                       uint64_t object_id, uint64_t base)
{
    if (!p->address)
        return;                                     /* a hole */
    if (level == 0 && s->fixed)
        return;                                     /* (35a) written in place: no checksum */
    uint8_t *buf = kmalloc(BLOCK);
    if (!buf)
        return;
    if (scrub_pointer(s, p, meta, level, object_id, base, buf) && level > 0) {
        uint64_t span = 1ULL << (7 * (level - 1));
        for (int i = 0; i < RFS_POINTERS_PER_BLOCK; i++) {
            struct rfs_pointer child;
            memcpy(&child, buf + i * 32, sizeof(child));
            scrub_tree(s, &child, level - 1, meta, object_id, base + (uint64_t)i * span);
        }
    }
    kfree(buf);
}

static void scrub_object(struct scrub *s, uint64_t id, const struct rfs_inode *ino)
{
    s->fixed = ino->flags & RFS_FLAG_FIXED;
    if (!(ino->flags & RFS_FLAG_INLINE))
        scrub_tree(s, &ino->root, ino->height, ino->type != RFS_TYPE_FILE, id, 0);
}

static int ritterfs_scrub_locked(struct ritterfs *fs, bool force_relocate)
{
    struct scrub s = {0};
    s.fs = fs;
    s.force_relocate = force_relocate;
    if (rfs_has_changes(fs)) {
        kprintf("  scrub:         skipped: uncommitted changes\n");
        return -1;
    }
    kprintf("  scrub:         verifying every copy of every block%s\n",
            force_relocate ? " (relocating damaged blocks)" : "");

    /* The object table first: every other inode is read through it. */
    scrub_object(&s, RFS_OBJ_TABLE, &fs->vol.sb.table);
    uint64_t slots = fs->vol.sb.table.size / sizeof(struct rfs_inode);
    for (uint64_t id = RFS_OBJ_BITMAP; id < slots; id++) {
        struct rfs_inode ino;
        if (ritterfs_get_inode(fs, id, &ino) && ino.type != RFS_TYPE_FREE)
            scrub_object(&s, id, &ino);
    }

    /* Blocks that could not be fixed in place move through a commit. */
    if (s.relocation_count) {
        int r = RFS_OK;
        for (uint64_t i = 0; i < s.relocation_count && r == RFS_OK; i++)
            r = rfs_mark_dirty(fs, s.relocations[i].object_id, s.relocations[i].index);
        if (r == RFS_OK)
            r = ritterfs_commit(fs);
        kprintf("                 relocation of %lu block(s): %s, generation %lu\n",
                s.relocation_count, ritterfs_error(r), fs->vol.sb.generation);
        if (r != RFS_OK)
            s.lost += s.relocation_count;
    }
    kprintf("  scrub:         %lu metadata blocks (2 copies each), %lu data blocks: "
            "%lu copies repaired, %lu not repairable\n", s.metadata_blocks, s.data_blocks,
            s.repaired, s.lost);
    kfree(s.relocations);
    return (int)s.lost;
}

int ritterfs_scrub(struct ritterfs *fs, bool force_relocate)
{
    rfs_lock(fs);
    int result = ritterfs_scrub_locked(fs, force_relocate);
    rfs_unlock(fs);
    return result;
}

#include "rfs_read.h"
#include "crc32.h"
#include "string.h"

bool rfs_superblock_valid(const struct rfs_volume *vol, const struct rfs_superblock *sb)
{
    return memcmp(sb->magic, "RITTERFS", 8) == 0 && sb->version >= 1 &&
           sb->version <= RFS_VERSION &&
           sb->block_size == RFS_BLOCK_SIZE && sb->block_count <= vol->capacity &&
           crc32c(sb, offsetof(struct rfs_superblock, checksum)) == sb->checksum;
}

bool rfs_open(struct rfs_volume *vol)
{
    struct rfs_superblock *candidate = rfs_alloc_block();
    vol->slot = 0;
    if (!candidate)
        return false;
    /* Both slots; the valid one with the highest generation wins. */
    for (int slot = RFS_SLOT_A; slot <= RFS_SLOT_B; slot++) {
        if (vol->read(vol, (uint64_t)slot, candidate) && rfs_superblock_valid(vol, candidate) &&
            (!vol->slot || candidate->generation > vol->sb.generation)) {
            vol->sb = *candidate;
            vol->slot = slot;
        }
    }
    rfs_free_block(candidate);
    return vol->slot != 0;
}

/* Read one copy of a block and check it against its checksum. */
static bool read_copy(struct rfs_volume *vol, uint64_t address, uint32_t checksum, void *buf)
{
    if (vol->read_verified && vol->read_verified(vol, address, checksum, buf))
        return true;
    if (!vol->read(vol, address, buf) || crc32c(buf, RFS_BLOCK_SIZE) != checksum)
        return false;
    if (vol->verified)
        vol->verified(vol, address, checksum);
    return true;
}

bool rfs_read_pointer(struct rfs_volume *vol, const struct rfs_pointer *p, void *buf)
{
    if (p->address == 0) {
        memset(buf, 0, RFS_BLOCK_SIZE);
        return true;
    }
    if (read_copy(vol, p->address, p->checksum, buf))
        return true;
    vol->bad_copies++;
    return p->address2 && read_copy(vol, p->address2, p->checksum, buf);
}

bool rfs_read_block(struct rfs_volume *vol, uint64_t object_id, const struct rfs_inode *inode,
                    uint64_t index, void *buf)
{
    const uint8_t *pending = vol->pending ? vol->pending(vol, object_id, index) : 0;
    if (pending) {
        memcpy(buf, pending, RFS_BLOCK_SIZE);
        return true;
    }
    if (inode->flags & RFS_FLAG_INLINE) {
        memset(buf, 0, RFS_BLOCK_SIZE);
        if (index == 0)
            memcpy(buf, inode->inline_data, inode->size < RFS_INLINE_MAX ? inode->size
                                                                          : RFS_INLINE_MAX);
        return true;
    }
    /* Walk down the tree: at each level the base-128 digit of the index
     * selects one of the 128 pointers. */
    struct rfs_pointer p = inode->root;
    for (int level = inode->height; level > 0; level--) {
        if (p.address == 0)
            break;                                  /* hole: the whole subtree is zero */
        if (!rfs_read_pointer(vol, &p, buf))
            return false;
        unsigned shift = 7 * (unsigned)(level - 1);
        if ((index >> shift) >= RFS_POINTERS_PER_BLOCK) {
            p.address = 0;                          /* beyond the tree: not written yet */
            break;
        }
        memcpy(&p, (uint8_t *)buf + ((index >> shift) & 127) * 32, sizeof(p));
        index &= (1ULL << shift) - 1;
    }
    return rfs_read_pointer(vol, &p, buf);
}

bool rfs_get_inode(struct rfs_volume *vol, uint64_t object_id, struct rfs_inode *out)
{
    if (object_id == RFS_OBJ_TABLE) {
        *out = vol->sb.table;
        return true;
    }
    uint64_t offset = object_id * sizeof(struct rfs_inode);
    if (object_id == 0 || offset + sizeof(*out) > vol->sb.table.size)
        return false;
    uint8_t *buf = rfs_alloc_block();
    bool ok = buf && rfs_read_block(vol, RFS_OBJ_TABLE, &vol->sb.table,
                                    offset / RFS_BLOCK_SIZE, buf);
    if (ok)
        memcpy(out, buf + offset % RFS_BLOCK_SIZE, sizeof(*out));
    if (buf)
        rfs_free_block(buf);
    return ok;
}

/*
 * The lowest index block on the way to data block `index` (it holds the
 * pointers to 128 data blocks), read into leaf; all zeros for a hole.
 * For a tree of height 1 or more, with index inside it.
 */
static bool read_leaf(struct rfs_volume *vol, const struct rfs_inode *inode, uint64_t index,
                      void *leaf)
{
    struct rfs_pointer p = inode->root;
    for (int level = inode->height; level > 1 && p.address; level--) {
        if (!rfs_read_pointer(vol, &p, leaf))
            return false;
        unsigned shift = 7 * (unsigned)(level - 1);
        memcpy(&p, (uint8_t *)leaf + ((index >> shift) & 127) * 32, sizeof(p));
        index &= (1ULL << shift) - 1;
    }
    return rfs_read_pointer(vol, &p, leaf);         /* address 0: zeros */
}

/*
 * Reading a file block by block from the top of its tree costs a read per
 * level for every block; consecutive blocks share their lowest index block,
 * so it is kept (leaf, covering blocks leaf_first .. leaf_first + 127).
 */
int64_t rfs_read(struct rfs_volume *vol, uint64_t object_id, const struct rfs_inode *inode,
                 uint64_t offset, void *buf, uint64_t size)
{
    if (offset >= inode->size)
        return 0;
    if (size > inode->size - offset)
        size = inode->size - offset;
    uint8_t *block = rfs_alloc_block();
    bool tree = inode->height > 0 && !(inode->flags & RFS_FLAG_INLINE);
    uint8_t *leaf = tree ? rfs_alloc_block() : 0;
    uint64_t leaf_first = UINT64_MAX;
    bool ok = block && (!tree || leaf);
    uint64_t done = 0;
    while (ok && done < size) {
        uint64_t pos = offset + done;
        uint64_t index = pos / RFS_BLOCK_SIZE, in_block = pos % RFS_BLOCK_SIZE;
        uint64_t n = RFS_BLOCK_SIZE - in_block;
        if (n > size - done)
            n = size - done;
        const uint8_t *pending = vol->pending ? vol->pending(vol, object_id, index) : 0;
        if (pending || !tree || index >> (7 * (unsigned)inode->height)) {
            ok = rfs_read_block(vol, object_id, inode, index, block);   /* the general way */
        } else {
            if (index - index % 128 != leaf_first) {
                ok = read_leaf(vol, inode, index, leaf);
                leaf_first = ok ? index - index % 128 : UINT64_MAX;
            }
            struct rfs_pointer p;
            memcpy(&p, leaf + (index % 128) * 32, sizeof(p));
            ok = ok && rfs_read_pointer(vol, &p, block);
        }
        if (ok)
            memcpy((uint8_t *)buf + done, block + in_block, n);
        done += n;
    }
    if (block)
        rfs_free_block(block);
    if (leaf)
        rfs_free_block(leaf);
    return ok ? (int64_t)done : -1;
}

bool rfs_iterate_dir(struct rfs_volume *vol, uint64_t dir_id, const struct rfs_inode *dir,
                     rfs_dir_callback cb, void *ctx)
{
    if (dir->type != RFS_TYPE_DIR)
        return false;
    uint8_t *block = rfs_alloc_block();
    if (!block)
        return false;
    bool ok = true;
    for (uint64_t pos = 0; pos < dir->size;) {
        uint64_t in_block = pos % RFS_BLOCK_SIZE;
        if (in_block == 0 && !rfs_read_block(vol, dir_id, dir, pos / RFS_BLOCK_SIZE, block)) {
            ok = false;
            break;
        }
        uint64_t object_id;
        uint16_t entry_size;
        uint8_t name_len = block[in_block + 10], type = block[in_block + 11];
        memcpy(&object_id, block + in_block, 8);
        memcpy(&entry_size, block + in_block + 8, 2);
        /* An entry must hold its name, be 8-aligned and stay in its block. */
        if (entry_size < 12u + name_len || entry_size % 8 || in_block + entry_size > RFS_BLOCK_SIZE ||
            pos + entry_size > dir->size) {
            ok = false;
            break;
        }
        if (object_id && name_len && !cb(ctx, (const char *)block + in_block + 12, name_len,
                                         object_id, type))
            break;
        pos += entry_size;
    }
    rfs_free_block(block);
    return ok;
}

int rfs_dir_next(struct rfs_volume *vol, uint64_t dir_id, const struct rfs_inode *dir,
                 uint64_t *pos, char *name, uint8_t *name_len, uint64_t *object_id,
                 uint8_t *type)
{
    if (dir->type != RFS_TYPE_DIR)
        return -1;
    uint8_t *block = rfs_alloc_block();
    if (!block)
        return -1;
    int result = 0;
    uint64_t loaded = ~0ULL;                /* the block in the buffer */
    while (*pos < dir->size) {
        uint64_t in_block = *pos % RFS_BLOCK_SIZE;
        if (*pos / RFS_BLOCK_SIZE != loaded) {
            loaded = *pos / RFS_BLOCK_SIZE;
            if (!rfs_read_block(vol, dir_id, dir, loaded, block)) {
                result = -1;
                break;
            }
        }
        uint64_t id;
        uint16_t entry_size;
        uint8_t len = block[in_block + 10];
        memcpy(&id, block + in_block, 8);
        memcpy(&entry_size, block + in_block + 8, 2);
        if (entry_size < 12u + len || entry_size % 8 || in_block + entry_size > RFS_BLOCK_SIZE ||
            *pos + entry_size > dir->size) {
            result = -1;
            break;
        }
        *pos += entry_size;
        if (id && len) {
            memcpy(name, block + in_block + 12, len);
            *name_len = len;
            *object_id = id;
            *type = block[in_block + 11];
            result = 1;
            break;
        }
    }
    rfs_free_block(block);
    return result;
}

struct lookup {
    const char *name;
    size_t len;
    uint64_t found;
};

static bool match_name(void *ctx, const char *name, size_t len, uint64_t id, uint8_t type)
{
    struct lookup *l = ctx;
    (void)type;
    if (len == l->len && memcmp(name, l->name, len) == 0) {
        l->found = id;
        return false;
    }
    return true;
}

bool rfs_lookup(struct rfs_volume *vol, const char *path, uint64_t *object_id,
                struct rfs_inode *inode)
{
    uint64_t id = RFS_OBJ_ROOT;
    struct rfs_inode cur;
    if (!rfs_get_inode(vol, id, &cur))
        return false;
    while (*path) {
        while (*path == '/')
            path++;
        if (!*path)
            break;
        size_t len = 0;
        while (path[len] && path[len] != '/')
            len++;
        struct lookup l = { path, len, 0 };
        if (cur.type != RFS_TYPE_DIR)
            return false;
        int indexed = vol->find_entry ? vol->find_entry(vol, id, &cur, path, len, &l.found) : -1;
        if ((indexed < 0 && !rfs_iterate_dir(vol, id, &cur, match_name, &l)) || !l.found ||
            !rfs_get_inode(vol, l.found, &cur))
            return false;
        id = l.found;
        path += len;
    }
    *object_id = id;
    *inode = cur;
    return true;
}

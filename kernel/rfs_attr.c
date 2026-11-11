/*
 * RitterFS attributes, the read side: named values attached to files and
 * directories (key=value), kept in one attribute table (docs/ritterfs.md).
 * Changes are in rfs_write.c. Snapshot views read the table as it was:
 * it is an ordinary object of each state.
 */
#include "ritterfs.h"
#include "heap.h"
#include "string.h"

bool rfs_attr_key_valid(const char *key, size_t len)
{
    if (len == 0 || len > RFS_ATTR_KEY_MAX)
        return false;
    for (size_t i = 0; i < len; i++)
        if (key[i] == '=' || key[i] == 0)
            return false;
    return true;
}

uint8_t *rfs_table_content(struct ritterfs *fs, uint64_t id, struct rfs_inode *ino,
                           uint64_t *size)
{
    *size = 0;
    memset(ino, 0, sizeof(*ino));
    if (!id)
        return kmalloc(1);
    if (!ritterfs_get_inode(fs, id, ino) || ino->type != RFS_TYPE_TABLE) {
        /* A view of a snapshot older than the table has the live state's
         * table ID, which meant something else then (or nothing): it had
         * no attributes. In the live state this is damage. */
        memset(ino, 0, sizeof(*ino));
        return fs->view ? kmalloc(1) : 0;
    }
    uint8_t *data = kmalloc(ino->size ? ino->size : 1);
    if (data && ritterfs_read(fs, id, ino, 0, data, ino->size) != (int64_t)ino->size) {
        kfree(data);
        return 0;
    }
    *size = ino->size;
    return data;
}

bool rfs_attr_next(const uint8_t *table, uint64_t size, uint64_t *at,
                   struct rfs_attr_record *r)
{
    if (*at + sizeof(*r) > size)
        return false;
    memcpy(r, table + *at, sizeof(*r));
    if (r->size < sizeof(*r) || r->size % RFS_ATTR_ALIGN || *at + r->size > size ||
        *at / RFS_BLOCK_SIZE != (*at + r->size - 1) / RFS_BLOCK_SIZE)
        return false;                       /* malformed: the check reports it */
    return true;
}

bool ritterfs_attr_each(struct ritterfs *fs, uint64_t object, ritterfs_attr_callback cb,
                        void *ctx)
{
    struct rfs_inode ino;
    uint64_t size;
    uint8_t *table = rfs_table_content(fs, fs->vol.sb.attribute_object, &ino, &size);
    if (!table)
        return false;
    struct rfs_attr_record r;
    for (uint64_t at = 0; rfs_attr_next(table, size, &at, &r); at += r.size) {
        if (!r.object_id || (object && r.object_id != object))
            continue;
        const char *key = (const char *)table + at + sizeof(r);
        if (!cb(ctx, r.object_id, key, r.key_length, (const uint8_t *)key + r.key_length,
                r.value_length))
            break;
    }
    kfree(table);
    return true;
}

struct attr_get {
    const char *key;
    size_t key_length;
    void *buf;
    uint64_t room;
    int64_t length;
};

static bool get_one(void *ctx, uint64_t object, const char *key, size_t key_length,
                    const uint8_t *value, size_t value_length)
{
    struct attr_get *g = ctx;
    (void)object;
    if (key_length != g->key_length || memcmp(key, g->key, key_length) != 0)
        return true;
    memcpy(g->buf, value, value_length < g->room ? value_length : g->room);
    g->length = (int64_t)value_length;
    return false;
}

int64_t ritterfs_attr_get(struct ritterfs *fs, uint64_t object, const char *key, void *buf,
                          uint64_t size)
{
    struct attr_get g = { key, strlen(key), buf, size, RFS_ERR_NOT_FOUND };
    if (!ritterfs_attr_each(fs, object, get_one, &g))
        return RFS_ERR_IO;
    return g.length;
}

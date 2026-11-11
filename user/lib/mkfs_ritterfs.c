/*
 * A new, empty RitterFS on a device (format, osinstall): what
 * scripts/ritterfs.py's format makes (docs/ritterfs.md): the object table
 * (objects 1-3: the table itself, the bitmap, the empty root directory,
 * inline) and the bitmap, each block twice (first copies from block 16
 * on, second copies from the middle of the device), then the superblock
 * of generation 1 in slot B. First the device's first 64 KiB are cleared:
 * both superblock slots and the marks of other file systems (a FAT boot
 * sector, an ext superblock), and for a whole disk the GPT's copy at the
 * end. scripts/samefs.py checks the result byte for byte.
 */
#include <stddef.h>
#include <string.h>
#include "mkfs.h"
#include "ritteros.h"
#include "../../common/rfs_format.h"

#define MIN_BLOCKS      512                 /* 2 MiB */
#define LEVELS          6                   /* tree levels: enough for any disk */
#define OBJECTS         4                   /* IDs 0-3: none, table, bitmap, root */
#define GENERATION      1

static int fd;
static uint64_t block_count;                /* RitterFS blocks on the device */
static uint64_t next[2];                    /* where the next first and second copy go */
static uint64_t now;
static uint8_t block[RFS_BLOCK_SIZE];

static bool write_at(uint64_t offset, const void *data, size_t n)
{
    return mkfs_write(fd, offset, data, n);
}

/* A metadata block, written twice: the pointer to it. */
static bool put_block(const void *data, struct rfs_pointer *p)
{
    memset(p, 0, sizeof(*p));
    p->address = next[0]++;
    p->address2 = next[1]++;
    p->checksum = crc32c(data, RFS_BLOCK_SIZE);
    p->birth = GENERATION;
    return write_at(p->address * RFS_BLOCK_SIZE, data, RFS_BLOCK_SIZE) &&
           write_at(p->address2 * RFS_BLOCK_SIZE, data, RFS_BLOCK_SIZE);
}

/* The tree of a table whose content is count blocks (all present): data
 * blocks in order, each indirect block written once its pointers are
 * complete. Its height and root go to *height and *root. */
static uint8_t indirect[LEVELS][RFS_BLOCK_SIZE];
static uint64_t filled[LEVELS];             /* pointers in indirect[level] */
static uint64_t pushed[LEVELS];             /* pointers handed to a level so far */
static uint64_t level_count[LEVELS + 1];    /* blocks on each level (0: data) */
static int tree_height;
static struct rfs_pointer tree_root;

static int height_for(uint64_t blocks)
{
    int height = 0;
    for (uint64_t capacity = 1; capacity < blocks; capacity *= RFS_POINTERS_PER_BLOCK)
        height++;
    return height;
}

/* The blocks of a tree of count data blocks: data and indirect ones. */
static uint64_t tree_blocks(uint64_t count)
{
    uint64_t total = count;
    for (int level = height_for(count); level > 0; level--)
        total += count = (count + RFS_POINTERS_PER_BLOCK - 1) / RFS_POINTERS_PER_BLOCK;
    return total;
}

static void tree_start(uint64_t count)
{
    tree_height = height_for(count);
    level_count[0] = count;
    for (int level = 1; level <= tree_height; level++)
        level_count[level] = (level_count[level - 1] + RFS_POINTERS_PER_BLOCK - 1) /
                             RFS_POINTERS_PER_BLOCK;
    memset(filled, 0, sizeof(filled));
    memset(pushed, 0, sizeof(pushed));
}

/* Hand the pointer to a block of level (0: data) to the level above. */
static bool tree_add(int level, const struct rfs_pointer *p)
{
    if (level == tree_height) {
        tree_root = *p;
        return true;
    }
    int up = level + 1;
    if (filled[up] == 0)
        memset(indirect[up], 0, RFS_BLOCK_SIZE);
    memcpy(indirect[up] + filled[up]++ * sizeof(*p), p, sizeof(*p));
    pushed[up]++;
    if (filled[up] < RFS_POINTERS_PER_BLOCK && pushed[up] < level_count[level])
        return true;
    struct rfs_pointer q;
    filled[up] = 0;
    return put_block(indirect[up], &q) && tree_add(up, &q);
}

static void inode(struct rfs_inode *n, uint16_t type, uint64_t size)
{
    memset(n, 0, sizeof(*n));
    n->type = type;
    n->link_count = 1;
    n->size = size;
    n->created = n->modified = n->changed = now;
    n->generation = GENERATION;
}

/* The first copies and the second ones must stay apart. */
static uint64_t meta_blocks(void)
{
    uint64_t bitmap_bytes = (block_count + 7) / 8;
    return 1 + tree_blocks((bitmap_bytes + RFS_BLOCK_SIZE - 1) / RFS_BLOCK_SIZE);
}

/* Set the bits of blocks [from, to) that fall into bitmap block i. */
static void mark(uint8_t *bits, uint64_t i, uint64_t from, uint64_t to)
{
    uint64_t first = i * RFS_BLOCK_SIZE * 8, last = first + RFS_BLOCK_SIZE * 8;
    for (uint64_t b = from > first ? from : first; b < to && b < last; b++)
        bits[(b - first) / 8] |= (uint8_t)(1 << (b % 8));
}

const char *mkfs_ritterfs_check(uint64_t bytes)
{
    block_count = bytes / RFS_BLOCK_SIZE;
    if (block_count < MIN_BLOCKS)
        return "is too small (RitterFS needs at least 2.0 MiB)";
    if (RFS_FIRST_ALLOCATABLE + meta_blocks() > block_count / 2)
        return "is too small for its bitmap";
    return 0;
}

bool mkfs_ritterfs(int device, const char *label, bool whole_disk, uint64_t device_bytes,
                   uint64_t at)
{
    fd = device, now = at;
    block_count = device_bytes / RFS_BLOCK_SIZE;
    /* Clear what was there: superblock slots, other file systems' marks. */
    if (!mkfs_zero(fd, 0, MKFS_CLEAR))
        return false;

    uint64_t meta = meta_blocks();
    uint64_t bitmap_bytes = (block_count + 7) / 8;
    uint64_t bitmap_blocks = (bitmap_bytes + RFS_BLOCK_SIZE - 1) / RFS_BLOCK_SIZE;
    next[0] = RFS_FIRST_ALLOCATABLE;
    next[1] = block_count / 2;

    /* The object table: one block (4 inodes of 256 bytes). */
    struct rfs_inode table[OBJECTS];
    memset(table, 0, sizeof(table));
    inode(&table[RFS_OBJ_ROOT], RFS_TYPE_DIR, 0);
    table[RFS_OBJ_ROOT].flags = RFS_FLAG_INLINE;
    struct rfs_pointer table_block;
    uint64_t table_address = next[0], table_address2 = next[1];
    next[0]++, next[1]++;                   /* written last: it holds the bitmap's inode */

    /* The bitmap: the blocks in use are known before it is written. */
    tree_start(bitmap_blocks);
    for (uint64_t i = 0; i < bitmap_blocks; i++) {
        /* In use: the reserved blocks and the first copies from the start,
         * the second copies from the middle. */
        memset(block, 0, sizeof(block));
        mark(block, i, 0, RFS_FIRST_ALLOCATABLE + meta);
        mark(block, i, block_count / 2, block_count / 2 + meta);
        struct rfs_pointer p;
        if (!put_block(block, &p) || !tree_add(0, &p))
            return false;
    }
    inode(&table[RFS_OBJ_BITMAP], RFS_TYPE_TABLE, bitmap_bytes);
    table[RFS_OBJ_BITMAP].height = (uint8_t)tree_height;
    table[RFS_OBJ_BITMAP].root = tree_root;

    uint64_t saved[2] = { next[0], next[1] };
    next[0] = table_address, next[1] = table_address2;
    memset(block, 0, sizeof(block));
    memcpy(block, table, sizeof(table));
    if (!put_block(block, &table_block))
        return false;
    next[0] = saved[0], next[1] = saved[1];

    /* A whole disk's GPT copy in its last sector would make the disk look
     * partitioned again (it lies in a free block, or after the last one). */
    if (whole_disk && !mkfs_zero(fd, device_bytes - 512, 512))
        return false;

    /* The superblock, last: generation 1 in slot B (generation % 2). */
    struct rfs_superblock *sb = (struct rfs_superblock *)block;
    memset(block, 0, sizeof(block));
    memcpy(sb->magic, "RITTERFS", 8);
    sb->version = 1;
    sb->block_size = RFS_BLOCK_SIZE;
    sb->block_count = block_count;
    sb->generation = GENERATION;
    sb->commit_time = sb->created_time = now;
    ritteros_random(sb->uuid, sizeof(sb->uuid));
    sb->uuid[7] = (uint8_t)((sb->uuid[7] & 0x0F) | 0x40);      /* a version 4 UUID */
    sb->uuid[8] = (uint8_t)((sb->uuid[8] & 0x3F) | 0x80);
    memcpy(sb->label, label, strlen(label));
    sb->used_blocks = RFS_FIRST_ALLOCATABLE + 2 * meta;
    sb->next_object_id = OBJECTS;
    inode(&sb->table, RFS_TYPE_TABLE, sizeof(table));
    sb->table.root = table_block;
    sb->checksum = crc32c(sb, offsetof(struct rfs_superblock, checksum));
    return write_at((uint64_t)RFS_SLOT_B * RFS_BLOCK_SIZE, block, RFS_BLOCK_SIZE);
}

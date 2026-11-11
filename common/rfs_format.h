/*
 * RitterFS on-disk structures, as specified in docs/ritterfs.md (and
 * implemented on the host by scripts/ritterfs.py). Shared by the bootloader
 * and the kernel.
 */
#ifndef RITTEROS_RFS_FORMAT_H
#define RITTEROS_RFS_FORMAT_H

#include <stdint.h>

#define RFS_BLOCK_SIZE      4096
#define RFS_INLINE_MAX      160
#define RFS_POINTERS_PER_BLOCK (RFS_BLOCK_SIZE / 32)
#define RFS_NAME_MAX        255
#define RFS_FIRST_ALLOCATABLE 16
#define RFS_SLOT_A          1           /* the two superblock slots (blocks) */
#define RFS_SLOT_B          2
#define RFS_VERSION         3           /* 1: no snapshots, 2: no attributes; all read */
#define RFS_SNAPSHOT_NAME_MAX 63

enum { RFS_TYPE_FREE, RFS_TYPE_FILE, RFS_TYPE_DIR, RFS_TYPE_TABLE,
       RFS_TYPE_SYMLINK };          /* (57a) its content: the path it names, 1-255 bytes */
enum { RFS_OBJ_TABLE = 1, RFS_OBJ_BITMAP = 2, RFS_OBJ_ROOT = 3 };
#define RFS_FLAG_INLINE 1
/* (35a) A fixed file (the swap file): its blocks stay where they are and are
 * written in place, so its data blocks' checksums are not kept (never
 * verified); it has no holes and is never written through the file system. */
#define RFS_FLAG_FIXED  2
#define RFS_MODE_SET     0x8000     /* the mode's permission bits are recorded */
#define RFS_MODE_SETUID  04000      /* a program runs as its file's owner (50b) */
#define RFS_MODE_STICKY  01000      /* (57b) a folder: only an entry's or the folder's owner
                                     * removes or renames the entry (/tmp) */
#define RFS_MODE_BITS    05777      /* what a mode may record: setuid, sticky, rwxrwxrwx */
#define RFS_MODE_DEFAULT 0755       /* what an inode without a recorded mode allows */

/* Whether an object's blocks are metadata (two copies): everything but a
 * file's data and a symbolic link's path. */
static inline int rfs_is_metadata(uint16_t type)
{
    return type != RFS_TYPE_FILE && type != RFS_TYPE_SYMLINK;
}

/* The permission bits an inode allows (0755 if none are recorded). */
static inline unsigned rfs_mode(uint16_t mode)
{
    return mode & RFS_MODE_SET ? mode & RFS_MODE_BITS : RFS_MODE_DEFAULT;
}

struct rfs_pointer {
    uint64_t address;               /* first copy; 0 = none */
    uint64_t address2;              /* second copy (metadata) */
    uint32_t checksum;              /* CRC-32C of the block */
    uint32_t reserved;
    uint64_t birth;                 /* generation that wrote it */
} __attribute__((packed));

struct rfs_inode {
    uint16_t type;
    uint16_t flags;
    uint32_t link_count;
    uint64_t size;
    uint64_t created, modified, changed;
    uint64_t generation;
    uint8_t height;
    /* Owner and permissions (all 0 before: root, 0755). */
    uint32_t uid, gid;
    uint16_t mode;                  /* RFS_MODE_SET | rwxrwxrwx bits; 0: not recorded */
    uint8_t reserved[5];
    struct rfs_pointer root;
    uint8_t inline_data[RFS_INLINE_MAX];
} __attribute__((packed));

struct rfs_superblock {
    char magic[8];                  /* "RITTERFS" */
    uint32_t version;
    uint32_t block_size;
    uint64_t block_count;
    uint64_t generation;
    uint64_t commit_time;
    uint64_t created_time;
    uint8_t uuid[16];
    char label[64];
    uint64_t used_blocks;
    uint64_t next_object_id;
    /* Version 2 (0 in version 1): blocks born in a generation up to
     * pinned_generation are shared with snapshots (or copies) and are not
     * freed by later changes; deferred_blocks counts such frees held back
     * since the last sweep; snapshot_object is the object ID of the
     * snapshot table (0: none). */
    uint64_t pinned_generation;
    uint64_t deferred_blocks;
    uint64_t snapshot_object;
    /* Version 3 (0 before): object ID of the attribute table (0: none). */
    uint64_t attribute_object;
    uint8_t reserved[80];
    struct rfs_inode table;         /* inode of the object table */
    uint8_t reserved2[3580];
    uint32_t checksum;              /* CRC-32C of everything before it */
} __attribute__((packed));

/* An entry of the snapshot table (512 bytes; generation 0: a free entry):
 * the object table's inode as it was when the snapshot was taken. */
struct rfs_snapshot {
    char name[64];                  /* UTF-8, NUL-padded */
    uint64_t generation;            /* the commit it shows */
    uint64_t created;               /* time */
    uint8_t reserved[176];
    struct rfs_inode table;
} __attribute__((packed));

/* A record of the attribute table: a key and a value for one object, then
 * the key, the value and zero padding. Sizes are multiples of 16 and a
 * record never crosses a block; object_id 0 marks free space. */
#define RFS_ATTR_KEY_MAX    63
#define RFS_ATTR_VALUE_MAX  1024
#define RFS_ATTR_ALIGN      16

struct rfs_attr_record {
    uint64_t object_id;
    uint16_t size;                  /* the whole record */
    uint8_t key_length;             /* 1-63, UTF-8, no '=' and no NUL */
    uint8_t reserved;
    uint16_t value_length;          /* 0-1024, any bytes */
    uint16_t reserved2;
} __attribute__((packed));

_Static_assert(sizeof(struct rfs_attr_record) == 16, "RitterFS attribute record size");
_Static_assert(sizeof(struct rfs_snapshot) == 512, "RitterFS snapshot entry size");
_Static_assert(sizeof(struct rfs_pointer) == 32, "RitterFS pointer size");
_Static_assert(sizeof(struct rfs_inode) == 256, "RitterFS inode size");
_Static_assert(sizeof(struct rfs_superblock) == RFS_BLOCK_SIZE, "RitterFS superblock size");

#endif

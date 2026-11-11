/*
 * FAT12/16/32 (see fat.h).
 *
 * The layout: the boot sector's BIOS parameter block (BPB) gives the
 * sector size, the sectors per cluster, the reserved sectors before the
 * FATs, how many FAT copies there are and how long each is. FAT12 and
 * FAT16 have a fixed root directory after the FATs; FAT32 keeps its root
 * in clusters like any directory. The data area follows, cluster 2 first.
 * The FAT has one entry per cluster: the next cluster of the same file, or
 * an end mark. Which of the three a volume is follows from its number of
 * clusters alone (Microsoft's rule: below 4085 FAT12, below 65525 FAT16).
 *
 * A directory is an array of 32-byte entries: an 8.3 short name,
 * attributes, times, the first cluster and the size. A long name is kept
 * in entries just before it (attribute 0x0F), 13 UTF-16 characters each,
 * last part first, each with a checksum of the short name it belongs to.
 *
 * Writing: data first, then the FAT (a window of two sectors kept in
 * memory and written to every FAT copy when it moves on and at each
 * commit), then the directory entry (size, first cluster, time). FAT has
 * no journal: a stick pulled out in the middle of a change may lose it,
 * as with every system that writes FAT. FAT32's FSInfo sector (the free
 * clusters, where to look for the next) is kept up to date when it was
 * valid, written at each commit.
 */
#include "fat.h"
#include "heap.h"
#include "kprintf.h"
#include "ritterfs.h"
#include "string.h"
#include "sync.h"
#include "uefi.h"
#include "vfs.h"

#define ATTR_READ_ONLY  0x01
#define ATTR_VOLUME_ID  0x08
#define ATTR_DIRECTORY  0x10
#define ATTR_ARCHIVE    0x20
#define ATTR_LONG_NAME  0x0F
#define ENTRY_SIZE      32
#define LFN_CHARS       13
#define LFN_MAX         20              /* parts: 260 characters */

struct fat {
    struct vfs vfs;                     /* first: vfs.c casts */
    struct block_device *dev;
    int bits;                           /* 12, 16 or 32 */
    uint32_t sector_size, cluster_sectors, cluster_bytes;
    uint32_t device_blocks_per_sector;
    uint64_t fat_start, fat_sectors;    /* the first FAT copy (sectors) */
    uint32_t fats;                      /* copies of the FAT */
    uint64_t root_start, root_sectors;  /* FAT12/16: the fixed root directory */
    uint64_t data_start;                /* cluster 2 */
    uint32_t root_cluster;              /* FAT32 */
    uint32_t clusters;                  /* data clusters: 2 .. clusters + 1 */
    char label[12];
    struct mutex lock;
    /* One FAT window of two sectors (an entry of FAT12 may span two), one
     * sector of a directory, and where the last read in a chain ended. */
    uint8_t *fat_window;
    uint64_t fat_window_at;             /* sector; ~0: empty */
    uint32_t fat_window_count;          /* sectors in it: 1 or 2 */
    bool fat_window_dirty;              /* changed: written to every copy later */
    uint8_t *dir_sector;
    uint64_t dir_sector_at;
    uint32_t chain_first, chain_index, chain_cluster;
    uint8_t *cluster;                   /* one cluster for reading and writing */
    bool read_only;                     /* /boot */
    uint64_t fsinfo_sector;             /* FAT32; 0: none */
    uint32_t free_count;                /* FSInfo's: free clusters, ~0: unknown */
    uint32_t next_free;                 /* where to look for a free cluster */
    bool fsinfo_dirty;
};

static uint16_t u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool read_sectors(struct fat *f, uint64_t sector, uint32_t count, void *buf)
{
    return f->dev->read(f->dev, sector * f->device_blocks_per_sector,
                        count * f->device_blocks_per_sector, buf);
}

/* --- the FAT and cluster chains -------------------------------------------- */

#define CHAIN_END   0xFFFFFFFFu         /* the end of a chain */
#define CHAIN_BAD   0xFFFFFFFEu         /* a damaged chain: free, bad or out of range */

static bool write_sectors(struct fat *f, uint64_t sector, uint32_t count, const void *buf)
{
    return f->dev->write && f->dev->write(f->dev, sector * f->device_blocks_per_sector,
                                          count * f->device_blocks_per_sector, buf);
}

/* Write the changed FAT window to every FAT copy. */
static bool flush_window(struct fat *f)
{
    if (!f->fat_window_dirty)
        return true;
    bool ok = true;
    for (uint32_t copy = 0; copy < f->fats; copy++)
        ok &= write_sectors(f, f->fat_window_at + copy * f->fat_sectors, f->fat_window_count,
                            f->fat_window);
    f->fat_window_dirty = false;
    return ok;
}

/* The FAT entry of cluster c as stored (12, 16 or 28 bits); false if the
 * FAT cannot be read. */
static bool fat_get(struct fat *f, uint32_t c, uint32_t *value)
{
    uint64_t byte = f->bits == 32 ? (uint64_t)c * 4 : f->bits == 16 ? (uint64_t)c * 2
                                                                     : (uint64_t)c * 3 / 2;
    uint64_t sector = f->fat_start + byte / f->sector_size;
    if (f->fat_window_at != sector) {
        if (!flush_window(f))
            return false;
        uint32_t count = sector + 1 < f->fat_start + f->fat_sectors ? 2 : 1;
        if (!read_sectors(f, sector, count, f->fat_window)) {
            f->fat_window_at = ~0ull;
            return false;
        }
        f->fat_window_at = sector;
        f->fat_window_count = count;
    }
    const uint8_t *p = f->fat_window + byte % f->sector_size;
    if (f->bits == 32)
        *value = u32(p) & 0x0FFFFFFF;
    else if (f->bits == 16)
        *value = u16(p);
    else
        *value = c & 1 ? u16(p) >> 4 : u16(p) & 0xFFF;
    return true;
}

/* Set the FAT entry of cluster c (in the window; written later). */
static bool fat_set(struct fat *f, uint32_t c, uint32_t value)
{
    uint32_t old;
    if (!fat_get(f, c, &old))                       /* the window holds c's entry now */
        return false;
    uint64_t byte = f->bits == 32 ? (uint64_t)c * 4 : f->bits == 16 ? (uint64_t)c * 2
                                                                     : (uint64_t)c * 3 / 2;
    uint8_t *p = f->fat_window + byte % f->sector_size;
    if (f->bits == 32) {
        value = (u32(p) & 0xF0000000) | (value & 0x0FFFFFFF);  /* the top 4 bits stay */
        p[0] = (uint8_t)value, p[1] = (uint8_t)(value >> 8);
        p[2] = (uint8_t)(value >> 16), p[3] = (uint8_t)(value >> 24);
    } else if (f->bits == 16) {
        p[0] = (uint8_t)value, p[1] = (uint8_t)(value >> 8);
    } else if (c & 1) {
        p[0] = (uint8_t)((p[0] & 0x0F) | (value << 4 & 0xF0));
        p[1] = (uint8_t)(value >> 4);
    } else {
        p[0] = (uint8_t)value;
        p[1] = (uint8_t)((p[1] & 0xF0) | (value >> 8 & 0x0F));
    }
    f->fat_window_dirty = true;
    f->chain_first = 0;                             /* chains may have changed */
    return true;
}

static uint32_t end_mark(struct fat *f)
{
    return f->bits == 32 ? 0x0FFFFFFF : f->bits == 16 ? 0xFFFF : 0xFFF;
}

/* The entry of cluster c: the next cluster, CHAIN_END or CHAIN_BAD. */
static uint32_t next_cluster(struct fat *f, uint32_t c)
{
    uint32_t v;
    if (!fat_get(f, c, &v))
        return CHAIN_BAD;
    uint32_t end = f->bits == 32 ? 0x0FFFFFF8 : f->bits == 16 ? 0xFFF8 : 0xFF8;
    if (v >= end)
        return CHAIN_END;
    if (v == end - 1 || v < 2 || v >= f->clusters + 2)
        return CHAIN_BAD;
    return v;
}

/* The index-th cluster of the chain from first (CHAIN_END past its end,
 * CHAIN_BAD if it is damaged). Reading on from the last place is quick. */
static uint32_t chain_at(struct fat *f, uint32_t first, uint32_t index)
{
    if (first < 2 || first >= f->clusters + 2)
        return CHAIN_BAD;
    uint32_t c = first, at = 0;
    if (f->chain_first == first && f->chain_index <= index)
        c = f->chain_cluster, at = f->chain_index;
    while (at < index) {
        c = next_cluster(f, c);
        if (c == CHAIN_END || c == CHAIN_BAD)
            return c;
        if (++at > f->clusters)
            return CHAIN_BAD;                       /* a loop */
    }
    f->chain_first = first, f->chain_index = index, f->chain_cluster = c;
    return c;
}

static uint64_t cluster_sector(struct fat *f, uint32_t c)
{
    return f->data_start + (uint64_t)(c - 2) * f->cluster_sectors;
}

/* --- directory entries ----------------------------------------------------- */

/* The 32 bytes at byte pos of the directory whose first cluster is first
 * (0: FAT12/16's fixed root), and where they lie on the device (bytes).
 * 1, 0 past the directory's end, -1 if it cannot be read. */
static int dir_entry(struct fat *f, uint32_t first, uint64_t pos, uint8_t *out, uint64_t *where)
{
    uint64_t sector;
    if (first == 0) {
        if (pos >= f->root_sectors * f->sector_size)
            return 0;
        sector = f->root_start + pos / f->sector_size;
    } else {
        uint32_t c = chain_at(f, first, (uint32_t)(pos / f->cluster_bytes));
        if (c == CHAIN_END)
            return 0;
        if (c == CHAIN_BAD)
            return -1;
        sector = cluster_sector(f, c) + pos % f->cluster_bytes / f->sector_size;
    }
    if (f->dir_sector_at != sector) {
        if (!read_sectors(f, sector, 1, f->dir_sector)) {
            f->dir_sector_at = ~0ull;
            return -1;
        }
        f->dir_sector_at = sector;
    }
    memcpy(out, f->dir_sector + pos % f->sector_size, ENTRY_SIZE);
    *where = sector * f->sector_size + pos % f->sector_size;
    return 1;
}

static uint8_t short_checksum(const uint8_t *name)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name[i]);
    return sum;
}

/* "README.TXT", or "readme.txt" with the case flags Windows sets (byte 12:
 * 0x08 a lower-case base, 0x10 a lower-case extension). */
static size_t short_name(const uint8_t *e, char *out)
{
    size_t n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        uint8_t c = i == 0 && e[0] == 0x05 ? 0xE5 : e[i];
        if (c >= 'A' && c <= 'Z' && (e[12] & 0x08))
            c = (uint8_t)(c + 32);
        out[n++] = c < 0x80 ? (char)c : '?';
    }
    if (e[8] != ' ')
        out[n++] = '.';
    for (int i = 8; i < 11 && e[i] != ' '; i++) {
        uint8_t c = e[i];
        if (c >= 'A' && c <= 'Z' && (e[12] & 0x10))
            c = (uint8_t)(c + 32);
        out[n++] = c < 0x80 ? (char)c : '?';
    }
    return n;
}

/* UTF-16 to UTF-8; 0 if it does not fit in 255 bytes or is malformed. */
static size_t utf8_name(const uint16_t *s, size_t count, char *out)
{
    size_t n = 0;
    for (size_t i = 0; i < count && s[i]; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < count && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        else if (c >= 0xD800 && c < 0xE000)
            return 0;
        size_t need = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (n + need > RFS_NAME_MAX)
            return 0;
        if (need == 1) {
            out[n++] = (char)c;
        } else if (need == 2) {
            out[n++] = (char)(0xC0 | c >> 6);
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else if (need == 3) {
            out[n++] = (char)(0xE0 | c >> 12);
            out[n++] = (char)(0x80 | (c >> 6 & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[n++] = (char)(0xF0 | c >> 18);
            out[n++] = (char)(0x80 | (c >> 12 & 0x3F));
            out[n++] = (char)(0x80 | (c >> 6 & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        }
    }
    return n;
}

/* One entry of a directory with both its names. */
struct found {
    uint8_t entry[ENTRY_SIZE];
    uint64_t where;                     /* the short entry's byte position */
    uint64_t long_where[LFN_MAX];       /* its long name's entries (device bytes) */
    int long_count;
    char name[RFS_NAME_MAX + 1];        /* the long name, else the short one */
    size_t name_len;
    char short_name[13];
    size_t short_len;
};

/* The next file or directory from byte *pos of a directory (labels, "."
 * and "..", deleted entries skipped). 1, 0 at its end, -1 if unreadable. */
static int next_entry(struct fat *f, uint32_t first, uint64_t *pos, struct found *out)
{
    uint16_t lfn[LFN_MAX * LFN_CHARS];
    int parts = 0, seen = 0;
    uint8_t checksum = 0;
    out->long_count = 0;
    for (;; *pos += ENTRY_SIZE) {
        uint8_t *e = out->entry;
        int r = dir_entry(f, first, *pos, e, &out->where);
        if (r <= 0)
            return r;
        if (e[0] == 0)
            return 0;                               /* no entries after this one */
        if (e[0] == 0xE5) {
            parts = 0;
            continue;
        }
        if ((e[11] & 0x3F) == ATTR_LONG_NAME) {
            int order = e[0] & 0x1F;
            if (e[0] & 0x40) {                      /* the last part comes first */
                parts = order <= LFN_MAX ? order : 0;
                seen = 0;
                checksum = e[13];
                memset(lfn, 0, sizeof(lfn));
                out->long_count = 0;
            }
            if (!parts || order < 1 || order > parts || e[13] != checksum) {
                parts = 0;
                continue;
            }
            if (out->long_count < LFN_MAX)
                out->long_where[out->long_count++] = out->where;
            static const uint8_t at[LFN_CHARS] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (int i = 0; i < LFN_CHARS; i++)
                lfn[(order - 1) * LFN_CHARS + i] = u16(e + at[i]);
            seen |= 1 << (order - 1);
            continue;
        }
        if (e[11] & ATTR_VOLUME_ID) {
            parts = 0;
            continue;
        }
        if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '))) {
            parts = 0;
            continue;
        }
        out->short_len = short_name(e, out->short_name);
        out->short_name[out->short_len] = 0;
        size_t n = 0;
        bool long_name = parts && seen == (1 << parts) - 1 && short_checksum(e) == checksum;
        if (!long_name)
            out->long_count = 0;
        if (long_name) {
            for (size_t i = 0; i < (size_t)parts * LFN_CHARS; i++)
                if (lfn[i] == 0xFFFF)
                    lfn[i] = 0;                     /* padding after the name */
            n = utf8_name(lfn, (size_t)parts * LFN_CHARS, out->name);
        }
        if (n == 0) {
            memcpy(out->name, out->short_name, out->short_len);
            n = out->short_len;
        }
        out->name[n] = 0;
        out->name_len = n;
        *pos += ENTRY_SIZE;
        return 1;
    }
}

/* --- inodes ------------------------------------------------------------------ */

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* A DOS date and time (2-second steps, from 1980) in nanoseconds since 1970. */
static uint64_t dos_time(uint16_t date, uint16_t time, uint8_t tenths)
{
    if (date == 0)
        return 0;
    unsigned day = date & 31, month = date >> 5 & 15;
    if (day == 0 || month == 0 || month > 12)
        return 0;
    int64_t secs = days_from_civil(1980 + (date >> 9), month, day) * 86400 +
                   (time >> 11) * 3600 + (time >> 5 & 63) * 60 + (time & 31) * 2 + tenths / 100;
    return (uint64_t)secs * 1000000000ULL + (uint64_t)(tenths % 100) * 10000000ULL;
}

static void root_inode(struct fat *f, struct rfs_inode *out)
{
    memset(out, 0, sizeof(*out));
    out->type = RFS_TYPE_DIR;
    out->link_count = 1;
    out->mode = RFS_MODE_SET | 0777;
    out->root.address = f->bits == 32 ? f->root_cluster : 0;
}

static void entry_inode(struct fat *f, const uint8_t *e, struct rfs_inode *out)
{
    memset(out, 0, sizeof(*out));
    bool dir = e[11] & ATTR_DIRECTORY;
    out->type = dir ? RFS_TYPE_DIR : RFS_TYPE_FILE;
    out->link_count = 1;
    out->size = dir ? 0 : u32(e + 28);
    out->created = dos_time(u16(e + 16), u16(e + 14), e[13]);
    out->modified = out->changed = dos_time(u16(e + 24), u16(e + 22), 0);
    unsigned mode = dir ? 0777 : 0666;
    if (e[11] & ATTR_READ_ONLY)
        mode &= ~0222u;
    out->mode = (uint16_t)(RFS_MODE_SET | mode);
    out->root.address = (f->bits == 32 ? (uint32_t)u16(e + 20) << 16 : 0) | u16(e + 26);
}

/* The directory a directory inode lists: its first cluster (0: the fixed
 * root of FAT12/16). */
static uint32_t dir_first(const struct rfs_inode *dir)
{
    return (uint32_t)dir->root.address;
}

/* --- the interface ------------------------------------------------------------ */

bool fat_get_inode(struct fat *f, uint64_t id, struct rfs_inode *out)
{
    if (id == FAT_ROOT_ID) {
        root_inode(f, out);
        return true;
    }
    if (id < 2)
        return false;
    uint64_t byte = (id - 2) * ENTRY_SIZE;
    uint64_t sector = byte / f->sector_size;
    mutex_lock(&f->lock);
    bool ok = true;
    if (f->dir_sector_at != sector) {
        ok = read_sectors(f, sector, 1, f->dir_sector);
        f->dir_sector_at = ok ? sector : ~0ull;
    }
    const uint8_t *e = f->dir_sector + byte % f->sector_size;
    ok = ok && e[0] != 0 && e[0] != 0xE5 && (e[11] & 0x3F) != ATTR_LONG_NAME &&
         !(e[11] & ATTR_VOLUME_ID);
    if (ok)
        entry_inode(f, e, out);
    mutex_unlock(&f->lock);
    return ok;
}

/* Names compare without regard to the case of A-Z. */
static bool same_name(const char *a, size_t an, const char *b, size_t bn)
{
    if (an != bn)
        return false;
    for (size_t i = 0; i < an; i++) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z')
            x = (char)(x - 32);
        if (y >= 'a' && y <= 'z')
            y = (char)(y - 32);
        if (x != y)
            return false;
    }
    return true;
}

/* Follow path from the root (under the lock): its ID and inode; with e,
 * also its directory entry (unless it is the root: e->where 0). */
static bool walk(struct fat *f, const char *path, uint64_t *id, struct rfs_inode *inode,
                 struct found *e)
{
    root_inode(f, inode);
    *id = FAT_ROOT_ID;
    e->where = 0;
    while (*path) {
        while (*path == '/')
            path++;
        const char *end = path;
        while (*end && *end != '/')
            end++;
        if (end == path)
            break;
        if (inode->type != RFS_TYPE_DIR)
            return false;
        uint32_t first = dir_first(inode);
        uint64_t pos = 0;
        int r;
        while ((r = next_entry(f, first, &pos, e)) == 1)
            if (same_name(e->name, e->name_len, path, (size_t)(end - path)) ||
                same_name(e->short_name, e->short_len, path, (size_t)(end - path)))
                break;
        if (r != 1)
            return false;
        entry_inode(f, e->entry, inode);
        *id = e->where / ENTRY_SIZE + 2;
        path = end;
    }
    return true;
}

bool fat_lookup(struct fat *f, const char *path, uint64_t *id, struct rfs_inode *inode)
{
    struct found *e = kmalloc(sizeof(*e));
    if (!e)
        return false;
    mutex_lock(&f->lock);
    bool ok = walk(f, path, id, inode, e);
    mutex_unlock(&f->lock);
    kfree(e);
    return ok;
}

int64_t fat_read(struct fat *f, uint64_t id, const struct rfs_inode *inode, uint64_t offset,
                 void *buf, uint64_t size)
{
    (void)id;
    if (inode->type != RFS_TYPE_FILE)
        return -1;
    if (offset >= inode->size)
        return 0;
    if (size > inode->size - offset)
        size = inode->size - offset;
    uint32_t first = (uint32_t)inode->root.address;
    mutex_lock(&f->lock);
    uint64_t done = 0;
    while (done < size) {
        uint64_t at = offset + done;
        uint32_t c = chain_at(f, first, (uint32_t)(at / f->cluster_bytes));
        if (c == CHAIN_END || c == CHAIN_BAD)
            break;                                  /* shorter than its size says */
        /* The sectors of this cluster that are wanted. */
        uint64_t within = at % f->cluster_bytes;
        uint64_t take = f->cluster_bytes - within;
        if (take > size - done)
            take = size - done;
        uint32_t s0 = (uint32_t)(within / f->sector_size);
        uint32_t s1 = (uint32_t)((within + take + f->sector_size - 1) / f->sector_size);
        if (!read_sectors(f, cluster_sector(f, c) + s0, s1 - s0, f->cluster))
            break;
        memcpy((uint8_t *)buf + done, f->cluster + within - (uint64_t)s0 * f->sector_size, take);
        done += take;
    }
    mutex_unlock(&f->lock);
    return done == size ? (int64_t)done : done > 0 ? (int64_t)done : -1;
}

int fat_dir_next(struct fat *f, uint64_t dir_id, const struct rfs_inode *dir, uint64_t *pos,
                 char *name, uint8_t *name_len, uint64_t *id, uint8_t *type)
{
    (void)dir_id;
    struct found *e = kmalloc(sizeof(*e));
    if (!e)
        return -1;
    mutex_lock(&f->lock);
    int r = next_entry(f, dir_first(dir), pos, e);
    mutex_unlock(&f->lock);
    if (r == 1) {
        memcpy(name, e->name, e->name_len);
        *name_len = (uint8_t)e->name_len;
        *id = e->where / ENTRY_SIZE + 2;
        *type = e->entry[11] & ATTR_DIRECTORY ? RFS_TYPE_DIR : RFS_TYPE_FILE;
    }
    kfree(e);
    return r;
}

struct vfs *fat_vfs(struct fat *f)
{
    return &f->vfs;
}

const char *fat_type(struct fat *f)
{
    return f->bits == 32 ? "FAT32" : f->bits == 16 ? "FAT16" : "FAT12";
}

struct block_device *fat_device(struct fat *f)
{
    return f->dev;
}

void fat_label(struct fat *f, char *out, size_t size)
{
    ksnprintf(out, size, "%s", f->label);
}

/* --- writing ----------------------------------------------------------------- */

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v), put16(p + 2, v >> 16);
}

/* A time (nanoseconds since 1970) as a DOS date and time (UTC: FAT keeps
 * no zone; before 1980, after 2107 or without a clock: 1980-01-01). */
static void dos_from(uint64_t ns, uint16_t *date, uint16_t *time, uint8_t *tenths)
{
    uint64_t secs = ns / 1000000000ULL, rest = secs % 86400;
    /* Days since 1970 to a civil date (Howard Hinnant's algorithm). */
    int64_t z = (int64_t)(secs / 86400) + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint64_t doe = (uint64_t)(z - era * 146097);
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint64_t mp = (5 * doy + 2) / 153;
    unsigned day = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
    unsigned month = (unsigned)(mp < 10 ? mp + 3 : mp - 9);
    int64_t year = (int64_t)yoe + era * 400 + (month <= 2);
    if (ns == 0 || year < 1980 || year > 2107) {
        *date = 1 << 5 | 1, *time = 0, *tenths = 0;
        return;
    }
    *date = (uint16_t)((year - 1980) << 9 | month << 5 | day);
    *time = (uint16_t)(rest / 3600 << 11 | rest / 60 % 60 << 5 | rest % 60 / 2);
    *tenths = (uint8_t)(rest % 2 * 100 + ns / 10000000 % 100);
}

static void dos_now(uint16_t *date, uint16_t *time, uint8_t *tenths)
{
    dos_from(uefi_clock_ns(), date, time, tenths);
}

/* The 32 bytes of the entry at device byte where (through the directory
 * sector kept in memory), and writing them back. */
static bool get_entry(struct fat *f, uint64_t where, uint8_t *e)
{
    uint64_t sector = where / f->sector_size;
    if (f->dir_sector_at != sector) {
        if (!read_sectors(f, sector, 1, f->dir_sector)) {
            f->dir_sector_at = ~0ull;
            return false;
        }
        f->dir_sector_at = sector;
    }
    memcpy(e, f->dir_sector + where % f->sector_size, ENTRY_SIZE);
    return true;
}

static bool put_entry(struct fat *f, uint64_t where, const uint8_t *e)
{
    uint8_t old[ENTRY_SIZE];
    if (!get_entry(f, where, old))
        return false;
    memcpy(f->dir_sector + where % f->sector_size, e, ENTRY_SIZE);
    return write_sectors(f, where / f->sector_size, 1, f->dir_sector);
}

/* A file's or directory's live short entry by its ID. */
static bool entry_by_id(struct fat *f, uint64_t id, uint8_t *e)
{
    return id >= 2 && get_entry(f, (id - 2) * ENTRY_SIZE, e) && e[0] != 0 && e[0] != 0xE5 &&
           (e[11] & 0x3F) != ATTR_LONG_NAME && !(e[11] & ATTR_VOLUME_ID);
}

static uint32_t entry_cluster(struct fat *f, const uint8_t *e)
{
    return (f->bits == 32 ? (uint32_t)u16(e + 20) << 16 : 0) | u16(e + 26);
}

static void set_entry_cluster(uint8_t *e, uint32_t c)
{
    put16(e + 20, c >> 16);
    put16(e + 26, c);
}

static void stamp(uint8_t *e, bool created)
{
    uint16_t date, time;
    uint8_t tenths;
    dos_now(&date, &time, &tenths);
    if (created) {
        e[13] = tenths;
        put16(e + 14, time), put16(e + 16, date);
    }
    put16(e + 18, date);                            /* last access */
    put16(e + 22, time), put16(e + 24, date);       /* last change */
}

/* A free cluster, marked as the end of a chain and linked after prev (if
 * not 0). 0 if there is none (or the FAT cannot be read). */
static uint32_t allocate(struct fat *f, uint32_t prev)
{
    uint32_t c = f->next_free >= 2 && f->next_free < f->clusters + 2 ? f->next_free : 2;
    for (uint32_t i = 0; i < f->clusters; i++) {
        uint32_t v;
        if (!fat_get(f, c, &v))
            return 0;
        if (v == 0) {
            if (!fat_set(f, c, end_mark(f)) || (prev && !fat_set(f, prev, c)))
                return 0;
            f->next_free = c + 1;
            if (f->free_count != ~0u)
                f->free_count--;
            f->fsinfo_dirty = true;
            return c;
        }
        c = c + 1 < f->clusters + 2 ? c + 1 : 2;
    }
    return 0;
}

static bool free_chain(struct fat *f, uint32_t first)
{
    uint32_t c = first;
    for (uint32_t n = 0; c >= 2 && c < f->clusters + 2 && n <= f->clusters; n++) {
        uint32_t next = next_cluster(f, c);
        if (!fat_set(f, c, 0))
            return false;
        if (f->free_count != ~0u)
            f->free_count++;
        f->fsinfo_dirty = true;
        if (next == CHAIN_END || next == CHAIN_BAD)
            break;
        c = next;
    }
    return true;
}

static bool zero_cluster(struct fat *f, uint32_t c)
{
    memset(f->cluster, 0, f->cluster_bytes);
    return write_sectors(f, cluster_sector(f, c), f->cluster_sectors, f->cluster);
}

/* The index-th cluster of the chain from *first, which grows to reach it
 * (a new chain's first cluster goes to *first). */
static int cluster_for(struct fat *f, uint32_t *first, uint32_t index, uint32_t *out)
{
    if (*first == 0 && !(*first = allocate(f, 0)))
        return RFS_ERR_NO_SPACE;
    uint32_t c = *first, i = 0;
    if (f->chain_first == *first && f->chain_index <= index)
        c = f->chain_cluster, i = f->chain_index;
    while (i < index) {
        uint32_t next = next_cluster(f, c);
        if (next == CHAIN_BAD)
            return RFS_ERR_IO;
        if (next == CHAIN_END && !(next = allocate(f, c)))
            return RFS_ERR_NO_SPACE;
        c = next;
        i++;
    }
    f->chain_first = *first, f->chain_index = index, f->chain_cluster = c;
    *out = c;
    return RFS_OK;
}

/* Write n bytes (zeros if data is 0) at byte pos of the chain from *first,
 * which grows as needed. Sectors written in part are read first. *done
 * counts the bytes written (also when it fails: a full volume). */
static int write_chain(struct fat *f, uint32_t *first, uint64_t pos, const uint8_t *data,
                       uint64_t n, uint64_t *written)
{
    uint32_t ss = f->sector_size;
    uint64_t done = 0;
    for (*written = 0; done < n; *written = done) {
        uint64_t at = pos + done;
        uint32_t c;
        int r = cluster_for(f, first, (uint32_t)(at / f->cluster_bytes), &c);
        if (r != RFS_OK)
            return r;
        uint64_t within = at % f->cluster_bytes;
        uint64_t take = f->cluster_bytes - within < n - done ? f->cluster_bytes - within : n - done;
        uint32_t s0 = (uint32_t)(within / ss), s1 = (uint32_t)((within + take + ss - 1) / ss);
        uint64_t sector = cluster_sector(f, c);
        if ((within % ss && !read_sectors(f, sector + s0, 1, f->cluster)) ||
            ((within + take) % ss &&
             !read_sectors(f, sector + s1 - 1, 1, f->cluster + (uint64_t)(s1 - 1 - s0) * ss)))
            return RFS_ERR_IO;
        uint8_t *to = f->cluster + within - (uint64_t)s0 * ss;
        if (data)
            memcpy(to, data + done, take);
        else
            memset(to, 0, take);
        if (!write_sectors(f, sector + s0, s1 - s0, f->cluster))
            return RFS_ERR_IO;
        done += take;
    }
    *written = done;
    return RFS_OK;
}

/* --- names --- */

static bool short_char(uint8_t c)
{
    static const char other[] = "!#$%&'()-@^_`{}~";
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return true;
    for (const char *o = other; *o; o++)
        if (c == (uint8_t)*o)
            return true;
    return false;
}

/* name as an 8.3 short name if it is one: each part 1-8 and 0-3 allowed
 * characters in one case (Windows' case flags keep lower case); false if
 * it needs a long name. */
static bool plain_short(const char *name, size_t len, uint8_t *out, uint8_t *flags)
{
    size_t dot = len;
    for (size_t i = 0; i < len; i++)
        if (name[i] == '.')
            dot = i;
    size_t base = dot, ext = dot < len ? len - dot - 1 : 0;
    if (base == 0 || base > 8 || ext > 3 || (dot < len && ext == 0))
        return false;
    memset(out, ' ', 11);
    *flags = 0;
    for (int part = 0; part < 2; part++) {
        size_t from = part ? dot + 1 : 0, n = part ? ext : base;
        bool lower = false, upper = false;
        for (size_t i = 0; i < n; i++) {
            uint8_t c = (uint8_t)name[from + i];
            lower |= c >= 'a' && c <= 'z';
            upper |= c >= 'A' && c <= 'Z';
            if (c >= 'a' && c <= 'z')
                c = (uint8_t)(c - 32);
            if (!short_char(c))
                return false;
            out[(part ? 8 : 0) + i] = c;
        }
        if (lower && upper)
            return false;
        if (lower)
            *flags |= part ? 0x10 : 0x08;
    }
    return true;
}

/* A long name as UTF-16 (units); -1 if it is no valid name. */
static int long_units(const char *name, size_t len, uint16_t *out)
{
    if (len == 0 || name[len - 1] == '.' || name[len - 1] == ' ' ||
        (len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
        return -1;
    int n = 0;
    for (size_t i = 0; i < len;) {
        uint8_t b = (uint8_t)name[i];
        uint32_t c;
        size_t need = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3
                    : (b & 0xF8) == 0xF0 ? 4 : 0;
        if (need == 0 || i + need > len)
            return -1;
        c = need == 1 ? b : need == 2 ? b & 0x1F : need == 3 ? b & 0x0F : b & 0x07;
        for (size_t k = 1; k < need; k++) {
            if (((uint8_t)name[i + k] & 0xC0) != 0x80)
                return -1;
            c = c << 6 | ((uint8_t)name[i + k] & 0x3F);
        }
        i += need;
        if (c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' ||
            c == '?' || c == '\\' || c == '|' || (c >= 0xD800 && c < 0xE000) || c > 0x10FFFF)
            return -1;
        if (n + (c >= 0x10000 ? 2 : 1) > 255)
            return -1;
        if (c >= 0x10000) {
            out[n++] = (uint16_t)(0xD800 + ((c - 0x10000) >> 10));
            out[n++] = (uint16_t)(0xDC00 + ((c - 0x10000) & 0x3FF));
        } else {
            out[n++] = (uint16_t)c;
        }
    }
    return n;
}

/* Whether a short name is taken in the directory. */
static int short_taken(struct fat *f, uint32_t dir, const uint8_t *name11, struct found *e)
{
    uint64_t pos = 0;
    int r;
    while ((r = next_entry(f, dir, &pos, e)) == 1)
        if (memcmp(e->entry, name11, 11) == 0)
            return 1;
    return r < 0 ? -1 : 0;
}

/* The alias Windows would make: the allowed characters, upper case, then
 * ~N (the lowest N free in the directory). */
static int make_alias(struct fat *f, uint32_t dir, const char *name, size_t len, uint8_t *out,
                      struct found *e)
{
    size_t dot = len;
    for (size_t i = 1; i < len; i++)
        if (name[i] == '.')
            dot = i;
    uint8_t base[8], ext[3];
    size_t nb = 0, ne = 0;
    for (size_t i = 0; i < dot && nb < 8; i++) {
        uint8_t c = (uint8_t)name[i];
        if (c == ' ' || c == '.')
            continue;
        if (c >= 'a' && c <= 'z')
            c = (uint8_t)(c - 32);
        if (c >= 0x80) {
            while (i + 1 < dot && ((uint8_t)name[i + 1] & 0xC0) == 0x80)
                i++;                                /* one '_' per character */
            c = '_';
        }
        base[nb++] = short_char(c) ? c : '_';
    }
    for (size_t i = dot + 1; i < len && ne < 3; i++) {
        uint8_t c = (uint8_t)name[i];
        if (c == ' ')
            continue;
        if (c >= 'a' && c <= 'z')
            c = (uint8_t)(c - 32);
        ext[ne++] = short_char(c) ? c : '_';
    }
    if (nb == 0)
        base[nb++] = '_';
    for (uint32_t n = 1; n < 1000000; n++) {
        char tail[9];
        int t = ksnprintf(tail, sizeof(tail), "~%u", n);
        size_t keep = nb + (size_t)t > 8 ? 8 - (size_t)t : nb;
        memset(out, ' ', 11);
        memcpy(out, base, keep);
        memcpy(out + keep, tail, (size_t)t);
        memcpy(out + 8, ext, ne);
        int r = short_taken(f, dir, out, e);
        if (r <= 0)
            return r < 0 ? RFS_ERR_IO : RFS_OK;
    }
    return RFS_ERR_EXISTS;
}

/* Where count free entries follow one another in the directory (device
 * bytes, in order); a directory in clusters grows by a cleared cluster
 * when there are not enough. */
static int free_run(struct fat *f, uint32_t dir, int count, uint64_t *where)
{
    uint8_t e[ENTRY_SIZE];
    int run = 0;
    for (uint64_t pos = 0;; pos += ENTRY_SIZE) {
        uint64_t w;
        int r = dir_entry(f, dir, pos, e, &w);
        if (r < 0)
            return RFS_ERR_IO;
        if (r == 0) {
            uint32_t c;
            if (dir == 0)
                return RFS_ERR_NO_SPACE;            /* FAT12/16's root is full */
            int error = cluster_for(f, &dir, (uint32_t)(pos / f->cluster_bytes), &c);
            if (error != RFS_OK)
                return error;
            if (!zero_cluster(f, c))
                return RFS_ERR_IO;
            f->dir_sector_at = ~0ull;
            pos -= ENTRY_SIZE;                      /* read it again */
            continue;
        }
        if (e[0] == 0 || e[0] == 0xE5) {
            where[run++] = w;
            if (run == count)
                return RFS_OK;
        } else {
            run = 0;
        }
    }
}

/* Add an entry named name (its other fields from entry) to the directory
 * dir: a short name if the name is one, else a long name and an alias. */
static int add_entry(struct fat *f, uint32_t dir, const char *name, size_t len, uint8_t *entry,
                     uint64_t *short_where, struct found *e)
{
    uint16_t units[256];
    int n = long_units(name, len, units);
    if (n < 0)
        return RFS_ERR_BAD_NAME;
    uint8_t flags;
    bool plain = plain_short(name, len, entry, &flags);
    if (plain) {
        int r = short_taken(f, dir, entry, e);
        if (r < 0)
            return RFS_ERR_IO;
        plain = r == 0;
    }
    int parts = 0;
    if (plain) {
        entry[12] = flags;
    } else {
        int r = make_alias(f, dir, name, len, entry, e);
        if (r != RFS_OK)
            return r;
        entry[12] = 0;
        parts = (n + LFN_CHARS - 1) / LFN_CHARS;
    }
    uint64_t where[LFN_MAX + 1];
    int r = free_run(f, dir, parts + 1, where);
    if (r != RFS_OK)
        return r;
    /* The long name: its last part first, in the entries before the short one. */
    uint8_t sum = short_checksum(entry);
    static const uint8_t at[LFN_CHARS] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    for (int k = 0; k < parts; k++) {
        int order = parts - k;
        uint8_t l[ENTRY_SIZE];
        memset(l, 0, sizeof(l));
        l[0] = (uint8_t)(order | (k == 0 ? 0x40 : 0));
        l[11] = ATTR_LONG_NAME;
        l[13] = sum;
        for (int i = 0; i < LFN_CHARS; i++) {
            int u = (order - 1) * LFN_CHARS + i;
            put16(l + at[i], u < n ? units[u] : u == n ? 0 : 0xFFFF);
        }
        if (!put_entry(f, where[k], l))
            return RFS_ERR_IO;
    }
    if (!put_entry(f, where[parts], entry))
        return RFS_ERR_IO;
    *short_where = where[parts];
    return RFS_OK;
}

static bool delete_entries(struct fat *f, const struct found *e)
{
    uint8_t x[ENTRY_SIZE];
    for (int i = 0; i < e->long_count; i++) {
        if (!get_entry(f, e->long_where[i], x))
            return false;
        x[0] = 0xE5;
        if (!put_entry(f, e->long_where[i], x))
            return false;
    }
    if (!get_entry(f, e->where, x))
        return false;
    x[0] = 0xE5;
    return put_entry(f, e->where, x);
}

/* path's parent directory (first cluster) and last name. */
static int parent_of(struct fat *f, const char *path, uint32_t *dir, const char **name,
                     size_t *len, struct found *e)
{
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/')
        n--;
    size_t start = n;
    while (start > 0 && path[start - 1] != '/')
        start--;
    *name = path + start;
    *len = n - start;
    if (*len == 0)
        return RFS_ERR_BAD_NAME;                    /* the root */
    char parent[256];
    if (start >= sizeof(parent))
        return RFS_ERR_BAD_NAME;
    memcpy(parent, path, start);
    parent[start] = 0;
    uint64_t id;
    struct rfs_inode inode;
    if (!walk(f, parent, &id, &inode, e))
        return RFS_ERR_NOT_FOUND;
    if (inode.type != RFS_TYPE_DIR)
        return RFS_ERR_NOT_DIR;
    *dir = dir_first(&inode);
    return RFS_OK;
}

/* --- the changes --- */

int fat_create(struct fat *f, const char *path, int type, uint64_t *id)
{
    if (f->read_only)
        return RFS_ERR_READ_ONLY;
    struct found *e = kmalloc(sizeof(*e));
    if (!e)
        return RFS_ERR_IO;
    mutex_lock(&f->lock);
    uint32_t dir;
    const char *name;
    size_t len;
    uint64_t existing;
    struct rfs_inode inode;
    int r = parent_of(f, path, &dir, &name, &len, e);
    if (r == RFS_OK && walk(f, path, &existing, &inode, e))
        r = RFS_ERR_EXISTS;
    uint8_t entry[ENTRY_SIZE];
    memset(entry, 0, sizeof(entry));
    entry[11] = type == RFS_TYPE_DIR ? ATTR_DIRECTORY : ATTR_ARCHIVE;
    stamp(entry, true);
    if (r == RFS_OK && type == RFS_TYPE_DIR) {
        /* Its first cluster, with "." and "..". */
        uint32_t c = allocate(f, 0);
        if (!c)
            r = RFS_ERR_NO_SPACE;
        else if (!zero_cluster(f, c))
            r = RFS_ERR_IO;
        uint8_t dot[ENTRY_SIZE];
        memcpy(dot, entry, sizeof(dot));
        memset(dot, ' ', 11);
        dot[0] = '.';
        set_entry_cluster(dot, c);
        uint64_t at = cluster_sector(f, c) * f->sector_size;
        f->dir_sector_at = ~0ull;
        if (r == RFS_OK && !put_entry(f, at, dot))
            r = RFS_ERR_IO;
        dot[1] = '.';
        set_entry_cluster(dot, dir == f->root_cluster ? 0 : dir);   /* the root: 0 */
        if (r == RFS_OK && !put_entry(f, at + ENTRY_SIZE, dot))
            r = RFS_ERR_IO;
        set_entry_cluster(entry, c);
    }
    uint64_t where = 0;
    if (r == RFS_OK)
        r = add_entry(f, dir, name, len, entry, &where, e);
    if (r == RFS_OK)
        *id = where / ENTRY_SIZE + 2;
    mutex_unlock(&f->lock);
    kfree(e);
    return r;
}

int fat_write(struct fat *f, uint64_t id, uint64_t offset, const void *data, uint64_t size)
{
    if (f->read_only)
        return RFS_ERR_READ_ONLY;
    if (offset + size > 0xFFFFFFFFull || offset + size < offset)
        return RFS_ERR_NO_SPACE;                    /* FAT's files end at 4 GiB */
    mutex_lock(&f->lock);
    uint8_t e[ENTRY_SIZE];
    int r = entry_by_id(f, id, e) ? RFS_OK : RFS_ERR_NOT_FOUND;
    if (r == RFS_OK && (e[11] & ATTR_DIRECTORY))
        r = RFS_ERR_IS_DIR;
    bool found = r == RFS_OK;
    uint32_t first = found ? entry_cluster(f, e) : 0, old = found ? u32(e + 28) : 0;
    uint64_t end = old, written = 0;                /* how far the file reaches now */
    if (r == RFS_OK && offset > old) {
        r = write_chain(f, &first, old, 0, offset - old, &written);  /* a gap: zeros */
        end = old + written;
    }
    if (r == RFS_OK) {
        r = write_chain(f, &first, offset, data, size, &written);
        if (offset + written > end)
            end = offset + written;
    }
    /* The entry, also after a failure (a full volume): its clusters must
     * stay the file's, and its size say what was written. */
    if (found && !entry_by_id(f, id, e))
        r = RFS_ERR_NOT_FOUND;
    else if (found) {
        set_entry_cluster(e, first);
        put32(e + 28, (uint32_t)end);
        e[11] |= ATTR_ARCHIVE;
        stamp(e, false);
        if (!put_entry(f, (id - 2) * ENTRY_SIZE, e) && r == RFS_OK)
            r = RFS_ERR_IO;
    }
    mutex_unlock(&f->lock);
    return r;
}

int fat_truncate(struct fat *f, uint64_t id)
{
    if (f->read_only)
        return RFS_ERR_READ_ONLY;
    mutex_lock(&f->lock);
    uint8_t e[ENTRY_SIZE];
    int r = entry_by_id(f, id, e) ? RFS_OK : RFS_ERR_NOT_FOUND;
    if (r == RFS_OK && (e[11] & ATTR_DIRECTORY))
        r = RFS_ERR_IS_DIR;
    if (r == RFS_OK && !free_chain(f, entry_cluster(f, e)))
        r = RFS_ERR_IO;
    if (r == RFS_OK) {
        set_entry_cluster(e, 0);
        put32(e + 28, 0);
        stamp(e, false);
        if (!put_entry(f, (id - 2) * ENTRY_SIZE, e))
            r = RFS_ERR_IO;
    }
    mutex_unlock(&f->lock);
    return r;
}

int fat_remove(struct fat *f, const char *path)
{
    if (f->read_only)
        return RFS_ERR_READ_ONLY;
    struct found *e = kmalloc(2 * sizeof(*e));
    if (!e)
        return RFS_ERR_IO;
    mutex_lock(&f->lock);
    uint64_t id;
    struct rfs_inode inode;
    int r = walk(f, path, &id, &inode, e) ? RFS_OK : RFS_ERR_NOT_FOUND;
    if (r == RFS_OK && id == FAT_ROOT_ID)
        r = RFS_ERR_BAD_NAME;
    if (r == RFS_OK && inode.type == RFS_TYPE_DIR) {
        uint64_t pos = 0;
        int n = next_entry(f, dir_first(&inode), &pos, &e[1]);
        r = n < 0 ? RFS_ERR_IO : n > 0 ? RFS_ERR_NOT_EMPTY : RFS_OK;
    }
    if (r == RFS_OK && (!free_chain(f, (uint32_t)inode.root.address) || !delete_entries(f, e)))
        r = RFS_ERR_IO;
    mutex_unlock(&f->lock);
    kfree(e);
    return r;
}

/* Whether path a lies in directory b (or is it), without regard to case. */
static bool inside(const char *a, const char *b)
{
    size_t n = strlen(b);
    return strlen(a) >= n && same_name(a, n, b, n) && (a[n] == 0 || a[n] == '/');
}

int fat_rename(struct fat *f, const char *from, const char *to)
{
    if (f->read_only)
        return RFS_ERR_READ_ONLY;
    struct found *e = kmalloc(3 * sizeof(*e));
    if (!e)
        return RFS_ERR_IO;
    struct found *src = &e[0], *dst = &e[1], *scratch = &e[2];
    mutex_lock(&f->lock);
    uint64_t id, other;
    struct rfs_inode inode, target;
    uint32_t dir;
    const char *name;
    size_t len;
    int r = walk(f, from, &id, &inode, src) ? RFS_OK : RFS_ERR_NOT_FOUND;
    if (r == RFS_OK && id == FAT_ROOT_ID)
        r = RFS_ERR_BAD_NAME;
    if (r == RFS_OK)
        r = parent_of(f, to, &dir, &name, &len, scratch);
    if (r == RFS_OK && inode.type == RFS_TYPE_DIR && inside(to, from))
        r = RFS_ERR_BAD_NAME;                       /* into itself */
    bool same = false, replace = false;
    if (r == RFS_OK && walk(f, to, &other, &target, dst)) {
        same = other == id;                         /* only the name's case changes */
        replace = !same;
        if (replace && inode.type == RFS_TYPE_DIR && target.type != RFS_TYPE_DIR)
            r = RFS_ERR_NOT_DIR;
        else if (replace && inode.type != RFS_TYPE_DIR && target.type == RFS_TYPE_DIR)
            r = RFS_ERR_IS_DIR;
    }
    /* (68a) An entry at to is replaced, as POSIX's rename (fat_remove checks
     * a directory is empty). FAT has no transactions: it goes first, and a
     * failure after it leaves from as it was and the target gone. */
    if (r == RFS_OK && replace)
        r = fat_remove(f, to);
    uint8_t entry[ENTRY_SIZE];
    memcpy(entry, src->entry, sizeof(entry));
    uint64_t where;
    if (r == RFS_OK && same && !delete_entries(f, src))
        r = RFS_ERR_IO;
    if (r == RFS_OK)
        r = add_entry(f, dir, name, len, entry, &where, scratch);
    if (r == RFS_OK && !same && !delete_entries(f, src))
        r = RFS_ERR_IO;
    /* A directory moved elsewhere: its ".." names the new parent. */
    uint32_t c = (uint32_t)inode.root.address;
    if (r == RFS_OK && inode.type == RFS_TYPE_DIR && c >= 2) {
        uint8_t dots[ENTRY_SIZE];
        uint64_t at = cluster_sector(f, c) * f->sector_size + ENTRY_SIZE;
        if (!get_entry(f, at, dots))
            r = RFS_ERR_IO;
        else if (dots[0] == '.' && dots[1] == '.') {
            set_entry_cluster(dots, dir == f->root_cluster ? 0 : dir);
            if (!put_entry(f, at, dots))
                r = RFS_ERR_IO;
        }
    }
    mutex_unlock(&f->lock);
    kfree(e);
    return r;
}

int fat_chmod(struct fat *f, uint64_t id, unsigned mode)
{
    if (f->read_only)
        return RFS_ERR_READ_ONLY;
    mutex_lock(&f->lock);
    uint8_t e[ENTRY_SIZE];
    int r = entry_by_id(f, id, e) ? RFS_OK : RFS_ERR_NOT_FOUND;
    if (r == RFS_OK && !(e[11] & ATTR_DIRECTORY)) {
        /* Only the owner's w is kept: as FAT's read-only attribute. */
        e[11] = (uint8_t)(mode & 0200 ? e[11] & ~ATTR_READ_ONLY : e[11] | ATTR_READ_ONLY);
        if (!put_entry(f, (id - 2) * ENTRY_SIZE, e))
            r = RFS_ERR_IO;
    }
    mutex_unlock(&f->lock);
    return r;
}

/* (63m) The time it was last changed (DOS keeps it in 2-second steps). The
 * root has no entry: nothing to set. */
int fat_set_time(struct fat *f, uint64_t id, uint64_t modified)
{
    if (f->read_only)
        return RFS_ERR_READ_ONLY;
    if (id == 1)
        return RFS_OK;
    mutex_lock(&f->lock);
    uint8_t e[ENTRY_SIZE];
    int r = entry_by_id(f, id, e) ? RFS_OK : RFS_ERR_NOT_FOUND;
    if (r == RFS_OK) {
        uint16_t date, time;
        uint8_t tenths;
        dos_from(modified, &date, &time, &tenths);
        put16(e + 22, time), put16(e + 24, date);
        if (!put_entry(f, (id - 2) * ENTRY_SIZE, e))
            r = RFS_ERR_IO;
    }
    mutex_unlock(&f->lock);
    return r;
}

int fat_commit(struct fat *f)
{
    if (f->read_only)
        return RFS_OK;
    mutex_lock(&f->lock);
    bool ok = flush_window(f);
    if (ok && f->fsinfo_dirty && f->fsinfo_sector) {
        uint8_t *b = f->cluster;
        if ((ok = read_sectors(f, f->fsinfo_sector, 1, b))) {
            put32(b + 488, f->free_count);
            put32(b + 492, f->next_free);
            ok = write_sectors(f, f->fsinfo_sector, 1, b);
        }
        f->fsinfo_dirty = !ok;
    }
    if (ok && f->dev->flush)
        ok = f->dev->flush(f->dev);
    mutex_unlock(&f->lock);
    return ok ? RFS_OK : RFS_ERR_IO;
}

bool fat_read_only(struct fat *f)
{
    return f->read_only;
}

void fat_set_read_only(struct fat *f)
{
    f->read_only = true;
}

/* --- mounting ------------------------------------------------------------------ */

static bool power_of_two(uint32_t v)
{
    return v && !(v & (v - 1));
}

struct fat *fat_mount(struct block_device *dev)
{
    if (dev->block_size > 4096)
        return 0;
    uint8_t *b = kmalloc(4096);
    if (!b)
        return 0;
    if (!dev->read(dev, 0, 4096 / dev->block_size, b) || b[510] != 0x55 || b[511] != 0xAA ||
        (b[0] != 0xEB && b[0] != 0xE9)) {
        kfree(b);
        return 0;
    }
    char label[24];
    ksnprintf(label, sizeof(label), "%s:", dev->name);
    uint32_t sector_size = u16(b + 11), cluster_sectors = b[13], reserved = u16(b + 14);
    uint32_t fats = b[16], root_entries = u16(b + 17);
    uint64_t total = u16(b + 19) ? u16(b + 19) : u32(b + 32);
    uint64_t fat_sectors = u16(b + 22) ? u16(b + 22) : u32(b + 36);
    const char *problem = 0;
    if (sector_size < 512 || sector_size > 4096 || !power_of_two(sector_size) ||
        sector_size % dev->block_size)
        problem = "bad sector size";
    else if (!power_of_two(cluster_sectors) || (uint64_t)cluster_sectors * sector_size > 256 * 1024)
        problem = "bad cluster size";
    else if (reserved == 0 || fats == 0 || fat_sectors == 0 || total == 0)
        problem = "bad layout";
    uint64_t root_sectors = ((uint64_t)root_entries * ENTRY_SIZE + sector_size - 1) /
                            (sector_size ? sector_size : 1);
    uint64_t data_start = reserved + fats * fat_sectors + root_sectors;
    uint64_t clusters = !problem && total > data_start ? (total - data_start) / cluster_sectors : 0;
    int bits = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
    if (!problem && clusters == 0)
        problem = "no data area";
    else if (!problem && total * sector_size > dev->block_count * dev->block_size)
        problem = "larger than its device";
    else if (!problem && fat_sectors * sector_size * 8 < (clusters + 2) * (uint64_t)bits)
        problem = "FAT too short for its clusters";
    else if (!problem && bits == 32 && (root_entries != 0 || u32(b + 44) < 2 ||
                                        u32(b + 44) >= clusters + 2))
        problem = "bad FAT32 root directory";
    else if (!problem && bits != 32 && root_entries == 0)
        problem = "no root directory";
    if (problem) {
        /* A FAT boot sector (by its file system name), but not usable. */
        if (memcmp(b + 82, "FAT32   ", 8) == 0 || memcmp(b + 54, "FAT1", 4) == 0)
            kprintf("  %-15sFAT: %s, not mounted\n", label, problem);
        kfree(b);
        return 0;
    }

    struct fat *f = kzalloc(sizeof(*f));
    if (!f) {
        kfree(b);
        return 0;
    }
    f->vfs.kind = VFS_FAT;
    f->dev = dev;
    f->bits = bits;
    f->sector_size = sector_size;
    f->cluster_sectors = cluster_sectors;
    f->cluster_bytes = cluster_sectors * sector_size;
    f->device_blocks_per_sector = sector_size / dev->block_size;
    f->fat_start = reserved;
    f->fat_sectors = fat_sectors;
    f->fats = fats;
    f->root_start = reserved + fats * fat_sectors;
    f->root_sectors = root_sectors;
    f->data_start = data_start;
    f->root_cluster = bits == 32 ? u32(b + 44) : 0;
    f->clusters = (uint32_t)clusters;
    f->fat_window_at = f->dir_sector_at = ~0ull;
    f->lock = (struct mutex)MUTEX_INIT("fat");
    /* The label in the boot sector (the same that disks shows). */
    const uint8_t *l = b + (bits == 32 ? 71 : 43);
    size_t n = 0;
    for (int i = 0; i < 11; i++)
        f->label[n++] = l[i] >= 0x20 && l[i] < 0x7F ? (char)l[i] : '?';
    while (n > 0 && f->label[n - 1] == ' ')
        n--;
    f->label[n] = 0;
    if (strcmp(f->label, "NO NAME") == 0)
        f->label[0] = 0;
    /* FAT32's FSInfo: the free clusters and where to look for one (hints;
     * used and kept up to date only if its signatures are right). */
    f->free_count = ~0u;
    f->next_free = 2;
    uint32_t fsinfo = bits == 32 ? u16(b + 48) : 0;
    if (fsinfo && fsinfo < reserved && dev->read(dev, (uint64_t)fsinfo * f->device_blocks_per_sector,
                                                 1, b) &&
        u32(b) == 0x41615252 && u32(b + 484) == 0x61417272 && u32(b + 508) == 0xAA550000) {
        f->fsinfo_sector = fsinfo;
        if (u32(b + 488) <= clusters)
            f->free_count = u32(b + 488);
        if (u32(b + 492) >= 2 && u32(b + 492) < clusters + 2)
            f->next_free = u32(b + 492);
    }
    kfree(b);
    f->fat_window = kmalloc(2 * sector_size);
    f->dir_sector = kmalloc(sector_size);
    f->cluster = kmalloc(f->cluster_bytes);
    if (!f->fat_window || !f->dir_sector || !f->cluster) {
        kfree(f->fat_window);
        kfree(f->dir_sector);
        kfree(f->cluster);
        kfree(f);
        return 0;
    }
    /* The root directory must be readable. */
    struct rfs_inode root;
    root_inode(f, &root);
    struct found *e = kmalloc(sizeof(*e));
    uint64_t pos = 0;
    int entries = 0, r = -1;
    if (e) {
        mutex_lock(&f->lock);
        while ((r = next_entry(f, dir_first(&root), &pos, e)) == 1)
            entries++;
        mutex_unlock(&f->lock);
        kfree(e);
    }
    if (r < 0) {
        kprintf("  %-15sFAT: the root directory cannot be read, not mounted\n", label);
        return 0;                                   /* f stays: its memory is small */
    }
    char unit[16];
    if (f->cluster_bytes < 1024)
        ksnprintf(unit, sizeof(unit), "%u bytes", f->cluster_bytes);
    else
        ksnprintf(unit, sizeof(unit), "%u KiB", f->cluster_bytes / 1024);
    kprintf("  %-15s%s '%s', %lu clusters of %s (%lu MiB), root directory %d entries\n",
            label, fat_type(f), f->label, (uint64_t)f->clusters, unit,
            (uint64_t)f->clusters * f->cluster_bytes >> 20, entries);
    return f;
}

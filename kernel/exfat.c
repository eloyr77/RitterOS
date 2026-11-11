/*
 * exFAT (see exfat.h), after Microsoft's exFAT file system specification.
 *
 * The layout: the boot region (12 sectors: the boot sector, 8 extended
 * boot sectors, OEM parameters, a reserved sector and a checksum sector;
 * kept twice) gives the sector and cluster sizes (as powers of two), where
 * the FAT and the cluster heap begin, the number of clusters and the root
 * directory's first cluster. The FAT has a 32-bit entry per cluster: the
 * next cluster of a chain, or 0xFFFFFFFF at its end. A file whose clusters
 * are one run says so in its stream entry (NoFatChain) and needs no FAT
 * entries at all. Which clusters are in use is kept in the allocation
 * bitmap, a file in the heap (used when writing).
 *
 * A directory is an array of 32-byte entries; the first byte is the type
 * (bit 7: in use; 0: no entries after this one). A file or directory is a
 * set of entries: the file entry (attributes, times, how many entries
 * follow), the stream extension (first cluster, data length, valid data
 * length, the name's length and hash) and file name entries (15 UTF-16
 * units each), with a checksum over the whole set. The root directory has
 * no entry of its own (its clusters follow the FAT) and holds the volume
 * label and the entries of the bitmap and the up-case table.
 *
 * Writing (46b): what RitterOS makes or grows always follows the FAT (one
 * run without it is read, not made); a file that was one run is turned
 * into a FAT chain when it grows. A cluster taken or freed is marked in
 * the bitmap and the FAT at once (both kept a sector at a time in memory,
 * written when another sector is needed and at each commit). After every
 * change of a file's set (size, clusters, time, attributes) its checksum
 * is made again. The volume is marked dirty (VolumeFlags) from the first
 * change until the commit, as the specification asks. A volume with two
 * FATs (TexFAT) is only read.
 */
#include "exfat.h"
#include "heap.h"
#include "kprintf.h"
#include "ritterfs.h"
#include "string.h"
#include "sync.h"
#include "uefi.h"
#include "vfs.h"

#define ENTRY               32
#define TYPE_END            0x00
#define TYPE_BITMAP         0x81
#define TYPE_UPCASE         0x82
#define TYPE_LABEL          0x83
#define TYPE_FILE           0x85
#define TYPE_STREAM         0xC0
#define TYPE_NAME           0xC1
#define ATTR_READ_ONLY      0x01
#define ATTR_DIRECTORY      0x10
#define FLAG_NO_FAT_CHAIN   0x02            /* stream entry: the clusters are one run */
#define NAME_UNITS          255             /* UTF-16 units of a name, at most */
#define NAME_ENTRY_UNITS    15
#define SET_MAX             19              /* entries of a set: file, stream, 17 names */
#define CONTIGUOUS_DIR      (1ull << 62)    /* in an ID: its directory is one run */
#define EXFAT_CONTIGUOUS    1               /* inode flags: the object's clusters are one run */
#define CHAIN_END           0xFFFFFFFFu
#define CHAIN_BAD           0xFFFFFFFEu
#define READ_CHUNK          (64 * 1024)     /* bytes read at once (clusters may be 32 MiB) */

struct exfat {
    struct vfs vfs;                         /* first: vfs.c casts */
    struct block_device *dev;
    uint32_t sector_size, cluster_sectors, cluster_bytes, device_blocks_per_sector;
    uint64_t fat_start, heap_start;         /* sectors from the volume's start */
    uint32_t clusters;                      /* in the heap: 2 .. clusters + 1 */
    uint32_t root_cluster;
    uint16_t *upcase;                       /* 65536 units: each one's capital */
    char label[34];
    bool read_only;
    struct mutex lock;
    uint8_t *fat_sector;                    /* one sector of the FAT */
    uint64_t fat_sector_at;                 /* ~0: none */
    bool fat_dirty;                         /* changed: written before another is read */
    uint32_t bitmap_first;                  /* the allocation bitmap's clusters */
    uint64_t bitmap_length;
    bool bitmap_contiguous;
    uint8_t *bitmap_sector;                 /* one sector of it */
    uint64_t bitmap_sector_at;
    bool bitmap_dirty;
    uint32_t free_count, next_free;         /* free clusters; where to look for one */
    bool dirty;                             /* VolumeDirty set on the disk */
    uint8_t *dir_sector;                    /* one sector of a directory */
    uint64_t dir_sector_at;
    uint8_t *chunk;                         /* READ_CHUNK bytes for file data */
    uint32_t chain_first, chain_index, chain_cluster;   /* where the last walk ended */
};

/* Where an object's clusters are. */
struct stream {
    uint32_t first;                         /* 0: none (an empty file) */
    bool contiguous;                        /* one run: the FAT is not used */
    uint64_t length;                        /* bytes; ~0: to the chain's end (the root) */
    uint64_t valid;                         /* bytes written; the rest reads as zeros */
};

/* One file or directory found in a directory: its entry set and name. */
struct found {
    uint8_t set[SET_MAX * ENTRY];           /* file entry, stream entry, names */
    int entries;
    uint64_t where;                         /* the file entry's byte position on the device */
    bool dir_contiguous;                    /* the directory it is in is one run */
    uint16_t name16[NAME_UNITS];
    size_t units;
    char name[RFS_NAME_MAX + 1];
    size_t name_len;
};

static uint16_t u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t u64(const uint8_t *p)
{
    return u32(p) | (uint64_t)u32(p + 4) << 32;
}

static bool read_sectors(struct exfat *x, uint64_t sector, uint32_t count, void *buf)
{
    return x->dev->read(x->dev, sector * x->device_blocks_per_sector,
                        count * x->device_blocks_per_sector, buf);
}

static bool write_sectors(struct exfat *x, uint64_t sector, uint32_t count, const void *buf)
{
    return x->dev->write && x->dev->write(x->dev, sector * x->device_blocks_per_sector,
                                          count * x->device_blocks_per_sector, buf);
}

/* The FAT's sector in memory (the one before written back if it changed). */
static bool fat_load(struct exfat *x, uint64_t sector)
{
    if (x->fat_sector_at == sector)
        return true;
    if (x->fat_dirty) {
        if (!write_sectors(x, x->fat_sector_at, 1, x->fat_sector))
            return false;
        x->fat_dirty = false;
    }
    if (!read_sectors(x, sector, 1, x->fat_sector)) {
        x->fat_sector_at = ~0ull;
        return false;
    }
    x->fat_sector_at = sector;
    return true;
}

/* --- clusters ------------------------------------------------------------- */

static uint64_t cluster_sector(struct exfat *x, uint32_t c)
{
    return x->heap_start + (uint64_t)(c - 2) * x->cluster_sectors;
}

static bool in_heap(struct exfat *x, uint32_t c)
{
    return c >= 2 && c < x->clusters + 2;
}

/* The FAT's entry for c: the next cluster, CHAIN_END or CHAIN_BAD. */
static uint32_t next_cluster(struct exfat *x, uint32_t c)
{
    uint64_t byte = (uint64_t)c * 4;
    if (!fat_load(x, x->fat_start + byte / x->sector_size))
        return CHAIN_BAD;
    uint32_t v = u32(x->fat_sector + byte % x->sector_size);
    if (v == 0xFFFFFFFFu)
        return CHAIN_END;
    return in_heap(x, v) ? v : CHAIN_BAD;
}

/* The index-th cluster of s (CHAIN_END past its end, CHAIN_BAD if its chain
 * is damaged). Walking on from the last place is quick. */
static uint32_t cluster_at(struct exfat *x, const struct stream *s, uint32_t index)
{
    if (!in_heap(x, s->first))
        return s->first == 0 ? CHAIN_END : CHAIN_BAD;
    if (s->length != ~0ull && (uint64_t)index * x->cluster_bytes >= s->length)
        return CHAIN_END;
    if (s->contiguous) {
        uint64_t c = (uint64_t)s->first + index;
        return c < (uint64_t)x->clusters + 2 ? (uint32_t)c : CHAIN_BAD;
    }
    uint32_t c = s->first, at = 0;
    if (x->chain_first == s->first && x->chain_index <= index)
        c = x->chain_cluster, at = x->chain_index;
    while (at < index) {
        c = next_cluster(x, c);
        if (c == CHAIN_END || c == CHAIN_BAD)
            return c;
        if (++at > x->clusters)
            return CHAIN_BAD;                       /* a loop */
    }
    x->chain_first = s->first, x->chain_index = index, x->chain_cluster = c;
    return c;
}

/* --- directories --------------------------------------------------------- */

/* The 32 bytes at byte pos of the directory s, and where they lie on the
 * device (bytes). 1, 0 past its end, -1 if it cannot be read. */
static int dir_entry(struct exfat *x, const struct stream *s, uint64_t pos, uint8_t *out,
                     uint64_t *where)
{
    if (s->length != ~0ull && pos + ENTRY > s->length)
        return 0;
    uint32_t c = cluster_at(x, s, (uint32_t)(pos / x->cluster_bytes));
    if (c == CHAIN_END)
        return 0;
    if (c == CHAIN_BAD)
        return -1;
    uint64_t sector = cluster_sector(x, c) + pos % x->cluster_bytes / x->sector_size;
    if (x->dir_sector_at != sector) {
        if (!read_sectors(x, sector, 1, x->dir_sector)) {
            x->dir_sector_at = ~0ull;
            return -1;
        }
        x->dir_sector_at = sector;
    }
    memcpy(out, x->dir_sector + pos % x->sector_size, ENTRY);
    *where = sector * x->sector_size + pos % x->sector_size;
    return 1;
}

/* The checksum over an entry set (its bytes 2-3, where it is kept, left out). */
static uint16_t set_checksum(const uint8_t *set, int entries)
{
    uint16_t sum = 0;
    for (int i = 0; i < entries * ENTRY; i++) {
        if (i == 2 || i == 3)
            continue;
        sum = (uint16_t)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + set[i]);
    }
    return sum;
}

/* UTF-16 to UTF-8; 0 if it does not fit in 255 bytes or is malformed. */
static size_t utf8_name(const uint16_t *s, size_t count, char *out)
{
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
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

/* UTF-8 (len bytes) to UTF-16: the units, or -1 if malformed or too long. */
static int utf16_name(const char *s, size_t len, uint16_t *out)
{
    int n = 0;
    for (size_t i = 0; i < len;) {
        uint8_t b = (uint8_t)s[i];
        uint32_t c;
        size_t extra = b < 0x80 ? 0 : (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2
                     : (b & 0xF8) == 0xF0 ? 3 : 4;
        if (extra == 4 || (extra && i + extra >= len))
            return -1;
        c = extra == 0 ? b : extra == 1 ? b & 0x1F : extra == 2 ? b & 0x0F : b & 0x07;
        for (size_t k = 1; k <= extra; k++) {
            if (((uint8_t)s[i + k] & 0xC0) != 0x80)
                return -1;
            c = c << 6 | ((uint8_t)s[i + k] & 0x3F);
        }
        i += extra + 1;
        if (c >= 0x10000) {
            if (n + 2 > NAME_UNITS)
                return -1;
            c -= 0x10000;
            out[n++] = (uint16_t)(0xD800 + (c >> 10));
            out[n++] = (uint16_t)(0xDC00 + (c & 0x3FF));
        } else {
            if (n + 1 > NAME_UNITS)
                return -1;
            out[n++] = (uint16_t)c;
        }
    }
    return n;
}

/* The next file or directory from byte *pos of directory s (other entries,
 * deleted sets and sets with a wrong checksum skipped). 1, 0 at its end,
 * -1 if it cannot be read. */
static int next_entry(struct exfat *x, const struct stream *s, uint64_t *pos, struct found *out)
{
    for (;; *pos += ENTRY) {
        uint8_t *set = out->set;
        int r = dir_entry(x, s, *pos, set, &out->where);
        if (r <= 0)
            return r;
        if (set[0] == TYPE_END)
            return 0;
        int count = set[1];
        if (set[0] != TYPE_FILE || count < 2 || count >= SET_MAX)
            continue;
        uint64_t where;
        for (int i = 1; i <= count; i++) {
            r = dir_entry(x, s, *pos + (uint64_t)i * ENTRY, set + i * ENTRY, &where);
            if (r <= 0)
                return r;                           /* cut off at the directory's end */
        }
        size_t units = set[ENTRY + 3];
        int names = (int)(units + NAME_ENTRY_UNITS - 1) / NAME_ENTRY_UNITS;
        bool ok = set[ENTRY] == TYPE_STREAM && units > 0 && names <= count - 1 &&
                  u16(set + 2) == set_checksum(set, count + 1);
        for (int i = 0; ok && i < names; i++)
            ok = set[(2 + i) * ENTRY] == TYPE_NAME;
        if (!ok)
            continue;
        for (size_t i = 0; i < units; i++)
            out->name16[i] = u16(set + (2 + i / NAME_ENTRY_UNITS) * ENTRY + 2 +
                                 i % NAME_ENTRY_UNITS * 2);
        out->units = units;
        out->name_len = utf8_name(out->name16, units, out->name);
        if (out->name_len == 0)
            continue;                               /* not a name RitterOS can show */
        out->name[out->name_len] = 0;
        out->entries = count + 1;
        out->dir_contiguous = s->contiguous;
        *pos += (uint64_t)(count + 1) * ENTRY;
        return 1;
    }
}

/* --- inodes -------------------------------------------------------------- */

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* An exFAT time (as FAT's, from 1980, in 2-second steps; plus 10 ms steps
 * and the zone's offset from UTC in 15 minutes) in nanoseconds since 1970. */
static uint64_t exfat_time(uint32_t t, uint8_t ten_ms, uint8_t zone)
{
    unsigned day = t >> 16 & 31, month = t >> 21 & 15;
    if (day == 0 || month == 0 || month > 12)
        return 0;
    int64_t secs = days_from_civil(1980 + (t >> 25), month, day) * 86400 +
                   (t >> 11 & 31) * 3600 + (t >> 5 & 63) * 60 + (t & 31) * 2 + ten_ms / 100;
    if (zone & 0x80) {
        int offset = zone & 0x7F;
        if (offset >= 64)
            offset -= 128;                          /* 7 bits, signed */
        secs -= (int64_t)offset * 15 * 60;          /* local time to UTC */
    }
    if (secs < 0)
        return 0;
    return (uint64_t)secs * 1000000000ULL + (uint64_t)(ten_ms % 100) * 10000000ULL;
}

static void set_lengths(struct rfs_inode *out, uint64_t valid, uint64_t length)
{
    memcpy(out->inline_data, &valid, 8);
    memcpy(out->inline_data + 8, &length, 8);
}

static void root_inode(struct exfat *x, struct rfs_inode *out)
{
    memset(out, 0, sizeof(*out));
    out->type = RFS_TYPE_DIR;
    out->link_count = 1;
    out->mode = RFS_MODE_SET | 0777;
    out->root.address = x->root_cluster;
    set_lengths(out, ~0ull, ~0ull);
}

/* An inode from a file entry and its stream entry. */
static void entry_inode(const uint8_t *file, const uint8_t *stream, struct rfs_inode *out)
{
    memset(out, 0, sizeof(*out));
    uint16_t attributes = u16(file + 4);
    bool dir = attributes & ATTR_DIRECTORY;
    out->type = dir ? RFS_TYPE_DIR : RFS_TYPE_FILE;
    out->link_count = 1;
    out->size = dir ? 0 : u64(stream + 24);
    out->created = exfat_time(u32(file + 8), file[20], file[22]);
    out->modified = out->changed = exfat_time(u32(file + 12), file[21], file[23]);
    unsigned mode = dir ? 0777 : 0666;
    if (attributes & ATTR_READ_ONLY)
        mode &= ~0222u;
    out->mode = (uint16_t)(RFS_MODE_SET | mode);
    out->root.address = u32(stream + 20);
    out->flags = stream[1] & FLAG_NO_FAT_CHAIN ? EXFAT_CONTIGUOUS : 0;
    set_lengths(out, u64(stream + 8), u64(stream + 24));
}

static void stream_of(const struct rfs_inode *inode, struct stream *s)
{
    s->first = (uint32_t)inode->root.address;
    s->contiguous = inode->flags & EXFAT_CONTIGUOUS;
    memcpy(&s->valid, inode->inline_data, 8);
    memcpy(&s->length, inode->inline_data + 8, 8);
}

/* --- the interface -------------------------------------------------------- */

bool exfat_get_inode(struct exfat *x, uint64_t id, struct rfs_inode *out)
{
    if (id == EXFAT_ROOT_ID) {
        root_inode(x, out);
        return true;
    }
    if ((id & ~CONTIGUOUS_DIR) < 2)
        return false;
    uint64_t where = ((id & ~CONTIGUOUS_DIR) - 2) * ENTRY;
    uint8_t file[ENTRY], stream[ENTRY];
    mutex_lock(&x->lock);
    bool ok = true;
    for (int i = 0; i < 2 && ok; i++) {
        uint64_t sector = where / x->sector_size;
        if (x->dir_sector_at != sector) {
            ok = read_sectors(x, sector, 1, x->dir_sector);
            x->dir_sector_at = ok ? sector : ~0ull;
        }
        if (ok)
            memcpy(i == 0 ? file : stream, x->dir_sector + where % x->sector_size, ENTRY);
        /* The stream entry follows: in the next cluster when the file entry
         * ends this one (the directory's next: one on, or the FAT's). */
        uint64_t heap = x->heap_start * x->sector_size;
        if (where >= heap && (where - heap + ENTRY) % x->cluster_bytes == 0) {
            uint32_t c = (uint32_t)((where - heap) / x->cluster_bytes) + 2;
            uint32_t next = id & CONTIGUOUS_DIR ? c + 1 : next_cluster(x, c);
            ok = ok && in_heap(x, next);
            where = cluster_sector(x, next) * x->sector_size;
        } else {
            where += ENTRY;
        }
    }
    ok = ok && file[0] == TYPE_FILE && stream[0] == TYPE_STREAM;
    if (ok)
        entry_inode(file, stream, out);
    mutex_unlock(&x->lock);
    return ok;
}

static uint64_t entry_id(const struct found *e)
{
    return (e->where / ENTRY + 2) | (e->dir_contiguous ? CONTIGUOUS_DIR : 0);
}

/* The hash of a name as the stream entry keeps it (over its capitals). */
static uint16_t name_hash(struct exfat *x, const uint16_t *name, size_t units)
{
    uint16_t hash = 0;
    for (size_t i = 0; i < units; i++) {
        uint16_t c = x->upcase[name[i]];
        hash = (uint16_t)(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (c & 0xFF));
        hash = (uint16_t)(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (c >> 8));
    }
    return hash;
}

/* Names compare by their capitals (the volume's up-case table). */
static bool same_name(struct exfat *x, const uint16_t *a, size_t an, const uint16_t *b, size_t bn)
{
    if (an != bn)
        return false;
    for (size_t i = 0; i < an; i++)
        if (x->upcase[a[i]] != x->upcase[b[i]])
            return false;
    return true;
}

/* Follow path from the root (under the lock): its ID and inode. */
static bool walk(struct exfat *x, const char *path, uint64_t *id, struct rfs_inode *inode,
                 struct found *e)
{
    root_inode(x, inode);
    *id = EXFAT_ROOT_ID;
    uint16_t want[NAME_UNITS];
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
        int units = utf16_name(path, (size_t)(end - path), want);
        if (units <= 0)
            return false;
        uint16_t hash = name_hash(x, want, (size_t)units);
        struct stream s;
        stream_of(inode, &s);
        uint64_t pos = 0;
        int r;
        while ((r = next_entry(x, &s, &pos, e)) == 1)
            if (u16(e->set + ENTRY + 4) == hash &&
                same_name(x, e->name16, e->units, want, (size_t)units))
                break;
        if (r != 1)
            return false;
        entry_inode(e->set, e->set + ENTRY, inode);
        *id = entry_id(e);
        path = end;
    }
    return true;
}

bool exfat_lookup(struct exfat *x, const char *path, uint64_t *id, struct rfs_inode *inode)
{
    struct found *e = kmalloc(sizeof(*e));
    if (!e)
        return false;
    mutex_lock(&x->lock);
    bool ok = walk(x, path, id, inode, e);
    mutex_unlock(&x->lock);
    kfree(e);
    return ok;
}

int64_t exfat_read(struct exfat *x, uint64_t id, const struct rfs_inode *inode, uint64_t offset,
                   void *buf, uint64_t size)
{
    (void)id;
    if (inode->type != RFS_TYPE_FILE)
        return -1;
    struct stream s;
    stream_of(inode, &s);
    if (offset >= s.length)
        return 0;
    if (size > s.length - offset)
        size = s.length - offset;
    mutex_lock(&x->lock);
    uint64_t done = 0;
    bool failed = false;
    while (done < size && !failed) {
        uint64_t at = offset + done;
        uint64_t take = size - done;
        if (at >= s.valid) {                        /* past the valid data: zeros */
            memset((uint8_t *)buf + done, 0, take);
            done += take;
            break;
        }
        if (take > s.valid - at)
            take = s.valid - at;
        uint32_t c = cluster_at(x, &s, (uint32_t)(at / x->cluster_bytes));
        if (c == CHAIN_END || c == CHAIN_BAD)
            break;                                  /* shorter than its length says */
        uint64_t within = at % x->cluster_bytes;
        if (take > x->cluster_bytes - within)
            take = x->cluster_bytes - within;
        if (take > READ_CHUNK - x->sector_size)
            take = READ_CHUNK - x->sector_size;
        uint32_t s0 = (uint32_t)(within / x->sector_size);
        uint32_t s1 = (uint32_t)((within + take + x->sector_size - 1) / x->sector_size);
        if (!read_sectors(x, cluster_sector(x, c) + s0, s1 - s0, x->chunk)) {
            failed = true;
            break;
        }
        memcpy((uint8_t *)buf + done, x->chunk + within - (uint64_t)s0 * x->sector_size, take);
        done += take;
    }
    mutex_unlock(&x->lock);
    return done == size ? (int64_t)done : done > 0 ? (int64_t)done : -1;
}

int exfat_dir_next(struct exfat *x, uint64_t dir_id, const struct rfs_inode *dir, uint64_t *pos,
                   char *name, uint8_t *name_len, uint64_t *id, uint8_t *type)
{
    (void)dir_id;
    struct found *e = kmalloc(sizeof(*e));
    if (!e)
        return -1;
    struct stream s;
    stream_of(dir, &s);
    mutex_lock(&x->lock);
    int r = next_entry(x, &s, pos, e);
    mutex_unlock(&x->lock);
    if (r == 1) {
        memcpy(name, e->name, e->name_len);
        *name_len = (uint8_t)e->name_len;
        *id = entry_id(e);
        *type = u16(e->set + 4) & ATTR_DIRECTORY ? RFS_TYPE_DIR : RFS_TYPE_FILE;
    }
    kfree(e);
    return r;
}

struct vfs *exfat_vfs(struct exfat *x)
{
    return &x->vfs;
}

struct block_device *exfat_device(struct exfat *x)
{
    return x->dev;
}

void exfat_label(struct exfat *x, char *out, size_t size)
{
    ksnprintf(out, size, "%s", x->label);
}

bool exfat_read_only(struct exfat *x)
{
    return x->read_only;
}

void exfat_set_read_only(struct exfat *x)
{
    x->read_only = true;
}

/* --- writing --------------------------------------------------------------- */

#define ATTR_ARCHIVE        0x20
#define FLAG_ALLOCATION     0x01            /* stream entry: clusters may be allocated */
#define CHAIN_END_MARK      0xFFFFFFFFu

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v), put16(p + 2, v >> 16);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v), put32(p + 4, (uint32_t)(v >> 32));
}

static bool fat_set(struct exfat *x, uint32_t c, uint32_t value)
{
    uint64_t byte = (uint64_t)c * 4;
    if (!fat_load(x, x->fat_start + byte / x->sector_size))
        return false;
    put32(x->fat_sector + byte % x->sector_size, value);
    x->fat_dirty = true;
    x->chain_first = 0;                             /* (a remembered walk may be stale) */
    return true;
}

/* The bitmap's sector holding cluster c's bit, in memory (the one before
 * written back if it changed); 0 if it cannot be read. */
static uint8_t *bitmap_byte(struct exfat *x, uint32_t c)
{
    uint64_t byte = (c - 2) / 8;
    struct stream s = { x->bitmap_first, x->bitmap_contiguous, x->bitmap_length, x->bitmap_length };
    uint32_t bc = cluster_at(x, &s, (uint32_t)(byte / x->cluster_bytes));
    if (!in_heap(x, bc))
        return 0;
    uint64_t sector = cluster_sector(x, bc) + byte % x->cluster_bytes / x->sector_size;
    if (x->bitmap_sector_at != sector) {
        if (x->bitmap_dirty) {
            if (!write_sectors(x, x->bitmap_sector_at, 1, x->bitmap_sector))
                return 0;
            x->bitmap_dirty = false;
        }
        if (!read_sectors(x, sector, 1, x->bitmap_sector)) {
            x->bitmap_sector_at = ~0ull;
            return 0;
        }
        x->bitmap_sector_at = sector;
    }
    return x->bitmap_sector + byte % x->sector_size;
}

/* A free cluster, marked in the bitmap (its FAT entry the caller's); 0 if
 * there is none or the bitmap cannot be read. */
static uint32_t allocate(struct exfat *x)
{
    uint32_t c = in_heap(x, x->next_free) ? x->next_free : 2;
    for (uint32_t i = 0; i < x->clusters; i++) {
        uint8_t *b = bitmap_byte(x, c);
        if (!b)
            return 0;
        uint8_t bit = (uint8_t)(1 << ((c - 2) % 8));
        if (!(*b & bit)) {
            *b |= bit;
            x->bitmap_dirty = true;
            x->next_free = c + 1;
            if (x->free_count)
                x->free_count--;
            return c;
        }
        c = c + 1 < x->clusters + 2 ? c + 1 : 2;
    }
    return 0;
}

static bool release(struct exfat *x, uint32_t c)
{
    uint8_t *b = bitmap_byte(x, c);
    if (!b)
        return false;
    *b &= (uint8_t)~(1 << ((c - 2) % 8));
    x->bitmap_dirty = true;
    x->free_count++;
    return true;
}

/* An object's clusters free again (a chain's FAT entries cleared too). */
static bool free_stream(struct exfat *x, const struct stream *s)
{
    if (!in_heap(x, s->first))
        return true;
    uint32_t n = (uint32_t)((s->length + x->cluster_bytes - 1) / x->cluster_bytes);
    if (s->contiguous) {
        for (uint32_t i = 0; i < n; i++)
            if (!release(x, s->first + i))
                return false;
        return true;
    }
    uint32_t c = s->first;
    for (uint32_t i = 0; in_heap(x, c) && i <= x->clusters; i++) {
        uint32_t next = next_cluster(x, c);
        if (!release(x, c) || !fat_set(x, c, 0))
            return false;
        c = next;
    }
    return true;
}

/* The clusters s has (its data length's worth; the root: its chain's). */
static uint32_t cluster_count(struct exfat *x, const struct stream *s)
{
    if (!in_heap(x, s->first))
        return 0;
    if (s->length != ~0ull)
        return (uint32_t)((s->length + x->cluster_bytes - 1) / x->cluster_bytes);
    uint32_t n = 1;
    for (uint32_t c = s->first; (c = next_cluster(x, c)) != CHAIN_END && n <= x->clusters; n++)
        if (c == CHAIN_BAD)
            break;
    return n;
}

/* Let s reach n clusters (a run made a FAT chain first); *have: how many
 * it has now (also when the volume is full: RFS_ERR_NO_SPACE). */
static int grow(struct exfat *x, struct stream *s, uint32_t n, uint32_t *have)
{
    *have = cluster_count(x, s);
    if (n <= *have)
        return RFS_OK;
    if (s->contiguous && *have) {
        for (uint32_t i = 0; i < *have; i++)
            if (!fat_set(x, s->first + i, i + 1 < *have ? s->first + i + 1 : CHAIN_END_MARK))
                return RFS_ERR_IO;
    }
    s->contiguous = false;
    uint32_t last = 0;
    if (*have) {
        struct stream all = *s;
        all.length = (uint64_t)*have * x->cluster_bytes;
        last = cluster_at(x, &all, *have - 1);
        if (!in_heap(x, last))
            return RFS_ERR_IO;
    }
    while (*have < n) {
        uint32_t c = allocate(x);
        if (!c)
            return RFS_ERR_NO_SPACE;
        if (!fat_set(x, c, CHAIN_END_MARK) || (last && !fat_set(x, last, c)))
            return RFS_ERR_IO;
        if (!last)
            s->first = c;
        last = c;
        (*have)++;
    }
    return RFS_OK;
}

/* Write n bytes (zeros if data is 0) at byte pos of s's first `room`
 * bytes of clusters; sectors written in part are read first. */
static int write_stream(struct exfat *x, const struct stream *s, uint64_t room, uint64_t pos,
                        const uint8_t *data, uint64_t n)
{
    struct stream w = *s;
    w.length = room;
    uint32_t ss = x->sector_size;
    for (uint64_t done = 0; done < n;) {
        uint64_t at = pos + done;
        uint32_t c = cluster_at(x, &w, (uint32_t)(at / x->cluster_bytes));
        if (!in_heap(x, c))
            return RFS_ERR_IO;
        uint64_t within = at % x->cluster_bytes;
        uint64_t take = x->cluster_bytes - within;
        if (take > n - done)
            take = n - done;
        if (take > READ_CHUNK - ss)
            take = READ_CHUNK - ss;
        uint32_t s0 = (uint32_t)(within / ss), s1 = (uint32_t)((within + take + ss - 1) / ss);
        uint64_t sector = cluster_sector(x, c);
        if ((within % ss && !read_sectors(x, sector + s0, 1, x->chunk)) ||
            ((within + take) % ss &&
             !read_sectors(x, sector + s1 - 1, 1, x->chunk + (uint64_t)(s1 - 1 - s0) * ss)))
            return RFS_ERR_IO;
        uint8_t *to = x->chunk + within - (uint64_t)s0 * ss;
        if (data)
            memcpy(to, data + done, take);
        else
            memset(to, 0, take);
        if (!write_sectors(x, sector + s0, s1 - s0, x->chunk))
            return RFS_ERR_IO;
        done += take;
    }
    return RFS_OK;
}

/* The volume marked dirty on the disk from the first change on. */
static bool mark_dirty(struct exfat *x)
{
    if (x->dirty)
        return true;
    uint8_t *b = x->chunk;
    if (!read_sectors(x, 0, 1, b))
        return false;
    b[106] |= 2;                                    /* VolumeDirty (not in the checksum) */
    if (!write_sectors(x, 0, 1, b))
        return false;
    x->dirty = true;
    return true;
}

/* A time (nanoseconds since 1970) as exFAT keeps it, in UTC (zone 0,
 * marked valid). */
static void exfat_from(uint64_t ns, uint32_t *stamp, uint8_t *ten_ms, uint8_t *zone)
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
    *zone = 0x80;
    if (ns == 0 || year < 1980 || year > 2107) {
        *stamp = 1u << 21 | 1u << 16, *ten_ms = 0;  /* 1980-01-01 */
        return;
    }
    *stamp = (uint32_t)(year - 1980) << 25 | month << 21 | day << 16 |
             (uint32_t)(rest / 3600) << 11 | (uint32_t)(rest / 60 % 60) << 5 |
             (uint32_t)(rest % 60 / 2);
    *ten_ms = (uint8_t)(rest % 2 * 100 + ns / 10000000 % 100);
}

static void exfat_now(uint32_t *stamp, uint8_t *ten_ms, uint8_t *zone)
{
    exfat_from(uefi_clock_ns(), stamp, ten_ms, zone);
}

/* An entry set in memory, with where each entry lies on the device. */
struct set {
    uint8_t e[SET_MAX * ENTRY];
    uint64_t where[SET_MAX];
    int count;
};

/* The entry after the one at device byte where in its directory (in the
 * next cluster when it ends one: one on, or the FAT's); ~0 if none. */
static uint64_t next_where(struct exfat *x, uint64_t where, bool contiguous_dir)
{
    uint64_t heap = x->heap_start * x->sector_size;
    if (where < heap || (where - heap + ENTRY) % x->cluster_bytes)
        return where + ENTRY;
    uint32_t c = (uint32_t)((where - heap) / x->cluster_bytes) + 2;
    uint32_t next = contiguous_dir ? c + 1 : next_cluster(x, c);
    return in_heap(x, next) ? cluster_sector(x, next) * x->sector_size : ~0ull;
}

static bool get_entry(struct exfat *x, uint64_t where, uint8_t *e)
{
    uint64_t sector = where / x->sector_size;
    if (x->dir_sector_at != sector) {
        if (!read_sectors(x, sector, 1, x->dir_sector)) {
            x->dir_sector_at = ~0ull;
            return false;
        }
        x->dir_sector_at = sector;
    }
    memcpy(e, x->dir_sector + where % x->sector_size, ENTRY);
    return true;
}

static bool put_entry(struct exfat *x, uint64_t where, const uint8_t *e)
{
    uint8_t old[ENTRY];
    if (!get_entry(x, where, old))
        return false;
    memcpy(x->dir_sector + where % x->sector_size, e, ENTRY);
    return write_sectors(x, where / x->sector_size, 1, x->dir_sector);
}

/* The set of the object id (a file entry and its secondaries). */
static bool set_read(struct exfat *x, uint64_t id, struct set *t)
{
    if ((id & ~CONTIGUOUS_DIR) < 2)
        return false;
    uint64_t where = ((id & ~CONTIGUOUS_DIR) - 2) * ENTRY;
    if (!get_entry(x, where, t->e) || t->e[0] != TYPE_FILE || t->e[1] < 1 || t->e[1] >= SET_MAX)
        return false;
    t->count = t->e[1] + 1;
    t->where[0] = where;
    for (int i = 1; i < t->count; i++) {
        where = next_where(x, where, id & CONTIGUOUS_DIR);
        if (where == ~0ull || !get_entry(x, where, t->e + i * ENTRY))
            return false;
        t->where[i] = where;
    }
    return t->e[ENTRY] == TYPE_STREAM;
}

static bool set_write(struct exfat *x, struct set *t)
{
    put16(t->e + 2, set_checksum(t->e, t->count));
    for (int i = 0; i < t->count; i++)
        if (!put_entry(x, t->where[i], t->e + i * ENTRY))
            return false;
    return true;
}

static void set_stream(const struct set *t, struct stream *s)
{
    const uint8_t *e = t->e + ENTRY;
    s->first = u32(e + 20);
    s->contiguous = e[1] & FLAG_NO_FAT_CHAIN;
    s->valid = u64(e + 8);
    s->length = u64(e + 24);
}

static void put_stream(struct set *t, const struct stream *s)
{
    uint8_t *e = t->e + ENTRY;
    e[1] = (uint8_t)(FLAG_ALLOCATION | (s->contiguous ? FLAG_NO_FAT_CHAIN : 0));
    put32(e + 20, s->first);
    put64(e + 8, s->valid);
    put64(e + 24, s->length);
}

/* The modification time (and the creation time too) now. */
static void stamp(struct set *t, bool created)
{
    uint32_t now;
    uint8_t ten_ms, zone;
    exfat_now(&now, &ten_ms, &zone);
    if (created) {
        put32(t->e + 8, now);
        t->e[20] = ten_ms;
        t->e[22] = zone;
    }
    put32(t->e + 12, now);
    put32(t->e + 16, now);
    t->e[21] = ten_ms;
    t->e[23] = t->e[24] = zone;
}

/* Room for n entries in a directory (dir_id's; 1: the root): the device
 * places of a run of free entries (deleted or past the end), the directory
 * growing by a cluster where it is full. *contiguous: whether it is one
 * run then (for the IDs). */
static int room(struct exfat *x, uint64_t dir_id, const struct rfs_inode *dir, int n,
                uint64_t *places, bool *contiguous)
{
    struct stream s;
    stream_of(dir, &s);
    for (int attempt = 0; attempt < 4; attempt++) {  /* (small clusters: a set may need two) */
        int run = 0;
        uint64_t pos = 0;
        for (;; pos += ENTRY) {
            uint8_t e[ENTRY];
            uint64_t where;
            int r = dir_entry(x, &s, pos, e, &where);
            if (r < 0)
                return RFS_ERR_IO;
            if (r == 0)
                break;                              /* full: it grows below */
            if (e[0] & 0x80) {
                run = 0;
                continue;
            }
            places[run++] = where;
            if (run == n) {
                *contiguous = s.contiguous;
                return RFS_OK;
            }
        }
        /* One cluster more, zeroed (its entries: the end of the directory). */
        uint32_t have, now = cluster_count(x, &s);
        struct set t;
        if (dir_id != EXFAT_ROOT_ID && !set_read(x, dir_id, &t))
            return RFS_ERR_IO;
        int r = grow(x, &s, now + 1, &have);
        if (r != RFS_OK)
            return r;
        struct stream w = s;
        w.length = (uint64_t)have * x->cluster_bytes;
        uint32_t c = cluster_at(x, &w, have - 1);
        memset(x->chunk, 0, x->cluster_bytes < READ_CHUNK ? x->cluster_bytes : READ_CHUNK);
        for (uint32_t done = 0; done < x->cluster_sectors;) {
            uint32_t k = x->cluster_sectors - done;
            if (k > READ_CHUNK / x->sector_size)
                k = READ_CHUNK / x->sector_size;
            if (!write_sectors(x, cluster_sector(x, c) + done, k, x->chunk))
                return RFS_ERR_IO;
            done += k;
        }
        x->dir_sector_at = ~0ull;
        if (dir_id != EXFAT_ROOT_ID) {
            s.length = s.valid = w.length;
            put_stream(&t, &s);
            stamp(&t, false);
            if (!set_write(x, &t))
                return RFS_ERR_IO;
        }
    }
    return RFS_ERR_NO_SPACE;
}

/* A name exFAT can hold: 1-255 UTF-16 units, none of \ / : * ? " < > |
 * and no control characters. Its units, or -1. */
static int valid_name(const char *name, size_t len, uint16_t *units)
{
    int n = utf16_name(name, len, units);
    if (n <= 0)
        return -1;
    static const char forbidden[] = "\\/:*?\"<>|";
    for (int i = 0; i < n; i++) {
        if (units[i] < 0x20)
            return -1;
        for (const char *c = forbidden; *c; c++)
            if (units[i] == (uint16_t)*c)
                return -1;
    }
    return n;
}

/* A new set for name (units) with the file and stream entries given. */
static void make_set(struct exfat *x, struct set *t, const uint8_t *file, const uint8_t *stream,
                     const uint16_t *name, int units)
{
    int names = (units + NAME_ENTRY_UNITS - 1) / NAME_ENTRY_UNITS;
    memset(t->e, 0, sizeof(t->e));
    memcpy(t->e, file, ENTRY);
    memcpy(t->e + ENTRY, stream, ENTRY);
    t->count = 2 + names;
    t->e[0] = TYPE_FILE;
    t->e[1] = (uint8_t)(1 + names);
    t->e[ENTRY] = TYPE_STREAM;
    t->e[ENTRY + 3] = (uint8_t)units;
    put16(t->e + ENTRY + 4, name_hash(x, name, (size_t)units));
    for (int i = 0; i < names; i++) {
        uint8_t *e = t->e + (2 + i) * ENTRY;
        e[0] = TYPE_NAME;
        for (int k = 0; k < NAME_ENTRY_UNITS && i * NAME_ENTRY_UNITS + k < units; k++)
            put16(e + 2 + 2 * k, name[i * NAME_ENTRY_UNITS + k]);
    }
}

/* Put set t into the directory dir_id: its places, then its entries. */
static int add_set(struct exfat *x, uint64_t dir_id, const struct rfs_inode *dir, struct set *t,
                   uint64_t *id)
{
    bool contiguous;
    int r = room(x, dir_id, dir, t->count, t->where, &contiguous);
    if (r != RFS_OK)
        return r;
    if (!set_write(x, t))
        return RFS_ERR_IO;
    *id = (t->where[0] / ENTRY + 2) | (contiguous ? CONTIGUOUS_DIR : 0);
    return RFS_OK;
}

/* A set no longer in use (its entries' bit 7 cleared). */
static bool delete_set(struct exfat *x, struct set *t)
{
    for (int i = 0; i < t->count; i++) {
        t->e[i * ENTRY] &= 0x7F;
        if (!put_entry(x, t->where[i], t->e + i * ENTRY))
            return false;
    }
    return true;
}

/* The directory a path's last part goes into (its ID and inode), and the
 * last part. */
static int parent_of(struct exfat *x, const char *path, uint64_t *dir_id, struct rfs_inode *dir,
                     const char **name, size_t *len, struct found *e)
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
    if (!walk(x, parent, dir_id, dir, e))
        return RFS_ERR_NOT_FOUND;
    return dir->type == RFS_TYPE_DIR ? RFS_OK : RFS_ERR_NOT_DIR;
}

int exfat_create(struct exfat *x, const char *path, int type, uint64_t *id)
{
    if (x->read_only)
        return RFS_ERR_READ_ONLY;
    struct found *e = kmalloc(sizeof(*e));
    struct set *t = kmalloc(sizeof(*t));
    if (!e || !t) {
        kfree(e);
        kfree(t);
        return RFS_ERR_IO;
    }
    mutex_lock(&x->lock);
    uint64_t dir_id, existing;
    struct rfs_inode dir, inode;
    const char *name;
    size_t len;
    uint16_t units[NAME_UNITS];
    int n = 0;
    int r = parent_of(x, path, &dir_id, &dir, &name, &len, e);
    if (r == RFS_OK && walk(x, path, &existing, &inode, e))
        r = RFS_ERR_EXISTS;
    if (r == RFS_OK && (n = valid_name(name, len, units)) < 0)
        r = RFS_ERR_BAD_NAME;
    if (r == RFS_OK && !mark_dirty(x))
        r = RFS_ERR_IO;
    uint8_t file[ENTRY], stream[ENTRY];
    memset(file, 0, sizeof(file));
    memset(stream, 0, sizeof(stream));
    struct stream s = { 0, false, 0, 0 };
    if (r == RFS_OK && type == RFS_TYPE_DIR) {
        /* Its first cluster, zeroed: an empty directory. */
        uint32_t have;
        r = grow(x, &s, 1, &have);
        if (r == RFS_OK) {
            s.length = s.valid = x->cluster_bytes;
            memset(x->chunk, 0, x->cluster_bytes < READ_CHUNK ? x->cluster_bytes : READ_CHUNK);
            for (uint32_t done = 0; r == RFS_OK && done < x->cluster_sectors;) {
                uint32_t k = x->cluster_sectors - done;
                if (k > READ_CHUNK / x->sector_size)
                    k = READ_CHUNK / x->sector_size;
                if (!write_sectors(x, cluster_sector(x, s.first) + done, k, x->chunk))
                    r = RFS_ERR_IO;
                done += k;
            }
        }
    }
    if (r == RFS_OK) {
        put16(file + 4, type == RFS_TYPE_DIR ? ATTR_DIRECTORY : ATTR_ARCHIVE);
        make_set(x, t, file, stream, units, n);
        put_stream(t, &s);
        stamp(t, true);
        r = add_set(x, dir_id, &dir, t, id);
    }
    mutex_unlock(&x->lock);
    kfree(e);
    kfree(t);
    return r;
}

int exfat_write(struct exfat *x, uint64_t id, uint64_t offset, const void *data, uint64_t size)
{
    if (x->read_only)
        return RFS_ERR_READ_ONLY;
    struct set *t = kmalloc(sizeof(*t));
    if (!t)
        return RFS_ERR_IO;
    mutex_lock(&x->lock);
    int r = set_read(x, id, t) ? RFS_OK : RFS_ERR_NOT_FOUND;
    if (r == RFS_OK && (u16(t->e + 4) & ATTR_DIRECTORY))
        r = RFS_ERR_IS_DIR;
    if (r == RFS_OK && !mark_dirty(x))
        r = RFS_ERR_IO;
    if (r == RFS_OK) {
        struct stream s;
        set_stream(t, &s);
        uint64_t end = offset + size;
        uint32_t have;
        r = grow(x, &s, (uint32_t)((end + x->cluster_bytes - 1) / x->cluster_bytes), &have);
        uint64_t space = (uint64_t)have * x->cluster_bytes;
        int w = RFS_OK;
        /* What lies between the valid data and the new bytes: zeros. */
        if (offset > s.valid) {
            uint64_t to = offset < space ? offset : space;
            w = write_stream(x, &s, space, s.valid, 0, to - s.valid);
            if (w == RFS_OK)
                s.valid = to;
        }
        uint64_t reach = end < space ? end : space;
        if (w == RFS_OK && reach > offset) {
            w = write_stream(x, &s, space, offset, data, reach - offset);
            if (w == RFS_OK && reach > s.valid)
                s.valid = reach;
        }
        /* The set, also after a failure (a full volume): its clusters stay
         * the file's, its lengths say what was written. */
        if (s.valid > s.length)
            s.length = s.valid;
        if (s.length > space)
            s.length = space;
        put_stream(t, &s);
        put16(t->e + 4, u16(t->e + 4) | ATTR_ARCHIVE);
        stamp(t, false);
        if (!set_write(x, t))
            w = RFS_ERR_IO;
        if (r == RFS_OK)
            r = w;
    }
    mutex_unlock(&x->lock);
    kfree(t);
    return r;
}

int exfat_truncate(struct exfat *x, uint64_t id)
{
    if (x->read_only)
        return RFS_ERR_READ_ONLY;
    struct set *t = kmalloc(sizeof(*t));
    if (!t)
        return RFS_ERR_IO;
    mutex_lock(&x->lock);
    int r = set_read(x, id, t) ? RFS_OK : RFS_ERR_NOT_FOUND;
    if (r == RFS_OK && (u16(t->e + 4) & ATTR_DIRECTORY))
        r = RFS_ERR_IS_DIR;
    if (r == RFS_OK && !mark_dirty(x))
        r = RFS_ERR_IO;
    struct stream s;
    if (r == RFS_OK) {
        set_stream(t, &s);
        if (!free_stream(x, &s))
            r = RFS_ERR_IO;
    }
    if (r == RFS_OK) {
        s = (struct stream){ 0, false, 0, 0 };
        put_stream(t, &s);
        stamp(t, false);
        if (!set_write(x, t))
            r = RFS_ERR_IO;
    }
    mutex_unlock(&x->lock);
    kfree(t);
    return r;
}

int exfat_remove(struct exfat *x, const char *path)
{
    if (x->read_only)
        return RFS_ERR_READ_ONLY;
    struct found *e = kmalloc(sizeof(*e));
    struct set *t = kmalloc(sizeof(*t));
    if (!e || !t) {
        kfree(e);
        kfree(t);
        return RFS_ERR_IO;
    }
    mutex_lock(&x->lock);
    uint64_t id;
    struct rfs_inode inode;
    int r = walk(x, path, &id, &inode, e) ? RFS_OK : RFS_ERR_NOT_FOUND;
    if (r == RFS_OK && id == EXFAT_ROOT_ID)
        r = RFS_ERR_BAD_NAME;
    if (r == RFS_OK && inode.type == RFS_TYPE_DIR) {
        struct stream s;
        stream_of(&inode, &s);
        uint64_t pos = 0;
        int k = next_entry(x, &s, &pos, e);
        r = k < 0 ? RFS_ERR_IO : k > 0 ? RFS_ERR_NOT_EMPTY : RFS_OK;
    }
    if (r == RFS_OK && (!mark_dirty(x) || !set_read(x, id, t)))
        r = RFS_ERR_IO;
    if (r == RFS_OK) {
        struct stream s;
        set_stream(t, &s);
        if (!free_stream(x, &s) || !delete_set(x, t))
            r = RFS_ERR_IO;
    }
    mutex_unlock(&x->lock);
    kfree(e);
    kfree(t);
    return r;
}

/* Whether path a lies in directory b (or is it), without regard to the
 * case of A-Z. */
static bool inside(const char *a, const char *b)
{
    size_t n = strlen(b);
    if (strlen(a) < n)
        return false;
    for (size_t i = 0; i < n; i++) {
        char p = a[i] >= 'a' && a[i] <= 'z' ? (char)(a[i] - 32) : a[i];
        char q = b[i] >= 'a' && b[i] <= 'z' ? (char)(b[i] - 32) : b[i];
        if (p != q)
            return false;
    }
    return a[n] == 0 || a[n] == '/';
}

int exfat_rename(struct exfat *x, const char *from, const char *to)
{
    if (x->read_only)
        return RFS_ERR_READ_ONLY;
    struct found *e = kmalloc(sizeof(*e));
    struct set *old = kmalloc(sizeof(*old)), *t = kmalloc(sizeof(*t));
    if (!e || !old || !t) {
        kfree(e);
        kfree(old);
        kfree(t);
        return RFS_ERR_IO;
    }
    mutex_lock(&x->lock);
    uint64_t id, other, dir_id, new_id;
    struct rfs_inode inode, target, dir;
    const char *name;
    size_t len;
    uint16_t units[NAME_UNITS];
    int n = 0;
    int r = walk(x, from, &id, &inode, e) ? RFS_OK : RFS_ERR_NOT_FOUND;
    if (r == RFS_OK && id == EXFAT_ROOT_ID)
        r = RFS_ERR_BAD_NAME;
    if (r == RFS_OK)
        r = parent_of(x, to, &dir_id, &dir, &name, &len, e);
    if (r == RFS_OK && inode.type == RFS_TYPE_DIR && inside(to, from))
        r = RFS_ERR_BAD_NAME;                       /* into itself */
    if (r == RFS_OK && (n = valid_name(name, len, units)) < 0)
        r = RFS_ERR_BAD_NAME;
    bool same = false, replace = false;
    if (r == RFS_OK && walk(x, to, &other, &target, e)) {
        same = (other & ~CONTIGUOUS_DIR) == (id & ~CONTIGUOUS_DIR);   /* only the case changes */
        replace = !same;
        if (replace && inode.type == RFS_TYPE_DIR && target.type != RFS_TYPE_DIR)
            r = RFS_ERR_NOT_DIR;
        else if (replace && inode.type != RFS_TYPE_DIR && target.type == RFS_TYPE_DIR)
            r = RFS_ERR_IS_DIR;
    }
    /* (68a) An entry at to is replaced, as POSIX's rename (exfat_remove
     * checks a directory is empty). exFAT has no transactions: it goes
     * first, and a failure after it leaves from as it was and the target
     * gone. Its directory is read again: the removal may have changed it. */
    if (r == RFS_OK && replace) {
        r = exfat_remove(x, to);
        if (r == RFS_OK)
            r = parent_of(x, to, &dir_id, &dir, &name, &len, e);
    }
    if (r == RFS_OK && (!mark_dirty(x) || !set_read(x, id, old)))
        r = RFS_ERR_IO;
    if (r == RFS_OK) {
        make_set(x, t, old->e, old->e + ENTRY, units, n);
        if (same && !delete_set(x, old))            /* (its places may be taken again) */
            r = RFS_ERR_IO;
        if (r == RFS_OK)
            r = add_set(x, dir_id, &dir, t, &new_id);
        if (r == RFS_OK && !same && !delete_set(x, old))
            r = RFS_ERR_IO;
    }
    mutex_unlock(&x->lock);
    kfree(e);
    kfree(old);
    kfree(t);
    return r;
}

int exfat_chmod(struct exfat *x, uint64_t id, unsigned mode)
{
    if (x->read_only)
        return RFS_ERR_READ_ONLY;
    struct set *t = kmalloc(sizeof(*t));
    if (!t)
        return RFS_ERR_IO;
    mutex_lock(&x->lock);
    int r = set_read(x, id, t) ? RFS_OK : RFS_ERR_NOT_FOUND;
    uint16_t attributes = r == RFS_OK ? u16(t->e + 4) : 0;
    if (r == RFS_OK && !(attributes & ATTR_DIRECTORY)) {
        /* Only the owner's w is kept: as the read-only attribute. */
        put16(t->e + 4, mode & 0200 ? attributes & ~ATTR_READ_ONLY : attributes | ATTR_READ_ONLY);
        if (!mark_dirty(x) || !set_write(x, t))
            r = RFS_ERR_IO;
    }
    mutex_unlock(&x->lock);
    kfree(t);
    return r;
}

/* (63m) The time it was last changed (10 ms steps, in UTC). The root has
 * no file entry: nothing to set. */
int exfat_set_time(struct exfat *x, uint64_t id, uint64_t modified)
{
    if (x->read_only)
        return RFS_ERR_READ_ONLY;
    if (id == 1)
        return RFS_OK;
    struct set *t = kmalloc(sizeof(*t));
    if (!t)
        return RFS_ERR_IO;
    mutex_lock(&x->lock);
    int r = set_read(x, id, t) ? RFS_OK : RFS_ERR_NOT_FOUND;
    if (r == RFS_OK) {
        uint32_t when;
        uint8_t ten_ms, zone;
        exfat_from(modified, &when, &ten_ms, &zone);
        put32(t->e + 12, when);
        t->e[21] = ten_ms;
        t->e[23] = zone;
        if (!mark_dirty(x) || !set_write(x, t))
            r = RFS_ERR_IO;
    }
    mutex_unlock(&x->lock);
    kfree(t);
    return r;
}

int exfat_commit(struct exfat *x)
{
    if (x->read_only)
        return RFS_OK;
    mutex_lock(&x->lock);
    bool ok = true;
    if (x->fat_dirty) {
        ok = write_sectors(x, x->fat_sector_at, 1, x->fat_sector);
        x->fat_dirty = !ok;
    }
    if (ok && x->bitmap_dirty) {
        ok = write_sectors(x, x->bitmap_sector_at, 1, x->bitmap_sector);
        x->bitmap_dirty = !ok;
    }
    if (ok && x->dirty && x->dev->flush)
        ok = x->dev->flush(x->dev);
    /* Everything written: the volume is clean again, its use up to date. */
    if (ok && x->dirty) {
        uint8_t *b = x->chunk;
        if ((ok = read_sectors(x, 0, 1, b))) {
            b[106] &= (uint8_t)~2;
            b[112] = (uint8_t)((uint64_t)(x->clusters - x->free_count) * 100 / x->clusters);
            ok = write_sectors(x, 0, 1, b);
        }
        x->dirty = !ok;
    }
    if (ok && x->dev->flush)
        ok = x->dev->flush(x->dev);
    mutex_unlock(&x->lock);
    return ok ? RFS_OK : RFS_ERR_IO;
}

/* --- mounting ------------------------------------------------------------- */

/* The boot region's checksum over its first 11 sectors (the volume flags
 * and the percent in use, which change, left out). */
static uint32_t boot_checksum(const uint8_t *region, uint32_t sector_size)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 11 * sector_size; i++) {
        if (i == 106 || i == 107 || i == 112)
            continue;
        sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + region[i];
    }
    return sum;
}

/* The up-case table: in its stored form, runs of units that are their own
 * capitals are kept as 0xFFFF and their number. Without a usable table,
 * a-z alone (logged). */
static void load_upcase(struct exfat *x, uint32_t first, uint64_t length, uint32_t checksum,
                        const char *label)
{
    for (uint32_t i = 0; i < 65536; i++)
        x->upcase[i] = (uint16_t)(i >= 'a' && i <= 'z' ? i - 32 : i);
    if (!in_heap(x, first) || length == 0 || length > 256 * 1024 || length % 2) {
        kprintf("  %-15sexFAT: no usable up-case table: a-z only\n", label);
        return;
    }
    uint8_t *data = kmalloc(length + x->cluster_bytes);
    if (!data)
        return;
    /* Its clusters: the FAT's chain, or one run where the FAT holds none. */
    uint64_t done = 0;
    bool ok = true;
    uint32_t c = first;
    while (ok && done < length) {
        ok = read_sectors(x, cluster_sector(x, c), x->cluster_sectors, data + done);
        done += x->cluster_bytes;
        if (done < length) {
            uint32_t next = next_cluster(x, c);
            c = next == CHAIN_BAD || next == CHAIN_END ? c + 1 : next;
            ok = ok && in_heap(x, c);
        }
    }
    uint32_t sum = 0;
    for (uint64_t i = 0; ok && i < length; i++)
        sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + data[i];
    if (!ok || sum != checksum) {
        kprintf("  %-15sexFAT: the up-case table is damaged: a-z only\n", label);
        kfree(data);
        return;
    }
    uint32_t at = 0;
    for (uint64_t i = 0; i + 1 < length && at < 65536; i += 2) {
        uint16_t v = u16(data + i);
        if (v == 0xFFFF && i + 3 < length) {
            uint32_t run = u16(data + i + 2);
            for (uint32_t k = 0; k < run && at < 65536; k++, at++)
                x->upcase[at] = (uint16_t)at;
            i += 2;
        } else {
            x->upcase[at++] = v;
        }
    }
    kfree(data);
}

struct exfat *exfat_mount(struct block_device *dev)
{
    if (dev->block_size > 4096)
        return 0;
    uint8_t *b = kmalloc(4096);
    if (!b)
        return 0;
    if (!dev->read(dev, 0, 4096 / dev->block_size, b) || b[510] != 0x55 || b[511] != 0xAA ||
        memcmp(b + 3, "EXFAT   ", 8) != 0) {
        kfree(b);
        return 0;
    }
    char label[24];
    ksnprintf(label, sizeof(label), "%s:", dev->name);
    uint32_t sector_shift = b[108], cluster_shift = b[109];
    kfree(b);
    const char *problem = 0;
    if (sector_shift < 9 || sector_shift > 12 || (1u << sector_shift) % dev->block_size)
        problem = "bad sector size";
    else if (cluster_shift > 25 - sector_shift)
        problem = "bad cluster size";
    uint32_t sector_size = problem ? 512 : 1u << sector_shift;
    /* The boot region (12 sectors) and its checksum; the backup copy (the
     * next 12) if the first is damaged. */
    uint8_t *region = problem ? 0 : kmalloc(12 * sector_size);
    int copy = -1;
    for (int k = 0; region && k < 2 && copy < 0; k++) {
        if (!dev->read(dev, (uint64_t)k * 12 * sector_size / dev->block_size,
                       12 * sector_size / dev->block_size, region))
            continue;
        uint32_t sum = boot_checksum(region, sector_size);
        bool ok = memcmp(region + 3, "EXFAT   ", 8) == 0;
        for (uint32_t i = 0; ok && i < sector_size / 4; i++)
            ok = u32(region + 11 * sector_size + i * 4) == sum;
        if (ok)
            copy = k;
    }
    if (!problem && copy < 0)
        problem = region ? "boot region checksum wrong (both copies)" : "out of memory";
    if (!problem && copy == 1)
        kprintf("  %-15sexFAT: the boot region is damaged, its backup copy used\n", label);
    uint64_t volume_length = 0, fat_offset = 0, fat_length = 0, heap_offset = 0;
    uint32_t clusters = 0, root = 0, region_fats = 1;
    if (!problem) {
        volume_length = u64(region + 72);
        fat_offset = u32(region + 80);
        fat_length = u32(region + 84);
        heap_offset = u32(region + 88);
        clusters = u32(region + 92);
        root = u32(region + 96);
        region_fats = region[110];
        if (region[110] != 1 && region[110] != 2)
            problem = "bad number of FATs";
        else if (volume_length * sector_size > dev->block_count * dev->block_size)
            problem = "larger than its device";
        else if (fat_offset < 24 || fat_length * sector_size < ((uint64_t)clusters + 2) * 4 ||
                 heap_offset < fat_offset + fat_length ||
                 heap_offset + ((uint64_t)clusters << cluster_shift) > volume_length)
            problem = "bad layout";
        else if (root < 2 || root >= clusters + 2)
            problem = "bad root directory";
    }
    kfree(region);
    if (problem) {
        kprintf("  %-15sexFAT: %s, not mounted\n", label, problem);
        return 0;
    }

    struct exfat *x = kzalloc(sizeof(*x));
    if (!x)
        return 0;
    x->vfs.kind = VFS_EXFAT;
    x->dev = dev;
    x->sector_size = sector_size;
    x->cluster_sectors = 1u << cluster_shift;
    x->cluster_bytes = sector_size << cluster_shift;
    x->device_blocks_per_sector = sector_size / dev->block_size;
    x->fat_start = fat_offset;
    x->heap_start = heap_offset;
    x->clusters = clusters;
    x->root_cluster = root;
    x->read_only = region_fats == 2;                /* TexFAT: read only */
    x->fat_sector_at = x->dir_sector_at = x->bitmap_sector_at = ~0ull;
    x->lock = (struct mutex)MUTEX_INIT("exfat");
    x->fat_sector = kmalloc(sector_size);
    x->dir_sector = kmalloc(sector_size);
    x->chunk = kmalloc(READ_CHUNK);
    x->upcase = kmalloc(65536 * sizeof(uint16_t));
    x->bitmap_sector = kmalloc(sector_size);
    struct found *e = kmalloc(sizeof(*e));
    if (!x->fat_sector || !x->dir_sector || !x->chunk || !x->upcase || !x->bitmap_sector || !e) {
        kfree(x->bitmap_sector);
        kfree(x->fat_sector);
        kfree(x->dir_sector);
        kfree(x->chunk);
        kfree(x->upcase);
        kfree(e);
        kfree(x);
        return 0;
    }
    /* The root directory: the label, the up-case table, and the count. */
    struct stream s = { root, false, ~0ull, ~0ull };
    uint32_t upcase_first = 0, upcase_sum = 0;
    uint64_t upcase_length = 0;
    bool readable = true;
    mutex_lock(&x->lock);
    for (uint64_t pos = 0;; pos += ENTRY) {
        uint64_t where;
        int r = dir_entry(x, &s, pos, e->set, &where);
        if (r < 0)
            readable = false;
        if (r <= 0 || e->set[0] == TYPE_END)
            break;
        if (e->set[0] == TYPE_LABEL) {
            uint16_t units[11];
            int n = e->set[1] < 11 ? e->set[1] : 11;
            for (int i = 0; i < n; i++)
                units[i] = u16(e->set + 2 + 2 * i);
            size_t len = utf8_name(units, (size_t)n, e->name);
            if (len >= sizeof(x->label))
                len = 0;
            memcpy(x->label, e->name, len);
            x->label[len] = 0;
        } else if (e->set[0] == TYPE_UPCASE) {
            upcase_sum = u32(e->set + 4);
            upcase_first = u32(e->set + 20);
            upcase_length = u64(e->set + 24);
        } else if (e->set[0] == TYPE_BITMAP && !(e->set[1] & 1)) {
            x->bitmap_first = u32(e->set + 20);     /* (the first FAT's bitmap) */
            x->bitmap_length = u64(e->set + 24);
        }
    }
    /* The bitmap: in one run where the FAT holds no chain for it; the free
     * clusters counted. Without one, nothing is written. */
    if (readable && in_heap(x, x->bitmap_first) &&
        x->bitmap_length * 8 >= x->clusters) {
        uint32_t entry = next_cluster(x, x->bitmap_first);
        x->bitmap_contiguous = entry == CHAIN_BAD && x->bitmap_length > x->cluster_bytes;
        x->free_count = 0;
        for (uint32_t c = 2; c < x->clusters + 2 && readable; c += 8) {
            uint8_t *b = bitmap_byte(x, c);
            if (!b) {
                readable = false;
                break;
            }
            for (uint32_t k = 0; k < 8 && c + k < x->clusters + 2; k++)
                x->free_count += !(*b >> k & 1);
        }
        x->next_free = 2;
    } else {
        x->read_only = true;
    }
    if (readable)
        load_upcase(x, upcase_first, upcase_length, upcase_sum, label);
    int entries = 0, r = -1;
    uint64_t pos = 0;
    if (readable)
        while ((r = next_entry(x, &s, &pos, e)) == 1)
            entries++;
    mutex_unlock(&x->lock);
    kfree(e);
    if (!readable || r < 0) {
        kprintf("  %-15sexFAT: the root directory cannot be read, not mounted\n", label);
        return 0;                                   /* x stays: something may point to it */
    }
    char unit[16];
    if (x->cluster_bytes < 1024)
        ksnprintf(unit, sizeof(unit), "%u bytes", x->cluster_bytes);
    else if (x->cluster_bytes < 1024 * 1024)
        ksnprintf(unit, sizeof(unit), "%u KiB", x->cluster_bytes / 1024);
    else
        ksnprintf(unit, sizeof(unit), "%u MiB", x->cluster_bytes >> 20);
    kprintf("  %-15sexFAT '%s', %lu clusters of %s (%lu MiB), root directory %d entries\n",
            label, x->label, (uint64_t)x->clusters, unit,
            (uint64_t)x->clusters * x->cluster_bytes >> 20, entries);
    return x;
}

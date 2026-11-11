/*
 * A new FAT32 on a device (format -t fat, osinstall's EFI partition), as
 * Windows lays it out: 32 reserved sectors (the boot sector with its BIOS
 * parameter block, FSInfo in sector 1, copies of both in 6 and 7), two
 * FATs, the root directory in cluster 2 (with the label); clusters of 512
 * bytes up to 260 MB, then 4, 8, 16 and 32 KiB up to 8, 16, 32 GB and
 * beyond. It needs at least 65525 clusters (about 33 MiB).
 */
#include <string.h>
#include "mkfs.h"
#include "ritteros.h"

#define FAT_RESERVED    32
#define FAT_MIN         65525           /* clusters: fewer would be FAT16 */
#define FAT_MAX         0x0FFFFFF5

static int fd;
static uint64_t now;
static uint8_t block[4096];

static bool write_at(uint64_t offset, const void *data, size_t n)
{
    return mkfs_write(fd, offset, data, n);
}

/* The layout for bytes on a device with blocks of sector bytes; false if
 * FAT32 does not fit (too small or too large). */
bool mkfs_fat_plan(uint64_t bytes, uint32_t sector, struct fat_layout *l)
{
    uint64_t mb = 1000 * 1000, gb = 1000 * mb;
    uint32_t cluster = bytes <= 260 * mb ? 512 : bytes <= 8 * gb ? 4096 : bytes <= 16 * gb ? 8192
                     : bytes <= 32 * gb ? 16384 : 32768;
    l->sector = sector;
    l->cluster_sectors = cluster > sector ? cluster / sector : 1;
    l->sectors = bytes / sector;
    if (l->sectors > 0xFFFFFFFFu)
        return false;
    /* The FATs' size: each cluster needs 4 bytes in each copy. */
    uint64_t fat = 1, clusters;
    for (;;) {
        clusters = (l->sectors - FAT_RESERVED - 2 * fat) / l->cluster_sectors;
        uint64_t need = ((clusters + 2) * 4 + sector - 1) / sector;
        if (need <= fat)
            break;
        fat = need;
    }
    l->fat_sectors = (uint32_t)fat;
    l->clusters = (uint32_t)clusters;
    return clusters >= FAT_MIN && clusters <= FAT_MAX;
}

static bool zero_bytes(uint64_t offset, uint64_t n)
{
    return mkfs_zero(fd, offset, n);
}

/* The time now as a DOS date and time (UTC). */
static void dos_time(uint16_t *date, uint16_t *time)
{
    uint64_t secs = now / 1000000000ULL, rest = secs % 86400;
    int64_t z = (int64_t)(secs / 86400) + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint64_t doe = (uint64_t)(z - era * 146097);
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint64_t mp = (5 * doy + 2) / 153;
    unsigned day = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
    unsigned month = (unsigned)(mp < 10 ? mp + 3 : mp - 9);
    int64_t year = (int64_t)yoe + era * 400 + (month <= 2);
    if (now == 0 || year < 1980 || year > 2107) {
        *date = 1 << 5 | 1, *time = 0;
        return;
    }
    *date = (uint16_t)((year - 1980) << 9 | month << 5 | day);
    *time = (uint16_t)(rest / 3600 << 11 | rest / 60 % 60 << 5 | rest % 60 / 2);
}

bool mkfs_fat32(int device, const struct fat_layout *l, const char *label, uint64_t hidden,
                bool whole_disk, uint64_t device_bytes, uint64_t at)
{
    fd = device, now = at;
    uint32_t ss = l->sector;
    uint64_t reserved_bytes = (uint64_t)FAT_RESERVED * ss;
    /* The old marks, the reserved sectors, both FATs and the root cluster:
     * zeros first. */
    uint64_t fats_bytes = 2ull * l->fat_sectors * ss;
    if (!zero_bytes(0, reserved_bytes > MKFS_CLEAR ? reserved_bytes : MKFS_CLEAR) ||
        !zero_bytes(reserved_bytes, fats_bytes + (uint64_t)l->cluster_sectors * ss))
        return false;
    if (whole_disk && !zero_bytes(device_bytes - 512, 512))
        return false;

    /* The FATs: entries 0 and 1 reserved, 2 the root directory (one cluster). */
    memset(block, 0, sizeof(block));
    mkfs_put32(block, 0x0FFFFFF8);
    mkfs_put32(block + 4, 0x0FFFFFFF);
    mkfs_put32(block + 8, 0x0FFFFFFF);
    for (int copy = 0; copy < 2; copy++)
        if (!write_at(reserved_bytes + (uint64_t)copy * l->fat_sectors * ss, block, 12))
            return false;

    char name[11];
    memset(name, ' ', sizeof(name));
    for (size_t i = 0; i < strlen(label); i++)
        name[i] = label[i] >= 'a' && label[i] <= 'z' ? (char)(label[i] - 32) : label[i];
    uint16_t date, time;
    dos_time(&date, &time);
    if (label[0]) {
        /* The label as Windows keeps it too: the root directory's first entry. */
        memset(block, 0, 32);
        memcpy(block, name, 11);
        block[11] = 0x08;
        mkfs_put16(block + 22, time), mkfs_put16(block + 24, date);
        if (!write_at(reserved_bytes + fats_bytes, block, 32))
            return false;
    }

    /* The boot sector with the BIOS parameter block. */
    uint8_t *b = block;
    memset(b, 0, sizeof(block));
    b[0] = 0xEB, b[1] = 0x58, b[2] = 0x90;          /* a jump over the BPB (x86 boot code) */
    memcpy(b + 3, "MSWIN4.1", 8);                   /* what most systems expect here */
    mkfs_put16(b + 11, ss);
    b[13] = (uint8_t)l->cluster_sectors;
    mkfs_put16(b + 14, FAT_RESERVED);
    b[16] = 2;                                      /* FATs */
    b[21] = 0xF8;                                   /* media: a fixed disk */
    mkfs_put16(b + 24, 63), mkfs_put16(b + 26, 255);          /* sectors per track, heads */
    mkfs_put32(b + 28, (uint32_t)hidden);                /* sectors before the volume */
    mkfs_put32(b + 32, (uint32_t)l->sectors);
    mkfs_put32(b + 36, l->fat_sectors);
    mkfs_put32(b + 44, 2);                               /* the root directory's cluster */
    mkfs_put16(b + 48, 1), mkfs_put16(b + 50, 6);             /* FSInfo, the boot sector's copy */
    b[64] = 0x80, b[66] = 0x29;                     /* drive, extended signature */
    uint8_t id[4];
    ritteros_random(id, sizeof(id));
    memcpy(b + 67, id, 4);
    memcpy(b + 71, label[0] ? name : "NO NAME    ", 11);
    memcpy(b + 82, "FAT32   ", 8);
    b[510] = 0x55, b[511] = 0xAA;
    if (!write_at(0, b, ss) || !write_at(6ull * ss, b, ss))
        return false;

    /* FSInfo: all clusters but the root's are free; the next free is 3. */
    memset(b, 0, sizeof(block));
    mkfs_put32(b, 0x41615252);
    mkfs_put32(b + 484, 0x61417272);
    mkfs_put32(b + 488, l->clusters - 1);
    mkfs_put32(b + 492, 3);
    mkfs_put32(b + 508, 0xAA550000);
    return write_at(ss, b, ss) && write_at(7ull * ss, b, ss);
}

/* A FAT label: up to 11 characters allowed in short names, or spaces
 * (lower case becomes upper case, as Windows does it). */
bool mkfs_fat_label_valid(const char *label)
{
    static const char other[] = "!#$%&'()-@^_`{}~ ";
    if (strlen(label) > 11)
        return false;
    for (const char *c = label; *c; c++) {
        bool ok = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9');
        for (const char *o = other; *o && !ok; o++)
            ok = *c == *o;
        if (!ok)
            return false;
    }
    return true;
}

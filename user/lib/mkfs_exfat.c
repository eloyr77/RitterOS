/*
 * A new exFAT on a device (format -t exfat, 46c), as the specification
 * and Windows lay it out: the boot region (12 sectors: the boot sector, 8
 * extended boot sectors, the OEM parameters, a reserved sector, the
 * checksum sector) and its backup, one FAT from 1 MiB on (sector 24 on
 * volumes under 64 MiB), the cluster heap on a cluster boundary after it,
 * holding the allocation bitmap, the up-case table (the full one, from
 * the Unicode Character Database at build time: scripts/exfat_upcase.c) and the
 * root directory (the label, the bitmap's and the table's entries), each
 * chained in the FAT. Clusters of 4 KiB up to 256 MiB, 32 KiB up to 32
 * GiB, 128 KiB beyond, as Windows chooses.
 */
#include <string.h>
#include "mkfs.h"
#include "ritteros.h"
#include "exfat_upcase.h"                   /* build/gen: made by the Makefile */

#define EXFAT_MAX_CLUSTERS  0xFFFFFFF5u
#define EXFAT_REGION        12              /* sectors of a boot region */

static int fd;
static uint8_t block[4096];

static bool write_at(uint64_t offset, const void *data, size_t n)
{
    return mkfs_write(fd, offset, data, n);
}

static uint32_t log2_of(uint64_t v)
{
    uint32_t n = 0;
    while (v > 1)
        v >>= 1, n++;
    return n;
}

/* The layout for bytes on a device with blocks of sector bytes; false if
 * exFAT does not fit (under 1 MiB, or too many clusters). */
bool mkfs_exfat_plan(uint64_t bytes, uint32_t sector, struct exfat_layout *l)
{
    uint64_t mib = 1024 * 1024, gib = 1024 * mib;
    uint64_t cluster = bytes <= 256 * mib ? 4096 : bytes <= 32 * gib ? 32768 : 131072;
    if (cluster < sector)
        cluster = sector;
    l->sector = sector;
    l->sector_shift = log2_of(sector);
    l->cluster = (uint32_t)cluster;
    l->cluster_shift = log2_of(cluster / sector);
    l->sectors = bytes / sector;
    if (bytes < mib)
        return false;
    uint64_t per = cluster / sector, fat_offset = bytes >= 64 * mib ? mib / sector : 24;
    if (fat_offset < 24)
        fat_offset = 24;
    /* The FAT's size: 4 bytes for each cluster, and the heap after it. */
    uint64_t fat = 1, heap, clusters;
    for (;;) {
        heap = (fat_offset + fat + per - 1) / per * per;
        if (heap >= l->sectors)
            return false;
        clusters = (l->sectors - heap) / per;
        uint64_t need = ((clusters + 2) * 4 + sector - 1) / sector;
        if (need <= fat)
            break;
        fat = need;
    }
    if (clusters > EXFAT_MAX_CLUSTERS || heap > 0xFFFFFFFFu)
        return false;
    l->fat_offset = (uint32_t)fat_offset;
    l->fat_length = (uint32_t)fat;
    l->heap_offset = (uint32_t)heap;
    l->clusters = (uint32_t)clusters;
    l->bitmap_clusters = (uint32_t)(((clusters + 7) / 8 + cluster - 1) / cluster);
    l->upcase_clusters = (uint32_t)((EXFAT_UPCASE_UNITS * 2 + cluster - 1) / cluster);
    return clusters >= l->bitmap_clusters + l->upcase_clusters + 1 + 16;
}

/* A label in UTF-16: up to 11 characters of the Basic Multilingual Plane,
 * none of those names may not hold ("*:<>?\|/ and control characters).
 * The units, or -1. */
int mkfs_exfat_label(const char *label, uint16_t *out)
{
    const unsigned char *c = (const unsigned char *)label;
    int n = 0;
    while (*c) {
        uint32_t u, more;
        if (*c < 0x80)
            u = *c, more = 0;
        else if ((*c & 0xE0) == 0xC0)
            u = *c & 0x1F, more = 1;
        else if ((*c & 0xF0) == 0xE0)
            u = *c & 0x0F, more = 2;
        else
            return -1;                      /* beyond the plane, or not UTF-8 */
        for (c++; more; more--, c++) {
            if ((*c & 0xC0) != 0x80)
                return -1;
            u = u << 6 | (*c & 0x3F);
        }
        if (u < 0x20 || (u < 0x80 && strchr("\"*/:<>?\\|", (int)u)) || (u >= 0xD800 && u < 0xE000) ||
            n == EXFAT_LABEL_MAX)
            return -1;
        out[n++] = (uint16_t)u;
    }
    return n;
}

static uint8_t exfat_region[EXFAT_REGION * 4096];
static uint8_t upcase_bytes[EXFAT_UPCASE_UNITS * 2];

bool mkfs_exfat(int device, const struct exfat_layout *l, const uint16_t *label, int label_units,
                uint64_t hidden, bool whole_disk, uint64_t device_bytes)
{
    fd = device;
    uint64_t ss = l->sector, heap = (uint64_t)l->heap_offset * ss;
    uint32_t used = l->bitmap_clusters + l->upcase_clusters + 1;
    uint32_t bitmap = 2, upcase = bitmap + l->bitmap_clusters, root = upcase + l->upcase_clusters;
    /* The old marks, both boot regions, the FAT and the three first parts of
     * the heap: zeros first. */
    uint64_t clear = heap + (uint64_t)used * l->cluster;
    if (!mkfs_zero(fd, 0, clear > MKFS_CLEAR ? clear : MKFS_CLEAR))
        return false;
    if (whole_disk && !mkfs_zero(fd, device_bytes - 512, 512))
        return false;

    /* The FAT: entries 0 and 1 reserved, then the bitmap's, the table's and
     * the root's clusters, each a chain. */
    memset(block, 0, sizeof(block));
    mkfs_put32(block, 0xFFFFFFF8);
    mkfs_put32(block + 4, 0xFFFFFFFF);
    for (uint32_t c = 2; c < 2 + used; c++)
        mkfs_put32(block + 4 * c, c + 1 == upcase || c + 1 == root || c == root ? 0xFFFFFFFF : c + 1);
    if ((2 + used) * 4 > sizeof(block) ||
        !write_at((uint64_t)l->fat_offset * ss, block, (2 + used) * 4))
        return false;

    /* The bitmap: the clusters used so far. */
    memset(block, 0, sizeof(block));
    for (uint32_t i = 0; i < used; i++)
        block[i / 8] |= (uint8_t)(1u << (i % 8));
    if (!write_at(heap, block, (used + 7) / 8))
        return false;

    /* The up-case table. */
    for (uint32_t i = 0; i < EXFAT_UPCASE_UNITS; i++)
        mkfs_put16(upcase_bytes + 2 * i, exfat_upcase[i]);
    if (!write_at(heap + (uint64_t)(upcase - 2) * l->cluster, upcase_bytes, sizeof(upcase_bytes)))
        return false;

    /* The root directory: the label, the bitmap's and the table's entries. */
    memset(block, 0, sizeof(block));
    uint8_t *e = block;
    if (label_units > 0) {
        e[0] = 0x83;
        e[1] = (uint8_t)label_units;
        for (int i = 0; i < label_units; i++)
            mkfs_put16(e + 2 + 2 * i, label[i]);
        e += 32;
    }
    e[0] = 0x81;
    mkfs_put32(e + 20, bitmap);
    mkfs_put64(e + 24, ((uint64_t)l->clusters + 7) / 8);
    e += 32;
    e[0] = 0x82;
    mkfs_put32(e + 4, EXFAT_UPCASE_CHECKSUM);
    mkfs_put32(e + 20, upcase);
    mkfs_put64(e + 24, sizeof(upcase_bytes));
    e += 32;
    if (!write_at(heap + (uint64_t)(root - 2) * l->cluster, block, (size_t)(e - block)))
        return false;

    /* The boot region: the boot sector, the extended boot sectors, then the
     * checksum of all of them (VolumeFlags and PercentInUse left out). */
    uint8_t *r = exfat_region;
    memset(r, 0, EXFAT_REGION * ss);
    r[0] = 0xEB, r[1] = 0x76, r[2] = 0x90;
    memcpy(r + 3, "EXFAT   ", 8);
    mkfs_put64(r + 64, hidden);                          /* sectors before the volume */
    mkfs_put64(r + 72, l->sectors);
    mkfs_put32(r + 80, l->fat_offset);
    mkfs_put32(r + 84, l->fat_length);
    mkfs_put32(r + 88, l->heap_offset);
    mkfs_put32(r + 92, l->clusters);
    mkfs_put32(r + 96, root);
    uint8_t id[4];
    ritteros_random(id, sizeof(id));
    memcpy(r + 100, id, 4);                         /* the volume's serial number */
    mkfs_put16(r + 104, 0x0100);                         /* revision 1.00 */
    r[108] = (uint8_t)l->sector_shift;
    r[109] = (uint8_t)l->cluster_shift;
    r[110] = 1;                                     /* one FAT */
    r[111] = 0x80;                                  /* drive */
    r[112] = (uint8_t)((uint64_t)used * 100 / l->clusters);
    memset(r + 120, 0xF4, 390);                     /* no boot code: halt */
    r[510] = 0x55, r[511] = 0xAA;
    for (int i = 1; i <= 8; i++)
        mkfs_put32(r + i * ss + ss - 4, 0xAA550000);
    uint32_t sum = 0;
    for (uint64_t i = 0; i < 11 * ss; i++)
        if (i != 106 && i != 107 && i != 112)
            sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + r[i];
    for (uint64_t i = 0; i < ss; i += 4)
        mkfs_put32(r + 11 * ss + i, sum);
    /* The backup first, the main region last: until then no exFAT is found. */
    return write_at(EXFAT_REGION * ss, r, EXFAT_REGION * ss) && write_at(0, r, EXFAT_REGION * ss);
}

#include "gpt.h"
#include "crc32.h"
#include "devpath.h"
#include "heap.h"
#include "kprintf.h"
#include "string.h"

struct gpt_header {
    char signature[8];              /* "EFI PART" */
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc;            /* CRC32 of header_size bytes, this field as 0 */
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;         /* the other copy of the header */
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t disk_guid[16];
    uint64_t entries_lba;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t entries_crc;           /* CRC32 of the whole entry array */
} __attribute__((packed));

struct gpt_entry {
    uint8_t type_guid[16];          /* all zero: unused entry */
    uint8_t unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;              /* inclusive */
    uint64_t attributes;
    uint16_t name[36];              /* UTF-16LE */
} __attribute__((packed));

static const struct {
    const char *guid;
    const char *name;
} known_types[] = {
    { "C12A7328-F81F-11D2-BA4B-00A0C93EC93B", "EFI system partition" },
    { "B5CA1B3E-6278-48F0-BBEF-87907326A8B3", "RitterOS" },
    { "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", "Microsoft basic data" },
    { "0FC63DAF-8483-4772-8E79-3D69D8477DE4", "Linux filesystem" },
    { "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F", "Linux swap" },
    { "E3C9E316-0B5C-4DB8-817D-F92DF00215AE", "Microsoft reserved" },
    { "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC", "Windows recovery" },
    { "7C3457EF-0000-11AA-AA11-00306543ECAC", "Apple APFS" },
    { "48465300-0000-11AA-AA11-00306543ECAC", "Apple HFS+" },
};

static const char *type_name(const char *guid_text)
{
    for (size_t i = 0; i < sizeof(known_types) / sizeof(known_types[0]); i++)
        if (memcmp(known_types[i].guid, guid_text, 36) == 0)
            return known_types[i].name;
    return "unknown type";
}

/* Read one header at lba and check signature and CRC. */
static bool read_header(struct block_device *dev, uint64_t lba, uint8_t *sector,
                        struct gpt_header *h)
{
    if (!dev->read(dev, lba, 1, sector))
        return false;
    memcpy(h, sector, sizeof(*h));
    if (memcmp(h->signature, "EFI PART", 8) != 0 || h->header_size < sizeof(*h) ||
        h->header_size > dev->block_size)
        return false;
    uint32_t stored = h->header_crc;
    memset(sector + 16, 0, 4);                      /* CRC is computed with its field zero */
    return crc32(sector, h->header_size) == stored;
}

bool gpt_report(struct block_device *dev)
{
    uint8_t *sector = kmalloc(dev->block_size);
    if (!sector)
        return false;
    bool ok = false;

    /* Sector 0: protective MBR, one partition of type 0xEE covering the disk. */
    if (!dev->read(dev, 0, 1, sector)) {
        kprintf("  GPT:           %s: cannot read sector 0\n", dev->name);
        goto out;
    }
    bool protective = sector[510] == 0x55 && sector[511] == 0xAA && sector[446 + 4] == 0xEE;

    struct gpt_header h;
    if (!read_header(dev, 1, sector, &h)) {
        kprintf("  GPT:           %s: no valid GPT header in sector 1\n", dev->name);
        goto out;
    }

    /* The partition entry array. */
    uint64_t array_bytes = (uint64_t)h.entry_count * h.entry_size;
    uint32_t array_blocks = (uint32_t)((array_bytes + dev->block_size - 1) / dev->block_size);
    uint8_t *entries = kmalloc((size_t)array_blocks * dev->block_size);
    if (!entries || h.entry_size < sizeof(struct gpt_entry) ||
        !dev->read(dev, h.entries_lba, array_blocks, entries)) {
        kprintf("  GPT:           %s: cannot read the partition entries\n", dev->name);
        kfree(entries);
        goto out;
    }
    bool entries_ok = crc32(entries, array_bytes) == h.entries_crc;

    /* The backup copy at the end of the disk. */
    struct gpt_header backup;
    bool backup_ok = read_header(dev, h.alternate_lba, sector, &backup) &&
                     backup.alternate_lba == 1 && backup.entries_crc == h.entries_crc;

    char guid[37];
    guid_to_text(h.disk_guid, guid);
    kprintf("  GPT:           %s: %s MBR, disk GUID %s\n", dev->name,
            protective ? "protective" : "no protective", guid);
    kprintf("                 header CRC ok, entries %u x %u bytes at LBA %lu, entries CRC %s, "
            "backup at LBA %lu %s\n", h.entry_count, h.entry_size, h.entries_lba,
            entries_ok ? "ok" : "BAD", h.alternate_lba, backup_ok ? "ok" : "BAD");

    for (uint32_t i = 0; i < h.entry_count; i++) {
        const struct gpt_entry *e = (const struct gpt_entry *)(entries + (size_t)i * h.entry_size);
        static const uint8_t unused[16];
        if (memcmp(e->type_guid, unused, 16) == 0)
            continue;
        char name[37], type[37];
        int n = 0;
        for (; n < 36 && e->name[n]; n++)
            name[n] = e->name[n] < 0x80 ? (char)e->name[n] : '?';
        name[n] = 0;
        guid_to_text(e->type_guid, type);
        guid_to_text(e->unique_guid, guid);
        uint64_t mib = (e->last_lba - e->first_lba + 1) * dev->block_size / (1024 * 1024);
        kprintf("    #%u  LBA %lu-%lu  %5lu MiB  '%s'  type %s (%s)\n", i + 1, e->first_lba,
                e->last_lba, mib, name, type, type_name(type));
        kprintf("         partition GUID %s\n", guid);
    }
    kfree(entries);
    ok = entries_ok && backup_ok;

out:
    kfree(sector);
    return ok;
}

/* --- partitions as block devices ------------------------------------------ */

struct partition {
    struct block_device dev;
    struct block_device *disk;
    uint64_t first_lba;
};

static bool in_range(struct block_device *dev, uint64_t lba, uint32_t count)
{
    return lba + count <= dev->block_count && lba + count >= lba;
}

static bool part_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buffer)
{
    struct partition *p = dev->driver_data;
    return in_range(dev, lba, count) && p->disk->read(p->disk, p->first_lba + lba, count, buffer);
}

static bool part_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buffer)
{
    struct partition *p = dev->driver_data;
    return in_range(dev, lba, count) && p->disk->write &&
           p->disk->write(p->disk, p->first_lba + lba, count, buffer);
}

static bool part_flush(struct block_device *dev)
{
    struct partition *p = dev->driver_data;
    return p->disk->flush ? p->disk->flush(p->disk) : true;
}

static bool part_discard(struct block_device *dev, uint64_t lba, uint64_t count)
{
    struct partition *p = dev->driver_data;
    return lba + count <= dev->block_count && lba + count >= lba &&
           p->disk->discard(p->disk, p->first_lba + lba, count);
}

/* Sector 0 holds a file system's boot sector (FAT, NTFS, exFAT on the
 * whole disk), not an MBR: its jump instruction and its name. */
static bool boot_sector(const uint8_t *s)
{
    return (s[0] == 0xEB || s[0] == 0xE9) &&
           (memcmp(s + 3, "NTFS    ", 8) == 0 || memcmp(s + 3, "EXFAT   ", 8) == 0 ||
            memcmp(s + 54, "FAT", 3) == 0 || memcmp(s + 82, "FAT32   ", 8) == 0);
}

static int read_mbr(struct block_device *disk, const uint8_t *sector, struct partition_info *out,
                    int max)
{
    if (sector[510] != 0x55 || sector[511] != 0xAA || boot_sector(sector))
        return -1;
    int n = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = sector + 446 + 16 * i;
        uint32_t first = (uint32_t)e[8] | (uint32_t)e[9] << 8 | (uint32_t)e[10] << 16 |
                         (uint32_t)e[11] << 24;
        uint32_t count = (uint32_t)e[12] | (uint32_t)e[13] << 8 | (uint32_t)e[14] << 16 |
                         (uint32_t)e[15] << 24;
        if (e[0] != 0 && e[0] != 0x80)
            return -1;                              /* not a partition table after all */
        if (e[4] == 0 || count == 0)
            continue;
        if (e[4] == 0xEE)
            return -1;                              /* a GPT's protective MBR */
        if (first == 0 || (uint64_t)first + count > disk->block_count)
            return -1;
        if (n == max)
            break;
        struct partition_info *p = &out[n++];
        memset(p, 0, sizeof(*p));
        p->number = (uint32_t)i + 1;
        p->first_lba = first;
        p->block_count = count;
        p->mbr_type = e[4];
    }
    return n;
}

int partition_table_read(struct block_device *disk, struct partition_info *out, int max,
                         struct partition_table *table)
{
    memset(table, 0, sizeof(*table));
    uint8_t *sector = kmalloc(disk->block_size);
    uint8_t *entries = 0;
    struct gpt_header h;
    int n = 0;
    if (!sector)
        return 0;
    /* The GPT at sector 1, or else its copy in the disk's last sector (a
     * change interrupted halfway, or a damaged first copy). */
    for (int copy = 0; copy < 2 && table->kind == PARTITION_TABLE_NONE; copy++) {
        if (!read_header(disk, copy == 0 ? 1 : disk->block_count - 1, sector, &h) ||
            h.entry_size < sizeof(struct gpt_entry) || h.entry_count > 1024)
            continue;
        uint64_t array_bytes = (uint64_t)h.entry_count * h.entry_size;
        uint32_t array_blocks = (uint32_t)((array_bytes + disk->block_size - 1) / disk->block_size);
        kfree(entries);
        entries = kmalloc((size_t)array_blocks * disk->block_size);
        if (entries && disk->read(disk, h.entries_lba, array_blocks, entries) &&
            crc32(entries, array_bytes) == h.entries_crc) {
            table->kind = PARTITION_TABLE_GPT;
            table->first_usable = h.first_usable_lba;
            table->last_usable = h.last_usable_lba;
            static const uint8_t unused[16];
            for (uint32_t i = 0; i < h.entry_count && n < max; i++) {
                const struct gpt_entry *e =
                    (const struct gpt_entry *)(entries + (size_t)i * h.entry_size);
                if (memcmp(e->type_guid, unused, 16) == 0 || e->last_lba < e->first_lba ||
                    e->last_lba > h.last_usable_lba)
                    continue;
                struct partition_info *p = &out[n++];
                memset(p, 0, sizeof(*p));
                p->number = i + 1;
                p->first_lba = e->first_lba;
                p->block_count = e->last_lba - e->first_lba + 1;
                memcpy(p->type_guid, e->type_guid, 16);
                memcpy(p->unique_guid, e->unique_guid, 16);
                int k = 0;
                for (; k < 36 && e->name[k]; k++)
                    p->name[k] = e->name[k] >= 0x20 && e->name[k] < 0x7F ? (char)e->name[k] : '?';
                p->name[k] = 0;
            }
        }
    }
    if (table->kind == PARTITION_TABLE_NONE && disk->read(disk, 0, 1, sector)) {
        int m = read_mbr(disk, sector, out, max);
        if (m >= 0) {
            table->kind = PARTITION_TABLE_MBR;
            table->first_usable = 1;
            table->last_usable = disk->block_count - 1 < 0xFFFFFFFFu ? disk->block_count - 1
                                                                      : 0xFFFFFFFFu;
            n = m;
        }
    }
    kfree(entries);
    kfree(sector);
    return n;
}

struct block_device *partition_open(struct block_device *disk, const struct partition_info *info)
{
    struct partition *p = kzalloc(sizeof(*p));
    if (!p)
        return 0;
    p->disk = disk;
    p->first_lba = info->first_lba;
    /* As Linux names them: a 'p' only after a digit (nvme0n1p2, sata0p2;
     * a disk name ending in a letter would get none). */
    size_t len = strlen(disk->name);
    bool digit = len && disk->name[len - 1] >= '0' && disk->name[len - 1] <= '9';
    ksnprintf(p->dev.name, sizeof(p->dev.name), "%s%s%u", disk->name, digit ? "p" : "",
              info->number);
    p->dev.block_size = disk->block_size;
    p->dev.block_count = info->block_count;
    p->dev.read = part_read;
    p->dev.write = part_write;
    p->dev.flush = part_flush;
    p->dev.discard = disk->discard ? part_discard : 0;
    p->dev.driver_data = p;
    p->dev.kind = "partition";
    return &p->dev;
}

uint64_t partition_first_lba(struct block_device *dev)
{
    return dev->read == part_read ? ((struct partition *)dev->driver_data)->first_lba : 0;
}

struct block_device *partition_disk(struct block_device *dev)
{
    if (dev->read != part_read)
        return dev;
    return ((struct partition *)dev->driver_data)->disk;
}

static const struct {
    uint8_t type;
    const char *name;
} mbr_types[] = {
    { 0x01, "FAT12" }, { 0x04, "FAT16" }, { 0x06, "FAT16" }, { 0x07, "NTFS or exFAT" },
    { 0x0B, "FAT32" }, { 0x0C, "FAT32 (LBA)" }, { 0x0E, "FAT16 (LBA)" }, { 0x05, "extended" },
    { 0x0F, "extended (LBA)" }, { 0x82, "Linux swap" }, { 0x83, "Linux" },
    { 0xEF, "EFI system partition" },
};

const char *partition_type_name(const struct partition_info *info)
{
    if (info->mbr_type) {
        for (size_t i = 0; i < sizeof(mbr_types) / sizeof(mbr_types[0]); i++)
            if (mbr_types[i].type == info->mbr_type)
                return mbr_types[i].name;
        return 0;
    }
    char type[37];
    guid_to_text(info->type_guid, type);
    const char *name = type_name(type);
    return strcmp(name, "unknown type") == 0 ? 0 : name;
}

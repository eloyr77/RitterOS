/*
 * The list of devices: disks and their partitions, in the order found (a
 * disk, then its partitions). Drivers' block devices are never freed (an
 * open /dev file may still point to one after an unplug: its reads then
 * fail); the list's entries are.
 *
 * What a partition (or a disk without a table) holds is found from its
 * first 12 KiB: RitterFS by its superblocks in blocks 1 and 2 (the valid
 * one with the higher generation), FAT, exFAT and NTFS by their boot
 * sector, ext by its superblock at byte 1024, APFS by its container
 * superblock. Nothing is written.
 */
#include "devices.h"
#include "ahci.h"
#include "crc32.h"
#include "devpath.h"
#include "gpt.h"
#include "heap.h"
#include "kprintf.h"
#include "mounts.h"
#include "rfs_format.h"
#include "ritterfs.h"
#include "vfs.h"
#include "string.h"
#include "sync.h"
#include "syscalls.h"

#define MAX_DEVICES     64
#define MAX_PARTITIONS  16          /* per disk */
#define PROBE_BYTES     (3 * RFS_BLOCK_SIZE)

struct device {
    struct block_device *bd;
    struct block_device *disk;      /* a partition's disk; 0 for a disk */
    struct partition_info part;     /* a partition's entry */
    struct partition_table table;   /* a disk's */
    int partitions;
    char filesystem[16];
    char label[65];
    uint64_t generation;
    bool empty;
    char mounted_at[64];
    struct vfs *fs;                 /* the mounted file system, or 0 */
    bool gone;                      /* no longer in the table: listed while mounted */
};

static struct device *list[MAX_DEVICES];
static int count;
static struct mutex lock = MUTEX_INIT("devices");

/* Copy a label of n bytes: printable ASCII, trailing spaces dropped. */
static void copy_label(char *out, const uint8_t *in, size_t n)
{
    size_t len = 0;
    for (size_t i = 0; i < n && in[i]; i++)
        out[len++] = in[i] >= 0x20 && in[i] < 0x7F ? (char)in[i] : '?';
    while (len > 0 && out[len - 1] == ' ')
        len--;
    out[len] = 0;
}

static void detect(struct device *d)
{
    struct block_device *bd = d->bd;
    if (bd->block_size > PROBE_BYTES || PROBE_BYTES % bd->block_size)
        return;
    uint64_t blocks = PROBE_BYTES / bd->block_size;
    if (blocks > bd->block_count)
        blocks = bd->block_count;
    uint8_t *b = kmalloc(PROBE_BYTES);
    if (!b)
        return;
    if (!bd->read(bd, 0, (uint32_t)blocks, b)) {
        kfree(b);
        return;
    }
    size_t have = (size_t)blocks * bd->block_size;
    d->empty = true;
    for (size_t i = 0; i < have && d->empty; i++)
        d->empty = b[i] == 0;

    uint64_t capacity = bd->block_count * bd->block_size / RFS_BLOCK_SIZE;
    for (int slot = RFS_SLOT_A; slot <= RFS_SLOT_B; slot++) {
        if (have < (size_t)(slot + 1) * RFS_BLOCK_SIZE)
            break;
        const struct rfs_superblock *sb = (const void *)(b + slot * RFS_BLOCK_SIZE);
        if (memcmp(sb->magic, "RITTERFS", 8) != 0 || sb->version < 1 ||
            sb->version > RFS_VERSION || sb->block_size != RFS_BLOCK_SIZE ||
            sb->block_count > capacity ||
            crc32c(sb, offsetof(struct rfs_superblock, checksum)) != sb->checksum)
            continue;
        if (!d->filesystem[0] || sb->generation > d->generation) {
            ksnprintf(d->filesystem, sizeof(d->filesystem), "RitterFS");
            copy_label(d->label, (const uint8_t *)sb->label, sizeof(sb->label));
            d->generation = sb->generation;
        }
    }
    bool boot = have >= 512 && b[510] == 0x55 && b[511] == 0xAA;
    if (d->filesystem[0]) {
        /* RitterFS */
    } else if (boot && memcmp(b + 82, "FAT32   ", 8) == 0) {
        ksnprintf(d->filesystem, sizeof(d->filesystem), "FAT32");
        copy_label(d->label, b + 71, 11);
    } else if (boot && memcmp(b + 54, "FAT1", 4) == 0) {
        ksnprintf(d->filesystem, sizeof(d->filesystem), "%.5s", (const char *)b + 54);
        copy_label(d->label, b + 43, 11);
    } else if (boot && memcmp(b + 3, "EXFAT   ", 8) == 0) {
        ksnprintf(d->filesystem, sizeof(d->filesystem), "exFAT");
    } else if (boot && memcmp(b + 3, "NTFS    ", 8) == 0) {
        ksnprintf(d->filesystem, sizeof(d->filesystem), "NTFS");
    } else if (have >= 2048 && b[1080] == 0x53 && b[1081] == 0xEF) {
        ksnprintf(d->filesystem, sizeof(d->filesystem), "ext");
        copy_label(d->label, b + 1144, 16);
    } else if (have >= 36 && memcmp(b + 32, "NXSB", 4) == 0) {
        ksnprintf(d->filesystem, sizeof(d->filesystem), "APFS");
    }
    if (strcmp(d->label, "NO NAME") == 0)
        d->label[0] = 0;                            /* FAT's "no label" */
    kfree(b);
}

/* "FAT32 'RITTEROS'", "RitterFS 'RitterOS', generation 5", "empty", ... */
static void describe(const struct device *d, char *out, size_t size)
{
    if (!d->filesystem[0]) {
        ksnprintf(out, size, "%s", d->empty ? "empty" : "no file system found");
        return;
    }
    size_t n = (size_t)ksnprintf(out, size, "%s", d->filesystem);
    if (d->label[0] && n < size)
        n += (size_t)ksnprintf(out + n, size - n, " '%s'", d->label);
    if (strcmp(d->filesystem, "RitterFS") == 0 && n < size)
        ksnprintf(out + n, size - n, ", generation %lu", d->generation);
}

static void log_partition(const struct device *d, int table)
{
    char label[24], what[128], type[48];
    ksnprintf(label, sizeof(label), "%s:", d->bd->name);
    describe(d, what, sizeof(what));
    const char *name = partition_type_name(&d->part);
    if (table == PARTITION_TABLE_MBR)
        ksnprintf(type, sizeof(type), "type 0x%02X (%s)", d->part.mbr_type, name ? name : "?");
    else
        ksnprintf(type, sizeof(type), "%s", name ? name : "unknown type");
    kprintf("  %-15s%s #%u, %lu MiB, %s: %s\n", label,
            table == PARTITION_TABLE_MBR ? "MBR" : "GPT", d->part.number,
            d->bd->block_count * d->bd->block_size >> 20, type, what);
}

/* Append under the lock; false if the list is full. */
static bool append(struct device *d)
{
    if (count == MAX_DEVICES) {
        kfree(d);
        return false;
    }
    list[count++] = d;
    return true;
}

void devices_add_disk(struct block_device *disk)
{
    struct device *d = kzalloc(sizeof(*d));
    struct partition_info *parts = kmalloc(MAX_PARTITIONS * sizeof(*parts));
    if (!d || !parts) {
        kfree(d);
        kfree(parts);
        return;
    }
    d->bd = disk;
    int n = partition_table_read(disk, parts, MAX_PARTITIONS, &d->table);
    d->partitions = n;
    if (d->table.kind == PARTITION_TABLE_NONE)
        detect(d);
    mutex_lock(&lock);
    bool ok = append(d);
    mutex_unlock(&lock);
    if (ok && d->table.kind == PARTITION_TABLE_NONE) {
        char label[24], what[128];
        ksnprintf(label, sizeof(label), "%s:", disk->name);
        describe(d, what, sizeof(what));
        kprintf("  %-15sno partition table: %s\n", label, what);
    }
    for (int i = 0; ok && i < n; i++) {
        struct device *p = kzalloc(sizeof(*p));
        if (!p || !(p->bd = partition_open(disk, &parts[i]))) {
            kfree(p);
            break;
        }
        p->disk = disk;
        p->part = parts[i];
        detect(p);
        mutex_lock(&lock);
        ok = append(p);
        mutex_unlock(&lock);
        if (ok)
            log_partition(p, d->table.kind);
    }
    kfree(parts);
}

/* The same partition: unchanged in the table (its name may differ). */
static bool same_partition(const struct partition_info *a, const struct partition_info *b)
{
    return a->number == b->number && a->first_lba == b->first_lba &&
           a->block_count == b->block_count && a->mbr_type == b->mbr_type &&
           memcmp(a->type_guid, b->type_guid, 16) == 0 &&
           memcmp(a->unique_guid, b->unique_guid, 16) == 0;
}

/* (68j) A mounted partition grown in place: the same entry, only its end
 * later (disks grow). It keeps its device and mount; its block count is
 * raised (only ever up, so a reader on another core sees either count
 * whole and in range), and the file system takes the room (SYS_GROW). */
static bool grown_partition(const struct device *d, const struct partition_info *b)
{
    struct partition_info longer = d->part;
    longer.block_count = b->block_count;
    return d->mounted_at[0] && b->block_count > d->part.block_count && same_partition(&longer, b);
}

/* A probe's results (what the device holds) into an entry. */
static void take_content(struct device *to, const struct device *from)
{
    memcpy(to->filesystem, from->filesystem, sizeof(to->filesystem));
    memcpy(to->label, from->label, sizeof(to->label));
    to->generation = from->generation;
    to->empty = from->empty;
}

/* What can be mounted: RitterFS, FAT and exFAT. */
static bool mountable(const struct device *d)
{
    return strcmp(d->filesystem, "RitterFS") == 0 || strncmp(d->filesystem, "FAT", 3) == 0 ||
           strcmp(d->filesystem, "exFAT") == 0;
}

/* What is mounted by itself: not an EFI system partition (as other systems
 * do, those are left alone: the system disk's own is /boot; others can be
 * mounted by hand). */
static bool mounted_by_itself(const struct device *d)
{
    char type[37];
    guid_to_text(d->part.type_guid, type);
    return mountable(d) && d->part.mbr_type != 0xEF && memcmp(type, GPT_TYPE_EFI, 36) != 0;
}

/* Whether dev's entry holds a file system that can be mounted and is not. */
static bool unmounted_volume(struct block_device *dev)
{
    bool found = false;
    mutex_lock(&lock);
    for (int i = 0; i < count && !found; i++)
        found = list[i]->bd == dev && !list[i]->mounted_at[0] && !list[i]->gone &&
                mountable(list[i]);
    mutex_unlock(&lock);
    return found;
}

void devices_rescan(struct block_device *disk, struct block_device *written)
{
    struct partition_info *parts = kmalloc(MAX_PARTITIONS * sizeof(*parts));
    struct device *fresh[MAX_PARTITIONS] = {0};     /* new entries for new partitions */
    struct device *probed[MAX_PARTITIONS] = {0};    /* unchanged ones probed again */
    struct block_device *known_bd[MAX_PARTITIONS] = {0};
    struct device *whole = kzalloc(sizeof(*whole)); /* the disk's own content */
    if (!parts || !whole) {
        kfree(parts);
        kfree(whole);
        return;
    }
    whole->bd = disk;
    int n = partition_table_read(disk, parts, MAX_PARTITIONS, &whole->table);
    if (whole->table.kind == PARTITION_TABLE_NONE)
        detect(whole);

    /* Which partitions are new: their entries are made (and probed) without
     * the lock. */
    bool known[MAX_PARTITIONS] = {0};
    mutex_lock(&lock);
    for (int k = 0; k < n; k++) {
        for (int i = 0; i < count && !known[k]; i++) {
            known[k] = list[i]->disk == disk && (same_partition(&list[i]->part, &parts[k]) ||
                                                 grown_partition(list[i], &parts[k]));
            if (known[k] && !list[i]->mounted_at[0])
                known_bd[k] = list[i]->bd;          /* never freed: safe without the lock */
        }
    }
    mutex_unlock(&lock);
    for (int k = 0; k < n; k++) {
        /* What an unchanged, unmounted partition holds may have changed
         * (format writes a new file system). */
        if (known_bd[k] && (probed[k] = kzalloc(sizeof(struct device)))) {
            probed[k]->bd = known_bd[k];
            detect(probed[k]);
        }
        if (known[k] || !(fresh[k] = kzalloc(sizeof(struct device))))
            continue;
        if (!(fresh[k]->bd = partition_open(disk, &parts[k]))) {
            kfree(fresh[k]);
            fresh[k] = 0;
            continue;
        }
        fresh[k]->disk = disk;
        fresh[k]->part = parts[k];
        detect(fresh[k]);
    }

    /* The list again: the disk's partitions in the table's order, those
     * unchanged with their entries (mounts, names), and a mounted one that
     * is gone from the table kept until it is unmounted. */
    struct device *old[MAX_PARTITIONS];
    char kept_mounted[MAX_PARTITIONS][128];
    int olds = 0, mounted = 0, grown = 0;
    mutex_lock(&lock);
    int at = -1, kept = 0;
    for (int i = 0; i < count; i++) {
        if (list[i]->disk == disk && olds < MAX_PARTITIONS)
            old[olds++] = list[i];
        else if (list[i]->disk != disk)
            list[kept++] = list[i];
        if (list[i]->bd == disk)
            at = kept - 1;
    }
    count = kept;
    struct device *mine[2 * MAX_PARTITIONS];
    int m = 0;
    for (int k = 0; k < n; k++) {
        struct device *d = fresh[k];
        for (int j = 0; j < olds && !d; j++) {
            if (old[j] && same_partition(&old[j]->part, &parts[k])) {
                d = old[j];
                old[j] = 0;
                d->gone = false;
                memcpy(d->part.name, parts[k].name, sizeof(d->part.name));
                if (probed[k] && probed[k]->bd == d->bd && !d->mounted_at[0])
                    take_content(d, probed[k]);
            } else if (old[j] && grown_partition(old[j], &parts[k]) &&
                       MAX_PARTITIONS - grown > mounted) {
                d = old[j];
                old[j] = 0;
                d->gone = false;
                d->part = parts[k];
                d->bd->block_count = parts[k].block_count;
                ksnprintf(kept_mounted[MAX_PARTITIONS - ++grown], sizeof(kept_mounted[0]),
                          "%s: grown to %lu blocks (%lu MiB), still mounted at %s", d->bd->name,
                          d->bd->block_count, d->bd->block_count * d->bd->block_size >> 20,
                          d->mounted_at);
            }
        }
        if (d)
            mine[m++] = d;
    }
    for (int j = 0; j < olds; j++) {
        if (old[j] && old[j]->mounted_at[0]) {
            old[j]->gone = true;
            ksnprintf(kept_mounted[mounted++], sizeof(kept_mounted[0]),
                      "%s: no longer in the partition table, still mounted at %s",
                      old[j]->bd->name, old[j]->mounted_at);
            mine[m++] = old[j];
        } else
            kfree(old[j]);
    }
    if (at < 0 || count + m > MAX_DEVICES) {
        /* Unplugged meanwhile (or the list is full): only what was there. */
        for (int i = 0; i < m; i++)
            if (at >= 0 && count < MAX_DEVICES)
                list[count++] = mine[i];
            else
                kfree(mine[i]);
        for (int k = 0; k < n; k++)
            fresh[k] = 0;
        mounted = grown = 0;
    } else {
        memmove(&list[at + 1 + m], &list[at + 1], (size_t)(count - at - 1) * sizeof(list[0]));
        memcpy(&list[at + 1], mine, (size_t)m * sizeof(list[0]));
        count += m;
        struct device *d = list[at];
        d->table = whole->table;
        d->partitions = n;
        if (!d->mounted_at[0])
            take_content(d, whole);
    }
    mutex_unlock(&lock);
    kfree(whole);
    kfree(parts);
    for (int k = 0; k < n; k++)
        kfree(probed[k]);

    for (int i = 0; i < mounted; i++)
        kprintf("  %s\n", kept_mounted[i]);
    for (int i = 1; i <= grown; i++)
        kprintf("  %s\n", kept_mounted[MAX_PARTITIONS - i]);
    /* New partitions with a file system are mounted, as when a stick is
     * plugged in, and so is one just written to the device (format). */
    for (int k = 0; k < n; k++)
        if (fresh[k] && mounted_by_itself(fresh[k]) && fresh[k]->bd != written)
            mounts_auto(fresh[k]->bd);
    if (written && unmounted_volume(written))
        mounts_auto(written);
}

void devices_mount_volumes(struct block_device *disk)
{
    struct block_device *found[MAX_PARTITIONS + 1];
    int n = 0;
    mutex_lock(&lock);
    for (int i = 0; i < count && n <= MAX_PARTITIONS; i++) {
        const struct device *d = list[i];
        if ((d->bd == disk || d->disk == disk) && !d->mounted_at[0] && !d->gone &&
            mounted_by_itself(d))
            found[n++] = d->bd;
    }
    mutex_unlock(&lock);
    for (int i = 0; i < n; i++)
        mounts_auto(found[i]);
}

bool devices_busy(struct block_device *dev, uint64_t lba, uint64_t blocks)
{
    struct block_device *disk = partition_disk(dev);
    if (disk != dev)
        lba += partition_first_lba(dev);
    bool busy = false;
    mutex_lock(&lock);
    for (int i = 0; i < count && !busy; i++) {
        const struct device *d = list[i];
        if (!d->mounted_at[0] || (d->bd != disk && d->disk != disk))
            continue;
        uint64_t first = d->disk ? d->part.first_lba : 0;
        busy = lba < first + d->bd->block_count && first < lba + blocks;
    }
    mutex_unlock(&lock);
    return busy;
}

void devices_remove_disk(struct block_device *disk)
{
    mutex_lock(&lock);
    int kept = 0;
    for (int i = 0; i < count; i++) {
        if (list[i]->bd == disk || list[i]->disk == disk)
            kfree(list[i]);
        else
            list[kept++] = list[i];
    }
    count = kept;
    mutex_unlock(&lock);
}

struct block_device *devices_find_partition(struct block_device *disk, const uint8_t *guid,
                                            const char *type)
{
    struct block_device *found = 0;
    mutex_lock(&lock);
    for (int i = 0; i < count && !found; i++) {
        const struct device *d = list[i];
        if (d->disk != disk)
            continue;
        char text[37];
        guid_to_text(d->part.type_guid, text);
        if (guid ? memcmp(d->part.unique_guid, guid, 16) == 0 : memcmp(text, type, 36) == 0)
            found = d->bd;
    }
    mutex_unlock(&lock);
    return found;
}

struct block_device *devices_by_name(const char *name)
{
    struct block_device *found = 0;
    mutex_lock(&lock);
    for (int i = 0; i < count && !found; i++)
        if (strcmp(list[i]->bd->name, name) == 0)
            found = list[i]->bd;
    mutex_unlock(&lock);
    return found;
}

int64_t devices_grow(const char *name)
{
    struct vfs *fs = 0;
    bool found = false;
    mutex_lock(&lock);
    for (int i = 0; i < count && !found; i++)
        if (strcmp(list[i]->bd->name, name) == 0)
            found = true, fs = list[i]->fs;
    mutex_unlock(&lock);
    /* (A file system's memory is never freed: an unmount meanwhile leaves
     * it whole, as mounts_find's callers rely on too.) */
    struct ritterfs *rfs = vfs_ritterfs(fs);
    if (!found || !rfs)
        return found ? -ERR_INVAL : -ERR_NOENT;
    uint64_t blocks;
    int r = ritterfs_grow(rfs, &blocks);
    return r == RFS_OK ? (int64_t)blocks : r == RFS_ERR_NO_SPACE ? -ERR_NOSPC :
           r == RFS_ERR_READ_ONLY ? -ERR_ROFS : -ERR_IO;
}

bool devices_info(int index, struct sys_device *out)
{
    memset(out, 0, sizeof(*out));
    mutex_lock(&lock);
    bool found = index >= 0 && index < count;
    if (found) {
        const struct device *d = list[index];
        ksnprintf(out->name, sizeof(out->name), "%s", d->bd->name);
        ksnprintf(out->kind, sizeof(out->kind), "%s", d->bd->kind ? d->bd->kind : "");
        out->block_size = d->bd->block_size;
        out->block_count = d->bd->block_count;
        if (d->disk) {
            const char *type = partition_type_name(&d->part);
            out->flags |= DEVICE_PARTITION;
            ksnprintf(out->disk, sizeof(out->disk), "%s", d->disk->name);
            out->number = d->part.number;
            out->first_block = d->part.first_lba;
            memcpy(out->type_guid, d->part.type_guid, 16);
            out->mbr_type = d->part.mbr_type;
            ksnprintf(out->type_name, sizeof(out->type_name), "%s", type ? type : "");
            ksnprintf(out->part_name, sizeof(out->part_name), "%s", d->part.name);
        } else {
            ksnprintf(out->model, sizeof(out->model), "%s", d->bd->model);
            out->table = (uint32_t)d->table.kind;
            out->partitions = (uint32_t)d->partitions;
            out->first_usable = d->table.first_usable;
            out->last_usable = d->table.last_usable;
        }
        if (d->bd->system || strcmp(d->mounted_at, "/") == 0)
            out->flags |= DEVICE_SYSTEM;
        if (d->empty)
            out->flags |= DEVICE_EMPTY;
        ksnprintf(out->filesystem, sizeof(out->filesystem), "%s", d->filesystem);
        ksnprintf(out->label, sizeof(out->label), "%s", d->label);
        out->generation = d->generation;
        if (d->fs) {
            vfs_label(d->fs, out->label, sizeof(out->label));
            out->generation = vfs_generation(d->fs);
            if (vfs_read_only(d->fs))
                out->flags |= DEVICE_READ_ONLY;
        }
        ksnprintf(out->mounted_at, sizeof(out->mounted_at), "%s", d->mounted_at);
    }
    mutex_unlock(&lock);
    return found;
}

void devices_set_mounted(struct block_device *dev, const char *path, struct vfs *fs)
{
    mutex_lock(&lock);
    for (int i = 0; i < count; i++) {
        if (list[i]->bd != dev)
            continue;
        if (list[i]->gone && !path[0]) {
            /* Gone from its disk's table: unmounted, it is no longer listed. */
            kfree(list[i]);
            memmove(&list[i], &list[i + 1], (size_t)(count - i - 1) * sizeof(list[0]));
            count--;
            break;
        }
        ksnprintf(list[i]->mounted_at, sizeof(list[i]->mounted_at), "%s", path);
        list[i]->fs = fs;
    }
    mutex_unlock(&lock);
}

/* --- the report (REPORT_DISKS) ---------------------------------------------- */

/* "958.0 MiB": one decimal, rounded down. */
static void size_text(uint64_t bytes, char *out, size_t n)
{
    static const char *units[] = { "bytes", "KiB", "MiB", "GiB", "TiB" };
    int u = 0;
    uint64_t whole = bytes, tenths = 0;
    while (whole >= 1024 && u < 4) {
        tenths = (whole % 1024) * 10 / 1024;
        whole /= 1024;
        u++;
    }
    if (u == 0)
        ksnprintf(out, n, "%lu bytes", bytes);
    else
        ksnprintf(out, n, "%lu.%lu %s", whole, tenths, units[u]);
}

/* "FAT32 'RITTEROS'", "RitterFS 'RitterOS', generation 5, mounted at /", ... */
static void content(const struct sys_device *d, char *out, size_t n)
{
    size_t used;
    if (!d->filesystem[0])
        used = (size_t)ksnprintf(out, n, "%s", d->flags & DEVICE_EMPTY ? "empty"
                                                                        : "no file system found");
    else if (d->label[0])
        used = (size_t)ksnprintf(out, n, "%s '%s'", d->filesystem, d->label);
    else
        used = (size_t)ksnprintf(out, n, "%s", d->filesystem);
    if (strcmp(d->filesystem, "RitterFS") == 0 && used < n)
        used += (size_t)ksnprintf(out + used, n - used, ", generation %lu", d->generation);
    if (d->mounted_at[0] && used < n)
        ksnprintf(out + used, n - used, ", mounted at %s", d->mounted_at);
}

void devices_print(void)
{
    struct sys_device *d = kmalloc(sizeof(*d));
    if (!d)
        return;
    int shown = 0;
    for (int i = 0; devices_info(i, d); i++, shown++) {
        char size[24], what[200];
        size_text(d->block_count * d->block_size, size, sizeof(size));
        content(d, what, sizeof(what));
        if (!(d->flags & DEVICE_PARTITION)) {
            const char *system = d->flags & DEVICE_SYSTEM ? "; the system disk" : "";
            if (d->table == DEVICE_TABLE_NONE)
                kprintf("%-14s%10s  %s '%s', no partition table: %s%s\n", d->name, size,
                        d->kind, d->model, what, system);
            else
                kprintf("%-14s%10s  %s '%s', %s, %u partition%s%s\n", d->name, size, d->kind,
                        d->model, d->table == DEVICE_TABLE_GPT ? "GPT" : "MBR", d->partitions,
                        d->partitions == 1 ? "" : "s", system);
            continue;
        }
        char type[64], name[48] = "";
        if (d->mbr_type)
            ksnprintf(type, sizeof(type), "type 0x%02X (%s)", d->mbr_type,
                      d->type_name[0] ? d->type_name : "?");
        else if (d->type_name[0])
            ksnprintf(type, sizeof(type), "%s", d->type_name);
        else
            guid_to_text(d->type_guid, type);
        if (d->part_name[0] && strcmp(d->part_name, d->type_name) != 0)
            ksnprintf(name, sizeof(name), " '%s'", d->part_name);
        kprintf("  %-12s%10s  #%u %s%s: %s\n", d->name, size, d->number, type, name, what);
    }
    if (shown == 0)
        kprintf("no disks\n");
    kfree(d);
    ahci_sleep_print();                 /* (37g-6) */
}

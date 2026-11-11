/*
 * disks: the disks and their partitions, with what each holds, and
 * changes to a disk's GPT partition table.
 *
 *   disks                      one line per disk (its size, kind and name,
 *                              its partition table) and one per partition
 *                              below it (size, number and type, file system
 *                              and label, where it is mounted): the
 *                              kernel's report, as its rescue console shows it
 *   disks DISK                 one disk in detail: each partition's blocks
 *                              and the free areas
 *   disks init DISK            a new, empty GPT
 *   disks add DISK SIZE|rest [TYPE [NAME]]
 *                              a new partition: SIZE in the first free area
 *                              that fits, or all of the largest (rest)
 *   disks delete PARTITION     its entry out of the table
 *   disks rename PARTITION NAME
 *   disks grow [DISK]          (68j) the disk's last partition, a mounted
 *                              RitterFS, to the disk's end, and the file
 *                              system with it (DISK: the system disk if not
 *                              given): a disk image written to a bigger
 *                              stick; login runs it at the first start
 *
 * Anyone may look; changes are root's (they write to /dev/DISK) and are
 * written only after the disk's name is typed to confirm (except grow's,
 * which only adds the free room at the disk's end: nothing can be lost). A mounted
 * partition is never deleted (unmount it first); the kernel refuses writes
 * to its blocks anyway. Partitions start on a 1 MiB boundary. When the
 * disk is closed, the kernel reads its table again: /dev and the list
 * show the change, and a partition holding a RitterFS is mounted.
 *
 * The GPT: sector 0 a protective MBR, sector 1 the header, then 128
 * entries of 128 bytes; a copy of both at the end of the disk (the copy is
 * written first, so an interrupted change leaves one whole table). Header
 * and entries carry CRC-32 checksums.
 */
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"
#include "gpt.h"

PROGRAM_HELP("disks [DISK] | disks init|add|delete|rename|grow ...",
             "the disks and their partitions; changes to a partition table (root)");

#define MAX_DEVICES     64
#define MiB             (1024 * 1024)

static char text[32 * 1024];

static const struct {
    const char *word, *guid, *name;
} types[] = {
    { "ritteros", GPT_TYPE_RITTEROS, "RitterOS" },
    { "efi",      GPT_TYPE_EFI, "EFI system partition" },
    { "data",     "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", "Microsoft basic data" },
    { "linux",    "0FC63DAF-8483-4772-8E79-3D69D8477DE4", "Linux filesystem" },
    { "swap",     "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F", "Linux swap" },
};

static struct sys_device devices[MAX_DEVICES];
static int device_count;

static void load_devices(void)
{
    device_count = 0;
    while (device_count < MAX_DEVICES && ritteros_device(device_count, &devices[device_count]) == 1)
        device_count++;
}

static const struct sys_device *device(const char *name)
{
    if (strncmp(name, "/dev/", 5) == 0)
        name += 5;
    for (int i = 0; i < device_count; i++)
        if (strcmp(devices[i].name, name) == 0)
            return &devices[i];
    return 0;
}

__attribute__((format(printf, 1, 2))) static int fail(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    dprintf(STDERR_FILENO, "disks: ");
    vdprintf(STDERR_FILENO, format, ap);
    dprintf(STDERR_FILENO, "\n");
    va_end(ap);
    return 1;
}

/* --- showing ----------------------------------------------------------------- */

/* "958.0 MiB": one decimal, rounded down (as the kernel's list). */
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
        snprintf(out, n, "%lu bytes", bytes);
    else
        snprintf(out, n, "%lu.%lu %s", whole, tenths, units[u]);
}

/* A GUID in its text form (the GPT stores the first three groups
 * little-endian). */
static void guid_text(const uint8_t *g, char *out, size_t n)
{
    snprintf(out, n, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12],
             g[13], g[14], g[15]);
}

/* "#2 RitterOS: RitterFS 'RitterOS', generation 5, mounted at /", as the
 * kernel's list shows a partition. */
static void describe(const struct sys_device *d, char *out, size_t n)
{
    char type[64], name[48] = "", what[160];
    if (d->mbr_type)
        snprintf(type, sizeof(type), "type 0x%02X (%s)", d->mbr_type,
                 d->type_name[0] ? d->type_name : "?");
    else if (d->type_name[0])
        snprintf(type, sizeof(type), "%s", d->type_name);
    else
        guid_text(d->type_guid, type, sizeof(type));
    if (d->part_name[0] && strcmp(d->part_name, d->type_name) != 0)
        snprintf(name, sizeof(name), " '%s'", d->part_name);
    if (!d->filesystem[0])
        snprintf(what, sizeof(what), "%s", d->flags & DEVICE_EMPTY ? "empty" : "no file system found");
    else if (d->label[0])
        snprintf(what, sizeof(what), "%s '%s'", d->filesystem, d->label);
    else
        snprintf(what, sizeof(what), "%s", d->filesystem);
    size_t used = strlen(what);
    if (strcmp(d->filesystem, "RitterFS") == 0)
        used += (size_t)snprintf(what + used, sizeof(what) - used, ", generation %lu", d->generation);
    if (d->mounted_at[0])
        snprintf(what + used, sizeof(what) - used, ", mounted at %s", d->mounted_at);
    snprintf(out, n, "#%u %s%s: %s", d->number, type, name, what);
}

/* Partitions start on a 1 MiB boundary. */
static uint64_t alignment(const struct sys_device *disk)
{
    return disk->block_size < MiB ? MiB / disk->block_size : 1;
}

/* The last block partitions may use when a GPT (of the usual 128 entries)
 * fills the whole disk. */
static uint64_t fitted_last(const struct sys_device *disk)
{
    uint64_t array_blocks = (GPT_ENTRIES * GPT_ENTRY_SIZE + disk->block_size - 1) / disk->block_size;
    return disk->block_count - 2 - array_blocks;
}

/* The last block partitions may use. A GPT made for a smaller disk (a disk
 * image copied to a bigger stick: its copy at the end lies in the middle)
 * is fitted to the whole disk whenever disks writes it, so its free space
 * reaches the disk's end. */
static uint64_t last_usable(const struct sys_device *disk)
{
    bool small = disk->table == DEVICE_TABLE_GPT && fitted_last(disk) > disk->last_usable;
    return small ? fitted_last(disk) : disk->last_usable;
}

/* The free areas of disk (aligned starts, at least one alignment unit
 * long) in order: firsts and lasts; how many. */
static int free_areas(const struct sys_device *disk, uint64_t *first, uint64_t *last, int max)
{
    uint64_t align = alignment(disk), at = disk->first_usable;
    int n = 0;
    for (;;) {
        /* The next partition from at on. */
        const struct sys_device *next = 0;
        for (int i = 0; i < device_count; i++) {
            const struct sys_device *p = &devices[i];
            if (strcmp(p->disk, disk->name) != 0 || p->first_block + p->block_count <= at)
                continue;
            if (!next || p->first_block < next->first_block)
                next = p;
        }
        uint64_t end = next ? next->first_block : last_usable(disk) + 1;
        uint64_t start = (at + align - 1) / align * align;
        if (end > start && end - start >= align && n < max) {
            first[n] = start;
            last[n++] = end - 1;
        }
        if (!next)
            return n;
        at = next->first_block + next->block_count;
    }
}

/* disks DISK */
static int show(const char *name)
{
    const struct sys_device *disk = device(name);
    if (!disk)
        return fail("%s: %s", name, strerror(ENOENT));
    if (disk->flags & DEVICE_PARTITION)
        return fail("%s: a partition (of %s); show its disk", disk->name, disk->disk);
    /* The disk's line from the kernel's list. */
    if (ritteros_report(REPORT_DISKS, text, sizeof(text)) < 0)
        return 1;
    size_t n = strlen(disk->name);
    for (char *line = text; *line;) {
        char *end = strchr(line, '\n');
        if (!end)
            break;
        if (strncmp(line, disk->name, n) == 0 && line[n] == ' ')
            printf("%.*s\n", (int)(end - line), line);
        line = end + 1;
    }
    if (disk->table == DEVICE_TABLE_NONE) {
        printf("  blocks of %lu bytes; no partition table (disks init makes one)\n",
               disk->block_size);
        return 0;
    }
    printf("  blocks of %lu bytes; partitions may use %lu-%lu\n", disk->block_size,
           disk->first_usable, last_usable(disk));
    if (last_usable(disk) != disk->last_usable) {
        char made[24], whole[24];
        size_text((disk->last_usable + 1) * disk->block_size, made, sizeof(made));
        size_text(disk->block_count * disk->block_size, whole, sizeof(whole));
        printf("  the table was made for a smaller disk (it covers %s of %s);\n"
               "  the next change (disks add) fits it to the whole disk\n", made, whole);
    }
    uint64_t first[MAX_DEVICES], last[MAX_DEVICES];
    int areas = free_areas(disk, first, last, MAX_DEVICES), a = 0;
    /* Partitions and free areas by their first block. */
    uint64_t at = 0;
    for (;;) {
        const struct sys_device *next = 0;
        for (int i = 0; i < device_count; i++) {
            const struct sys_device *p = &devices[i];
            if (strcmp(p->disk, disk->name) == 0 && p->first_block >= at &&
                (!next || p->first_block < next->first_block))
                next = p;
        }
        char size[24], blocks[48], what[256];
        while (a < areas && (!next || first[a] < next->first_block)) {
            size_text((last[a] - first[a] + 1) * disk->block_size, size, sizeof(size));
            snprintf(blocks, sizeof(blocks), "%lu-%lu", first[a], last[a]);
            printf("  %-12s%10s  %s\n", "(free)", size, blocks);
            a++;
        }
        if (!next)
            return 0;
        size_text(next->block_count * next->block_size, size, sizeof(size));
        snprintf(blocks, sizeof(blocks), "%lu-%lu", next->first_block,
                 next->first_block + next->block_count - 1);
        describe(next, what, sizeof(what));
        printf("  %-12s%10s  %-22s%s\n", next->name, size, blocks, what);
        at = next->first_block + 1;
    }
}

/* --- changing ------------------------------------------------------------------ */

/* The user types the disk's name to go on. */
/* (68j) A change made without asking and shown in its own lines (grow). */
static bool unattended;

static bool confirmed(const struct sys_device *disk)
{
    if (unattended)
        return true;
    char line[64];
    printf("Type the disk's name (%s) to write the new table: ", disk->name);
    ssize_t n = read(STDIN_FILENO, line, sizeof(line) - 1);
    if (n <= 0) {
        printf("\n");
        return false;
    }
    line[n] = 0;
    if (line[n - 1] == '\n')
        line[n - 1] = 0;
    if (strcmp(line, disk->name) == 0)
        return true;
    printf("Nothing written.\n");
    return false;
}

static int open_disk(const struct sys_device *disk, int flags)
{
    char path[48];
    snprintf(path, sizeof(path), "/dev/%s", disk->name);
    int fd = open(path, flags);
    if (fd < 0 && errno == EACCES)
        fail("%s: %s (changing disks is root's)", path, strerror(errno));
    else if (fd < 0)
        fail("%s: %s", path, strerror(errno));
    return fd;
}

/* The disk to change: a disk, root may write it. */
static const struct sys_device *disk_arg(const char *name)
{
    const struct sys_device *d = device(name);
    if (!d)
        fail("%s: %s", name, strerror(ENOENT));
    else if (d->flags & DEVICE_PARTITION)
        fail("%s: a partition (of %s); this needs a disk", d->name, d->disk);
    return d && !(d->flags & DEVICE_PARTITION) ? d : 0;
}

/* A partition of a disk with a GPT, and its disk. */
static const struct sys_device *partition_arg(const char *name, const struct sys_device **disk)
{
    const struct sys_device *p = device(name);
    if (!p)
        fail("%s: %s", name, strerror(ENOENT));
    else if (!(p->flags & DEVICE_PARTITION))
        fail("%s: a disk; this needs a partition (%sp1, ...)", p->name, p->name);
    else if (!(*disk = device(p->disk)) || (*disk)->table != DEVICE_TABLE_GPT)
        fail("%s: on a disk with an MBR; disks changes GPT tables only (disks init %s makes one)",
             p->name, p->disk);
    else
        return p;
    return 0;
}

/* Read the table of disk, have the change made (change returns false if
 * it cannot be: it said why), confirm, write. */
static int change(const struct sys_device *disk, bool (*apply)(const struct sys_device *,
                                                               struct gpt_table *, void *),
                  void *arg, bool fresh)
{
    int fd = open_disk(disk, O_RDONLY);
    if (fd < 0)
        return 1;
    struct gpt_table t;
    if (fresh) {
        gpt_empty(disk, &t);
    } else if (!gpt_read(fd, disk, &t)) {
        close(fd);
        return fail("%s: no valid GPT found", disk->name);
    }
    close(fd);
    uint64_t array_blocks = (t.entries_bytes + disk->block_size - 1) / disk->block_size;
    /* Made for a smaller disk: fitted to this one (its copy at the end then
     * goes to the disk's end, write_table). */
    uint64_t fitted = disk->block_count - 2 - array_blocks;
    bool grown = !fresh && t.h.last_usable < fitted;
    uint64_t old_copy = t.h.alternate_lba;      /* (68j) the copy left in the middle */
    if (grown) {
        printf("%s: the table is fitted to the whole disk: partitions may use %lu-%lu "
               "(before: %lu-%lu)\n", disk->name, t.h.first_usable, fitted, t.h.first_usable,
               t.h.last_usable);
        t.h.last_usable = fitted;
    }
    if (t.h.last_usable + 1 + array_blocks >= disk->block_count ||
        t.entries_lba + array_blocks > t.h.first_usable)
        return fail("%s: the table does not fit the disk (smaller than it says)", disk->name);
    if (!apply(disk, &t, arg))
        return 1;
    if (!confirmed(disk))
        return 1;
    if ((fd = open_disk(disk, O_RDWR)) < 0)
        return 1;
    bool ok = (fresh ? gpt_write_protective_mbr(fd, disk) : !grown || gpt_fit_protective_mbr(fd, disk)) &&
              gpt_write(fd, disk, &t);
    /* (68j) A table fitted to a bigger disk: the old copy's header in the
     * middle is cleared (nothing would read it, but a disk tool might). */
    if (ok && grown && old_copy > 1 && old_copy < disk->block_count - 1) {
        static uint8_t zero[4096];
        ok = disk->block_size <= sizeof(zero) &&
             lseek(fd, (off_t)(old_copy * disk->block_size), SEEK_SET) >= 0 &&
             write(fd, zero, disk->block_size) == (ssize_t)disk->block_size;
    }
    int saved = errno;
    /* Closing has the kernel read the table again. */
    if (close(fd) < 0 && ok) {
        ok = false;
        saved = errno;
    }
    if (!ok)
        return fail("%s: writing failed: %s", disk->name, strerror(saved));
    load_devices();
    return unattended ? 0 : show(disk->name);
}

/* A mounted partition of disk (or the disk itself), or 0. */
static const struct sys_device *mounted_on(const struct sys_device *disk)
{
    for (int i = 0; i < device_count; i++)
        if ((strcmp(devices[i].disk, disk->name) == 0 || &devices[i] == disk) &&
            devices[i].mounted_at[0])
            return &devices[i];
    return 0;
}

static bool apply_init(const struct sys_device *disk, struct gpt_table *t, void *arg)
{
    (void)t, (void)arg;
    char what[96];
    if (disk->table != DEVICE_TABLE_NONE)
        snprintf(what, sizeof(what), ", replacing its %s with %u partition%s",
                 disk->table == DEVICE_TABLE_GPT ? "GPT" : "MBR", disk->partitions,
                 disk->partitions == 1 ? "" : "s");
    else if (disk->filesystem[0])
        snprintf(what, sizeof(what), ", replacing the %s on the whole disk", disk->filesystem);
    else
        what[0] = 0;
    printf("%s: a new, empty GPT partition table%s\n", disk->name, what);
    return true;
}

struct add {
    uint64_t bytes;                     /* 0: all of the largest free area */
    uint8_t type[16];
    const char *type_name, *name;
};

static bool apply_add(const struct sys_device *disk, struct gpt_table *t, void *arg)
{
    struct add *a = arg;
    uint64_t first[MAX_DEVICES], last[MAX_DEVICES];
    int areas = free_areas(disk, first, last, MAX_DEVICES), chosen = -1;
    uint64_t blocks = (a->bytes + disk->block_size - 1) / disk->block_size;
    for (int i = 0; i < areas; i++) {
        uint64_t length = last[i] - first[i] + 1;
        if (a->bytes ? length >= blocks && chosen < 0
                     : chosen < 0 || length > last[chosen] - first[chosen] + 1)
            chosen = i;
    }
    char size[24];
    size_text(a->bytes, size, sizeof(size));
    if (chosen < 0) {
        if (a->bytes)
            fail("%s: no free area of %s", disk->name, size);
        else
            fail("%s: no free area", disk->name);
        return false;
    }
    if (!a->bytes)
        blocks = last[chosen] - first[chosen] + 1;
    uint32_t number = 0;
    while (number < t->h.entry_count && !gpt_unused(gpt_entry_at(t, number)))
        number++;
    if (number == t->h.entry_count) {
        fail("%s: the table is full (%u entries)", disk->name, t->h.entry_count);
        return false;
    }
    struct gpt_entry *e = gpt_entry_at(t, number);
    memset(e, 0, t->h.entry_size);
    memcpy(e->type_guid, a->type, 16);
    gpt_new_guid(e->unique_guid);
    e->first_lba = first[chosen];
    e->last_lba = first[chosen] + blocks - 1;
    gpt_set_name(e, a->name);
    size_text(blocks * disk->block_size, size, sizeof(size));
    const char *digit = disk->name[strlen(disk->name) - 1] >= '0' &&
                        disk->name[strlen(disk->name) - 1] <= '9' ? "p" : "";
    printf("%s: new partition #%u (%s%s%u): %s, blocks %lu-%lu, %s", disk->name, number + 1,
           disk->name, digit, number + 1, size, e->first_lba, e->last_lba, a->type_name);
    if (a->name[0] && strcmp(a->name, a->type_name) != 0)
        printf(" '%s'", a->name);
    printf("\n");
    return true;
}

static bool apply_delete(const struct sys_device *disk, struct gpt_table *t, void *arg)
{
    const struct sys_device *p = arg;
    struct gpt_entry *e = p->number >= 1 && p->number <= t->h.entry_count ? gpt_entry_at(t, p->number - 1) : 0;
    if (!e || gpt_unused(e) || e->first_lba != p->first_block) {
        fail("%s: not in the table on %s (changed meanwhile?)", p->name, disk->name);
        return false;
    }
    memset(e, 0, t->h.entry_size);
    char size[24], what[256];
    size_text(p->block_count * p->block_size, size, sizeof(size));
    describe(p, what, sizeof(what));
    printf("%s: delete partition %s (%s, %s)\n", disk->name, p->name, size, what);
    return true;
}

struct rename {
    const struct sys_device *partition;
    const char *name;
};

static bool apply_rename(const struct sys_device *disk, struct gpt_table *t, void *arg)
{
    const struct sys_device *p = ((struct rename *)arg)->partition;
    const char *name = ((struct rename *)arg)->name;
    struct gpt_entry *e = p->number >= 1 && p->number <= t->h.entry_count ? gpt_entry_at(t, p->number - 1) : 0;
    if (!e || gpt_unused(e) || e->first_lba != p->first_block) {
        fail("%s: not in the table on %s (changed meanwhile?)", p->name, disk->name);
        return false;
    }
    gpt_set_name(e, name);
    printf("%s: partition %s named '%s' (was '%s')\n", disk->name, p->name, name, p->part_name);
    return true;
}

/* "16M", "2G", "512K", "1048576": bytes (K, M, G, T: binary units). */
static bool parse_size(const char *s, uint64_t *out)
{
    uint64_t n = 0;
    if (*s < '0' || *s > '9')
        return false;
    while (*s >= '0' && *s <= '9') {
        if (n > (UINT64_MAX - 9) / 10)
            return false;
        n = n * 10 + (uint64_t)(*s++ - '0');
    }
    int shift = 0;
    switch (*s) {
    case 0:   break;
    case 'K': shift = 10; break;
    case 'M': shift = 20; break;
    case 'G': shift = 30; break;
    case 'T': shift = 40; break;
    default:  return false;
    }
    if (*s && s[1])
        return false;
    if (n == 0 || n > UINT64_MAX >> shift)
        return false;
    *out = n << shift;
    return true;
}

static bool name_valid(const char *name)
{
    if (strlen(name) > GPT_NAME_CHARS)
        return false;
    for (const char *c = name; *c; c++)
        if (*c < 0x20 || *c > 0x7E)
            return false;
    return true;
}

/* (68j) disks grow [DISK] */
struct grow {
    const struct sys_device *partition;
    uint64_t last;                      /* its new last block */
};

static bool apply_grow(const struct sys_device *disk, struct gpt_table *t, void *arg)
{
    const struct grow *g = arg;
    const struct sys_device *p = g->partition;
    struct gpt_entry *e = p->number >= 1 && p->number <= t->h.entry_count ? gpt_entry_at(t, p->number - 1) : 0;
    if (!e || gpt_unused(e) || e->first_lba != p->first_block ||
        e->last_lba != p->first_block + p->block_count - 1) {
        fail("%s: not in the table on %s as before (changed meanwhile?)", p->name, disk->name);
        return false;
    }
    e->last_lba = g->last;
    char before[24], after[24];
    size_text(p->block_count * p->block_size, before, sizeof(before));
    size_text((g->last - p->first_block + 1) * p->block_size, after, sizeof(after));
    printf("disks: %s grows from %s to %s, to the end of %s\n", p->name, before, after, disk->name);
    return true;
}

static int grow(const char *name)
{
    const struct sys_device *disk = 0, *p = 0;
    if (name && !(disk = disk_arg(name)))
        return 1;
    for (int i = 0; i < device_count && !disk; i++)
        if ((devices[i].flags & (DEVICE_SYSTEM | DEVICE_PARTITION)) == DEVICE_SYSTEM)
            disk = &devices[i];
    if (!disk)
        return fail("no system disk found (name one: disks grow DISK)");
    if (disk->table != DEVICE_TABLE_GPT)
        return fail("%s: %s; disks grows partitions of a GPT only", disk->name,
                    disk->table == DEVICE_TABLE_MBR ? "an MBR" : "no partition table");
    for (int i = 0; i < device_count; i++)
        if (strcmp(devices[i].disk, disk->name) == 0 &&
            (!p || devices[i].first_block > p->first_block))
            p = &devices[i];
    if (!p || strcmp(p->filesystem, "RitterFS") != 0 || !p->mounted_at[0])
        return fail("%s: its last partition%s%s holds no mounted RitterFS (disks grows only that)",
                    disk->name, p ? ", " : "", p ? p->name : "");
    char partition[sizeof(p->name)];
    snprintf(partition, sizeof(partition), "%s", p->name);
    uint64_t align = alignment(disk), end = (fitted_last(disk) + 1) / align * align - 1;
    bool room = end > p->first_block + p->block_count - 1;
    if (room) {
        struct grow g = { p, end };
        unattended = true;
        if (change(disk, apply_grow, &g, false) != 0)
            return 1;
    }
    /* Also when the table was grown before but not the file system (an
     * earlier grow stopped between the two). */
    int64_t blocks = ritteros_grow(partition);
    if (blocks < 0)
        return fail("%s: the file system did not grow: %s", partition, strerror(errno));
    char size[24];
    size_text((uint64_t)blocks * 4096, size, sizeof(size));
    if (room)
        printf("disks: the RitterFS on %s now has %s (%ld blocks of 4 KiB)\n", partition, size,
               (long)blocks);
    else
        printf("disks: %s reaches the end of %s already (its RitterFS has %s)\n", partition,
               disk->name, size);
    return 0;
}

static int usage(void)
{
    dprintf(STDERR_FILENO,
            "usage: disks                         the disks and their partitions\n"
            "       disks DISK                    one disk: partitions' blocks, free areas\n"
            "       disks init DISK               a new, empty GPT partition table\n"
            "       disks add DISK SIZE|rest [TYPE [NAME]]\n"
            "                                     a new partition (SIZE: 100M, 2G, ...;\n"
            "                                     TYPE: ritteros, efi, data, linux, swap\n"
            "                                     or a type GUID; ritteros if not given)\n"
            "       disks delete PARTITION        take a partition out of the table\n"
            "       disks rename PARTITION NAME   a partition's name\n"
            "       disks grow [DISK]             the last partition (a mounted RitterFS) and\n"
            "                                     its file system to the disk's end (DISK:\n"
            "                                     the system disk if not given)\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        int64_t n = ritteros_report(REPORT_DISKS, text, sizeof(text));
        if (n < 0)
            return 1;
        write(STDOUT_FILENO, text, n < (int64_t)sizeof(text) ? (size_t)n : sizeof(text) - 1);
        return 0;
    }
    load_devices();
    const char *op = argv[1];
    if (argc == 2 && strcmp(op, "init") && strcmp(op, "add") && strcmp(op, "delete") &&
        strcmp(op, "rename") && strcmp(op, "grow"))
        return show(op);
    if (strcmp(op, "grow") == 0 && argc <= 3)
        return grow(argc == 3 ? argv[2] : 0);
    if (strcmp(op, "init") == 0 && argc == 3) {
        const struct sys_device *disk = disk_arg(argv[2]), *m;
        if (!disk)
            return 1;
        if ((m = mounted_on(disk)))
            return fail("%s is mounted at %s (unmount it first)", m->name, m->mounted_at);
        return change(disk, apply_init, 0, true);
    }
    if (strcmp(op, "add") == 0 && argc >= 4 && argc <= 6) {
        const struct sys_device *disk = disk_arg(argv[2]);
        if (!disk)
            return 1;
        if (disk->table != DEVICE_TABLE_GPT)
            return fail("%s: %s (disks init %s makes one)", disk->name,
                        disk->table == DEVICE_TABLE_MBR ? "an MBR; disks changes GPT tables only"
                                                        : "no partition table", disk->name);
        struct add a = { 0 };
        if (strcmp(argv[3], "rest") != 0 && !parse_size(argv[3], &a.bytes))
            return fail("%s: not a size (like 100M, 2G, or rest)", argv[3]);
        const char *type = argc >= 5 ? argv[4] : "ritteros";
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]) && !a.type_name; i++) {
            if (strcmp(type, types[i].word) == 0) {
                gpt_parse_guid(types[i].guid, a.type);
                a.type_name = types[i].name;
            }
        }
        if (!a.type_name && gpt_parse_guid(type, a.type))
            a.type_name = type;
        if (!a.type_name)
            return fail("%s: not a type (ritteros, efi, data, linux, swap, or a GUID)", type);
        a.name = argc == 6 ? argv[5] : a.type_name == type ? "" : a.type_name;
        if (!name_valid(a.name))
            return fail("'%s': not a name (at most 36 characters of plain ASCII)", a.name);
        return change(disk, apply_add, &a, false);
    }
    if (strcmp(op, "delete") == 0 && argc == 3) {
        const struct sys_device *disk, *p = partition_arg(argv[2], &disk);
        if (!p)
            return 1;
        if (p->mounted_at[0])
            return fail("%s is mounted at %s (unmount it first)", p->name, p->mounted_at);
        return change(disk, apply_delete, (void *)p, false);
    }
    if (strcmp(op, "rename") == 0 && argc == 4) {
        const struct sys_device *disk, *p = partition_arg(argv[2], &disk);
        if (!p)
            return 1;
        if (!name_valid(argv[3]))
            return fail("'%s': not a name (at most 36 characters of plain ASCII)", argv[3]);
        struct rename r = { p, argv[3] };
        return change(disk, apply_rename, &r, false);
    }
    return usage();
}

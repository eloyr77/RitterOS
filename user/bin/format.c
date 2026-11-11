/*
 * format [-t fat|exfat] DEVICE [LABEL]: make a new, empty RitterFS on a
 * partition (or a whole disk), with LABEL (default "RitterFS"), or with
 * -t fat a FAT32, with -t exfat an exFAT (for sharing with other systems;
 * LABEL up to 11 characters, none by default); whatever the device held
 * is no longer found. Root only, after the device's name is typed to confirm;
 * refused while it (or, for a whole disk, one of its partitions) is
 * mounted. When the device is closed, the kernel mounts the new file
 * system at /volumes/<label>.
 *
 * The writers are in the programs' library (mkfs.h: user/lib/mkfs_ritterfs.c,
 * mkfs_fat.c, mkfs_exfat.c), shared with osinstall; each comment there says
 * what it lays out.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"
#include "mkfs.h"
#include "../../common/rfs_format.h"

PROGRAM_HELP("format [-t ritterfs|fat|exfat] DEVICE [LABEL]",
             "make a new, empty RitterFS (or FAT32, exFAT) on a partition or disk (root)");

#define MAX_DEVICES     64

static struct sys_device devices[MAX_DEVICES];
static int device_count;

__attribute__((format(printf, 1, 2))) static int fail(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    dprintf(STDERR_FILENO, "format: ");
    vdprintf(STDERR_FILENO, format, ap);
    dprintf(STDERR_FILENO, "\n");
    va_end(ap);
    return 1;
}

/* "958.0 MiB": one decimal, rounded down (as disks shows sizes). */
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

static int fd;                              /* the device, open for writing */
static uint64_t now;

/* --- checking and asking --------------------------------------------------------- */

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

/* "RitterFS 'Photos', generation 5", "FAT32 'STICK'", "nothing (empty)" */
static void content(const struct sys_device *d, char *out, size_t n)
{
    if (!d->filesystem[0])
        snprintf(out, n, "%s", d->flags & DEVICE_EMPTY ? "nothing (empty)"
                                                        : "no file system found");
    else if (d->label[0])
        snprintf(out, n, "%s '%s'", d->filesystem, d->label);
    else
        snprintf(out, n, "%s", d->filesystem);
    if (strcmp(d->filesystem, "RitterFS") == 0) {
        size_t used = strlen(out);
        snprintf(out + used, n - used, ", generation %lu", d->generation);
    }
}

static bool label_valid(const char *label)
{
    size_t n = strlen(label);
    if (n == 0 || n >= sizeof(((struct rfs_superblock *)0)->label))
        return false;
    for (const char *c = label; *c; c++)
        if (*c < 0x20 || *c > 0x7E || *c == '/')
            return false;
    return true;
}

static bool confirmed(const struct sys_device *d)
{
    char line[64];
    printf("Type the device's name (%s) to format it: ", d->name);
    ssize_t n = read(STDIN_FILENO, line, sizeof(line) - 1);
    if (n <= 0) {
        printf("\n");
        return false;
    }
    line[n] = 0;
    if (line[n - 1] == '\n')
        line[n - 1] = 0;
    if (strcmp(line, d->name) == 0)
        return true;
    printf("Nothing written.\n");
    return false;
}

/* What the device holds now, before it is formatted. */
static void tell_loss(const struct sys_device *d, bool whole_disk)
{
    char what[160];
    if (whole_disk && d->table != DEVICE_TABLE_NONE)
        printf("It replaces the whole disk's %s partition table with %u partition%s.\n",
               d->table == DEVICE_TABLE_GPT ? "GPT" : "MBR", d->partitions,
               d->partitions == 1 ? "" : "s");
    else if (d->filesystem[0] || !(d->flags & DEVICE_EMPTY)) {
        content(d, what, sizeof(what));
        printf("What it holds now is lost: %s.\n", what);
    }
}

/* Open, write, close (the kernel then mounts it), report. */
static int finish(const char *name, bool (*write_it)(void *), void *arg)
{
    char path[48];
    snprintf(path, sizeof(path), "/dev/%s", name);
    if ((fd = open(path, O_RDWR)) < 0)
        return fail("%s: %s%s", path, strerror(errno),
                    errno == EACCES ? " (formatting is root's)" : "");
    now = ritteros_time();
    bool ok = write_it(arg);
    int saved = errno;
    /* Closing has the kernel look at the device again and mount it. */
    if (close(fd) < 0 && ok) {
        ok = false;
        saved = errno;
    }
    if (!ok)
        return fail("%s: writing failed: %s", name, strerror(saved));
    load_devices();
    const struct sys_device *d = device(name);
    if (d && d->mounted_at[0])
        printf("%s: formatted, mounted at %s\n", d->name, d->mounted_at);
    else
        printf("%s: formatted\n", name);
    return 0;
}

struct fat_job {
    struct fat_layout layout;
    const char *label;
    uint64_t hidden, bytes;
    bool whole_disk;
};

static bool fat_job(void *arg)
{
    struct fat_job *j = arg;
    return mkfs_fat32(fd, &j->layout, j->label, j->hidden, j->whole_disk, j->bytes, now);
}

static int format_fat(const struct sys_device *d, const char *label, bool whole_disk,
                      uint64_t bytes)
{
    struct fat_job j = { .label = label, .hidden = d->first_block, .bytes = bytes,
                         .whole_disk = whole_disk };
    char size[24], unit[24];
    size_text(bytes, size, sizeof(size));
    if (d->block_size < 512 || d->block_size > 4096 || (d->block_size & (d->block_size - 1)))
        return fail("%s: blocks of %lu bytes (FAT needs 512 to 4096)", d->name, d->block_size);
    if (!mkfs_fat_plan(bytes, (uint32_t)d->block_size, &j.layout))
        return fail("%s: %s does not suit FAT32 (from about 33 MiB to 2 TiB)", d->name, size);
    size_text((uint64_t)j.layout.cluster_sectors * j.layout.sector, unit, sizeof(unit));
    char shown[12];                                 /* as FAT keeps it: upper case */
    size_t n = 0;
    for (; label[n] && n < 11; n++)
        shown[n] = label[n] >= 'a' && label[n] <= 'z' ? (char)(label[n] - 32) : label[n];
    shown[n] = 0;
    printf("%s: a new FAT32%s%s%s, %s (%u clusters of %s)\n", d->name, label[0] ? " '" : "",
           shown, label[0] ? "'" : "", size, j.layout.clusters, unit);
    tell_loss(d, whole_disk);
    if (d->flags & DEVICE_PARTITION && strcmp(d->type_name, "RitterOS") == 0)
        printf("Its partition type is RitterOS: other systems may not show it.\n");
    if (!confirmed(d))
        return 1;
    return finish(d->name, fat_job, &j);
}

struct exfat_job {
    struct exfat_layout layout;
    uint16_t label[EXFAT_LABEL_MAX];
    int label_units;
    uint64_t hidden, bytes;
    bool whole_disk;
};

static bool exfat_job(void *arg)
{
    struct exfat_job *j = arg;
    return mkfs_exfat(fd, &j->layout, j->label, j->label_units, j->hidden, j->whole_disk, j->bytes);
}

static int format_exfat(const struct sys_device *d, const char *label, bool whole_disk,
                        uint64_t bytes)
{
    static struct exfat_job j;
    j.label_units = mkfs_exfat_label(label, j.label);
    j.hidden = d->first_block, j.bytes = bytes, j.whole_disk = whole_disk;
    char size[24], unit[24];
    size_text(bytes, size, sizeof(size));
    if (d->block_size < 512 || d->block_size > 4096 || (d->block_size & (d->block_size - 1)))
        return fail("%s: blocks of %lu bytes (exFAT needs 512 to 4096)", d->name, d->block_size);
    if (!mkfs_exfat_plan(bytes, (uint32_t)d->block_size, &j.layout))
        return fail("%s: %s does not suit exFAT (from 1 MiB on)", d->name, size);
    size_text(j.layout.cluster, unit, sizeof(unit));
    printf("%s: a new exFAT%s%s%s, %s (%u clusters of %s)\n", d->name, label[0] ? " '" : "",
           label, label[0] ? "'" : "", size, j.layout.clusters, unit);
    tell_loss(d, whole_disk);
    if (d->flags & DEVICE_PARTITION && strcmp(d->type_name, "RitterOS") == 0)
        printf("Its partition type is RitterOS: other systems may not show it.\n");
    if (!confirmed(d))
        return 1;
    return finish(d->name, exfat_job, &j);
}

struct ritterfs_job {
    const char *label;
    bool whole_disk;
    uint64_t bytes;
};

static bool ritterfs_job(void *arg)
{
    struct ritterfs_job *j = arg;
    return mkfs_ritterfs(fd, j->label, j->whole_disk, j->bytes, now);
}

int main(int argc, char **argv)
{
    bool fat = argc > 2 && strcmp(argv[1], "-t") == 0 && strcmp(argv[2], "fat") == 0;
    bool exfat = argc > 2 && strcmp(argv[1], "-t") == 0 && strcmp(argv[2], "exfat") == 0;
    bool ritterfs = argc > 2 && strcmp(argv[1], "-t") == 0 && strcmp(argv[2], "ritterfs") == 0;
    if (fat || exfat || ritterfs) {
        argc -= 2;
        argv += 2;
    }
    if (argc < 2 || argc > 3 || argv[1][0] == '-') {
        dprintf(STDERR_FILENO, "usage: format [-t fat|exfat] DEVICE [LABEL]\n");
        return 2;
    }
    const char *label = argc == 3 ? argv[2] : fat || exfat ? "" : "RitterFS";
    uint16_t units[EXFAT_LABEL_MAX];
    if (exfat && mkfs_exfat_label(label, units) < 0)
        return fail("'%s': not an exFAT label (at most 11 characters, none of \"*/:<>?\\|)", label);
    if (fat && !mkfs_fat_label_valid(label))
        return fail("'%s': not a FAT label (at most 11 letters, digits, spaces or "
                    "!#$%%&'()-@^_`{}~)", label);
    if (!fat && !exfat && !label_valid(label))
        return fail("'%s': not a label (1-63 plain characters, no '/')", label);
    load_devices();
    const struct sys_device *d = device(argv[1]);
    if (!d)
        return fail("%s: %s", argv[1], strerror(ENOENT));
    if (d->mounted_at[0])
        return fail("%s is mounted at %s (unmount it first)", d->name, d->mounted_at);
    bool whole_disk = !(d->flags & DEVICE_PARTITION);
    for (int i = 0; whole_disk && i < device_count; i++)
        if (strcmp(devices[i].disk, d->name) == 0 && devices[i].mounted_at[0])
            return fail("%s: its partition %s is mounted at %s (unmount it first)", d->name,
                        devices[i].name, devices[i].mounted_at);
    uint64_t bytes = d->block_count * d->block_size;
    if (fat)
        return format_fat(d, label, whole_disk, bytes);
    if (exfat)
        return format_exfat(d, label, whole_disk, bytes);
    if (RFS_BLOCK_SIZE % d->block_size)
        return fail("%s: blocks of %lu bytes (RitterFS needs a size that divides 4096)", d->name,
                    d->block_size);
    char size[24];
    size_text(bytes, size, sizeof(size));
    const char *why = mkfs_ritterfs_check(bytes);
    if (why)
        return fail("%s: %s %s", d->name, size, why);

    printf("%s: a new RitterFS '%s', %s (%lu blocks of 4 KiB)\n", d->name, label, size,
           bytes / RFS_BLOCK_SIZE);
    tell_loss(d, whole_disk);
    if (!confirmed(d))
        return 1;
    struct ritterfs_job j = { label, whole_disk, bytes };
    return finish(d->name, ritterfs_job, &j);
}

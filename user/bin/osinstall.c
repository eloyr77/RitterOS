/*
 * osinstall: install RitterOS from the disk it booted from (a stick) on
 * another disk, the whole disk (roadmap item 14, 48a). It asks its
 * questions one by one: which disk (every disk but the one the system
 * booted from), then the disk's name typed to confirm. Then it unmounts
 * the disk's volumes, writes a new GPT (an EFI system partition of 64 MiB,
 * FAT32 'EFI', and a RitterFS 'RitterOS' on the rest, named as on the
 * stick, with new random GUIDs: the kernel finds the partition it booted
 * from by its GUID, so the stick and the new disk must differ), and
 * copies onto it:
 *   - the bootloader for this machine's architecture to the fallback path
 *     \EFI\BOOT\BOOTX64.EFI (BOOTAA64.EFI), which most firmware boots
 *     without a boot entry of its own;
 *   - \ritteros\cmdline.txt without the options of tests (only noruntime
 *     and video stay);
 *   - /system/<arch> (the kernel and the programs, not the test programs)
 *     and /system/config/README;
 * No users, passwords, settings, homes or logs: the new system asks for
 * root's password on its first start, and (37e-1b) its settings files are
 * written with their defaults there by whoever reads them (never the
 * running system's: its sudo names its users). Last, a firmware boot entry
 * "RitterOS" for that file (48c: the J1900's firmware offered the SATA
 * disk only with one), first in the firmware's boot order; an earlier
 * "RitterOS" entry in that order is reused. Root only.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "gpt.h"
#include "mkfs.h"
#include "ritteros.h"

PROGRAM_HELP("osinstall", "install RitterOS on another disk, the whole disk (root; it asks)");

#if defined(__x86_64__)
#define ARCH        "x86_64"
#define BOOT_FILE   "BOOTX64.EFI"
#else
#define ARCH        "aarch64"
#define BOOT_FILE   "BOOTAA64.EFI"
#endif

#define MAX_DEVICES     64
#define MiB             (1024 * 1024)
#define ESP_BYTES       (64 * MiB)
#define MIN_BYTES       (256 * (uint64_t)MiB)
#define LABEL           "RitterOS"

static struct sys_device devices[MAX_DEVICES];
static int device_count;
static uint8_t esp_guid[16];                /* the new EFI partition: its GUID, place, size */
static uint64_t esp_first, esp_blocks;

__attribute__((format(printf, 1, 2))) static int fail(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    dprintf(STDERR_FILENO, "osinstall: ");
    vdprintf(STDERR_FILENO, format, ap);
    dprintf(STDERR_FILENO, "\n");
    va_end(ap);
    return 1;
}

static void load_devices(void)
{
    device_count = 0;
    while (device_count < MAX_DEVICES && ritteros_device(device_count, &devices[device_count]) == 1)
        device_count++;
}

static const struct sys_device *device(const char *name)
{
    for (int i = 0; i < device_count; i++)
        if (strcmp(devices[i].name, name) == 0)
            return &devices[i];
    return 0;
}

/* Partition number of disk, or 0. */
static const struct sys_device *partition(const char *disk, uint32_t number)
{
    for (int i = 0; i < device_count; i++)
        if (strcmp(devices[i].disk, disk) == 0 && devices[i].number == number)
            return &devices[i];
    return 0;
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

/* What a disk holds: "GPT, 2 partitions", "FAT32 'STICK'", "empty". */
static void content(const struct sys_device *d, char *out, size_t n)
{
    if (d->table != DEVICE_TABLE_NONE)
        snprintf(out, n, "%s, %u partition%s", d->table == DEVICE_TABLE_GPT ? "GPT" : "MBR",
                 d->partitions, d->partitions == 1 ? "" : "s");
    else if (d->filesystem[0])
        snprintf(out, n, "%s%s%s%s", d->filesystem, d->label[0] ? " '" : "", d->label,
                 d->label[0] ? "'" : "");
    else
        snprintf(out, n, "%s", d->flags & DEVICE_EMPTY ? "empty" : "no partition table");
}

/* The disk the system runs from: the one holding "/". */
static const char *system_disk(void)
{
    for (int i = 0; i < device_count; i++)
        if (strcmp(devices[i].mounted_at, "/") == 0)
            return devices[i].disk[0] ? devices[i].disk : devices[i].name;
    return "";
}

/* A line typed by the user, without its line break; false at the end of
 * the input. */
static bool ask(const char *question, char *line, size_t size)
{
    printf("%s", question);
    ssize_t n = read(STDIN_FILENO, line, size - 1);
    if (n <= 0) {
        printf("\n");
        return false;
    }
    line[n] = 0;
    if (line[n - 1] == '\n')
        line[n - 1] = 0;
    return true;
}

/* Unmount every mounted volume on disk; false if one stays. */
static bool unmount_all(const char *disk)
{
    bool ok = true;
    for (int i = 0; i < device_count; i++) {
        const struct sys_device *d = &devices[i];
        if ((strcmp(d->disk, disk) != 0 && strcmp(d->name, disk) != 0) || !d->mounted_at[0])
            continue;
        if (ritteros_unmount(d->mounted_at) < 0) {
            fail("%s: unmounting %s: %s", d->name, d->mounted_at, strerror(errno));
            ok = false;
        } else {
            printf("osinstall: %s unmounted from %s\n", d->name, d->mounted_at);
        }
    }
    load_devices();
    return ok;
}

/* The new table: the EFI system partition from 1 MiB on, RitterOS on the
 * rest (both on 1 MiB boundaries). */
static bool write_table(const struct sys_device *disk)
{
    struct gpt_table t;
    uint64_t bs = disk->block_size, align = MiB / bs;
    gpt_empty(disk, &t);
    struct gpt_entry *esp = gpt_entry_at(&t, 0), *data = gpt_entry_at(&t, 1);
    gpt_parse_guid(GPT_TYPE_EFI, esp->type_guid);
    gpt_new_guid(esp->unique_guid);
    esp->first_lba = (t.h.first_usable + align - 1) / align * align;
    esp->last_lba = esp->first_lba + ESP_BYTES / bs - 1;
    gpt_set_name(esp, "EFI system partition");
    memcpy(esp_guid, esp->unique_guid, 16);
    esp_first = esp->first_lba;
    esp_blocks = esp->last_lba - esp->first_lba + 1;
    gpt_parse_guid(GPT_TYPE_RITTEROS, data->type_guid);
    gpt_new_guid(data->unique_guid);
    data->first_lba = esp->last_lba + 1;
    data->last_lba = t.h.last_usable;
    gpt_set_name(data, "RitterOS");
    char path[48];
    snprintf(path, sizeof(path), "/dev/%s", disk->name);
    int fd = open(path, O_RDWR);
    if (fd < 0)
        return !fail("%s: %s", path, strerror(errno));
    bool ok = gpt_write_protective_mbr(fd, disk) && gpt_write(fd, disk, &t);
    int saved = errno;
    /* Closing has the kernel read the table again: the partitions appear. */
    if (close(fd) < 0 && ok)
        ok = false, saved = errno;
    if (!ok)
        return !fail("%s: writing the partition table: %s", disk->name, strerror(saved));
    printf("osinstall: %s: a new GPT partition table\n", disk->name);
    return true;
}

/* Make a file system on partition p (FAT32 'EFI' or RitterFS). */
static bool make_file_system(const struct sys_device *p, bool esp)
{
    char path[48], size[24];
    snprintf(path, sizeof(path), "/dev/%s", p->name);
    uint64_t bytes = p->block_count * p->block_size;
    struct fat_layout layout;
    if (esp && !mkfs_fat_plan(bytes, (uint32_t)p->block_size, &layout))
        return !fail("%s: no FAT32 fits", p->name);
    const char *why = esp ? 0 : mkfs_ritterfs_check(bytes);
    if (why)
        return !fail("%s: %s", p->name, why);
    int fd = open(path, O_RDWR);
    if (fd < 0)
        return !fail("%s: %s", path, strerror(errno));
    uint64_t now = ritteros_time();
    bool ok = esp ? mkfs_fat32(fd, &layout, "EFI", p->first_block, false, bytes, now)
                  : mkfs_ritterfs(fd, LABEL, false, bytes, now);
    int saved = errno;
    if (close(fd) < 0 && ok)
        ok = false, saved = errno;
    if (!ok)
        return !fail("%s: writing: %s", p->name, strerror(saved));
    size_text(bytes, size, sizeof(size));
    printf("osinstall: %s: %s, %s\n", p->name, esp ? "FAT32 'EFI'" : "RitterFS '" LABEL "'", size);
    return true;
}

static bool make_dir(const char *path)
{
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
        return !fail("%s: %s", path, strerror(errno));
    return true;
}

static bool copy(const char *from, const char *to)
{
    char failed[256];
    if (ritteros_copy_bytes(from, to, failed, sizeof(failed)) < 0)
        return !fail("copying %s: %s", failed, strerror(errno));
    return true;
}

/* A copy keeps only the rwx bits (as cp does): the programs of from that
 * run as root for everyone (setuid, 50b: su, sudo) get their bit back in to. */
static bool keep_setuid(const char *from, const char *to)
{
    DIR *dir = opendir(from);
    if (!dir)
        return !fail("%s: %s", from, strerror(errno));
    struct dirent *e;
    bool ok = true;
    while ((e = readdir(dir))) {
        char a[300], b[300];
        struct stat st;
        snprintf(a, sizeof(a), "%s/%s", from, e->d_name);
        if (stat(a, &st) < 0 || !(st.st_mode & S_ISUID))
            continue;
        snprintf(b, sizeof(b), "%s/%s", to, e->d_name);
        if (chmod(b, st.st_mode & 04777) < 0)
            ok = !fail("%s: %s", b, strerror(errno));
    }
    closedir(dir);
    return ok;
}

static bool write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    bool ok = fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text);
    int saved = errno;
    if (fd >= 0 && close(fd) < 0)
        ok = false, saved = errno;
    if (!ok)
        return !fail("%s: %s", path, strerror(saved));
    return true;
}

/* The stick's command line without the options of tests: only noruntime
 * and video (the firmware's quirks) go along. */
static void command_line(char *out, size_t size)
{
    char text[512];
    int fd = open("/boot/ritteros/cmdline.txt", O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, text, sizeof(text) - 1) : 0;
    if (fd >= 0)
        close(fd);
    text[n > 0 ? n : 0] = 0;
    out[0] = 0;
    for (char *c = text; *c;) {
        while (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n')
            c++;
        char *word = c;
        while (*c && *c != ' ' && *c != '\t' && *c != '\r' && *c != '\n')
            c++;
        if (*c)
            *c++ = 0;
        if (!*word || (strcmp(word, "noruntime") != 0 && strcmp(word, "video") != 0 &&
                       strncmp(word, "video=", 6) != 0))
            continue;
        size_t used = strlen(out);
        snprintf(out + used, size - used, "%s%s", used ? " " : "", word);
    }
    size_t used = strlen(out);
    snprintf(out + used, size - used, "\n");
}

/* The EFI partition: the bootloader at the fallback path, the command line. */
static bool fill_esp(const char *at)
{
    char path[160], line[512];
    snprintf(path, sizeof(path), "%s/EFI", at);
    if (!make_dir(path))
        return false;
    snprintf(path, sizeof(path), "%s/EFI/BOOT", at);
    if (!make_dir(path))
        return false;
    snprintf(path, sizeof(path), "%s/EFI/BOOT/" BOOT_FILE, at);
    if (!copy("/boot/EFI/BOOT/" BOOT_FILE, path))
        return false;
    snprintf(path, sizeof(path), "%s/ritteros", at);
    if (!make_dir(path))
        return false;
    snprintf(path, sizeof(path), "%s/ritteros/cmdline.txt", at);
    command_line(line, sizeof(line));
    if (!write_text(path, line))
        return false;
    struct stat st;
    if (stat("/boot/EFI/BOOT/" BOOT_FILE, &st) < 0)
        st.st_size = 0;
    printf("osinstall: the bootloader copied (\\EFI\\BOOT\\" BOOT_FILE ", %ld bytes), "
           "command line \"%.*s\"\n", (long)st.st_size, (int)strlen(line) - 1, line);
    return true;
}

/* The RitterFS: the kernel and the programs of this architecture. */
static bool fill_system(const char *at)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/system", at);
    if (!make_dir(path))
        return false;
    snprintf(path, sizeof(path), "%s/system/" ARCH, at);
    if (!make_dir(path))
        return false;
    snprintf(path, sizeof(path), "%s/system/" ARCH "/kernel.elf", at);
    if (!copy("/system/" ARCH "/kernel.elf", path))
        return false;
    snprintf(path, sizeof(path), "%s/system/" ARCH "/bin", at);
    if (!copy("/system/" ARCH "/bin", path) || !keep_setuid("/system/" ARCH "/bin", path))
        return false;
    snprintf(path, sizeof(path), "%s/system/" ARCH "/apps", at);
    if (!copy("/system/" ARCH "/apps", path))             /* the windowed apps */
        return false;
    snprintf(path, sizeof(path), "%s/system/" ARCH "/lib", at);
    if (!copy("/system/" ARCH "/lib", path))              /* ld.so and the shared libraries */
        return false;
    struct stat st;
    snprintf(path, sizeof(path), "%s/system/" ARCH "/drivers", at);
    if (stat("/system/" ARCH "/drivers", &st) == 0 &&     /* the kernel's modules (67a): network, */
        !copy("/system/" ARCH "/drivers", path))          /* Wi-Fi, sound, graphics drivers */
        return false;
    snprintf(path, sizeof(path), "%s/system/config", at);
    if (!make_dir(path))
        return false;
    snprintf(path, sizeof(path), "%s/system/config/README", at);
    if (stat("/system/config/README", &st) == 0 && !copy("/system/config/README", path))
        return false;
    snprintf(path, sizeof(path), "%s/system/fonts", at);
    if (stat("/system/fonts", &st) == 0 && !copy("/system/fonts", path))   /* libwindow's fonts */
        return false;
    snprintf(path, sizeof(path), "%s/system/certs", at);
    if (stat("/system/certs", &st) == 0 && !copy("/system/certs", path))   /* libtls's roots */
        return false;
    snprintf(path, sizeof(path), "%s/system/firmware", at);
    if (stat("/system/firmware", &st) == 0 && !copy("/system/firmware", path))  /* Wi-Fi firmware */
        return false;
    snprintf(path, sizeof(path), "%s/system/include", at);
    if (stat("/system/include", &st) == 0 && !copy("/system/include", path))   /* cc's headers (60) */
        return false;
    snprintf(path, sizeof(path), "%s/system/sdk", at);
    if (stat("/system/sdk", &st) == 0 && !copy("/system/sdk", path))   /* the SDK on the machine (63f) */
        return false;
    snprintf(path, sizeof(path), "%s/system/doc", at);
    if (stat("/system/doc", &st) == 0 && !copy("/system/doc", path))   /* man's pages (50j) */
        return false;
    /* (57b) The Unix folders, as mkdisk makes them: links into /system, and
     * /tmp for everyone (sticky: only owners remove their files). */
    static const char *const links[][2] = {
        { "/bin", "/system/" ARCH "/bin" }, { "/usr/bin", "/system/" ARCH "/bin" },
        { "/lib", "/system/" ARCH "/lib" }, { "/etc", "/system/config" },
    };
    snprintf(path, sizeof(path), "%s/usr", at);
    if (!make_dir(path))
        return false;
    for (size_t i = 0; i < sizeof(links) / sizeof(links[0]); i++) {
        snprintf(path, sizeof(path), "%s%s", at, links[i][0]);
        if (symlink(links[i][1], path) < 0 && errno != EEXIST)
            return !fail("%s: %s", path, strerror(errno));
    }
    snprintf(path, sizeof(path), "%s/tmp", at);
    if (!make_dir(path))
        return false;
    if (chmod(path, 01777) < 0)
        return !fail("%s: %s", path, strerror(errno));
    /* What went: the kernel and how many programs (bin and apps). */
    int programs = 0;
    const char *subdirs[2] = { "/bin", "/apps" };
    for (int s = 0; s < 2; s++) {
        snprintf(path, sizeof(path), "%s/system/" ARCH "%s", at, subdirs[s]);
        DIR *dir = opendir(path);
        for (struct dirent *e; dir && (e = readdir(dir));) {
            size_t len = strlen(e->d_name);
            programs += s == 0 ? !strchr(e->d_name, '.')                 /* a program in bin, */
                               : len > 8 && !strcmp(e->d_name + len - 8, ".program");   /* (42b) an app's bundle */
        }
        if (dir)
            closedir(dir);
    }
    printf("osinstall: /system/" ARCH " copied: the kernel and %d programs\n", programs);
    return true;
}

/* --- the firmware's boot entry --------------------------------------------- */

#define GLOBAL_VARIABLE "8BE4DF61-93CA-11D2-AA0D-00E098032B8C"
#define DESCRIPTION     "RitterOS"
#define LOAD_OPTION_ACTIVE  1
#define ATTRIBUTES      (EFIVAR_NON_VOLATILE | EFIVAR_BOOT_ACCESS | EFIVAR_RUNTIME)

static struct sys_efivar var;

static int get_var(const char *name)
{
    memset(&var, 0, sizeof(var));
    snprintf(var.name, sizeof(var.name), "%s", name);
    gpt_parse_guid(GLOBAL_VARIABLE, var.guid);
    return ritteros_efivar(EFIVAR_GET, &var);
}

static int set_var(const char *name, const void *data, uint32_t size)
{
    memset(&var, 0, sizeof(var));
    snprintf(var.name, sizeof(var.name), "%s", name);
    gpt_parse_guid(GLOBAL_VARIABLE, var.guid);
    var.attributes = ATTRIBUTES;
    var.size = size;
    memcpy(var.data, data, size);
    return ritteros_efivar(EFIVAR_SET, &var);
}

/* Is the load option in var ours (its description "RitterOS")? */
static bool ours(void)
{
    const char *d = DESCRIPTION;
    size_t n = strlen(d);
    if (var.size < 6 + 2 * (n + 1))
        return false;
    for (size_t i = 0; i <= n; i++)
        if (var.data[6 + 2 * i] != (uint8_t)d[i] || var.data[7 + 2 * i] != 0)
            return false;
    return true;
}

static size_t put16(uint8_t *at, uint16_t v)
{
    at[0] = (uint8_t)v, at[1] = (uint8_t)(v >> 8);
    return 2;
}

static size_t put_n(uint8_t *at, uint64_t v, int bytes)
{
    for (int i = 0; i < bytes; i++)
        at[i] = (uint8_t)(v >> (8 * i));
    return (size_t)bytes;
}

/* The load option: active, "RitterOS", the device path HD(1,GPT,<the EFI
 * partition's GUID>,start,size)/\EFI\BOOT\BOOTX64.EFI (the short form
 * Windows writes too: the firmware finds the disk by the partition's GUID). */
static size_t load_option(uint8_t *o)
{
    static const char file[] = "\\EFI\\BOOT\\" BOOT_FILE;
    size_t at = put_n(o, LOAD_OPTION_ACTIVE, 4) + 2;
    for (const char *c = DESCRIPTION; ; c++) {
        at += put16(o + at, (uint8_t)*c);
        if (!*c)
            break;
    }
    size_t path = at;
    o[at++] = 4, o[at++] = 1;                       /* media: hard drive */
    at += put16(o + at, 42);
    at += put_n(o + at, 1, 4);                      /* partition 1 */
    at += put_n(o + at, esp_first, 8);
    at += put_n(o + at, esp_blocks, 8);
    memcpy(o + at, esp_guid, 16);
    at += 16;
    o[at++] = 2, o[at++] = 2;                       /* GPT, a GUID signature */
    o[at++] = 4, o[at++] = 4;                       /* media: file path */
    at += put16(o + at, (uint16_t)(4 + 2 * sizeof(file)));
    for (size_t i = 0; i < sizeof(file); i++)
        at += put16(o + at, (uint8_t)file[i]);
    o[at++] = 0x7F, o[at++] = 0xFF;                 /* the end */
    at += put16(o + at, 4);
    put16(o + 4, (uint16_t)(at - path));
    return at;
}

/* Write the boot entry and put it first in BootOrder; says what it did.
 * Failing is not fatal: most firmware also starts the fallback path. */
static void boot_entry(void)
{
    static uint16_t order[EFIVAR_DATA_MAX / 2 + 1];
    static uint8_t option[512];
    char name[16];
    int count = 0, number = -1;
    if (get_var("BootOrder") == 0) {
        count = (int)(var.size / 2);
        memcpy(order, var.data, (size_t)count * 2);
    } else if (errno != ENOENT) {
        printf("osinstall: no firmware boot entry: %s%s\n", strerror(errno),
               errno == ENOSYS ? " (no runtime services)" : "");
        return;
    }
    for (int i = 0; i < count && number < 0; i++) {
        snprintf(name, sizeof(name), "Boot%04X", order[i]);
        if (get_var(name) == 0 && ours())
            number = order[i];
    }
    for (int n = 0; n < 0x10000 && number < 0; n++) {
        snprintf(name, sizeof(name), "Boot%04X", n);
        if (get_var(name) < 0 && errno == ENOENT)
            number = n;
    }
    snprintf(name, sizeof(name), "Boot%04X", number);
    size_t size = load_option(option);
    if (number < 0 || set_var(name, option, (uint32_t)size) < 0) {
        printf("osinstall: no firmware boot entry: %s\n", strerror(errno));
        return;
    }
    int kept = 0;
    for (int i = 0; i < count; i++)
        if (order[i] != number)
            order[kept++] = order[i];
    memmove(order + 1, order, (size_t)kept * 2);
    order[0] = (uint16_t)number;
    if (set_var("BootOrder", order, (uint32_t)(kept + 1) * 2) < 0) {
        printf("osinstall: the firmware's boot entry %s '" DESCRIPTION "' written, but not put "
               "in its boot order: %s\n", name, strerror(errno));
        return;
    }
    printf("osinstall: the firmware's boot entry %s '" DESCRIPTION "', first in its boot order\n",
           name);
}

/* Which disk: its number in the list or its name; 0 to stop. */
static const struct sys_device *choose(void)
{
    const char *skip = system_disk();
    const struct sys_device *offered[MAX_DEVICES];
    int count = 0;
    printf("osinstall: installs RitterOS (%s) from %s on another disk, the whole disk.\n",
           ARCH, skip[0] ? skip : "this system");
    for (int i = 0; i < device_count; i++) {
        const struct sys_device *d = &devices[i];
        if (d->flags & DEVICE_PARTITION || strcmp(d->name, skip) == 0)
            continue;
        offered[count++] = d;
    }
    if (!count) {
        fail("no other disk to install on");
        return 0;
    }
    printf("Disks:\n");
    for (int i = 0; i < count; i++) {
        char size[24], what[96];
        size_text(offered[i]->block_count * offered[i]->block_size, size, sizeof(size));
        content(offered[i], what, sizeof(what));
        printf("  %d  %-12s %10s  %-5s %s: %s\n", i + 1, offered[i]->name, size, offered[i]->kind,
               offered[i]->model, what);
    }
    char line[64];
    for (;;) {
        if (!ask("Which disk (its number or name; Enter to stop)? ", line, sizeof(line)) ||
            !line[0]) {
            printf("Nothing written.\n");
            return 0;
        }
        const struct sys_device *d = 0;
        int n = 0;
        for (const char *c = line; *c >= '0' && *c <= '9' && n <= MAX_DEVICES; c++)
            n = n * 10 + (*c - '0');
        if (n >= 1 && n <= count && line[0] != '0')
            d = offered[n - 1];
        for (int i = 0; i < count && !d; i++)
            if (strcmp(offered[i]->name, line) == 0)
                d = offered[i];
        if (d)
            return d;
        printf("'%s' is not one of the disks above.\n", line);
    }
}

int main(void)
{
    if (getuid() != 0)
        return fail("only root may install");
    struct stat st;
    if (stat("/boot/EFI/BOOT/" BOOT_FILE, &st) < 0)
        return fail("/boot/EFI/BOOT/" BOOT_FILE ": %s (the bootloader to copy)", strerror(errno));
    load_devices();
    const struct sys_device *disk = choose();
    if (!disk)
        return 1;
    char name[32], size[24], esp_size[24], rest[24], what[96], line[64];
    snprintf(name, sizeof(name), "%s", disk->name);
    uint64_t bytes = disk->block_count * disk->block_size;
    size_text(bytes, size, sizeof(size));
    if (bytes < MIN_BYTES)
        return fail("%s: %s is too small (at least 256 MiB)", name, size);
    if (disk->block_size > 4096 || MiB % disk->block_size)
        return fail("%s: blocks of %lu bytes", name, disk->block_size);
    size_text(ESP_BYTES, esp_size, sizeof(esp_size));
    size_text(bytes - ESP_BYTES - 2 * MiB, rest, sizeof(rest));
    const char *p = name[strlen(name) - 1] >= '0' && name[strlen(name) - 1] <= '9' ? "p" : "";
    printf("%s (%s) gets a new partition table:\n", name, size);
    printf("  %s%s1  %s, EFI system partition, FAT32 'EFI' (the bootloader)\n", name, p,
           esp_size);
    printf("  %s%s2  about %s, RitterOS, RitterFS '" LABEL "' (the system)\n", name, p, rest);
    content(disk, what, sizeof(what));
    printf("Everything on %s is lost (now: %s).\n", name, what);
    for (int i = 0; i < device_count; i++)
        if ((strcmp(devices[i].disk, name) == 0 || &devices[i] == disk) && devices[i].mounted_at[0])
            printf("Mounted now, unmounted first: %s at %s\n", devices[i].name,
                   devices[i].mounted_at);
    snprintf(what, sizeof(what), "Type the disk's name (%s) to install: ", name);
    if (!ask(what, line, sizeof(line)) || strcmp(line, name) != 0) {
        printf("Nothing written.\n");
        return 1;
    }

    if (!unmount_all(name) || !(disk = device(name)) || !write_table(disk))
        return 1;
    /* A partition may have been mounted at once from what an earlier
     * install left there: unmounted before it is formatted. */
    load_devices();
    if (!unmount_all(name))
        return 1;
    const struct sys_device *esp = partition(name, 1), *data = partition(name, 2);
    if (!esp || !data)
        return fail("%s: the new partitions did not appear", name);
    char esp_name[32], data_name[32];
    snprintf(esp_name, sizeof(esp_name), "%s", esp->name);
    snprintf(data_name, sizeof(data_name), "%s", data->name);
    if (!make_file_system(esp, true) || !make_file_system(data, false))
        return 1;

    /* Both were mounted when they were closed (the kernel looks at a
     * written device again; it leaves EFI partitions alone only when it
     * finds them at the boot or plugged in): mounted here otherwise. */
    load_devices();
    char system_at[128], esp_at[128];
    data = device(data_name);
    if (!data || !data->mounted_at[0])
        return fail("%s: the new RitterFS is not mounted", data_name);
    snprintf(system_at, sizeof(system_at), "%s", data->mounted_at);
    esp = device(esp_name);
    if (esp && esp->mounted_at[0])
        snprintf(esp_at, sizeof(esp_at), "%s", esp->mounted_at);
    else if (ritteros_mount(esp_name, 0, esp_at, sizeof(esp_at)) < 0)
        return fail("%s: mounting: %s", esp_name, strerror(errno));
    bool ok = fill_esp(esp_at) && fill_system(system_at);
    if (ritteros_unmount(esp_at) < 0 && ok)
        ok = !fail("%s: unmounting %s: %s", esp_name, esp_at, strerror(errno));
    if (ritteros_unmount(system_at) < 0 && ok)
        ok = !fail("%s: unmounting %s: %s", data_name, system_at, strerror(errno));
    if (!ok)
        return 1;
    boot_entry();
    printf("RitterOS is installed on %s. Remove the stick and restart (reboot); the firmware "
           "starts it from %s, and its first start asks for root's password.\n", name, name);
    return 0;
}

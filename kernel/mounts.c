/*
 * The mount table: file systems of any kind (struct vfs: RitterFS, FAT).
 * A path's file system is the mount with the longest
 * path that the path is or lies in (process.c's locate asks mounts_find);
 * everything else is the system's file system. /volumes is not on any
 * disk: it lists the mounts there.
 *
 * A file system, once mounted, stays allocated: an open file, a current
 * directory or a lookup in progress may still use it after an unmount,
 * and mounting the same device again finds it as it was (with its block
 * cache). After an unplug its reads fail (EIO).
 */
#include "mounts.h"
#include "devices.h"
#include "gpt.h"
#include "kprintf.h"
#include "vfs.h"
#include "string.h"
#include "sync.h"
#include "syscalls.h"

#define MAX_MOUNTS  16
#define MAX_KNOWN   32

struct mount {
    char path[MOUNT_PATH_MAX];
    struct vfs *fs;
    struct block_device *dev;
};

struct known {
    struct vfs *fs;
    uint32_t number;                /* stat's device: never reused */
};

static struct mount mounts[MAX_MOUNTS];
static int count;
static struct known known[MAX_KNOWN];          /* every file system mounted so far */
static int known_count;
static uint32_t next_number = 1;
static struct mutex lock = MUTEX_INIT("mounts");           /* the tables */
static struct mutex mounting = MUTEX_INIT("mounting");     /* one mount and check at a time */
static struct block_device *root_dev;
static struct vfs *root_fs;

/* Under the lock. */
static struct vfs *known_fs(const struct block_device *dev)
{
    for (int i = 0; i < known_count; i++)
        if (vfs_device(known[i].fs) == dev)
            return known[i].fs;
    return 0;
}

static void remember(struct vfs *fs)
{
    mutex_lock(&lock);
    if (known_count < MAX_KNOWN)
        known[known_count++] = (struct known){ fs, next_number++ };
    mutex_unlock(&lock);
}

void mounts_set_root(struct block_device *dev, struct vfs *fs)
{
    root_dev = dev;
    root_fs = fs;
    remember(fs);
}

struct vfs *mounts_root(void)
{
    return root_fs;
}

/* dev's file system: the one mounted before, or a new mount (a RitterFS
 * checked: read only if the check finds problems). 0 if dev holds none
 * that can be mounted. The check takes a while: the tables stay free
 * meanwhile. */
static struct vfs *file_system(struct block_device *dev)
{
    mutex_lock(&mounting);
    mutex_lock(&lock);
    struct vfs *fs = known_fs(dev);
    bool full = known_count == MAX_KNOWN;
    mutex_unlock(&lock);
    if (!fs && !full && (fs = vfs_mount(dev)))
        remember(fs);
    mutex_unlock(&mounting);
    return fs;
}

/* A name for /volumes: the label (no '/', printable), else the device's. */
static void volume_name(struct vfs *fs, const struct block_device *dev, char *out,
                        size_t size)
{
    vfs_label(fs, out, size);
    size_t n = strlen(out);
    for (size_t i = 0; i < n; i++)
        if (out[i] == '/')
            out[i] = '_';
    if (n == 0 || strcmp(out, ".") == 0 || strcmp(out, "..") == 0)
        ksnprintf(out, size, "%s", dev->name);
}

static int find_path(const char *path)
{
    for (int i = 0; i < count; i++)
        if (strcmp(mounts[i].path, path) == 0)
            return i;
    return -1;
}

int mounts_add(struct block_device *dev, const char *path, char *out, size_t size)
{
    if (dev == root_dev)
        return ERR_BUSY;                            /* it is / */
    struct vfs *fs = file_system(dev);
    if (!fs)
        return ERR_INVAL;
    mutex_lock(&lock);
    int error = 0;
    char chosen[MOUNT_PATH_MAX];
    for (int i = 0; i < count && !error; i++)
        if (mounts[i].dev == dev)
            error = ERR_BUSY;                       /* mounted already */
    if (!error && count == MAX_MOUNTS)
        error = ERR_AGAIN;
    if (!error && path) {
        if (strlen(path) >= sizeof(chosen))
            error = ERR_NAMETOOLONG;
        else if (find_path(path) >= 0)
            error = ERR_BUSY;
        else
            ksnprintf(chosen, sizeof(chosen), "%s", path);
    } else if (!error) {
        /* /volumes/<label>, then <label>-2, -3, ... */
        char name[72];
        volume_name(fs, dev, name, sizeof(name));
        ksnprintf(chosen, sizeof(chosen), "%s/%s", VOLUMES_DIR, name);
        for (int n = 2; find_path(chosen) >= 0; n++)
            ksnprintf(chosen, sizeof(chosen), "%s/%s-%d", VOLUMES_DIR, name, n);
    }
    if (!error) {
        struct mount *m = &mounts[count++];
        ksnprintf(m->path, sizeof(m->path), "%s", chosen);
        m->fs = fs;
        m->dev = dev;
        if (out && size)
            ksnprintf(out, size, "%s", chosen);
    }
    mutex_unlock(&lock);
    if (!error)
        devices_set_mounted(dev, chosen, fs);
    return error;
}

void mounts_auto(struct block_device *dev)
{
    mounts_auto_at(dev, 0, false);
}

void mounts_auto_at(struct block_device *dev, const char *at, bool read_only)
{
    char path[MOUNT_PATH_MAX], label[24];
    ksnprintf(label, sizeof(label), "%s:", dev->name);
    int error = mounts_add(dev, at, path, sizeof(path));
    if (!error) {
        struct vfs *fs;
        mounts_get(path, 0, 0, &fs, 0);
        if (read_only)
            vfs_set_read_only(fs);
        kprintf("  %-15smounted at %s%s\n", label, path, vfs_read_only(fs) ? " (read only)" : "");
    } else if (error != ERR_INVAL) {
        kprintf("  %-15snot mounted (error %d)\n", label, error);
    }
}

bool mounts_get(const char *path, char *mount_path, size_t size, struct vfs **fs,
                struct block_device **dev)
{
    mutex_lock(&lock);
    int found = find_path(path);
    for (int i = 0; i < count && found < 0; i++)
        if (strcmp(mounts[i].dev->name, path) == 0)
            found = i;
    if (found >= 0) {
        if (mount_path && size)
            ksnprintf(mount_path, size, "%s", mounts[found].path);
        if (fs)
            *fs = mounts[found].fs;
        if (dev)
            *dev = mounts[found].dev;
    }
    mutex_unlock(&lock);
    return found >= 0;
}

void mounts_remove(const char *mount_path)
{
    struct block_device *dev = 0;
    mutex_lock(&lock);
    int i = find_path(mount_path);
    if (i >= 0) {
        dev = mounts[i].dev;
        mounts[i] = mounts[--count];
    }
    mutex_unlock(&lock);
    if (dev) {
        devices_set_mounted(dev, "", 0);
        if (dev->flush)
            dev->flush(dev);                        /* everything committed is on the disk */
    }
}

bool mounts_find(const char *path, struct vfs **fs, const char **rest)
{
    size_t best = 0;
    mutex_lock(&lock);
    for (int i = 0; i < count; i++) {
        size_t n = strlen(mounts[i].path);
        if (n > best && strncmp(path, mounts[i].path, n) == 0 &&
            (path[n] == 0 || path[n] == '/')) {
            best = n;
            *fs = mounts[i].fs;
        }
    }
    mutex_unlock(&lock);
    if (best)
        *rest = path[best] ? path + best : "/";
    return best > 0;
}

/* The name of a mount directly in /volumes ("/volumes/NAME", no further
 * '/'), or 0. */
static const char *volume_entry(const char *p)
{
    size_t n = sizeof(VOLUMES_DIR) - 1;
    if (strncmp(p, VOLUMES_DIR, n) != 0 || p[n] != '/')
        return 0;
    for (const char *c = p + n + 1; *c; c++)
        if (*c == '/')
            return 0;
    return p + n + 1;
}

bool mounts_volume(int index, char *name, size_t size)
{
    /* In the order of their names: the index-th smallest. */
    bool found = false;
    mutex_lock(&lock);
    for (int i = 0; i < count && !found; i++) {
        const char *mine = volume_entry(mounts[i].path);
        if (!mine)
            continue;
        int before = 0;
        for (int j = 0; j < count; j++) {
            const char *other = volume_entry(mounts[j].path);
            before += other && strcmp(other, mine) < 0;
        }
        if (before == index) {
            ksnprintf(name, size, "%s", mine);
            found = true;
        }
    }
    mutex_unlock(&lock);
    return found;
}

uint32_t mounts_fs_number(struct vfs *fs)
{
    uint32_t number = 0;
    struct block_device *dev = vfs_device(fs);
    mutex_lock(&lock);
    for (int i = 0; i < known_count && !number; i++)
        if (vfs_device(known[i].fs) == dev)
            number = known[i].number;
    mutex_unlock(&lock);
    return number;
}

void mounts_forget(struct block_device *disk)
{
    mutex_lock(&mounting);
    mutex_lock(&lock);
    int kept = 0;
    for (int i = 0; i < known_count; i++) {
        bool mounted = vfs_device(known[i].fs) == root_dev;
        for (int j = 0; j < count && !mounted; j++)
            mounted = mounts[j].fs == known[i].fs;
        if (mounted || partition_disk(vfs_device(known[i].fs)) != disk)
            known[kept++] = known[i];
    }
    known_count = kept;
    mutex_unlock(&lock);
    mutex_unlock(&mounting);
}

void mounts_disk_gone(struct block_device *disk)
{
    /* Its file systems are never mounted again: a stick plugged in again
     * is a new device. (Their memory stays: an open file may still point
     * to one.) */
    mutex_lock(&lock);
    int kept = 0;
    for (int i = 0; i < known_count; i++)
        if (partition_disk(vfs_device(known[i].fs)) != disk)
            known[kept++] = known[i];
    known_count = kept;
    mutex_unlock(&lock);
    for (;;) {
        char path[MOUNT_PATH_MAX] = "";
        mutex_lock(&lock);
        for (int i = 0; i < count && !path[0]; i++)
            if (partition_disk(mounts[i].dev) == disk)
                ksnprintf(path, sizeof(path), "%s", mounts[i].path);
        mutex_unlock(&lock);
        if (!path[0])
            return;
        mounts_remove(path);
        kprintf("  %s: unmounted (%s is gone)\n", path, disk->name);
    }
}

/*
 * (37e-1b, the user's rule) The settings files the kernel reads, written
 * with their default texts when missing (common/settings_defaults.h): a
 * new system has them after its first start, and a deleted one comes back
 * at the next. The boot reads them before this runs (a missing file's
 * defaults are the same text's), so writing waits until the boot's check
 * of the disk is done, on its thread: a change taken during the check
 * would wait for it in the boot. With every file there, nothing is written
 * (normal starts still write nothing).
 */
#include "settings.h"
#include "kprintf.h"
#include "ritterfs.h"
#include "settings_defaults.h"
#include "string.h"

static const struct {
    const char *path, *text;
} files[] = {
    { SETTINGS_CPU, SETTINGS_CPU_TEXT },
    { SETTINGS_POWER, SETTINGS_POWER_TEXT },
    { SETTINGS_DISPLAYS, SETTINGS_DISPLAYS_TEXT },
    { SETTINGS_SWAP, SETTINGS_SWAP_TEXT },
};

/* The file, root's and 0644 as a program's would be, in one commit. */
static int write_new(struct vfs *fs, const char *path, const char *text)
{
    uint64_t id;
    int r = vfs_create(fs, path, RFS_TYPE_FILE, &id);
    if (r == RFS_OK)
        r = vfs_chown(fs, id, 0, 0);
    if (r == RFS_OK)
        r = vfs_chmod(fs, id, 0644);
    if (r == RFS_OK)
        r = vfs_write(fs, id, 0, text, strlen(text));
    if (r == RFS_OK)
        r = vfs_commit(fs);
    else if (r == RFS_ERR_IO || r == RFS_ERR_NO_SPACE)
        vfs_abort(fs);
    return r;
}

void settings_write_missing(struct vfs *fs)
{
    if (!fs || vfs_read_only(fs))
        return;
    char written[128] = "";
    size_t at = 0;
    uint64_t id;
    struct rfs_inode inode;
    if (!vfs_lookup(fs, "/system/config", &id, &inode)) {
        int r = vfs_create(fs, "/system/config", RFS_TYPE_DIR, &id);
        if (r == RFS_OK)
            r = vfs_chmod(fs, id, 0755);
        if (r == RFS_OK)
            r = vfs_commit(fs);
        if (r != RFS_OK) {
            kprintf("  %-15s/system/config cannot be made (error %d)\n", "settings:", r);
            return;
        }
    }
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (vfs_lookup(fs, files[i].path, &id, &inode))
            continue;
        int r = write_new(fs, files[i].path, files[i].text);
        const char *name = files[i].path + sizeof("/system/config/") - 1;
        if (r != RFS_OK)
            kprintf("  %-15s%s cannot be written (error %d)\n", "settings:", files[i].path, r);
        else
            at += (size_t)ksnprintf(written + at, sizeof(written) - at, "%s%s", at ? ", " : "",
                                    name);
    }
    if (at)
        kprintf("  %-15smissing, written with their defaults: %s (/system/config)\n",
                "settings:", written);
}

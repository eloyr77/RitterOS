/*
 * mount [DEVICE [PATH]]: without arguments, the mounted file systems; with
 * DEVICE (a partition's name, as disks and /dev show it: usbdisk0p2, or
 * /dev/usbdisk0p2), mount the RitterFS or FAT on it at /volumes/<label>, or at
 * PATH (a new name in /volumes, or an existing directory, whose content
 * is hidden meanwhile). Volumes found at the boot or plugged in are
 * mounted by themselves; mount is for those unmounted before, or another
 * place. Root only.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("mount [DEVICE [PATH]]", "list mounted volumes, or mount one (root only)");

static void list(void)
{
    struct sys_device d;
    for (int i = 0; ritteros_device(i, &d) == 1; i++) {
        if (!d.mounted_at[0])
            continue;
        printf("%-12s on %s (%s%s%s%s, %s)\n", d.name, d.mounted_at, d.filesystem,
               d.label[0] ? " '" : "", d.label, d.label[0] ? "'" : "",
               d.flags & DEVICE_READ_ONLY ? "read only" : "read-write");
    }
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        list();
        return 0;
    }
    if (argc > 3) {
        dprintf(STDERR_FILENO, "usage: mount [DEVICE [PATH]]\n");
        return 2;
    }
    const char *name = argv[1];
    if (strncmp(name, "/dev/", 5) == 0)
        name += 5;
    /* A clearer word than "Invalid argument" for what is not a RitterFS. */
    struct sys_device d;
    int i = 0;
    while (ritteros_device(i, &d) == 1 && strcmp(d.name, name) != 0)
        i++;
    if (strcmp(d.name, name) != 0) {
        dprintf(STDERR_FILENO, "mount: %s: %s\n", name, strerror(ENOENT));
        return 1;
    }
    if (strcmp(d.filesystem, "RitterFS") != 0 && strncmp(d.filesystem, "FAT", 3) != 0 &&
        strcmp(d.filesystem, "exFAT") != 0) {
        dprintf(STDERR_FILENO, "mount: %s: holds %s; only RitterFS, FAT and exFAT can be mounted\n",
                name, d.filesystem[0] ? d.filesystem : "no file system");
        return 1;
    }
    char path[128];
    if (ritteros_mount(name, argc == 3 ? argv[2] : 0, path, sizeof(path)) < 0) {
        if (errno == EBUSY && d.mounted_at[0])
            dprintf(STDERR_FILENO, "mount: %s: mounted already, at %s\n", name, d.mounted_at);
        else
            dprintf(STDERR_FILENO, "mount: %s: %s\n", argc == 3 ? argv[2] : name,
                    strerror(errno));
        return 1;
    }
    printf("mount: %s at %s\n", name, path);
    return 0;
}

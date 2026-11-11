/*
 * unmount PATH|DEVICE...: unmount volumes, by where they are mounted
 * (/volumes/STICK) or by their device (usbdisk0p2). Everything written is
 * on the disk already (each change is committed at once); unmounting also
 * empties the disk's write cache, so the stick may be pulled afterwards.
 * Refused while a program has a file open there or its current directory
 * there. Root only.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("unmount PATH|DEVICE...", "unmount volumes, e.g. before pulling a stick (root only)");

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: unmount PATH|DEVICE...\n");
        return 2;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        const char *what = argv[i];
        if (strncmp(what, "/dev/", 5) == 0)
            what += 5;
        if (ritteros_unmount(what) == 0)
            continue;
        if (errno == EINVAL)
            dprintf(STDERR_FILENO, "unmount: %s: not mounted\n", argv[i]);
        else if (errno == EBUSY)
            dprintf(STDERR_FILENO, "unmount: %s: in use (a program has a file open there, "
                    "or its current directory)\n", argv[i]);
        else
            dprintf(STDERR_FILENO, "unmount: %s: %s\n", argv[i], strerror(errno));
        status = 1;
    }
    return status;
}

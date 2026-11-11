/*
 * chmod MODE PATH...: set permissions, MODE in octal (e.g. 644: the owner
 * reads and writes, the group and everyone else read; 4755: setuid, the
 * program runs as its owner for anyone; 1777 on a folder: everyone writes,
 * only owners remove their entries, as /tmp). Only a file's owner or root may.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("chmod MODE PATH...", "set permissions (MODE in octal, e.g. 644)");

int main(int argc, char **argv)
{
    unsigned mode = 0;
    const char *m = argc > 1 ? argv[1] : "";
    bool valid = *m != 0;
    for (; *m && valid; m++) {
        valid = *m >= '0' && *m <= '7';
        mode = mode * 8 + (unsigned)(*m - '0');
    }
    if (argc < 3 || !valid || (mode > 0777 && (mode & ~05777u))) {
        dprintf(STDERR_FILENO, "usage: chmod MODE PATH... (MODE: octal, 0 to 777, plus 4000 for "
                "setuid and 1000 for the sticky bit; setgid, 2000, is not supported)\n");
        return 2;
    }
    int status = 0;
    for (int i = 2; i < argc; i++)
        if (chmod(argv[i], mode) < 0) {
            dprintf(STDERR_FILENO, "chmod: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    return status;
}

/*
 * rmdir DIR...: remove empty directories.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("rmdir DIR...", "remove empty directories");

int main(int argc, char **argv)
{
    int status = 0;
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: rmdir DIR...\n");
        return 2;
    }
    for (int i = 1; i < argc; i++)
        if (rmdir(argv[i]) < 0) {
            dprintf(STDERR_FILENO, "rmdir: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    return status;
}

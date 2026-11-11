/*
 * mkdir [-p] DIR...: create directories; -p also creates missing parent
 * directories and accepts directories that exist already.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("mkdir [-p] DIR...", "create directories (-p: with their parents)");

static int make(const char *path, bool parents)
{
    if (!parents) {
        if (mkdir(path, 0755) == 0)
            return 0;
        dprintf(STDERR_FILENO, "mkdir: %s: %s\n", path, strerror(errno));
        return 1;
    }
    char part[512];
    size_t n = strlen(path);
    if (n >= sizeof(part)) {
        dprintf(STDERR_FILENO, "mkdir: %s: %s\n", path, strerror(ENAMETOOLONG));
        return 1;
    }
    /* Each prefix that ends before a '/', then the whole path. */
    for (size_t i = 1; i <= n; i++) {
        if (i < n && path[i] != '/')
            continue;
        memcpy(part, path, i);
        part[i] = 0;
        struct stat st;
        if (stat(part, &st) == 0 && S_ISDIR(st.st_mode))
            continue;
        if (mkdir(part, 0755) < 0) {
            dprintf(STDERR_FILENO, "mkdir: %s: %s\n", part, strerror(errno));
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    /* Options before the names (50k: -p anywhere there, --). */
    bool parents = false;
    int first = 1, status = 0;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        if (!strcmp(argv[first], "--")) {
            first++;
            break;
        }
        for (const char *c = argv[first] + 1; *c; c++) {
            if (*c != 'p') {
                dprintf(STDERR_FILENO, "usage: mkdir [-p] DIR...\n");
                return 2;
            }
            parents = true;
        }
    }
    if (first >= argc) {
        dprintf(STDERR_FILENO, "usage: mkdir [-p] DIR...\n");
        return 2;
    }
    for (int i = first; i < argc; i++)
        status |= make(argv[i], parents);
    return status;
}

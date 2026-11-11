/*
 * ln [-s] [-f] TARGET LINK, ln [-s] [-f] TARGET... FOLDER (57a): make LINK
 * a second name for TARGET (a hard link: the same file under two names,
 * on the same volume, not for folders), or with -s a symbolic link that
 * names TARGET (any path, kept as written: a relative one is taken from
 * LINK's folder when it is used). Into a FOLDER each link gets TARGET's
 * last name. -f removes a LINK that is there first. Exit status 1 if a
 * link could not be made.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("ln [-s] [-f] TARGET [LINK] | ln [-s] [-f] TARGET... FOLDER",
             "make a link: a second name for a file, or with -s a symbolic link");

static bool symbolic, force;

static int make(const char *target, const char *name)
{
    struct stat st;
    if (force && lstat(name, &st) == 0 && !S_ISDIR(st.st_mode) && unlink(name) < 0) {
        dprintf(STDERR_FILENO, "ln: %s: %s\n", name, strerror(errno));
        return 1;
    }
    if ((symbolic ? symlink(target, name) : link(target, name)) < 0) {
        dprintf(STDERR_FILENO, "ln: %s: %s\n", name, strerror(errno));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 's')
                symbolic = true;
            else if (*o == 'f')
                force = true;
            else {
                dprintf(STDERR_FILENO, "usage: ln [-s] [-f] TARGET LINK\n");
                return 2;
            }
        }
    }
    int count = argc - i;
    if (count < 1) {
        dprintf(STDERR_FILENO, "usage: ln [-s] [-f] TARGET LINK\n");
        return 2;
    }
    const char *last = argv[argc - 1];
    struct stat st;
    bool into = count == 1 || (stat(last, &st) == 0 && S_ISDIR(st.st_mode));
    if (count > 2 && !into) {
        dprintf(STDERR_FILENO, "ln: %s: %s\n", last, strerror(ENOTDIR));
        return 1;
    }
    if (!into)
        return make(argv[i], last);
    int status = 0;
    for (int k = i; k < (count == 1 ? argc : argc - 1); k++) {
        const char *name = strrchr(argv[k], '/');
        name = name && name[1] ? name + 1 : argv[k];
        const char *folder = count == 1 ? "." : last;
        char path[512];
        snprintf(path, sizeof(path), "%s%s%s", folder,
                 folder[strlen(folder) - 1] == '/' ? "" : "/", name);
        status |= make(argv[k], path);
    }
    return status;
}

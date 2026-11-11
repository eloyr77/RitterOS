/*
 * rm [-r] PATH...: remove files; -r also removes directories with
 * everything in them. Removed files stay readable in snapshots taken
 * before. -r never enters a mounted volume (one stat shows on another file
 * system): it stops there, so a stick is not emptied by mistake.
 */
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("rm [-rf] PATH...", "remove files (-r: directories with their contents; -f: missing ones are no error)");

#define PATH_LENGTH 512

static int fail(const char *path)
{
    dprintf(STDERR_FILENO, "rm: %s: %s\n", path, strerror(errno));
    return 1;
}

static uint32_t top_device;         /* the file system rm -r works in */

static int remove_tree(char *path, size_t length)
{
    struct stat st;
    if (lstat(path, &st) < 0)                       /* (57a: a link itself, never its folder) */
        return fail(path);
    if (st.st_dev != top_device) {
        dprintf(STDERR_FILENO, "rm: %s: a mounted volume (unmount it first)\n", path);
        return 1;
    }
    if (!S_ISDIR(st.st_mode))
        return unlink(path) < 0 ? fail(path) : 0;
    /* Empty the directory first. Entries are read again after each
     * removal: the position in a changed directory would be stale. */
    for (;;) {
        DIR *dir = opendir(path);
        if (!dir)
            return fail(path);
        struct dirent *e = readdir(dir);
        char name[256];
        if (e)
            snprintf(name, sizeof(name), "%s", e->d_name);
        closedir(dir);
        if (!e)
            break;
        size_t n = strlen(name);
        bool slash = path[length - 1] != '/';
        if (length + slash + n + 1 > PATH_LENGTH) {
            errno = ENAMETOOLONG;
            return fail(path);
        }
        if (slash)
            path[length] = '/';
        memcpy(path + length + slash, name, n + 1);
        int r = remove_tree(path, length + slash + n);
        path[length] = 0;
        if (r)
            return r;
    }
    return rmdir(path) < 0 ? fail(path) : 0;
}

int main(int argc, char **argv)
{
    static char path[PATH_LENGTH];
    bool recursive = false, force = false;
    int first = 1, status = 0;
    /* -r (or -R), -f, together as -rf (63: make's recipes say rm -f) */
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        if (strcmp(argv[first], "--") == 0) {
            first++;
            break;
        }
        for (const char *o = argv[first] + 1; *o; o++)
            if (*o == 'r' || *o == 'R')
                recursive = true;
            else if (*o == 'f')
                force = true;
            else {
                dprintf(STDERR_FILENO, "usage: rm [-rf] PATH...\n");
                return 2;
            }
    }
    if (first >= argc) {
        if (force)
            return 0;                           /* (POSIX: rm -f alone is no error) */
        dprintf(STDERR_FILENO, "usage: rm [-rf] PATH...\n");
        return 2;
    }
    for (int i = first; i < argc; i++) {
        struct stat seen;
        if (force && lstat(argv[i], &seen) < 0 && errno == ENOENT)
            continue;                           /* -f: not there, nothing to say */
        if (!recursive) {
            if (unlink(argv[i]) < 0)
                status |= fail(argv[i]);
            continue;
        }
        snprintf(path, sizeof(path), "%s", argv[i]);
        struct stat st;
        if (ritteros_mount_point(path)) {
            dprintf(STDERR_FILENO, "rm: %s: a mounted volume (unmount it first)\n", path);
            status = 1;
            continue;
        }
        top_device = lstat(path, &st) == 0 ? st.st_dev : 0;
        status |= remove_tree(path, strlen(path));
    }
    return status;
}

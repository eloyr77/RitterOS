/*
 * mv FROM TO: move or rename a file or directory. If TO is a directory,
 * FROM goes into it under its own name. Nothing is copied: only directory
 * entries change. (68a) An existing target is replaced, as POSIX's mv: a
 * file by a file, an empty directory by a directory (rename does it in one
 * step). To another volume (/volumes/...) the target is removed first, the
 * data copied byte by byte, and FROM removed once the copy is complete.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("mv FROM TO", "move or rename a file or directory");

/* A path's last name: where it starts, and its length (trailing slashes
 * left out). */
static const char *last_name(const char *path, int *len)
{
    const char *name = path, *end = path + strlen(path);
    while (end > path + 1 && end[-1] == '/')
        end--;
    for (const char *c = path; c < end; c++)
        if (*c == '/' && c + 1 < end)
            name = c + 1;
    *len = (int)(end - name);
    return name;
}

/* TO, or TO/<FROM's last name> if TO is a directory (cp does the same). */
static void target_path(const char *from, const char *to, char *out, size_t size)
{
    struct stat st;
    if (stat(to, &st) < 0 || !S_ISDIR(st.st_mode)) {
        snprintf(out, size, "%s", to);
        return;
    }
    int len;
    const char *name = last_name(from, &len);
    snprintf(out, size, "%s%s%.*s", to, to[strlen(to) - 1] == '/' ? "" : "/", len, name);
}

/* (68a) Do from and to (one object, a and b) name the same entry, or two
 * names of a file? Else they are one entry under another spelling, as
 * FAT's README.TXT and ReadMe.txt: a rename that changes the case. */
static bool same_file(const char *from, const char *to, const struct stat *a,
                      const struct stat *b)
{
    if (a->st_dev != b->st_dev || a->st_ino != b->st_ino)
        return false;
    int n, m;
    const char *x = last_name(from, &n), *y = last_name(to, &m);
    return (n == m && memcmp(x, y, (size_t)n) == 0) || (!S_ISDIR(a->st_mode) && a->st_nlink > 1);
}

int main(int argc, char **argv)
{
    char to[512];
    if (argc != 3) {
        dprintf(STDERR_FILENO, "usage: mv FROM TO\n");
        return 2;
    }
    target_path(argv[1], argv[2], to, sizeof(to));
    if (ritteros_mount_point(argv[1])) {
        dprintf(STDERR_FILENO, "mv: %s: a mounted volume (unmount it, or mount it "
                "elsewhere)\n", argv[1]);
        return 1;
    }
    struct stat a, b;
    bool exists = lstat(to, &b) == 0;
    if (exists && lstat(argv[1], &a) == 0 && same_file(argv[1], to, &a, &b)) {
        dprintf(STDERR_FILENO, "mv: %s and %s are the same file\n", argv[1], to);
        return 1;                               /* (GNU's: the Mac's mv says so only at times) */
    }
    if (rename(argv[1], to) == 0)
        return 0;
    if (errno != EXDEV) {
        dprintf(STDERR_FILENO, "mv: %s -> %s: %s\n", argv[1], to, strerror(errno));
        return 1;
    }
    /* Another volume: the target goes as rename would take it, then the
     * copy, then the original. A copy that fails halfway is removed again
     * (TO did not exist: the copy refuses that). */
    if (exists) {
        int error = 0;
        if (lstat(argv[1], &a) < 0)
            error = errno;
        else if (S_ISDIR(a.st_mode) && !S_ISDIR(b.st_mode))
            error = ENOTDIR;
        else if (!S_ISDIR(a.st_mode) && S_ISDIR(b.st_mode))
            error = EISDIR;
        else if ((S_ISDIR(b.st_mode) ? rmdir(to) : unlink(to)) < 0)
            error = errno;
        if (error) {
            dprintf(STDERR_FILENO, "mv: %s -> %s: %s\n", argv[1], to, strerror(error));
            return 1;
        }
    }
    char failed[512];
    if (ritteros_copy_bytes(argv[1], to, failed, sizeof(failed)) < 0) {
        int error = errno;
        if (strcmp(failed, to) != 0 || error != EEXIST)
            ritteros_remove_tree(to);
        dprintf(STDERR_FILENO, "mv: %s: %s\n", failed, strerror(error));
        return 1;
    }
    if (ritteros_remove_tree(argv[1]) < 0) {
        dprintf(STDERR_FILENO, "mv: copied to %s, but %s: %s\n", to, argv[1], strerror(errno));
        return 1;
    }
    return 0;
}

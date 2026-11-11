/*
 * Copying byte by byte, for cp and mv between file systems (another
 * volume): an instant copy shares blocks, which only works within one
 * file system (the kernel says EXDEV otherwise).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

#define PATH_LENGTH 512

static int fail_at(const char *path, char *failed, size_t size)
{
    int saved = errno;
    if (failed && size)
        snprintf(failed, size, "%s", path);
    errno = saved;
    return -1;
}

static int copy_file(const char *from, const char *to, unsigned mode, char *failed,
                     size_t size)
{
    int in = open(from, O_RDONLY);
    if (in < 0)
        return fail_at(from, failed, size);
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        close(in);
        return fail_at(to, failed, size);
    }
    char buffer[4096];
    ssize_t n;
    int r = 0;
    while (r == 0 && (n = read(in, buffer, sizeof(buffer))) != 0) {
        if (n < 0) {
            r = fail_at(from, failed, size);
            break;
        }
        for (ssize_t done = 0; done < n && r == 0;) {
            ssize_t w = write(out, buffer + done, (size_t)(n - done));
            if (w <= 0)
                r = fail_at(to, failed, size);
            else
                done += w;
        }
    }
    close(in);
    if (close(out) < 0 && r == 0)                   /* the commit */
        r = fail_at(to, failed, size);
    if (r == 0 && chmod(to, mode & 0777) < 0)
        r = fail_at(to, failed, size);
    return r;
}

int ritteros_copy_bytes(const char *from, const char *to, char *failed, size_t size)
{
    struct stat st, there;
    if (lstat(from, &st) < 0)
        return fail_at(from, failed, size);
    if (lstat(to, &there) == 0) {
        errno = EEXIST;                             /* as an instant copy: TO must be new */
        return fail_at(to, failed, size);
    }
    if (S_ISLNK(st.st_mode)) {                      /* (57a) a link stays a link */
        char target[256];
        ssize_t n = readlink(from, target, sizeof(target) - 1);
        if (n < 0)
            return fail_at(from, failed, size);
        target[n] = 0;
        return symlink(target, to) < 0 ? fail_at(to, failed, size) : 0;
    }
    if (!S_ISDIR(st.st_mode))
        return copy_file(from, to, st.st_mode, failed, size);
    if (mkdir(to, st.st_mode & 0777) < 0)
        return fail_at(to, failed, size);
    DIR *dir = opendir(from);
    if (!dir)
        return fail_at(from, failed, size);
    struct dirent *e;
    int r = 0;
    while (r == 0 && (e = readdir(dir))) {
        char inner_from[PATH_LENGTH], inner_to[PATH_LENGTH];
        if ((size_t)snprintf(inner_from, sizeof(inner_from), "%s/%s", from, e->d_name) >=
                sizeof(inner_from) ||
            (size_t)snprintf(inner_to, sizeof(inner_to), "%s/%s", to, e->d_name) >=
                sizeof(inner_to)) {
            errno = ENAMETOOLONG;
            r = fail_at(from, failed, size);
            break;
        }
        r = ritteros_copy_bytes(inner_from, inner_to, failed, size);
    }
    closedir(dir);
    return r;
}

/* Never into another file system (a volume mounted inside): EBUSY. */
static int remove_in(const char *path, uint32_t device)
{
    struct stat st;
    if (lstat(path, &st) < 0)                       /* (a link goes, never what it names) */
        return -1;
    if (st.st_dev != device) {
        errno = EBUSY;
        return -1;
    }
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    /* Entries are read again after each removal: the position in a
     * changed directory would be stale. */
    for (;;) {
        DIR *dir = opendir(path);
        if (!dir)
            return -1;
        struct dirent *e = readdir(dir);
        char inner[PATH_LENGTH];
        if (e)
            snprintf(inner, sizeof(inner), "%s/%s", path, e->d_name);
        closedir(dir);
        if (!e)
            break;
        if (remove_in(inner, device) < 0)
            return -1;
    }
    return rmdir(path);
}

bool ritteros_mount_point(const char *path)
{
    char parent[PATH_LENGTH];
    struct stat here, up;
    snprintf(parent, sizeof(parent), "%s/..", path);
    return stat(path, &here) == 0 && stat(parent, &up) == 0 && here.st_dev != up.st_dev;
}

int ritteros_remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return -1;
    if (ritteros_mount_point(path)) {
        errno = EBUSY;                              /* never empty a volume */
        return -1;
    }
    return remove_in(path, st.st_dev);
}

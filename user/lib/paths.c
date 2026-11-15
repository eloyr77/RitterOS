/*
 * (43a) Paths: realpath (the path without ".", ".." or symbolic links,
 * each part looked at with lstat and links read, at most 40 of them, as
 * the kernel's resolve_path allows), and libgen.h's POSIX basename and
 * dirname (they may write into the path they are given).
 */
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char *realpath(const char *path, char *resolved)
{
    if (!path) {
        errno = EINVAL;
        return 0;
    }
    if (!*path) {
        errno = ENOENT;
        return 0;
    }
    char out[PATH_MAX], rest[PATH_MAX], link[PATH_MAX];
    size_t n = 0;
    if (path[0] != '/') {
        if (!getcwd(out, sizeof(out)))
            return 0;
        n = strlen(out);
        if (n == 1)
            n = 0;                              /* ("/": parts are added after a '/') */
    }
    if (strlen(path) >= sizeof(rest)) {
        errno = ENAMETOOLONG;
        return 0;
    }
    strcpy(rest, path);
    int links = 0;
    char *p = rest;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == 1 && p[0] == '.') {
            p += len;
            continue;
        }
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            while (n > 0 && out[n - 1] != '/')
                n--;
            if (n > 0)
                n--;                            /* (the '/' before the part) */
            p += len;
            continue;
        }
        if (n + 1 + len >= sizeof(out)) {
            errno = ENAMETOOLONG;
            return 0;
        }
        size_t before = n;
        out[n++] = '/';
        memcpy(out + n, p, len);
        n += len;
        out[n] = 0;
        p += len;
        struct stat st;
        if (lstat(out, &st) < 0)
            return 0;
        if (S_ISLNK(st.st_mode)) {
            if (++links > 40) {
                errno = ELOOP;
                return 0;
            }
            ssize_t k = readlink(out, link, sizeof(link) - 1);
            if (k < 0)
                return 0;
            link[k] = 0;
            if (strlen(p) + (size_t)k + 2 > sizeof(rest)) {
                errno = ENAMETOOLONG;
                return 0;
            }
            /* the link's text, then what was left of the path (43e: a '/'
             * between only when something is left: a link to a file is
             * no folder) */
            size_t left = strlen(p);
            memmove(rest + k + (left ? 1 : 0), p, left + 1);
            memcpy(rest, link, (size_t)k);
            if (left)
                rest[k] = '/';
            p = rest;
            n = link[0] == '/' ? 0 : before;
            continue;
        }
        if (*p && !S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            return 0;
        }
    }
    if (n == 0)
        out[n++] = '/';
    out[n] = 0;
    if (!resolved) {
        resolved = strdup(out);
        if (!resolved)
            errno = ENOMEM;
        return resolved;
    }
    strcpy(resolved, out);
    return resolved;
}

char *basename(char *path)
{
    if (!path || !*path)
        return (char *)".";
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/')
        path[--n] = 0;                          /* (trailing slashes go) */
    char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

char *dirname(char *path)
{
    if (!path || !*path)
        return (char *)".";
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/')
        n--;
    while (n > 0 && path[n - 1] != '/')
        n--;
    if (n == 0)
        return (char *)".";
    while (n > 1 && path[n - 1] == '/')
        n--;
    path[n] = 0;
    return path;
}

int access(const char *path, int mode)
{
    struct stat st;
    if (mode & ~(R_OK | W_OK | X_OK)) {
        errno = EINVAL;
        return -1;
    }
    if (stat(path, &st) < 0)
        return -1;
    uid_t uid = getuid();
    unsigned bits;
    if (uid == 0) {
        if ((mode & X_OK) && !S_ISDIR(st.st_mode) && !(st.st_mode & 0111)) {
            errno = EACCES;
            return -1;
        }
        return 0;
    }
    if (st.st_uid == uid)
        bits = (st.st_mode >> 6) & 7;
    else if (st.st_gid == getgid())
        bits = (st.st_mode >> 3) & 7;
    else
        bits = st.st_mode & 7;
    if ((unsigned)mode & ~bits) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

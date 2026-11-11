/*
 * cp [-r] FROM TO: copy a file, or with -r a directory with everything in
 * it. If TO is a directory, the copy goes into it under FROM's name. The
 * copy is instant: it shares the data with FROM until one of them changes.
 * FROM may be in a snapshot (/.snapshots/NAME/...): that restores an old
 * version. To another volume (/volumes/...) the data is copied byte by
 * byte instead: blocks are shared only within one file system. A file TO
 * that exists is written over, as POSIX's cp (59: until then cp refused
 * it): its content replaced, its mode and other names kept. -p (63m) also
 * gives the copy (with -r everything in it) its original's modification
 * time and mode, and when root copies its owner and group (setuid only
 * with the owner, as POSIX's).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("cp [-rp] FROM TO", "copy a file (-r: a directory), instantly; -p: keep times and modes");

/* TO, or TO/<FROM's last name> if TO is a directory (mv does the same). */
static void target_path(const char *from, const char *to, char *out, size_t size)
{
    struct stat st;
    if (stat(to, &st) < 0 || !S_ISDIR(st.st_mode)) {
        snprintf(out, size, "%s", to);
        return;
    }
    const char *name = from, *end = from + strlen(from);
    while (end > from + 1 && end[-1] == '/')
        end--;
    for (const char *c = from; c < end; c++)
        if (*c == '/' && c + 1 < end)
            name = c + 1;
    snprintf(out, size, "%s%s%.*s", to, to[strlen(to) - 1] == '/' ? "" : "/",
             (int)(end - name), name);
}

/* FROM's bytes into the existing file TO. */
static int overwrite(const char *from, const char *to)
{
    int in = open(from, O_RDONLY);
    if (in < 0) {
        dprintf(STDERR_FILENO, "cp: %s: %s\n", from, strerror(errno));
        return 1;
    }
    int out = open(to, O_WRONLY | O_TRUNC);
    if (out < 0) {
        dprintf(STDERR_FILENO, "cp: %s: %s\n", to, strerror(errno));
        close(in);
        return 1;
    }
    static char buffer[65536];
    ssize_t n;
    int status = 0;
    while ((n = read(in, buffer, sizeof(buffer))) > 0)
        for (ssize_t done = 0; done < n;) {
            ssize_t w = write(out, buffer + done, (size_t)(n - done));
            if (w <= 0) {
                dprintf(STDERR_FILENO, "cp: %s: %s\n", to, strerror(errno));
                status = 1;
                n = 0;
                break;
            }
            done += w;
        }
    if (n < 0) {
        dprintf(STDERR_FILENO, "cp: %s: %s\n", from, strerror(errno));
        status = 1;
    }
    close(in);
    if (close(out) < 0 && !status) {
        dprintf(STDERR_FILENO, "cp: %s: %s\n", to, strerror(errno));
        status = 1;
    }
    return status;
}

/* (63m) -p: to (and with a folder all inside it) gets from's mode, time,
 * and for root its owner; a folder's time last (filling it changed it). */
static int preserve(const char *from, const char *to)
{
    struct stat st;
    if (lstat(from, &st) < 0) {
        dprintf(STDERR_FILENO, "cp: %s: %s\n", from, strerror(errno));
        return 1;
    }
    int status = 0;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(from);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char a[1024], b[1024];
            snprintf(a, sizeof(a), "%s/%s", from, e->d_name);
            snprintf(b, sizeof(b), "%s/%s", to, e->d_name);
            status |= preserve(a, b);
        }
        if (d)
            closedir(d);
    }
    bool link = S_ISLNK(st.st_mode);
    if (getuid() == 0 && !link && chown(to, st.st_uid, st.st_gid) < 0) {
        dprintf(STDERR_FILENO, "cp: %s: %s\n", to, strerror(errno));
        status = 1;
    }
    if (!link && chmod(to, st.st_mode & (getuid() == 0 ? 07777 : 01777)) < 0) {
        dprintf(STDERR_FILENO, "cp: %s: %s\n", to, strerror(errno));
        status = 1;
    }
    struct timespec when[2] = { { st.st_mtime, 0 }, { st.st_mtime, 0 } };
    if (utimensat(AT_FDCWD, to, when, AT_SYMLINK_NOFOLLOW) < 0) {
        dprintf(STDERR_FILENO, "cp: %s: %s\n", to, strerror(errno));
        status = 1;
    }
    return status;
}

/* (63m) An instant copy shares its original's inode times: a copy gets the
 * time now, as POSIX's cp (without -p) gives it, everything in it too. */
static int now_times(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return 1;
    int status = 0;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char child[1024];
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            status |= now_times(child);
        }
        if (d)
            closedir(d);
    }
    if (utimensat(AT_FDCWD, path, 0, AT_SYMLINK_NOFOLLOW) < 0) {
        dprintf(STDERR_FILENO, "cp: %s: %s\n", path, strerror(errno));
        status = 1;
    }
    return status;
}

static int copy(const char *from, const char *to_arg, bool recursive);

int main(int argc, char **argv)
{
    bool recursive = false, keep = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        for (const char *k = argv[first] + 1; *k; k++) {
            if (*k == 'r' || *k == 'R')
                recursive = true;
            else if (*k == 'p')
                keep = true;
            else {
                dprintf(STDERR_FILENO, "usage: cp [-rp] FROM TO\n");
                return 2;
            }
        }
    }
    if (argc - first != 2) {
        dprintf(STDERR_FILENO, "usage: cp [-rp] FROM TO\n");
        return 2;
    }
    char to[512];                               /* (before the copy makes it: a folder then) */
    target_path(argv[first], argv[first + 1], to, sizeof(to));
    int status = copy(argv[first], argv[first + 1], recursive);
    if (status == 0 && keep)
        status = preserve(argv[first], to);
    return status;
}

static int copy(const char *from, const char *to_arg, bool recursive)
{
    char to[512];
    struct stat st;
    if (stat(from, &st) < 0) {
        dprintf(STDERR_FILENO, "cp: %s: %s\n", from, strerror(errno));
        return 1;
    }
    if (S_ISDIR(st.st_mode) && !recursive) {
        dprintf(STDERR_FILENO, "cp: %s: %s (use -r)\n", from, strerror(EISDIR));
        return 1;
    }
    target_path(from, to_arg, to, sizeof(to));
    struct stat there;
    if (!S_ISDIR(st.st_mode) && stat(to, &there) == 0 && !S_ISDIR(there.st_mode))
        return overwrite(from, to);
    if (ritteros_copy(from, to) == 0)
        return now_times(to);
    if (errno != EXDEV) {
        dprintf(STDERR_FILENO, "cp: %s -> %s: %s\n", from, to, strerror(errno));
        return 1;
    }
    char failed[512];
    if (ritteros_copy_bytes(from, to, failed, sizeof(failed)) < 0) {
        dprintf(STDERR_FILENO, "cp: %s: %s\n", failed, strerror(errno));
        return 1;
    }
    return 0;
}

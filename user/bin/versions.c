/*
 * versions PATH: a file's history. Every snapshot that has the file
 * (/.snapshots/NAME/PATH), oldest first, then the file as it is now; each
 * with its size and whether its content differs from the version before.
 * Snapshots share unchanged blocks, so old versions cost little space.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("versions PATH", "a file's versions in the snapshots and now");

#define MAX_LIST 64
#define PATH_LENGTH 512

static char chunk_a[4096], chunk_b[4096];

/* Read up to size bytes (fewer only at the end of the file). */
static long read_full(int fd, char *buf, long size)
{
    long done = 0;
    while (done < size) {
        long n = read(fd, buf + done, (size_t)(size - done));
        if (n < 0)
            return -1;
        if (n == 0)
            break;
        done += n;
    }
    return done;
}

/* 1: the two files' contents differ, 0: identical, -1: a read error. */
static int differ(const char *x, const char *y)
{
    int fx = open(x, O_RDONLY), fy = open(y, O_RDONLY), result = -1;
    while (fx >= 0 && fy >= 0) {
        long nx = read_full(fx, chunk_a, sizeof(chunk_a));
        long ny = read_full(fy, chunk_b, sizeof(chunk_b));
        if (nx < 0 || ny < 0)
            break;
        if (nx != ny || memcmp(chunk_a, chunk_b, (size_t)nx) != 0) {
            result = 1;
            break;
        }
        if (nx == 0) {
            result = 0;
            break;
        }
    }
    if (fx >= 0)
        close(fx);
    if (fy >= 0)
        close(fy);
    return result;
}

/* One version: its label, size and how it compares with the one before
 * (previous: that version's path, empty if there was none). */
static void show(const char *label, const char *path, char *previous)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        printf("  %-28s (not there)\n", label);
        previous[0] = 0;
        return;
    }
    const char *how = "first version";
    if (previous[0]) {
        int d = differ(previous, path);
        how = d < 0 ? "(cannot be read)" : d ? "changed" : "unchanged";
    }
    printf("  %-28s %10ld bytes  %s\n", label, (long)st.st_size, how);
    snprintf(previous, PATH_LENGTH, "%s", path);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        dprintf(STDERR_FILENO, "usage: versions PATH\n");
        return 2;
    }
    char path[PATH_LENGTH], cwd[PATH_LENGTH];
    if (argv[1][0] == '/')
        snprintf(path, sizeof(path), "%s", argv[1]);
    else if (getcwd(cwd, sizeof(cwd)))
        snprintf(path, sizeof(path), "%s%s%s", cwd, strcmp(cwd, "/") ? "/" : "", argv[1]);
    else
        return 1;
    if (strncmp(path, "/.snapshots/", 12) == 0 || strcmp(path, "/.snapshots") == 0) {
        dprintf(STDERR_FILENO, "versions: %s: give the path as it is now, not in a snapshot\n",
                argv[1]);
        return 1;
    }
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        dprintf(STDERR_FILENO, "versions: %s: %s\n", argv[1], strerror(EISDIR));
        return 1;
    }

    struct sys_snapshot s[MAX_LIST];
    int64_t n = ritteros_snapshot(SNAPSHOT_LIST, 0, s, sizeof(s));
    if (n < 0) {
        dprintf(STDERR_FILENO, "versions: %s\n", strerror(errno));
        return 1;
    }
    if (n > MAX_LIST)
        n = MAX_LIST;
    for (int64_t i = 1; i < n; i++)                 /* oldest first */
        for (int64_t k = i; k > 0 && s[k - 1].generation > s[k].generation; k--) {
            struct sys_snapshot t = s[k];
            s[k] = s[k - 1];
            s[k - 1] = t;
        }

    char previous[PATH_LENGTH] = "", version[PATH_LENGTH], label[64];
    printf("%s:\n", path);
    for (int64_t i = 0; i < n; i++) {
        snprintf(version, sizeof(version), "/.snapshots/%s%s", s[i].name, path);
        snprintf(label, sizeof(label), "%s (generation %lu)", s[i].name,
                 (unsigned long)s[i].generation);
        show(label, version, previous);
    }
    show("now", path, previous);
    return 0;
}

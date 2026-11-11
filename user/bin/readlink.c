/*
 * readlink [-f] PATH... (57a): print what the symbolic link at PATH names,
 * as it was written. -f: the full path PATH leads to, every link on the
 * way followed (the path itself need not be a link; its folder must
 * exist). Exit status 1 if one is not a link (without -f) or cannot be
 * read.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("readlink [-f] PATH...", "print what a symbolic link names (-f: the full path)");

#define LINKS_MAX 40

/* -f: the path with every link followed, as the kernel follows it. */
static int full_path(const char *path, char *out, size_t size)
{
    char work[1024], rest[1024];
    if (path[0] == '/')
        snprintf(work, sizeof(work), "%s", path);
    else {
        char cwd[512];
        if (!getcwd(cwd, sizeof(cwd)))
            return -1;
        snprintf(work, sizeof(work), "%s/%s", cwd, path);
    }
    size_t n = 0;
    out[0] = 0;
    int links = 0;
    const char *s = work;
    while (*s) {
        while (*s == '/')
            s++;
        if (!*s)
            break;
        size_t len = strcspn(s, "/");
        if (len == 1 && s[0] == '.') {
            s += len;
            continue;
        }
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            while (n > 0 && out[n - 1] != '/')
                n--;
            if (n > 0)
                n--;
            out[n] = 0;
            s += len;
            continue;
        }
        if (n + 1 + len >= size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        size_t before = n;
        out[n++] = '/';
        memcpy(out + n, s, len);
        n += len;
        out[n] = 0;
        s += len;
        struct stat st;
        if (lstat(out, &st) < 0) {
            if (*s && strspn(s, "/") < strlen(s))
                return -1;                          /* a folder on the way is missing */
            continue;
        }
        if (!S_ISLNK(st.st_mode))
            continue;
        if (++links > LINKS_MAX) {
            errno = ELOOP;
            return -1;
        }
        char target[256];
        ssize_t t = readlink(out, target, sizeof(target) - 1);
        if (t < 0)
            return -1;
        target[t] = 0;
        snprintf(rest, sizeof(rest), "%s%s", target, s);
        snprintf(work, sizeof(work), "%s", rest);
        s = work;
        n = target[0] == '/' ? 0 : before;
        out[n] = 0;
    }
    if (n == 0)
        snprintf(out, size, "/");
    return 0;
}

int main(int argc, char **argv)
{
    /* Options before the paths (50k: -f anywhere there, --). */
    bool full = false;
    int first = 1, status = 0;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        if (!strcmp(argv[first], "--")) {
            first++;
            break;
        }
        for (const char *c = argv[first] + 1; *c; c++) {
            if (*c != 'f') {
                dprintf(STDERR_FILENO, "usage: readlink [-f] PATH...\n");
                return 2;
            }
            full = true;
        }
    }
    if (first >= argc) {
        dprintf(STDERR_FILENO, "usage: readlink [-f] PATH...\n");
        return 2;
    }
    for (int i = first; i < argc; i++) {
        char out[512];
        if (full) {
            if (full_path(argv[i], out, sizeof(out)) < 0) {
                dprintf(STDERR_FILENO, "readlink: %s: %s\n", argv[i], strerror(errno));
                status = 1;
                continue;
            }
            printf("%s\n", out);
            continue;
        }
        ssize_t n = readlink(argv[i], out, sizeof(out) - 1);
        if (n < 0) {
            dprintf(STDERR_FILENO, "readlink: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        out[n] = 0;
        printf("%s\n", out);
    }
    return status;
}

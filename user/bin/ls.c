/*
 * ls [-l] [PATH]...: list directories (default: the current one), one entry
 * per line: the size, or <dir>, and the name (directories with a '/'), as
 * scripts/ritterfs.py ls does. A file is listed with its size, a symbolic
 * link as <link> with what it names (57a: `name -> target`). -l puts the
 * type and permissions, the number of names (hard links), the owner and
 * the group in front; a link named on the command line is then shown
 * itself, without -l the folder it names is listed.
 */
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("ls [-l] [PATH]...", "list directories (default: the current one)");

static bool longer;

/* -l: "drwxr-xr-x  links  uid  gid " in front of an entry. */
static void details(const struct stat *st)
{
    if (!longer)
        return;
    char m[11] = "-rwxrwxrwx";
    m[0] = S_ISDIR(st->st_mode) ? 'd' : S_ISLNK(st->st_mode) ? 'l' : S_ISBLK(st->st_mode) ? 'b'
         : S_ISCHR(st->st_mode) ? 'c' : '-';
    for (int i = 0; i < 9; i++)
        if (!(st->st_mode & (0400 >> i)))
            m[i + 1] = '-';
    if (st->st_mode & S_ISUID)                  /* setuid: runs as its owner (s, or S without x) */
        m[3] = m[3] == 'x' ? 's' : 'S';
    if (st->st_mode & S_ISVTX)                  /* (57b) sticky: t, or T without x */
        m[9] = m[9] == 'x' ? 't' : 'T';
    printf("%s %3u %5u %5u ", m, st->st_nlink, st->st_uid, st->st_gid);
}

/* A symbolic link's line: <link> (or its length, with -l) and its target. */
static void link_line(const char *path, const char *name, const struct stat *st)
{
    char target[256];
    ssize_t n = readlink(path, target, sizeof(target) - 1);
    target[n > 0 ? n : 0] = 0;
    details(st);
    if (longer)
        printf("%10ld  %s -> %s\n", (long)st->st_size, name, target);
    else
        printf("    <link>  %s -> %s\n", name, target);
}

static int list(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) {
        dprintf(STDERR_FILENO, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (S_ISLNK(st.st_mode) && longer) {
        link_line(path, path, &st);
        return 0;
    }
    if (S_ISLNK(st.st_mode) && stat(path, &st) < 0) {
        dprintf(STDERR_FILENO, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        details(&st);
        printf("%10ld  %s\n", (long)st.st_size, path);
        return 0;
    }
    DIR *dir = opendir(path);
    if (!dir) {
        dprintf(STDERR_FILENO, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    struct dirent *e;
    while ((e = readdir(dir))) {
        char full[512];
        snprintf(full, sizeof(full), "%s%s%s", path,
                 path[strlen(path) - 1] == '/' ? "" : "/", e->d_name);
        bool known = lstat(full, &st) == 0;
        if (known && S_ISLNK(st.st_mode)) {
            link_line(full, e->d_name, &st);
            continue;
        }
        if (known)
            details(&st);
        else if (longer)
            printf("?????????? %3s %5s %5s ", "?", "?", "?");
        if (e->d_type == DT_DIR)
            printf("     <dir>  %s/\n", e->d_name);
        else if (known)
            printf("%10ld  %s\n", (long)st.st_size, e->d_name);
        else
            printf("         ?  %s\n", e->d_name);
    }
    closedir(dir);
    return 0;
}

int main(int argc, char **argv)
{
    /* Options before the names (50k: -l anywhere there, -ll, --). */
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        if (!strcmp(argv[first], "--")) {
            first++;
            break;
        }
        for (const char *c = argv[first] + 1; *c; c++) {
            if (*c != 'l') {
                dprintf(STDERR_FILENO, "usage: ls [-l] [PATH]...\n");
                return 2;
            }
            longer = true;
        }
    }
    if (first == argc)
        return list(".");
    int status = 0;
    for (int i = first; i < argc; i++)
        status |= list(argv[i]);
    return status;
}

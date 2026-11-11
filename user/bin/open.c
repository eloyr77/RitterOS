/*
 * open FILE...: opens each file with its app (41g-2, the user's; macOS's
 * meaning), as owl's double click does: (42b) an app's bundle NAME.program
 * started (its program NAME), another folder in owl, a file in the
 * program the user's ~/.config/filetypes gives its ending (window_app_for,
 * 42a), in a terminal when that program is in bin; another
 * program in a terminal; anything else in the editor. The apps start on
 * their own (a session of their own) and open returns at once; they need
 * windowd running.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"
#include "window.h"

PROGRAM_HELP("open FILE...", "open files and folders with their apps (needs windowd)");

#if defined(__x86_64__)
#define ARCH "x86_64"
#else
#define ARCH "aarch64"
#endif

/* Start the program NAME (a windowed app's bundle in apps, else bin) with
 * argv. */
static int start(const char *name, char **argv)
{
    char path[192];
    struct stat st;
    snprintf(path, sizeof(path), "/system/" ARCH "/apps/%s.program/%s", name, name);   /* (42b) */
    if (stat(path, &st) != 0)
        snprintf(path, sizeof(path), "/system/" ARCH "/bin/%s", name);
    int fds[3] = { 0, 1, 2 };
    return ritteros_spawn(path, argv, SPAWN_NEW_GROUP | SPAWN_NEW_SESSION, fds) < 0 ? -1 : 0;
}

/* (42b) The bundle NAME.program that dir is, as NAME into name; 0 if dir is
 * no bundle (a folder of another name, or one without its program). */
static int bundle(const char *dir, char *name, size_t size)
{
    size_t n = strlen(dir);
    while (n > 1 && dir[n - 1] == '/')
        n--;
    size_t start = n;
    while (start > 0 && dir[start - 1] != '/')
        start--;
    if (n - start <= 8 || strncmp(dir + n - 8, ".program", 8) || n - start - 8 >= size)
        return 0;
    snprintf(name, size, "%.*s", (int)(n - start - 8), dir + start);
    char path[512];
    struct stat st;
    snprintf(path, sizeof(path), "%.*s/%s", (int)n, dir, name);
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* (42b) A bundle's own file other than its program (info, icon.png): data,
 * though installed with an x bit as everything in /system. */
static int app_file(const char *file)
{
    char dir[512], name[64];
    const char *base = strrchr(file, '/');
    if (!base)
        return 0;
    snprintf(dir, sizeof(dir), "%.*s", (int)(base - file), file);
    return bundle(dir, name, sizeof(name)) && strcmp(base + 1, name);
}

static int open_one(char *file)
{
    struct stat st;
    if (stat(file, &st) != 0) {
        dprintf(2, "open: %s: %s\n", file, strerror(errno));
        return -1;
    }
    char app[64];
    int how = S_ISDIR(st.st_mode) ? -1 : window_app_for(file, app, sizeof(app));
    int r;
    char name[64], path[512];
    if (S_ISDIR(st.st_mode) && bundle(file, name, sizeof(name))) {   /* (42b) an app: run it */
        snprintf(path, sizeof(path), "%s/%s", file, name);
        char *av[] = { name, 0 };
        int fds[3] = { 0, 1, 2 };
        r = ritteros_spawn(path, av, SPAWN_NEW_GROUP | SPAWN_NEW_SESSION, fds) < 0 ? -1 : 0;
        printf("open: %s started\n", file);
    } else if (S_ISDIR(st.st_mode)) {
        char *av[] = { "owl", file, 0 };
        r = start("owl", av);
        printf("open: %s in owl\n", file);
    } else if (how == 0) {                              /* its windowed app */
        char *av[] = { app, file, 0 };
        r = start(app, av);
        printf("open: %s in %s\n", file, app);
    } else if (how == 1) {                              /* its app, in a terminal */
        char *av[] = { "term", app, file, 0 };
        r = start("term", av);
        printf("open: %s in %s, in a terminal\n", file, app);
    } else if ((st.st_mode & 0111) && !app_file(file)) {   /* a program: run in a terminal */
        char *av[] = { "term", file, 0 };
        r = start("term", av);
        printf("open: %s run in a terminal\n", file);
    } else {                                            /* anything else: the editor */
        char *av[] = { "term", "edit", file, 0 };
        r = start("term", av);
        printf("open: %s in edit, in a terminal\n", file);
    }
    if (r < 0)
        dprintf(2, "open: %s: cannot start its app: %s\n", file, strerror(errno));
    return r;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(2, "usage: open FILE...\n");
        return 2;
    }
    int failed = 0;
    for (int i = 1; i < argc; i++)
        failed |= open_one(argv[i]) < 0;
    return failed;
}

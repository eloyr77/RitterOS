/*
 * The exec family (55b): the calling process runs another program
 * (SYS_EXECVE). execvp and execlp look a name without '/' up in PATH's
 * folders, or without PATH in /system/<arch>/bin then /apps (as the
 * shell; 42b: in each folder NAME, then an app bundle's NAME.program/NAME); a file the system will not start that is no binary runs as a
 * shell script (sh FILE ARGS), as POSIX's execvp does.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pathsearch.h"
#include "ritteros.h"

int execve(const char *path, char *const argv[], char *const envp[])
{
    int64_t r = syscall6(SYS_EXECVE, (int64_t)path, (int64_t)argv, (int64_t)envp, 0, 0, 0);
    errno = (int)-r;                            /* (it only comes back on an error) */
    return -1;
}

int execv(const char *path, char *const argv[])
{
    return execve(path, argv, environ);
}

bool __ritteros_is_script(const char *path)
{
    unsigned char b[512];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    ssize_t n = read(fd, b, sizeof(b));
    close(fd);
    if (n < 0 || (n >= 4 && !memcmp(b, "\177ELF", 4)))
        return false;
    return !memchr(b, 0, (size_t)n);
}

/* path, and for a script without #! sh with it. */
static int exec_or_script(const char *path, char *const argv[], char *const envp[])
{
    execve(path, argv, envp);
    if (errno != ENOEXEC || !__ritteros_is_script(path))
        return -1;
    int count = 0;
    while (argv[count])
        count++;
    char **again = malloc((size_t)(count + 2) * sizeof(char *));
    if (!again)
        return -1;
    again[0] = "sh";
    again[1] = (char *)path;
    for (int i = 1; i <= count; i++)
        again[i + 1] = argv[i];
    execve(SYSTEM_DIR "/bin/sh", again, envp);
    free(again);
    return -1;
}

int __ritteros_path_search(const char *name, int (*try)(const char *path, void *ctx), void *ctx)
{
    if (!*name)
        return ENOENT;
    const char *dirs = getenv("PATH");
    if (!dirs || !*dirs)
        dirs = SYSTEM_DIR "/bin:" SYSTEM_DIR "/apps";
    int seen = ENOENT;
    while (*dirs) {
        const char *end = strchr(dirs, ':');
        size_t n = end ? (size_t)(end - dirs) : strlen(dirs);
        char path[512];
        if (n + strlen(name) + 2 <= sizeof(path)) {
            if (n == 0)
                path[0] = '.', n = 1;               /* (an empty part: the current folder) */
            else
                memcpy(path, dirs, n);
            path[n] = '/';
            strcpy(path + n + 1, name);
            int e = try(path, ctx);
            if ((e == ENOENT || e == ENOTDIR) && n + 2 * strlen(name) + 11 <= sizeof(path)) {
                /* (42b) an app bundle there: NAME.program/NAME */
                char *at = path + n + 1 + strlen(name);
                strcpy(at, ".program/");
                strcpy(at + 9, name);
                e = try(path, ctx);
            }
            if (!e)
                return 0;
            if (e != ENOENT && e != ENOTDIR)
                seen = e;                       /* (EACCES and the like: remembered) */
        }
        dirs += n;
        if (*dirs == ':')
            dirs++;
    }
    return seen;
}

static int try_exec(const char *path, void *argv)
{
    exec_or_script(path, argv, environ);
    return errno;                               /* (it only comes back on an error) */
}

int execvp(const char *name, char *const argv[])
{
    if (strchr(name, '/'))
        return exec_or_script(name, argv, environ);
    errno = __ritteros_path_search(name, try_exec, (void *)argv);
    return -1;
}

/* The list forms: the arguments up to a null pointer into an array. The
 * list goes by pointer: execle reads envp after it, and on ARM a va_list
 * passed by value is a copy whose position the caller never sees. */
#define LIST_MAX 128

static int collect(const char *first, va_list *ap, char **argv)
{
    int n = 0;
    argv[n++] = (char *)first;
    char *a;
    while ((a = va_arg(*ap, char *)) && n < LIST_MAX - 1)
        argv[n++] = a;
    argv[n] = 0;
    if (a) {
        errno = E2BIG;
        return -1;
    }
    return n;
}

int execl(const char *path, const char *arg, ...)
{
    char *argv[LIST_MAX];
    va_list ap;
    va_start(ap, arg);
    int n = collect(arg, &ap, argv);
    va_end(ap);
    return n < 0 ? -1 : execve(path, argv, environ);
}

int execlp(const char *name, const char *arg, ...)
{
    char *argv[LIST_MAX];
    va_list ap;
    va_start(ap, arg);
    int n = collect(arg, &ap, argv);
    va_end(ap);
    return n < 0 ? -1 : execvp(name, argv);
}

int execle(const char *path, const char *arg, ...)
{
    char *argv[LIST_MAX];
    va_list ap;
    va_start(ap, arg);
    int n = collect(arg, &ap, argv);
    char *const *envp = n < 0 ? 0 : va_arg(ap, char *const *);  /* (after the null pointer) */
    va_end(ap);
    return n < 0 ? -1 : execve(path, argv, envp);
}

/*
 * env [-i] [NAME=VALUE]... [COMMAND [ARG]...] (22t): without a command, the
 * environment variables, one "NAME=value" per line; with one, COMMAND runs
 * with the variables given set (and with -i only those). A command named
 * without a '/' is looked up as the shell does: in /system/<arch>/bin, then
 * apps. Its exit code is the command's; 127 if it cannot be started.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("env [-i] [NAME=VALUE]... [COMMAND [ARG]...]",
             "show the environment variables, or run a command with some set");

#if defined(__x86_64__)
#define SYSTEM_DIR "/system/x86_64"
#else
#define SYSTEM_DIR "/system/aarch64"
#endif

/* (52g) Not a binary (no ELF header, no NUL in its first 512 bytes): a
 * file the kernel refuses (ENOEXEC) runs as a shell script, as POSIX's
 * execvp does. */
static bool is_script(const char *path)
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

/* spawn, and for a script without #! the shell with it: sh PATH ARGS. */
static pid_t spawn_or_script(const char *path, char **argv, int flags, const int fds[3])
{
    pid_t pid = ritteros_spawn(path, argv, flags, fds);
    if (pid >= 0 || errno != ENOEXEC || !is_script(path))
        return pid;
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
    pid = ritteros_spawn(SYSTEM_DIR "/bin/sh", again, flags, fds);
    free(again);
    return pid;
}

int main(int argc, char **argv)
{
    int i = 1;
    if (i < argc && !strcmp(argv[i], "-i")) {
        environ[0] = 0;                         /* (an empty environment, then the given) */
        i++;
    }
    for (; i < argc && strchr(argv[i], '=') && argv[i][0] != '='; i++)
        if (putenv(argv[i]) < 0) {
            dprintf(STDERR_FILENO, "env: %s: %s\n", argv[i], strerror(errno));
            return 125;
        }
    if (i == argc) {
        for (char **e = environ; *e; e++)
            printf("%s\n", *e);
        return 0;
    }
    char path[256];
    struct stat st;
    if (strchr(argv[i], '/')) {
        snprintf(path, sizeof(path), "%s", argv[i]);
    } else {
        snprintf(path, sizeof(path), SYSTEM_DIR "/bin/%s", argv[i]);
        if (stat(path, &st) < 0)
            snprintf(path, sizeof(path), SYSTEM_DIR "/apps/%s.program/%s", argv[i], argv[i]);   /* (42b) */
    }
    int fds[3] = { -1, -1, -1 };
    pid_t pid = spawn_or_script(path, argv + i, SPAWN_FOREGROUND, fds);
    if (pid < 0) {
        dprintf(STDERR_FILENO, "env: %s: %s\n", argv[i], errno == ENOENT ? "command not found" : strerror(errno));
        return 127;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

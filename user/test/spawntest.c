/*
 * Test: posix_spawn and posix_spawnp (55c). Lines "spawntest: ...".
 *   spawntest SELF DIR [own]   SELF: this program's path, DIR: a folder
 * Plain POSIX: the test builds it on the Mac too and compares the
 * "spawntest:" lines; "spawntest-own:" lines (with "own", not on the Mac)
 * are what only RitterOS does or reports. The
 * children are this program again: what they print comes back through a
 * pipe made their standard output by a file action.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static const char *self, *dir;

static const char *err(int e)
{
    return e == 0 ? "0" : e == ENOENT ? "ENOENT" : e == EACCES ? "EACCES" : e == ENOEXEC ? "ENOEXEC"
         : e == EBADF ? "EBADF" : e == EINVAL ? "EINVAL" : e == ENOTSUP ? "ENOTSUP"
         : e == EPERM ? "EPERM" : "another";
}

static int child(int argc, char **argv)
{
    const char *mode = argv[1];
    if (!strcmp(mode, "args")) {
        printf("child: %d args:", argc - 2);
        for (int i = 2; i < argc; i++)
            printf("%s%s", i == 2 ? " " : "|", argv[i]);
        printf("\n");
    } else if (!strcmp(mode, "env")) {
        const char *v = getenv("SPAWNTEST");
        printf("child: SPAWNTEST=%s\n", v ? v : "(none)");
    } else if (!strcmp(mode, "stderr")) {
        write(2, "child: to stderr\n", 17);
    } else if (!strcmp(mode, "stdin")) {
        char b[100];
        ssize_t n = read(0, b, sizeof(b) - 1);
        b[n > 0 ? n : 0] = 0;
        printf("child: read %s", b);
    } else if (!strcmp(mode, "group")) {
        printf("child: own group %s\n", getpgrp() == getpid() ? "yes" : "no");
    } else if (!strcmp(mode, "mask")) {
        sigset_t s;
        sigprocmask(SIG_BLOCK, 0, &s);
        printf("child: SIGUSR1 blocked %s, SIGUSR2 blocked %s\n",
               sigismember(&s, SIGUSR1) ? "yes" : "no", sigismember(&s, SIGUSR2) ? "yes" : "no");
    } else if (!strcmp(mode, "sigdef")) {
        struct sigaction a;
        sigaction(SIGUSR1, 0, &a);
        printf("child: SIGUSR1 at its default %s\n", a.sa_handler == SIG_DFL ? "yes" : "no");
    } else if (!strcmp(mode, "exit")) {
        return atoi(argv[2]);
    } else if (!strcmp(mode, "fd")) {               /* (56b) read descriptor N */
        char b[100];
        ssize_t n = read(atoi(argv[2]), b, sizeof(b) - 1);
        b[n > 0 ? n : 0] = 0;
        printf("child: the descriptor reads %s", n > 0 ? b : "nothing\n");
    } else if (!strcmp(mode, "closed")) {
        printf("child: the descriptor is %s\n", fcntl(atoi(argv[2]), F_GETFD) < 0 && errno == EBADF
                                               ? "closed" : "open");
    } else if (!strcmp(mode, "ignored")) {
        struct sigaction a;
        sigaction(SIGUSR2, 0, &a);
        printf("child: SIGUSR2 ignored %s\n", a.sa_handler == SIG_IGN ? "yes" : "no");
    } else if (!strcmp(mode, "pgrp")) {
        printf("child: in the first one's group %s\n", getpgrp() == atoi(argv[2]) ? "yes" : "no");
    } else if (!strcmp(mode, "ppid")) {
        printf("child: its parent is the caller %s\n", getppid() == atoi(argv[2]) ? "yes" : "no");
    } else if (!strcmp(mode, "wait")) {
        char c;
        while (read(0, &c, 1) > 0)
            ;
    }
    return 0;
}

/* Spawn with ACTIONS (made here if 0) plus stdout into a pipe; print what came. */
static void capture(const char *what, posix_spawn_file_actions_t *actions,
                    const posix_spawnattr_t *attr, char **argv, char **envp, int extra_dup2)
{
    int p[2];
    pipe(p);
    posix_spawn_file_actions_t own;
    if (!actions) {
        posix_spawn_file_actions_init(&own);
        actions = &own;
    }
    posix_spawn_file_actions_adddup2(actions, p[1], 1);
    if (extra_dup2)
        posix_spawn_file_actions_adddup2(actions, 1, 2);    /* (stderr to the same pipe) */
    pid_t pid;
    int e = posix_spawn(&pid, self, actions, attr, argv, envp ? envp : environ);
    close(p[1]);
    char out[300];
    size_t got = 0;
    ssize_t n;
    while ((n = read(p[0], out + got, sizeof(out) - 1 - got)) > 0)
        got += (size_t)n;
    out[got] = 0;
    close(p[0]);
    int status = -1;
    if (!e)
        waitpid(pid, &status, 0);
    if (got && out[got - 1] == '\n')
        out[got - 1] = 0;
    printf("spawntest: %s: %s, exit %d: %s\n", what, err(e),
           WIFEXITED(status) ? WEXITSTATUS(status) : -1, out);
    posix_spawn_file_actions_destroy(actions);
}

static int status_of(int e, pid_t pid)
{
    int status = -1;
    if (!e)
        waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void write_file(const char *path, const char *text, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    write(fd, text, strlen(text));
    close(fd);
    chmod(path, mode);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && (!strcmp(argv[1], "args") || !strcmp(argv[1], "env") ||
                      !strcmp(argv[1], "stderr") || !strcmp(argv[1], "stdin") ||
                      !strcmp(argv[1], "group") || !strcmp(argv[1], "mask") ||
                      !strcmp(argv[1], "sigdef") || !strcmp(argv[1], "exit") ||
                      !strcmp(argv[1], "fd") || !strcmp(argv[1], "closed") ||
                      !strcmp(argv[1], "ignored") || !strcmp(argv[1], "pgrp") ||
                      !strcmp(argv[1], "ppid") || !strcmp(argv[1], "wait")))
        return child(argc, argv);
    if (argc < 3) {
        printf("usage: spawntest SELF DIR [own]\n");
        return 2;
    }
    self = argv[1];
    dir = argv[2];
    char path[300], text[300];

    char *args[] = { "spawntest", "args", "a", "b c", 0 };
    capture("arguments", 0, 0, args, 0, 0);
    char *env_argv[] = { "spawntest", "env", 0 };
    char *envp[] = { "SPAWNTEST=given", 0 };
    capture("an environment of its own", 0, 0, env_argv, envp, 0);

    posix_spawn_file_actions_t a;
    snprintf(path, sizeof(path), "%s/spawn-out.txt", dir);
    posix_spawn_file_actions_init(&a);
    posix_spawn_file_actions_addclose(&a, 1);
    posix_spawn_file_actions_addopen(&a, 1, path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char *file_args[] = { "spawntest", "args", "into a file", 0 };
    pid_t pid;
    int e = posix_spawn(&pid, self, &a, 0, file_args, environ);
    int code = status_of(e, pid);
    posix_spawn_file_actions_destroy(&a);
    int fd = open(path, O_RDONLY);
    ssize_t n = fd < 0 ? 0 : read(fd, text, sizeof(text) - 1);
    text[n > 0 ? n : 0] = 0;
    if (fd >= 0)
        close(fd);
    printf("spawntest: close and open stdout: %s, exit %d, the file: %s", err(e), code, text);

    char *err_argv[] = { "spawntest", "stderr", 0 };
    capture("stderr duplicated onto stdout", 0, 0, err_argv, 0, 1);

    snprintf(path, sizeof(path), "%s/spawn-in.txt", dir);
    write_file(path, "the input file\n", 0644);
    posix_spawn_file_actions_init(&a);
    posix_spawn_file_actions_addopen(&a, 0, path, O_RDONLY, 0);
    char *in_argv[] = { "spawntest", "stdin", 0 };
    capture("stdin opened from a file", &a, 0, in_argv, 0, 0);

    posix_spawnattr_t attr;
    char *group_argv[] = { "spawntest", "group", 0 };
    capture("no attributes", 0, 0, group_argv, 0, 0);
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    capture("POSIX_SPAWN_SETPGROUP 0", 0, &attr, group_argv, 0, 0);
    posix_spawnattr_destroy(&attr);

    sigset_t s, after;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR2);
    sigprocmask(SIG_BLOCK, &s, 0);
    char *mask_argv[] = { "spawntest", "mask", 0 };
    capture("the caller's blocked signals", 0, 0, mask_argv, 0, 0);
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);
    sigemptyset(&s);
    sigaddset(&s, SIGUSR1);
    posix_spawnattr_setsigmask(&attr, &s);
    capture("POSIX_SPAWN_SETSIGMASK", 0, &attr, mask_argv, 0, 0);
    posix_spawnattr_destroy(&attr);
    sigprocmask(SIG_BLOCK, 0, &after);
    printf("spawntest: the caller's own set after: SIGUSR1 %s, SIGUSR2 %s\n",
           sigismember(&after, SIGUSR1) ? "blocked" : "not blocked",
           sigismember(&after, SIGUSR2) ? "blocked" : "not blocked");

    signal(SIGUSR1, SIG_IGN);
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF);
    sigemptyset(&s);
    sigaddset(&s, SIGUSR1);
    posix_spawnattr_setsigdefault(&attr, &s);
    char *def_argv[] = { "spawntest", "sigdef", 0 };
    capture("POSIX_SPAWN_SETSIGDEF", 0, &attr, def_argv, 0, 0);
    posix_spawnattr_destroy(&attr);
    signal(SIGUSR1, SIG_DFL);

    short flags;
    pid_t group;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK);
    posix_spawnattr_setpgroup(&attr, 7);
    posix_spawnattr_getflags(&attr, &flags);
    posix_spawnattr_getpgroup(&attr, &group);
    printf("spawntest: attributes kept: flags %s, group %d\n",
           flags == (POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK) ? "the same" : "changed",
           (int)group);
    posix_spawnattr_destroy(&attr);

    char *exit_argv[] = { "spawntest", "exit", "3", 0 };
    e = posix_spawn(&pid, self, 0, 0, exit_argv, environ);
    printf("spawntest: a child's exit code: %s, %d\n", err(e), status_of(e, pid));
    char *sh_argv[] = { "sh", "-c", "exit 4", 0 };
    e = posix_spawnp(&pid, "sh", 0, 0, sh_argv, environ);
    printf("spawntest: posix_spawnp by name: %s, exit %d\n", err(e), status_of(e, pid));

    char *x_argv[] = { "x", 0 };
    printf("spawntest: nothing there: %s\n", err(posix_spawn(&pid, "/nonexistent", 0, 0, x_argv,
                                                              environ)));
    snprintf(path, sizeof(path), "%s/spawn-plain.txt", dir);
    write_file(path, "not a program\n", 0644);
    printf("spawntest: a file without x: %s\n", err(posix_spawn(&pid, path, 0, 0, x_argv,
                                                                 environ)));
    char script[300];
    snprintf(script, sizeof(script), "%s/spawn-script.sh", dir);
    write_file(script, "exit 5\n", 0755);
    printf("spawntest: a script without #!: %s\n", err(posix_spawn(&pid, script, 0, 0, x_argv,
                                                                    environ)));
    posix_spawn_file_actions_init(&a);
    posix_spawn_file_actions_adddup2(&a, 40, 1);
    printf("spawntest: duplicating a closed descriptor: %s\n",
           err(posix_spawn(&pid, self, &a, 0, exit_argv, environ)));
    posix_spawn_file_actions_destroy(&a);
    /* (56b, by fork and exec) Any descriptor, inherited ones, ignored
     * signals, an existing group, the parent. */
    snprintf(path, sizeof(path), "%s/spawn-in.txt", dir);
    posix_spawn_file_actions_init(&a);
    posix_spawn_file_actions_addopen(&a, 5, path, O_RDONLY, 0);
    char *fd5_argv[] = { "spawntest", "fd", "5", 0 };
    capture("a file action for descriptor 5", &a, 0, fd5_argv, 0, 0);
    posix_spawn_file_actions_init(&a);
    posix_spawn_file_actions_addclose(&a, 0);
    char *closed0_argv[] = { "spawntest", "closed", "0", 0 };
    capture("stdin closed", &a, 0, closed0_argv, 0, 0);
    int kept = open(path, O_RDONLY), closing = open(path, O_RDONLY);
    fcntl(closing, F_SETFD, FD_CLOEXEC);
    char kept_text[16], closing_text[16];
    snprintf(kept_text, sizeof(kept_text), "%d", kept);
    snprintf(closing_text, sizeof(closing_text), "%d", closing);
    char *kept_argv[] = { "spawntest", "fd", kept_text, 0 };
    capture("a descriptor of the caller's", 0, 0, kept_argv, 0, 0);
    char *closing_argv[] = { "spawntest", "closed", closing_text, 0 };
    capture("one marked FD_CLOEXEC", 0, 0, closing_argv, 0, 0);
    close(kept);
    close(closing);
    signal(SIGUSR2, SIG_IGN);
    char *ign_argv[] = { "spawntest", "ignored", 0 };
    capture("an ignored signal", 0, 0, ign_argv, 0, 0);
    signal(SIGUSR2, SIG_DFL);
    int hold[2];
    pipe(hold);
    fcntl(hold[1], F_SETFD, FD_CLOEXEC);            /* (or the waiting child holds its own end) */
    posix_spawn_file_actions_init(&a);
    posix_spawn_file_actions_adddup2(&a, hold[0], 0);
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    char *wait_argv[] = { "spawntest", "wait", 0 };
    pid_t first;
    e = posix_spawn(&first, self, &a, &attr, wait_argv, environ);
    posix_spawn_file_actions_destroy(&a);
    close(hold[0]);
    char first_text[16];
    snprintf(first_text, sizeof(first_text), "%d", (int)first);
    posix_spawnattr_setpgroup(&attr, first);
    char *pgrp_argv[] = { "spawntest", "pgrp", first_text, 0 };
    capture("POSIX_SPAWN_SETPGROUP of another child's group", 0, &attr, pgrp_argv, 0, 0);
    posix_spawnattr_destroy(&attr);
    close(hold[1]);
    printf("spawntest: the first one: %s, exit %d\n", err(e), status_of(e, first));
    char me_text[16];
    snprintf(me_text, sizeof(me_text), "%d", (int)getpid());
    char *ppid_argv[] = { "spawntest", "ppid", me_text, 0 };
    capture("getppid", 0, 0, ppid_argv, 0, 0);

    posix_spawn_file_actions_init(&a);
    printf("spawntest: addclose(-1): %s\n", err(posix_spawn_file_actions_addclose(&a, -1)));
    posix_spawn_file_actions_destroy(&a);

#ifndef __APPLE__
    if (argc == 4 && !strcmp(argv[3], "own")) {
        e = posix_spawnp(&pid, script, 0, 0, x_argv, environ);
        printf("spawntest-own: posix_spawnp of a script without #!: %s, exit %d\n", err(e),
               status_of(e, pid));
        posix_spawnattr_init(&attr);
        printf("spawntest-own: an unknown flag: %s\n", err(posix_spawnattr_setflags(&attr, 0x4000)));
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_RESETIDS);
        e = posix_spawn(&pid, self, 0, &attr, exit_argv, environ);
        printf("spawntest-own: POSIX_SPAWN_RESETIDS as the real user: %s, exit %d\n", err(e),
               status_of(e, pid));
    }
#endif
    printf("spawntest: done\n");
    return 0;
}

/*
 * posix_spawn (55c; since 56b fork + exec, as POSIX describes it): the
 * child takes the attributes and does the file actions in order, then
 * runs the program; what fails there (an action, the exec) goes back to
 * the caller through a pipe that the exec closes (FD_CLOEXEC), and the
 * child ends with 127. The child must not allocate: another thread of the
 * caller may hold malloc's lock, and the child has only this thread (so
 * the raw system call, not fork(): no atfork handlers either). All signals
 * are blocked across the fork, so no handler runs in the child before the
 * exec; it sets them to their default first.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "pathsearch.h"
#include "ritteros.h"

enum { ACT_OPEN, ACT_DUP2, ACT_CLOSE };

struct __spawn_action {
    int kind, fd, newfd, oflag;
    mode_t mode;
    char *path;
};

#define FLAGS_KNOWN (POSIX_SPAWN_RESETIDS | POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | \
                     POSIX_SPAWN_SETSIGMASK)

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *a)
{
    a->count = a->room = 0;
    a->actions = 0;
    return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *a)
{
    for (int i = 0; i < a->count; i++)
        free(a->actions[i].path);
    free(a->actions);
    return posix_spawn_file_actions_init(a);
}

static int add(posix_spawn_file_actions_t *a, struct __spawn_action act)
{
    if (act.fd < 0 || act.fd >= FD_MAX || (act.kind == ACT_DUP2 && (act.newfd < 0 ||
                                                                    act.newfd >= FD_MAX)))
        return EBADF;
    if (a->count == a->room) {
        int room = a->room ? a->room * 2 : 4;
        struct __spawn_action *more = realloc(a->actions, (size_t)room * sizeof(*more));
        if (!more)
            return ENOMEM;
        a->actions = more;
        a->room = room;
    }
    a->actions[a->count++] = act;
    return 0;
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *a, int fd, const char *path,
                                     int oflag, mode_t mode)
{
    char *copy = strdup(path);
    if (!copy)
        return ENOMEM;
    int e = add(a, (struct __spawn_action){ ACT_OPEN, fd, 0, oflag, mode, copy });
    if (e)
        free(copy);
    return e;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *a, int fd, int newfd)
{
    return add(a, (struct __spawn_action){ ACT_DUP2, fd, newfd, 0, 0, 0 });
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *a, int fd)
{
    return add(a, (struct __spawn_action){ ACT_CLOSE, fd, 0, 0, 0, 0 });
}

int posix_spawnattr_init(posix_spawnattr_t *attr)
{
    memset(attr, 0, sizeof(*attr));
    return 0;
}

int posix_spawnattr_destroy(posix_spawnattr_t *attr)
{
    (void)attr;
    return 0;
}

int posix_spawnattr_getflags(const posix_spawnattr_t *attr, short *flags)
{
    *flags = attr->flags;
    return 0;
}

int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags)
{
    if (flags & ~FLAGS_KNOWN)
        return EINVAL;
    attr->flags = flags;
    return 0;
}

int posix_spawnattr_getpgroup(const posix_spawnattr_t *attr, pid_t *pgroup)
{
    *pgroup = attr->pgroup;
    return 0;
}

int posix_spawnattr_setpgroup(posix_spawnattr_t *attr, pid_t pgroup)
{
    attr->pgroup = pgroup;
    return 0;
}

int posix_spawnattr_getsigmask(const posix_spawnattr_t *attr, sigset_t *mask)
{
    *mask = attr->sigmask;
    return 0;
}

int posix_spawnattr_setsigmask(posix_spawnattr_t *attr, const sigset_t *mask)
{
    attr->sigmask = *mask;
    return 0;
}

int posix_spawnattr_getsigdefault(const posix_spawnattr_t *attr, sigset_t *signals)
{
    *signals = attr->sigdefault;
    return 0;
}

int posix_spawnattr_setsigdefault(posix_spawnattr_t *attr, const sigset_t *signals)
{
    attr->sigdefault = *signals;
    return 0;
}

/* The child's side: never returns. */
static __attribute__((noreturn)) void child(int report, const char *file, bool search,
                                            const posix_spawn_file_actions_t *a,
                                            const posix_spawnattr_t *attr, const sigset_t *mask,
                                            char *const argv[], char *const envp[]);

static void fail(int report, int e)
{
    write(report, &e, sizeof(e));
    _exit(127);
}

/* The report pipe's write end, moved where no file action will reach it. */
static int out_of_the_way(int fd, const posix_spawn_file_actions_t *a)
{
    for (int n = FD_MAX - 1; n > 2; n--) {
        bool used = fcntl(n, F_GETFD) >= 0;
        for (int i = 0; a && i < a->count && !used; i++)
            used = a->actions[i].fd == n || (a->actions[i].kind == ACT_DUP2 &&
                                             a->actions[i].newfd == n);
        if (used)
            continue;
        if (dup2(fd, n) < 0)
            return fd;
        fcntl(n, F_SETFD, FD_CLOEXEC);
        close(fd);
        return n;
    }
    return fd;
}

struct attempt {
    bool search;                                    /* posix_spawnp: scripts through sh too */
    char *const *argv, *const *envp;
};

/* path, and (posix_spawnp) sh with it for a script without #!: an error
 * number if neither started. */
static int try_exec(const char *path, void *ctx)
{
    struct attempt *at = ctx;
    execve(path, (char *const *)at->argv, (char *const *)at->envp);
    int e = errno;
    if (e != ENOEXEC || !at->search || !__ritteros_is_script(path))
        return e;
    int count = 0;
    while (at->argv[count])
        count++;
    char *again[count + 2];                         /* (no malloc in the child) */
    again[0] = "sh";
    again[1] = (char *)path;
    for (int i = 1; i <= count; i++)
        again[i + 1] = at->argv[i];
    execve(SYSTEM_DIR "/bin/sh", again, (char *const *)at->envp);
    return errno;
}

static void child(int report, const char *file, bool search, const posix_spawn_file_actions_t *a,
                  const posix_spawnattr_t *attr, const sigset_t *mask, char *const argv[],
                  char *const envp[])
{
    report = out_of_the_way(report, a);
    for (int s = 1; s < 64; s++) {                  /* handlers would not survive the exec */
        struct sigaction now;
        if (sigaction(s, 0, &now) == 0 && now.sa_handler != SIG_DFL && now.sa_handler != SIG_IGN)
            signal(s, SIG_DFL);
    }
    short flags = attr ? attr->flags : 0;
    if (flags & POSIX_SPAWN_SETSIGDEF)
        for (int s = 1; s < 64; s++)
            if (sigismember(&attr->sigdefault, s))
                signal(s, SIG_DFL);
    if ((flags & POSIX_SPAWN_SETPGROUP) && setpgid(0, attr->pgroup) < 0)
        fail(report, errno);
    if ((flags & POSIX_SPAWN_RESETIDS) && (setgid(getgid()) < 0 || setuid(getuid()) < 0))
        fail(report, errno);
    for (int i = 0; a && i < a->count; i++) {
        struct __spawn_action *act = &a->actions[i];
        if (act->kind == ACT_OPEN) {
            int fd = open(act->path, act->oflag, act->mode);
            if (fd < 0)
                fail(report, errno);
            if (fd != act->fd) {
                if (dup2(fd, act->fd) < 0)
                    fail(report, errno);
                close(fd);
            }
        } else if (act->kind == ACT_DUP2) {
            if (act->fd == act->newfd) {            /* (POSIX: the same one, now passed on) */
                int fl = fcntl(act->fd, F_GETFD);
                if (fl < 0 || fcntl(act->fd, F_SETFD, fl & ~FD_CLOEXEC) < 0)
                    fail(report, EBADF);
            } else if (dup2(act->fd, act->newfd) < 0) {
                fail(report, errno);
            }
        } else {
            close(act->fd);                         /* (one not open: no error, as POSIX 2024) */
        }
    }
    sigprocmask(SIG_SETMASK, (flags & POSIX_SPAWN_SETSIGMASK) ? &attr->sigmask : mask, 0);
    struct attempt at = { search, argv, envp };
    int e = !search || strchr(file, '/') ? try_exec(file, &at)
                                          : __ritteros_path_search(file, try_exec, &at);
    fail(report, e);
    _exit(127);
}

static int run(pid_t *pid, const char *file, const posix_spawn_file_actions_t *actions,
               const posix_spawnattr_t *attr, char *const argv[], char *const envp[],
               bool search)
{
    if (attr && (attr->flags & ~FLAGS_KNOWN))
        return EINVAL;
    if (attr && (attr->flags & POSIX_SPAWN_RESETIDS) && geteuid() != getuid() && geteuid() != 0)
        return EPERM;                               /* (giving up a user is root's) */
    if (!envp)
        envp = environ;
    int report[2];
    if (pipe(report) < 0)
        return errno;
    fcntl(report[0], F_SETFD, FD_CLOEXEC);
    fcntl(report[1], F_SETFD, FD_CLOEXEC);
    sigset_t all, old;
    sigfillset(&all);
    sigprocmask(SIG_SETMASK, &all, &old);
    int64_t r = syscall6(SYS_FORK, 0, 0, 0, 0, 0, 0);
    if (r == 0) {
        close(report[0]);
        child(report[1], file, search, actions, attr, &old, argv, envp);
    }
    sigprocmask(SIG_SETMASK, &old, 0);
    close(report[1]);
    if (r < 0) {
        close(report[0]);
        return (int)-r;
    }
    int e = 0;
    ssize_t n;
    while ((n = read(report[0], &e, sizeof(e))) < 0 && errno == EINTR)
        ;
    close(report[0]);
    if (n == (ssize_t)sizeof(e)) {                  /* it never ran the program */
        int status;
        while (waitpid((pid_t)r, &status, 0) < 0 && errno == EINTR)
            ;
        return e;
    }
    if (pid)
        *pid = (pid_t)r;
    return 0;
}

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                const posix_spawnattr_t *attr, char *const argv[], char *const envp[])
{
    return run(pid, path, actions, attr, argv, envp, false);
}

int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *actions,
                 const posix_spawnattr_t *attr, char *const argv[], char *const envp[])
{
    return run(pid, file, actions, attr, argv, envp, true);
}

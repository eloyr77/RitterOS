/* The system call wrappers: the kernel returns -ERR_*, C returns -1 and
 * puts the error number into errno. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/time.h>
#include <utime.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <thread.h>
#include <unistd.h>
#include "cancel.h"
#include "threadsize.h"

static int64_t check(int64_t result)
{
    if (result < 0) {
        errno = (int)-result;
        return -1;
    }
    return result;
}

ssize_t read(int fd, void *buffer, size_t size)
{
    cancel_point();                             /* (54c: a cancellation point) */
    return check(cancel_after(syscall6(SYS_READ, fd, (int64_t)buffer, (int64_t)size, 0, 0, 0)));
}

ssize_t write(int fd, const void *buffer, size_t size)
{
    cancel_point();
    return check(cancel_after(syscall6(SYS_WRITE, fd, (int64_t)buffer, (int64_t)size, 0, 0, 0)));
}

int open(const char *path, int flags, ...)
{
    unsigned mode = 0;
    if (flags & O_CREAT) {                      /* (52b) O_CREAT's mode, as POSIX's */
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, unsigned);
        va_end(ap);
        flags |= OPEN_MODE;
    }
    return (int)check(syscall6(SYS_OPEN, (int64_t)path, flags, mode, 0, 0, 0));
}

int fcntl(int fd, int command, ...)
{
    va_list ap;
    va_start(ap, command);
    long arg = command == F_GETFD || command == F_GETFL ? 0 : va_arg(ap, long);  /* (53b) */
    va_end(ap);
    return (int)check(syscall6(SYS_FCNTL, fd, command, arg, 0, 0, 0));
}

int creat(const char *path, mode_t mode)
{
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int close(int fd)
{
    return (int)check(syscall6(SYS_CLOSE, fd, 0, 0, 0, 0, 0));
}

int dup(int fd)
{
    return (int)check(syscall6(SYS_DUP2, fd, (int64_t)-1, 0, 0, 0, 0));
}

int dup2(int fd, int newfd)
{
    if (newfd < 0) {
        errno = EBADF;                  /* (-1 means dup to the kernel) */
        return -1;
    }
    return (int)check(syscall6(SYS_DUP2, fd, newfd, 0, 0, 0, 0));
}

int pipe(int fds[2])
{
    return (int)check(syscall6(SYS_PIPE, (int64_t)fds, 0, 0, 0, 0, 0));
}

int ritteros_pipe(int fds[2])
{
    return (int)check(syscall6(SYS_PIPE, (int64_t)fds, 0, 0, 0, 0, 0));
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    int64_t r = check(syscall6(SYS_MMAP, (int64_t)addr, (int64_t)length, prot, flags, fd, offset));
    return r < 0 ? MAP_FAILED : (void *)r;
}

int munmap(void *addr, size_t length)
{
    return (int)check(syscall6(SYS_MUNMAP, (int64_t)addr, (int64_t)length, 0, 0, 0, 0));
}

int msync(void *addr, size_t length, int flags)
{
    return (int)check(syscall6(SYS_MSYNC, (int64_t)addr, (int64_t)length, flags, 0, 0, 0));
}

int mprotect(void *addr, size_t length, int prot)
{
    return (int)check(syscall6(SYS_MPROTECT, (int64_t)addr, (int64_t)length, prot, 0, 0, 0));
}

pid_t getpid(void)
{
    return (pid_t)syscall6(SYS_GETPID, 0, 0, 0, 0, 0, 0);
}

int usleep(useconds_t microseconds)
{
    cancel_point();
    return (int)check(cancel_after(syscall6(SYS_SLEEP, (microseconds + 999) / 1000, 0, 0, 0, 0, 0)));
}

unsigned int sleep(unsigned int seconds)
{
    cancel_point();
    cancel_after(syscall6(SYS_SLEEP, (int64_t)seconds * 1000, 0, 0, 0, 0, 0));
    return 0;
}

pid_t spawn(const char *path, char *const argv[])
{
    return (pid_t)check(syscall6(SYS_SPAWN_ENV, (int64_t)path, (int64_t)argv,
                                 (int64_t)*__ritteros_environ(), 0, 0, 0));
}

/* (22t) Every spawn passes an environment: the caller's own, unless given. */
int ritteros_spawn_env(const char *path, char *const argv[], char *const envp[], int flags,
                       const int fds[3])
{
    int32_t copy[3] = { fds[0], fds[1], fds[2] };
    return (int)check(syscall6(SYS_SPAWN_ENV, (int64_t)path, (int64_t)argv, (int64_t)envp,
                               flags, (int64_t)copy, 0));
}

int ritteros_spawn(const char *path, char *const argv[], int flags, const int fds[3])
{
    return ritteros_spawn_env(path, argv, *__ritteros_environ(), flags, fds);
}

int ritteros_spawn_as(const char *path, char *const argv[], int flags, const int fds[3],
                      uid_t uid, gid_t gid)
{
    int32_t copy[3] = { fds[0], fds[1], fds[2] };
    return (int)check(syscall6(SYS_SPAWN_ENV, (int64_t)path, (int64_t)argv,
                               (int64_t)*__ritteros_environ(), flags | SPAWN_AS, (int64_t)copy,
                               (int64_t)uid | (int64_t)gid << 32));
}

pid_t spawn_new_group(const char *path, char *const argv[])
{
    return (pid_t)check(syscall6(SYS_SPAWN_ENV, (int64_t)path, (int64_t)argv,
                                 (int64_t)*__ritteros_environ(), SPAWN_NEW_GROUP, 0, 0));
}

pid_t waitpid(pid_t pid, int *status, int flags)
{
    if (flags & ~WNOHANG) {
        errno = EINVAL;
        return -1;
    }
    cancel_point();
    return (pid_t)check(cancel_after(syscall6(SYS_WAIT, pid, (int64_t)status,
                                              flags & WNOHANG ? WAIT_NO_HANG : 0, 0, 0, 0)));
}

int chdir(const char *path)
{
    return (int)check(syscall6(SYS_CHDIR, (int64_t)path, 0, 0, 0, 0, 0));
}

char *getcwd(char *buffer, size_t size)
{
    return check(syscall6(SYS_GETCWD, (int64_t)buffer, (int64_t)size, 0, 0, 0, 0)) < 0
           ? 0 : buffer;
}

pid_t getpgrp(void)
{
    return (pid_t)syscall6(SYS_GETPGRP, 0, 0, 0, 0, 0, 0);
}

pid_t getppid(void)
{
    return (pid_t)syscall6(SYS_GETPPID, 0, 0, 0, 0, 0, 0);
}

int setpgid(pid_t pid, pid_t group)
{
    if (group < 0) {
        errno = EINVAL;
        return -1;
    }
    return (int)check(syscall6(SYS_SETPGID, pid, group, 0, 0, 0, 0));
}

pid_t getpgid(pid_t pid)
{
    return (pid_t)check(syscall6(SYS_SETPGID, pid, -1, 0, 0, 0, 0));
}

pid_t setsid(void)
{
    return (pid_t)check(syscall6(SYS_SETSID, 0, 0, 0, 0, 0, 0));
}

pid_t tcgetpgrp(int fd)
{
    (void)fd;                                   /* there is one terminal */
    return (pid_t)check(syscall6(SYS_FOREGROUND, 0, 0, 0, 0, 0, 0));
}

int tcsetpgrp(int fd, pid_t group)
{
    (void)fd;
    if (group <= 0) {
        errno = EINVAL;
        return -1;
    }
    return check(syscall6(SYS_FOREGROUND, group, 0, 0, 0, 0, 0)) < 0 ? -1 : 0;
}

/* (22r) Handlers: the library's restorer (syscall.S) is where they return. */
void __ritteros_signal_return(void);

int sigaction(int sig, const struct sigaction *action, struct sigaction *old)
{
    struct sys_sigaction want, had;
    if (action) {
        want.handler = action->sa_handler == SIG_DFL ? SIGNAL_DEFAULT
                     : action->sa_handler == SIG_IGN ? SIGNAL_IGNORE
                     : (uint64_t)(uintptr_t)action->sa_handler;
        want.mask = action->sa_mask;
        want.flags = (action->sa_flags & SA_RESETHAND ? SIGACTION_RESETHAND : 0) |
                     (action->sa_flags & SA_NODEFER ? SIGACTION_NODEFER : 0);
        want.reserved = 0;
    }
    int64_t r = syscall6(SYS_SIGACTION, sig, action ? (int64_t)(uintptr_t)&want : 0,
                         old ? (int64_t)(uintptr_t)&had : 0,
                         (int64_t)(uintptr_t)__ritteros_signal_return, 0, 0);
    if (check(r) < 0)
        return -1;
    if (old) {
        old->sa_handler = had.handler == SIGNAL_DEFAULT ? SIG_DFL
                        : had.handler == SIGNAL_IGNORE ? SIG_IGN
                        : (sighandler_t)(uintptr_t)had.handler;
        old->sa_mask = had.mask;
        old->sa_flags = (had.flags & SIGACTION_RESETHAND ? SA_RESETHAND : 0) |
                        (had.flags & SIGACTION_NODEFER ? SA_NODEFER : 0);
    }
    return 0;
}

/* As glibc's and the BSDs': the handler stays, its signal is held back
 * while it runs. */
sighandler_t signal(int sig, sighandler_t action)
{
    struct sigaction a = { .sa_handler = action }, old;
    if (action == SIG_ERR) {
        errno = EINVAL;
        return SIG_ERR;
    }
    return sigaction(sig, &a, &old) < 0 ? SIG_ERR : old.sa_handler;
}

int raise(int sig)
{
    return kill(getpid(), sig);             /* (its handler has run when kill returns) */
}

int sigemptyset(sigset_t *set)
{
    *set = 0;
    return 0;
}

int sigfillset(sigset_t *set)
{
    *set = ~(sigset_t)1;                    /* (signals are 1 to 63) */
    return 0;
}

static bool signal_number(int sig)
{
    if (sig > 0 && sig < 64)
        return true;
    errno = EINVAL;
    return false;
}

int sigaddset(sigset_t *set, int sig)
{
    if (!signal_number(sig))
        return -1;
    *set |= (sigset_t)1 << sig;
    return 0;
}

int sigdelset(sigset_t *set, int sig)
{
    if (!signal_number(sig))
        return -1;
    *set &= ~((sigset_t)1 << sig);
    return 0;
}

int sigismember(const sigset_t *set, int sig)
{
    if (!signal_number(sig))
        return -1;
    return (int)(*set >> sig & 1);
}

int sigpending(sigset_t *set)
{
    return (int)check(syscall6(SYS_SIGPENDING, (int64_t)set, 0, 0, 0, 0, 0));
}

int sigsuspend(const sigset_t *mask)
{
    cancel_point();
    return (int)check(cancel_after(syscall6(SYS_SIGSUSPEND, (int64_t)mask, 0, 0, 0, 0, 0)));
}

int sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
    _Static_assert(sizeof(sigset_t) == sizeof(uint64_t), "the kernel's set");
    return (int)check(syscall6(SYS_SIGPROCMASK, how, (int64_t)set, (int64_t)old, 0, 0, 0));
}

static void stat_of(const struct sys_stat *s, struct stat *st)
{
    static const unsigned kinds[] = {
        [FILE_TYPE_FILE] = S_IFREG, [FILE_TYPE_DIR] = S_IFDIR, [FILE_TYPE_BLOCK] = S_IFBLK,
        [FILE_TYPE_CHAR] = S_IFCHR, [FILE_TYPE_FIFO] = S_IFIFO, [FILE_TYPE_SOCKET] = S_IFSOCK,
        [FILE_TYPE_SYMLINK] = S_IFLNK,
    };
    st->st_ino = s->id;
    st->st_dev = s->device;
    st->st_mode = (s->type < sizeof(kinds) / sizeof(kinds[0]) && kinds[s->type] ? kinds[s->type]
                                                                              : S_IFREG) |
                  (s->mode & 05777);   /* (setuid too, 50b; sticky, 57b) */
    st->st_uid = s->uid;
    st->st_gid = s->gid;
    st->st_nlink = s->links;
    st->st_size = (int64_t)s->size;
    st->st_ctime = (int64_t)(s->created / 1000000000);
    st->st_mtime = (int64_t)(s->modified / 1000000000);
}

int poll(struct pollfd *fds, nfds_t count, int timeout)
{
    _Static_assert(sizeof(struct pollfd) == sizeof(struct sys_pollfd), "the kernel's layout");
    cancel_point();
    return (int)check(cancel_after(syscall6(SYS_POLL, (int64_t)fds, (int64_t)count, timeout, 0, 0, 0)));
}

mode_t umask(mode_t mask)
{
    return (mode_t)syscall6(SYS_UMASK, mask, 0, 0, 0, 0, 0);
}

int fstat(int fd, struct stat *st)
{
    struct sys_stat s;
    if (check(syscall6(SYS_FSTAT, fd, (int64_t)&s, 0, 0, 0, 0)) < 0)
        return -1;
    stat_of(&s, st);
    return 0;
}

/* (63m) utimensat and its older forms: SYS_UTIMENS. */
static int utimens(const char *path, int fd, const struct timespec *times, int flags)
{
    struct sys_timespec t[2];
    for (int i = 0; times && i < 2; i++) {
        t[i].sec = times[i].tv_sec;
        t[i].nsec = times[i].tv_nsec == UTIME_NOW ? UTIMENS_NOW
                  : times[i].tv_nsec == UTIME_OMIT ? UTIMENS_OMIT : times[i].tv_nsec;
    }
    return (int)check(syscall6(SYS_UTIMENS, (int64_t)path, fd, times ? (int64_t)t : 0, flags, 0, 0));
}

int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags)
{
    if (!path) {
        errno = EFAULT;
        return -1;
    }
    if (dirfd != AT_FDCWD && path[0] != '/') {
        errno = ENOTSUP;                        /* (other folders' descriptors: not yet) */
        return -1;
    }
    if (flags & ~AT_SYMLINK_NOFOLLOW) {
        errno = EINVAL;
        return -1;
    }
    return utimens(path, 0, times, flags & AT_SYMLINK_NOFOLLOW ? UTIMENS_NOFOLLOW : 0);
}

int futimens(int fd, const struct timespec times[2])
{
    return utimens(0, fd, times, 0);
}

int utime(const char *path, const struct utimbuf *times)
{
    if (!times)
        return utimensat(AT_FDCWD, path, 0, 0);
    struct timespec t[2] = { { times->actime, 0 }, { times->modtime, 0 } };
    return utimensat(AT_FDCWD, path, t, 0);
}

int utimes(const char *path, const struct timeval times[2])
{
    if (!times)
        return utimensat(AT_FDCWD, path, 0, 0);
    struct timespec t[2];
    for (int i = 0; i < 2; i++) {
        if (times[i].tv_usec < 0 || times[i].tv_usec > 999999) {
            errno = EINVAL;
            return -1;
        }
        t[i].tv_sec = times[i].tv_sec, t[i].tv_nsec = times[i].tv_usec * 1000;
    }
    return utimensat(AT_FDCWD, path, t, 0);
}

int stat(const char *path, struct stat *st)
{
    struct sys_stat s;
    if (check(syscall6(SYS_STAT, (int64_t)path, (int64_t)&s, 0, 0, 0, 0)) < 0)
        return -1;
    stat_of(&s, st);
    return 0;
}

int lstat(const char *path, struct stat *st)
{
    struct sys_stat s;
    if (check(syscall6(SYS_LSTAT, (int64_t)path, (int64_t)&s, 0, 0, 0, 0)) < 0)
        return -1;
    stat_of(&s, st);
    return 0;
}

int symlink(const char *target, const char *path)
{
    return (int)check(syscall6(SYS_SYMLINK, (int64_t)target, (int64_t)path, 0, 0, 0, 0));
}

ssize_t readlink(const char *path, char *buffer, size_t size)
{
    return (ssize_t)check(syscall6(SYS_READLINK, (int64_t)path, (int64_t)buffer, (int64_t)size,
                                   0, 0, 0));
}

int link(const char *from, const char *to)
{
    return (int)check(syscall6(SYS_LINK, (int64_t)from, (int64_t)to, 0, 0, 0, 0));
}

/* Directories: a descriptor opened on the directory, read one entry at a
 * time. Each DIR is allocated (63k; until then a table of 8 from before
 * there was malloc, so a ninth open folder was EMFILE): as many folders
 * open as descriptors allow. */
struct DIR {
    int fd;
    struct dirent entry;
};

DIR *opendir(const char *path)
{
    DIR *dir = calloc(1, sizeof(*dir));
    if (!dir)
        return 0;                               /* (ENOMEM, from calloc) */
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        free(dir);
        return 0;
    }
    struct stat st;                             /* (the folder opened: not the path again) */
    if (fstat(fd, &st) < 0 || !S_ISDIR(st.st_mode)) {
        close(fd);
        free(dir);
        errno = ENOTDIR;
        return 0;
    }
    dir->fd = fd;
    return dir;
}

struct dirent *readdir(DIR *dir)
{
    struct sys_dirent e;
    if (check(syscall6(SYS_READDIR, dir->fd, (int64_t)&e, 0, 0, 0, 0)) <= 0)
        return 0;
    dir->entry.d_ino = e.id;
    dir->entry.d_type = e.type == FILE_TYPE_DIR       ? DT_DIR
                      : e.type == FILE_TYPE_BLOCK     ? DT_BLK
                      : e.type == FILE_TYPE_SYMLINK   ? DT_LNK : DT_REG;
    size_t n = e.name_length < sizeof(dir->entry.d_name) - 1 ? e.name_length
                                                             : sizeof(dir->entry.d_name) - 1;
    for (size_t i = 0; i < n; i++)
        dir->entry.d_name[i] = e.name[i];
    dir->entry.d_name[n] = 0;
    return &dir->entry;
}

int closedir(DIR *dir)
{
    int r = close(dir->fd);
    free(dir);
    return r;
}

int ritteros_sysinfo(struct sys_info *info)
{
    return (int)check(syscall6(SYS_SYSINFO, (int64_t)info, 0, 0, 0, 0, 0));
}

int ritteros_swap(int op, struct sys_swap *status)
{
    return (int)check(syscall6(SYS_SWAP, op, (int64_t)status, 0, 0, 0, 0));
}

int64_t ritteros_power_table(int index, void *buffer, size_t size)
{
    return check(syscall6(SYS_POWER, POWER_TABLE, index, (int64_t)buffer, (int64_t)size, 0, 0));
}

int64_t ritteros_power_names(char *buffer, size_t size)
{
    return check(syscall6(SYS_POWER, POWER_NAMES, (int64_t)buffer, (int64_t)size, 0, 0, 0));
}

int64_t ritteros_power_eval(const char *path, char *buffer, size_t size)
{
    return check(syscall6(SYS_POWER, POWER_EVAL, (int64_t)path, (int64_t)buffer, (int64_t)size,
                          0, 0));
}

int ritteros_power_sleep(unsigned seconds)
{
    return (int)check(syscall6(SYS_POWER, POWER_SLEEP, (int64_t)seconds, 0, 0, 0, 0));
}

off_t lseek(int fd, off_t offset, int whence)
{
    return check(syscall6(SYS_SEEK, fd, offset, whence, 0, 0, 0));
}

int64_t ritteros_report(int which, char *buffer, size_t size)
{
    return check(syscall6(SYS_REPORT, which, (int64_t)buffer, (int64_t)size, 0, 0, 0));
}

int64_t ritteros_snapshot(int op, const char *name, struct sys_snapshot *buffer, size_t size)
{
    return check(syscall6(SYS_SNAPSHOT, op, (int64_t)name, (int64_t)buffer, (int64_t)size, 0,
                          0));
}

int mkdir(const char *path, unsigned int mode)
{
    return (int)check(syscall6(SYS_MKDIR, (int64_t)path, mode, 0, 0, 0, 0));
}

int chmod(const char *path, mode_t mode)
{
    return (int)check(syscall6(SYS_CHMOD, (int64_t)path, mode, 0, 0, 0, 0));
}

int chown(const char *path, uid_t uid, gid_t gid)
{
    return (int)check(syscall6(SYS_CHOWN, (int64_t)path, uid, gid, 0, 0, 0));
}

int64_t ritteros_random(void *buffer, size_t n)
{
    return check(syscall6(SYS_RANDOM, (int64_t)buffer, (int64_t)n, 0, 0, 0, 0));
}

int ritteros_echo(bool on)
{
    return (int)check(syscall6(SYS_ECHO, on, 0, 0, 0, 0, 0));
}

int ritteros_mount(const char *device, const char *path, char *buffer, size_t size)
{
    return (int)check(syscall6(SYS_MOUNT, (int64_t)device, (int64_t)path, (int64_t)buffer,
                               (int64_t)size, 0, 0));
}

int ritteros_unmount(const char *path_or_device)
{
    return (int)check(syscall6(SYS_UNMOUNT, (int64_t)path_or_device, 0, 0, 0, 0, 0));
}

uint64_t ritteros_time(void)
{
    return (uint64_t)syscall6(SYS_TIME, 0, 0, 0, 0, 0, 0);
}

int ritteros_device(int index, struct sys_device *out)
{
    return (int)check(syscall6(SYS_DEVICE, index, (int64_t)out, 0, 0, 0, 0));
}

int64_t ritteros_grow(const char *partition)
{
    return check(syscall6(SYS_GROW, (int64_t)partition, 0, 0, 0, 0, 0));
}

int ritteros_terminal_raw(bool on)
{
    return (int)check(syscall6(SYS_TERMINAL, TERMINAL_RAW, on, 0, 0, 0, 0));
}

int ritteros_efivar(int op, struct sys_efivar *v)
{
    return (int)check(syscall6(SYS_EFIVAR, op, (int64_t)v, 0, 0, 0, 0));
}

int ritteros_log_as(const char *name)
{
    return (int)check(syscall6(SYS_LOG, LOG_AS, (int64_t)name, 0, 0, 0, 0));
}

int ritteros_log_write(const char *name, const char *text)
{
    return (int)check(syscall6(SYS_LOG, LOG_WRITE, (int64_t)name, (int64_t)text, 0, 0, 0));
}

int ritteros_log_take(void *buffer, size_t size)
{
    return (int)check(syscall6(SYS_LOG, LOG_TAKE, (int64_t)buffer, (int64_t)size, 0, 0, 0));
}

int ritteros_terminal_line_editor(void)
{
    return (int)check(syscall6(SYS_TERMINAL, TERMINAL_RAW, 2, 0, 0, 0, 0));
}

int ritteros_terminal_size(int *rows, int *cols)
{
    int64_t r = syscall6(SYS_TERMINAL, TERMINAL_SIZE, 0, 0, 0, 0, 0);
    *rows = (int)(r >> 16);
    *cols = (int)(r & 0xFFFF);
    return 0;
}

int isatty(int fd)
{
    return (int)syscall6(SYS_TERMINAL, TERMINAL_ISATTY, fd, 0, 0, 0, 0);
}

uid_t getuid(void)
{
    return (uid_t)syscall6(SYS_GETUID, 0, 0, 0, 0, 0, 0);
}

uid_t geteuid(void)
{
    return (uid_t)syscall6(SYS_GETEUID, 0, 0, 0, 0, 0, 0);
}

gid_t getgid(void)
{
    return (gid_t)syscall6(SYS_GETGID, 0, 0, 0, 0, 0, 0);
}

int setuid(uid_t uid)
{
    return (int)check(syscall6(SYS_SETUID, uid, 0, 0, 0, 0, 0));
}

int setgid(gid_t gid)
{
    return (int)check(syscall6(SYS_SETGID, gid, 0, 0, 0, 0, 0));
}

int rmdir(const char *path)
{
    return (int)check(syscall6(SYS_RMDIR, (int64_t)path, 0, 0, 0, 0, 0));
}

int unlink(const char *path)
{
    return (int)check(syscall6(SYS_UNLINK, (int64_t)path, 0, 0, 0, 0, 0));
}

int rename(const char *from, const char *to)
{
    return (int)check(syscall6(SYS_RENAME, (int64_t)from, (int64_t)to, 0, 0, 0, 0));
}

int ritteros_copy(const char *from, const char *to)
{
    return (int)check(syscall6(SYS_COPY, (int64_t)from, (int64_t)to, 0, 0, 0, 0));
}

int64_t ritteros_attr(int op, const char *path, const char *key, void *buffer, size_t size)
{
    return check(syscall6(SYS_ATTR, op, (int64_t)path, (int64_t)key, (int64_t)buffer,
                          (int64_t)size, 0));
}

int ritteros_power(int how)
{
    return (int)check(syscall6(SYS_POWER, how, 0, 0, 0, 0, 0));
}

pid_t wait(int *status)
{
    return waitpid(-1, status, 0);
}

int kill(pid_t pid, int signal)
{
    return (int)check(syscall6(SYS_KILL, pid, signal, 0, 0, 0, 0));
}

/* C's streams are flushed at exit (file.c; weak: a program that uses none
 * does not take them in). */
__attribute__((weak)) void ritteros_flush_streams(void);
/* atexit's functions are called first (cstd.c, 22p). */
__attribute__((weak)) void ritteros_run_atexit(void);

void exit(int code)
{
    if (ritteros_run_atexit)
        ritteros_run_atexit();
    if (ritteros_flush_streams)
        ritteros_flush_streams();
    syscall6(SYS_EXIT, code, 0, 0, 0, 0, 0);
    for (;;)
        ;                               /* the kernel never returns from exit */
}

/* --- threads --- */

void __thread_start(void);          /* start.S: calls fn(arg), then thread_exit */
void *__ritteros_tls_area(uintptr_t *tp);   /* threaddata.c */
void __ritteros_tls_release(void *area);

/* Each thread's per-thread data (22i): its area, made by thread_create and
 * freed by thread_join; the new thread sets its thread pointer before
 * anything else (until then it must not touch errno). */
struct thread_start {
    int (*fn)(void *arg);
    void *arg;
    uintptr_t tp;
};
static void *areas[PROCESS_THREADS];

static int thread_begin(void *data)
{
    struct thread_start s = *(struct thread_start *)data;
    if (s.tp)
        syscall6(SYS_THREAD_POINTER, (int64_t)s.tp, 0, 0, 0, 0, 0);
    free(data);
    return s.fn(s.arg);
}

int thread_create(int (*fn)(void *arg), void *arg)
{
    return __ritteros_thread_create_sized(fn, arg, 0);
}

/* (65e-53) Set before the first other thread starts: stdio's streams take
 * their locks from then on (file.c). */
int __ritteros_threads_made;

int __ritteros_thread_create_sized(int (*fn)(void *arg), void *arg, size_t stack_size)
{
    struct thread_start *s = malloc(sizeof(*s));
    if (!s)
        return -1;
    __atomic_store_n(&__ritteros_threads_made, 1, __ATOMIC_RELEASE);
    s->fn = fn;
    s->arg = arg;
    s->tp = 0;
    void *area = __ritteros_tls_area(&s->tp);
    int number = (int)check(syscall6(SYS_THREAD_CREATE, (int64_t)__thread_start,
                                     (int64_t)thread_begin, (int64_t)s, (int64_t)stack_size, 0, 0));
    if (number < 0) {
        free(s);
        __ritteros_tls_release(area);
        return -1;
    }
    if (number < PROCESS_THREADS)
        areas[number] = area;
    return number;
}

int thread_join(int number, int *result)
{
    int64_t value = 0;
    if (check(cancel_after(syscall6(SYS_THREAD_JOIN, number, (int64_t)&value, 0, 0, 0, 0))) < 0)
        return -1;
    if (number >= 0 && number < PROCESS_THREADS) {
        __ritteros_tls_release(areas[number]);  /* (an ended thread no longer needs its data) */
        areas[number] = 0;
    }
    if (result)
        *result = (int)value;
    return 0;
}

void thread_exit(int result)
{
    syscall6(SYS_THREAD_EXIT, result, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

int thread_self(void)
{
    return (int)syscall6(SYS_THREAD_SELF, 0, 0, 0, 0, 0, 0);
}

/* (68i) The layouts are one: the kernel fills the program's struct. */
_Static_assert(sizeof(struct utsname) == sizeof(struct sys_utsname), "utsname's layout");

int uname(struct utsname *name)
{
    return (int)check(syscall6(SYS_UNAME, (int64_t)name, 0, 0, 0, 0, 0));
}

/* The system call wrappers: the kernel returns -ERR_*, C returns -1 and
 * puts the error number into errno. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/auxv.h>
#include <sys/time.h>
#include <utime.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
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
    /* (43b-1) O_NOFOLLOW and O_DIRECTORY are the library's checks */
    struct stat st;
    if ((flags & O_NOFOLLOW) && path && lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) {
        errno = ELOOP;
        return -1;
    }
    int fd = (int)check(syscall6(SYS_OPEN, (int64_t)path, flags & ~(O_NOFOLLOW | O_DIRECTORY), mode, 0, 0, 0));
    if (fd >= 0 && (flags & O_DIRECTORY) && (fstat(fd, &st) < 0 || !S_ISDIR(st.st_mode))) {
        close(fd);
        errno = ENOTDIR;
        return -1;
    }
    return fd;
}

/* (43b-4) A record lock's range made absolute for the kernel, and back. */
static int record_lock(int fd, int command, struct flock *l)
{
    if (!l || (l->l_type != F_RDLCK && l->l_type != F_WRLCK && l->l_type != F_UNLCK)) {
        errno = EINVAL;
        return -1;
    }
    off_t base = 0;
    if (l->l_whence == SEEK_CUR) {
        base = lseek(fd, 0, SEEK_CUR);
    } else if (l->l_whence == SEEK_END) {
        struct stat st;
        base = fstat(fd, &st) == 0 ? st.st_size : -1;
    } else if (l->l_whence != SEEK_SET) {
        errno = EINVAL;
        return -1;
    }
    if (base < 0)
        return -1;
    int64_t start = base + l->l_start, length = l->l_len;
    if (length < 0) {
        start += length;
        length = -length;
    }
    if (start < 0 || (length && start > INT64_MAX - length)) {
        errno = EINVAL;
        return -1;
    }
    struct sys_flock k = { (uint64_t)start, (uint64_t)length, (uint32_t)l->l_type, 0 };
    if (check(syscall6(SYS_FCNTL, fd, command, (int64_t)&k, 0, 0, 0)) < 0)
        return -1;
    if (command == F_GETLK) {
        l->l_type = (short)k.type;
        if (k.type != F_UNLCK) {
            l->l_whence = SEEK_SET;
            l->l_start = (off_t)k.start;
            l->l_len = (off_t)k.length;
            l->l_pid = (pid_t)k.pid;
        }
    }
    return 0;
}

int fcntl(int fd, int command, ...)
{
    va_list ap;
    va_start(ap, command);
    long arg = command == F_GETFD || command == F_GETFL || command == F_GET_SEALS ? 0
               : va_arg(ap, long);                                              /* (53b) */
    va_end(ap);
    if (command == F_GETLK || command == F_SETLK || command == F_SETLKW)
        return record_lock(fd, command, (struct flock *)arg);
    return (int)check(syscall6(SYS_FCNTL, fd, command, arg, 0, 0, 0));
}

/* (46b) sys/ioctl.h's requests, through fcntl (no ioctl system call). */
int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (request == FIOCLEX || request == FIONCLEX) {
        int flags = fcntl(fd, F_GETFD);
        if (flags < 0)
            return -1;
        return fcntl(fd, F_SETFD, request == FIOCLEX ? flags | FD_CLOEXEC : flags & ~FD_CLOEXEC);
    }
    if (request == FIONBIO) {
        int flags = fcntl(fd, F_GETFL);
        if (flags < 0)
            return -1;
        return fcntl(fd, F_SETFL, *(int *)arg ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
    }
    if (request == SNDCTL_DSP_GETODELAY) {      /* (68v) sys/soundcard.h's */
        int64_t r = check(syscall6(SYS_SOUND, fd, SOUND_DELAY, 0, 0, 0, 0));
        if (r < 0)
            return -1;
        *(int *)arg = (int)r;
        return 0;
    }
    if (request == SNDCTL_DSP_RESET)
        return (int)check(syscall6(SYS_SOUND, fd, SOUND_RESET, 0, 0, 0, 0));
    if (fcntl(fd, F_GETFD) < 0)
        return -1;                      /* EBADF first, as Linux */
    errno = ENOTTY;
    return -1;
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
    flags &= ~MAP_NORESERVE;                    /* (45c: what RitterOS always does) */
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

/* (46b) No memory locking (sys/mman.h). */
int mlock(const void *addr, size_t length)
{
    (void)addr, (void)length;
    errno = ENOSYS;
    return -1;
}

int munlock(const void *addr, size_t length)
{
    (void)addr, (void)length;
    errno = ENOSYS;
    return -1;
}

int mlockall(int flags)
{
    (void)flags;
    errno = ENOSYS;
    return -1;
}

int munlockall(void)
{
    errno = ENOSYS;
    return -1;
}

void __clear_cache(void *start, void *end)
{
#if defined(__aarch64__)
    /* (45d) CTR_EL0: the smallest data and instruction cache lines; IDC and
     * DIC say a step is not needed on this processor. The kernel lets
     * programs do this (SCTLR_EL1.UCT, UCI). */
    uint64_t ctr;
    __asm__ volatile ("mrs %0, ctr_el0" : "=r"(ctr));
    uintptr_t from = (uintptr_t)start, to = (uintptr_t)end;
    uintptr_t dline = (uintptr_t)4 << ((ctr >> 16) & 15), iline = (uintptr_t)4 << (ctr & 15);
    if (!(ctr & (1ULL << 28)))
        for (uintptr_t a = from & ~(dline - 1); a < to; a += dline)
            __asm__ volatile ("dc cvau, %0" : : "r"(a) : "memory");
    __asm__ volatile ("dsb ish" : : : "memory");
    if (!(ctr & (1ULL << 29)))
        for (uintptr_t a = from & ~(iline - 1); a < to; a += iline)
            __asm__ volatile ("ic ivau, %0" : : "r"(a) : "memory");
    __asm__ volatile ("dsb ish\n\tisb" : : : "memory");
#else
    (void)start;
    (void)end;
#endif
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
    if (flags & ~(WNOHANG | WUNTRACED | WCONTINUED)) {
        errno = EINVAL;
        return -1;
    }
    cancel_point();
    return (pid_t)check(cancel_after(syscall6(SYS_WAIT, pid, (int64_t)status,
                                              (flags & WNOHANG ? WAIT_NO_HANG : 0) |
                                                  (flags & WUNTRACED ? WAIT_UNTRACED : 0) |
                                                  (flags & WCONTINUED ? WAIT_CONTINUED : 0),
                                              0, 0, 0)));
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

pid_t getsid(pid_t pid)
{
    return (pid_t)check(syscall6(SYS_GETPGRP, pid, 1, 0, 0, 0, 0));
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
                     (action->sa_flags & SA_NODEFER ? SIGACTION_NODEFER : 0) |
                     (action->sa_flags & SA_ONSTACK ? SIGACTION_ONSTACK : 0) |
                     (action->sa_flags & SA_SIGINFO ? SIGACTION_SIGINFO : 0) |
                     (action->sa_flags & SA_NOCLDSTOP ? SIGACTION_NOCLDSTOP : 0);   /* (44a) */
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
                        (had.flags & SIGACTION_NODEFER ? SA_NODEFER : 0) |
                        (had.flags & SIGACTION_ONSTACK ? SA_ONSTACK : 0) |
                        (had.flags & SIGACTION_SIGINFO ? SA_SIGINFO : 0) |
                        (had.flags & SIGACTION_NOCLDSTOP ? SA_NOCLDSTOP : 0);
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

int sigaltstack(const stack_t *stack, stack_t *old)
{
    struct sys_sigstack want, had;
    if (stack) {
        want.sp = (uint64_t)(uintptr_t)stack->ss_sp;
        want.flags = stack->ss_flags;
        want.pad = 0;
        want.size = stack->ss_size;
    }
    if (check(syscall6(SYS_SIGALTSTACK, stack ? (int64_t)(uintptr_t)&want : 0,
                       old ? (int64_t)(uintptr_t)&had : 0, 0, 0, 0, 0)) < 0)
        return -1;
    if (old) {
        old->ss_sp = (void *)(uintptr_t)had.sp;
        old->ss_flags = had.flags;
        old->ss_size = had.size;
    }
    return 0;
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
    memset(st, 0, sizeof(*st));
    st->st_ino = s->id;
    st->st_dev = s->device;
    st->st_mode = (s->type < sizeof(kinds) / sizeof(kinds[0]) && kinds[s->type] ? kinds[s->type]
                                                                              : S_IFREG) |
                  (s->mode & 05777);   /* (setuid too, 50b; sticky, 57b) */
    st->st_uid = s->uid;
    st->st_gid = s->gid;
    st->st_nlink = s->links;
    st->st_size = (off_t)s->size;
    st->st_blksize = 4096;                      /* (43a) RitterFS's block */
    st->st_blocks = (blkcnt_t)((s->size + 4095) / 4096 * 8);
    st->st_mtim.tv_sec = (time_t)(s->modified / 1000000000);
    st->st_mtim.tv_nsec = (long)(s->modified % 1000000000);
    st->st_atim = st->st_mtim;                  /* (no access times kept) */
    st->st_ctim.tv_sec = (time_t)(s->created / 1000000000);
    st->st_ctim.tv_nsec = (long)(s->created % 1000000000);
}

/* (43a) The struct stat of programs built before 43a, and their stat,
 * fstat and lstat, which the library keeps under those names. */
struct stat_v1 {
    uint32_t st_dev;
    uint64_t st_ino;
    unsigned int st_mode;
    uid_t st_uid;
    gid_t st_gid;
    unsigned int st_nlink;
    int64_t st_size;
    int64_t st_ctime_v1, st_mtime_v1;
};

static int stat_v1_of(int64_t r, const struct sys_stat *s, struct stat_v1 *old)
{
    if (check(r) < 0)
        return -1;
    struct stat st;
    stat_of(s, &st);
    old->st_dev = (uint32_t)st.st_dev;
    old->st_ino = st.st_ino;
    old->st_mode = st.st_mode;
    old->st_uid = st.st_uid;
    old->st_gid = st.st_gid;
    old->st_nlink = (unsigned)st.st_nlink;
    old->st_size = st.st_size;
    old->st_ctime_v1 = st.st_ctim.tv_sec;
    old->st_mtime_v1 = st.st_mtim.tv_sec;
    return 0;
}

int stat_v1(const char *path, struct stat_v1 *st) __asm__("stat");
int fstat_v1(int fd, struct stat_v1 *st) __asm__("fstat");
int lstat_v1(const char *path, struct stat_v1 *st) __asm__("lstat");

int stat_v1(const char *path, struct stat_v1 *st)
{
    struct sys_stat s;
    return stat_v1_of(syscall6(SYS_STAT, (int64_t)path, (int64_t)&s, 0, 0, 0, 0), &s, st);
}

int fstat_v1(int fd, struct stat_v1 *st)
{
    struct sys_stat s;
    return stat_v1_of(syscall6(SYS_FSTAT, fd, (int64_t)&s, 0, 0, 0, 0), &s, st);
}

int lstat_v1(const char *path, struct stat_v1 *st)
{
    struct sys_stat s;
    return stat_v1_of(syscall6(SYS_LSTAT, (int64_t)path, (int64_t)&s, 0, 0, 0, 0), &s, st);
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
    if (flags & ~AT_SYMLINK_NOFOLLOW) {
        errno = EINVAL;
        return -1;
    }
    char whole[PATH_MAX];                       /* (43b-1: a folder's descriptor too) */
    if (dirfd != AT_FDCWD && path[0] != '/') {
        int64_t k = check(syscall6(SYS_FDPATH, dirfd, (int64_t)whole, sizeof(whole), 0, 0, 0));
        if (k < 0)
            return -1;
        if ((size_t)k + 2 + strlen(path) > sizeof(whole)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        whole[k] = '/';
        strcpy(whole + k + 1, path);
        path = whole;
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

/* (43b-1) see dirent.h */
DIR *fdopendir(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0)
        return 0;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return 0;
    }
    DIR *dir = calloc(1, sizeof(*dir));
    if (dir)
        dir->fd = fd;
    return dir;
}

int dirfd(DIR *dir)
{
    return dir->fd;
}

void rewinddir(DIR *dir)
{
    lseek(dir->fd, 0, SEEK_SET);
}

long telldir(DIR *dir)
{
    return (long)lseek(dir->fd, 0, SEEK_CUR);
}

void seekdir(DIR *dir, long place)
{
    lseek(dir->fd, place, SEEK_SET);
}

int readdir_r(DIR *dir, struct dirent *entry, struct dirent **result)
{
    int saved = errno;
    errno = 0;
    struct dirent *e = readdir(dir);
    int r = e ? 0 : errno;
    errno = saved;
    if (e) {
        *entry = *e;
        *result = entry;
    } else {
        *result = 0;
    }
    return r;
}

int alphasort(const struct dirent **a, const struct dirent **b)
{
    return strcoll((*a)->d_name, (*b)->d_name);
}

int versionsort(const struct dirent **a, const struct dirent **b)
{
    return strverscmp((*a)->d_name, (*b)->d_name);
}

int scandir(const char *path, struct dirent ***list, int (*filter)(const struct dirent *),
            int (*compare)(const struct dirent **, const struct dirent **))
{
    DIR *dir = opendir(path);
    if (!dir)
        return -1;
    struct dirent **found = 0;
    size_t count = 0, room = 0;
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (filter && !filter(e))
            continue;
        if (count == room) {
            room = room ? room * 2 : 16;
            struct dirent **bigger = realloc(found, room * sizeof(*found));
            if (!bigger)
                goto failed;
            found = bigger;
        }
        if (!(found[count] = malloc(sizeof(**found))))
            goto failed;
        *found[count++] = *e;
    }
    closedir(dir);
    if (compare)
        qsort(found, count, sizeof(*found), (int (*)(const void *, const void *))compare);
    *list = found;
    return (int)count;
failed:
    for (size_t i = 0; i < count; i++)
        free(found[i]);
    free(found);
    closedir(dir);
    errno = ENOMEM;
    return -1;
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

/* (68p) The kernel's clock page (struct sys_clock, at the auxiliary
 * vector's AT_RITTEROS_CLOCK): looked for once; 0 on a kernel without it. */
static const volatile struct sys_clock *clock_page;
static int clock_looked;

static const volatile struct sys_clock *clock_find(void)
{
    if (!__atomic_load_n(&clock_looked, __ATOMIC_ACQUIRE)) {
        int saved = errno;                      /* (getauxval sets it when there is none) */
        const volatile struct sys_clock *c = (const volatile struct sys_clock *)getauxval(AT_RITTEROS_CLOCK);
        errno = saved;
        __atomic_store_n(&clock_page, c, __ATOMIC_RELAXED);
        __atomic_store_n(&clock_looked, 1, __ATOMIC_RELEASE);
    }
    return __atomic_load_n(&clock_page, __ATOMIC_RELAXED);
}

/* The processor's counter, read as the kernel's arch_counter reads it. */
static uint64_t counter_read(void)
{
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ volatile ("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | lo;
#else
    uint64_t v;
    __asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r"(v) : : "memory");
    return v;
#endif
}

/* (68p) Nanoseconds since the clock started (CLOCK_MONOTONIC) and the wall
 * clock then (*base, 0: unknown), from the clock page without a system call;
 * false where there is none (ask SYS_TIME). The page's sequence is odd
 * while the kernel changes it: read until it was even and the same around
 * the reading, the counter read inside. */
bool __ritteros_clock(uint64_t *since, uint64_t *base)
{
    const volatile struct sys_clock *c = clock_find();
    if (!c)
        return false;
    for (;;) {
        uint32_t seq = __atomic_load_n(&c->sequence, __ATOMIC_ACQUIRE);
        if (seq & 1)
            continue;
        uint32_t kind = c->counter;
        uint64_t offset = c->offset, hz = c->hz, start = c->base_counter, at = c->base_ns;
        uint64_t delta = counter_read() + offset - start;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&c->sequence, __ATOMIC_RELAXED) != seq)
            continue;
        if ((kind != SYS_CLOCK_TSC && kind != SYS_CLOCK_CNTVCT) || !hz)
            return false;
        /* (as the kernel's since_base: seconds and the rest apart) */
        *since = delta / hz * 1000000000ULL + delta % hz * 1000000000ULL / hz;
        *base = at;
        return true;
    }
}

uint64_t ritteros_time(void)
{
    uint64_t since, base;
    if (__ritteros_clock(&since, &base))
        return base ? base + since : 0;
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
/* (43a-4) each thread's stack size as asked (0: the default), for
 * pthread_getattr_np */
size_t __ritteros_stack_sizes[PROCESS_THREADS];

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
    if (number < PROCESS_THREADS) {
        areas[number] = area;
        __ritteros_stack_sizes[number] = stack_size ? (stack_size + 4095) & ~(size_t)4095 : 0;
    }
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

/* (43a-4) a thread's thread_local destructors (cxa.c; weak: a program
 * that has none does not take it in) */
__attribute__((weak)) void __ritteros_thread_dtors(void);

void thread_exit(int result)
{
    if (__ritteros_thread_dtors)
        __ritteros_thread_dtors();
    syscall6(SYS_THREAD_EXIT, result, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

/* (68q) The caller's thread number, asked once per thread: kept as number
 * + 1 in the word at the thread pointer + 8, a word of the per-thread
 * area's header nothing else uses (x86: the 64-byte header, whose first
 * word points to itself; AArch64: the 16-byte header before the modules'
 * blocks; threaddata.c). A new area starts zeroed; a fork's child keeps its
 * thread's number (the kernel's), so the kept word stays right. Not
 * __thread: ld.so links this file and has no per-thread data of its own.
 * Without the areas yet (ld.so, the start), the kernel is asked. */
uintptr_t __ritteros_thread_area(void);    /* (threaddata.c) */

int thread_self(void)
{
    uintptr_t tp = __ritteros_thread_area();
    if (tp) {
        int kept = *(volatile int *)(tp + 8);
        if (kept)
            return kept - 1;
    }
    int self = (int)syscall6(SYS_THREAD_SELF, 0, 0, 0, 0, 0, 0);
    if (tp && self >= 0)
        *(volatile int *)(tp + 8) = self + 1;
    return self;
}

/* (68i) The layouts are one: the kernel fills the program's struct. */
_Static_assert(sizeof(struct utsname) == sizeof(struct sys_utsname), "utsname's layout");

int uname(struct utsname *name)
{
    return (int)check(syscall6(SYS_UNAME, (int64_t)name, 0, 0, 0, 0, 0));
}

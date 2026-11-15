/*
 * (43b-4) Event counters, timers and whole-file locks as Linux has them:
 * eventfd (SYS_EVENTFD), timerfd (SYS_TIMERFD: times in nanoseconds for
 * the kernel), flock (SYS_FLOCK), and lockf on fcntl's record locks.
 */
#include <errno.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include "ritteros.h"

static int64_t check(int64_t r)
{
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

int eventfd(unsigned int initial, int flags)
{
    if (flags & ~(EFD_SEMAPHORE | EFD_CLOEXEC | EFD_NONBLOCK)) {
        errno = EINVAL;
        return -1;
    }
    return (int)check(syscall6(SYS_EVENTFD, initial, flags & (EFD_CLOEXEC | EFD_NONBLOCK),
                               flags & EFD_SEMAPHORE ? 1 : 0, 0, 0, 0));
}

int eventfd_read(int fd, eventfd_t *value)
{
    return read(fd, value, sizeof(*value)) == (ssize_t)sizeof(*value) ? 0 : -1;
}

int eventfd_write(int fd, eventfd_t value)
{
    return write(fd, &value, sizeof(value)) == (ssize_t)sizeof(value) ? 0 : -1;
}

int timerfd_create(int clock, int flags)
{
    if (flags & ~(TFD_CLOEXEC | TFD_NONBLOCK)) {
        errno = EINVAL;
        return -1;
    }
    return (int)check(syscall6(SYS_TIMERFD, TIMERFD_CREATE, clock, flags, 0, 0, 0));
}

static int to_ns(const struct timespec *t, uint64_t *ns)
{
    if (t->tv_sec < 0 || t->tv_nsec < 0 || t->tv_nsec >= 1000000000 ||
        (uint64_t)t->tv_sec > (UINT64_MAX - 999999999) / 1000000000) {
        errno = EINVAL;
        return -1;
    }
    *ns = (uint64_t)t->tv_sec * 1000000000 + (uint64_t)t->tv_nsec;
    return 0;
}

static void from_ns(uint64_t ns, struct timespec *t)
{
    t->tv_sec = (time_t)(ns / 1000000000);
    t->tv_nsec = (long)(ns % 1000000000);
}

int timerfd_settime(int fd, int flags, const struct itimerspec *value, struct itimerspec *old)
{
    uint64_t first, every, was[2];
    if (!value) {
        errno = EFAULT;
        return -1;
    }
    if ((flags & ~(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET)) || to_ns(&value->it_value, &first) < 0 ||
        to_ns(&value->it_interval, &every) < 0) {
        errno = EINVAL;
        return -1;
    }
    if (check(syscall6(SYS_TIMERFD, TIMERFD_SET, fd, (int64_t)first, (int64_t)every,
                       flags & TFD_TIMER_ABSTIME ? 1 : 0, (int64_t)was)) < 0)
        return -1;
    if (old) {
        from_ns(was[0], &old->it_value);
        from_ns(was[1], &old->it_interval);
    }
    return 0;
}

int timerfd_gettime(int fd, struct itimerspec *value)
{
    uint64_t now[2];
    if (check(syscall6(SYS_TIMERFD, TIMERFD_GET, fd, (int64_t)now, 0, 0, 0)) < 0)
        return -1;
    from_ns(now[0], &value->it_value);
    from_ns(now[1], &value->it_interval);
    return 0;
}

int flock(int fd, int op)
{
    return (int)check(syscall6(SYS_FLOCK, fd, op, 0, 0, 0, 0));
}

int lockf(int fd, int command, off_t length)
{
    struct flock l = { F_WRLCK, SEEK_CUR, 0, length, 0 };
    switch (command) {
    case F_ULOCK:
        l.l_type = F_UNLCK;
        return fcntl(fd, F_SETLK, &l);
    case F_LOCK:
        return fcntl(fd, F_SETLKW, &l);
    case F_TLOCK:
        return fcntl(fd, F_SETLK, &l);
    case F_TEST:
        if (fcntl(fd, F_GETLK, &l) < 0)
            return -1;
        if (l.l_type == F_UNLCK || l.l_pid == getpid())
            return 0;
        errno = EACCES;
        return -1;
    default:
        errno = EINVAL;
        return -1;
    }
}

/*
 * (43a-4) ppoll and pselect: poll and select under another set of blocked
 * signals. The set is changed, the wait made and the set put back by the
 * library, three steps (the kernel has no one call for it): a signal that
 * arrives between the first two is handled before the wait instead of
 * ending it.
 */
#include <poll.h>
#include <signal.h>
#include <sys/select.h>
#include <time.h>

int ppoll(struct pollfd *fds, nfds_t count, const struct timespec *timeout, const sigset_t *mask)
{
    int ms = -1;
    if (timeout) {
        long long t = (long long)timeout->tv_sec * 1000 + (timeout->tv_nsec + 999999) / 1000000;
        ms = t > 0x7fffffff ? 0x7fffffff : (int)t;
    }
    sigset_t before;
    if (mask)
        sigprocmask(SIG_SETMASK, mask, &before);
    int r = poll(fds, count, ms);
    if (mask)
        sigprocmask(SIG_SETMASK, &before, 0);
    return r;
}

int pselect(int count, fd_set *read, fd_set *write, fd_set *except, const struct timespec *timeout,
            const sigset_t *mask)
{
    struct timeval tv, *tvp = 0;
    if (timeout) {
        tv.tv_sec = timeout->tv_sec;
        tv.tv_usec = (timeout->tv_nsec + 999) / 1000;
        tvp = &tv;
    }
    sigset_t before;
    if (mask)
        sigprocmask(SIG_SETMASK, mask, &before);
    int r = select(count, read, write, except, tvp);
    if (mask)
        sigprocmask(SIG_SETMASK, &before, 0);
    return r;
}

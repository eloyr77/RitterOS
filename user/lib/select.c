/* select (53c): the sets turned into poll's array and back. */
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/select.h>

int select(int count, fd_set *read, fd_set *write, fd_set *except, struct timeval *timeout)
{
    if (count < 0 || count > FD_SETSIZE ||
        (timeout && (timeout->tv_sec < 0 || timeout->tv_usec < 0 || timeout->tv_usec >= 1000000))) {
        errno = EINVAL;
        return -1;
    }
    struct pollfd fds[FD_SETSIZE];              /* (2 KiB of the stack) */
    int n = 0;
    for (int fd = 0; fd < count; fd++) {
        short events = (read && FD_ISSET(fd, read) ? POLLIN : 0) |
                       (write && FD_ISSET(fd, write) ? POLLOUT : 0) |
                       (except && FD_ISSET(fd, except) ? POLLPRI : 0);
        if (events)
            fds[n++] = (struct pollfd){ fd, events, 0 };
    }
    long ms = -1;
    if (timeout) {
        long long t = (long long)timeout->tv_sec * 1000 + (timeout->tv_usec + 999) / 1000;
        ms = t > 0x7FFFFFFF ? 0x7FFFFFFF : (long)t;
    }
    int r = poll(fds, (nfds_t)n, (int)ms);
    if (r < 0)
        return -1;
    for (int i = 0; i < n; i++)
        if (fds[i].revents & POLLNVAL) {
            errno = EBADF;
            return -1;
        }
    if (read)
        FD_ZERO(read);
    if (write)
        FD_ZERO(write);
    if (except)
        FD_ZERO(except);
    int ready = 0;
    for (int i = 0; i < n; i++) {
        short got = fds[i].revents;
        int fd = fds[i].fd;
        if (read && (fds[i].events & POLLIN) && (got & (POLLIN | POLLHUP | POLLERR)))
            FD_SET(fd, read), ready++;
        if (write && (fds[i].events & POLLOUT) && (got & (POLLOUT | POLLERR)))
            FD_SET(fd, write), ready++;
        if (except && (fds[i].events & POLLPRI) && (got & POLLPRI))
            FD_SET(fd, except), ready++;
    }
    return ready;
}

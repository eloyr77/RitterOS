/* Waiting for several descriptors at once (52d). */
#ifndef RITTEROS_POLL_H
#define RITTEROS_POLL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "../../common/syscalls.h"   /* POLLIN, POLLOUT, ... (46b: not ritteros.h) */

struct pollfd {
    int fd;                     /* a negative one is skipped (revents 0) */
    short events;               /* what to wait for: POLLIN, POLLOUT */
    short revents;              /* what was found; POLLERR, POLLHUP and POLLNVAL always */
};

typedef unsigned long nfds_t;

/* How many of the count descriptors have something to report, waiting up
 * to timeout milliseconds for the first (-1: as long as it takes, 0: not
 * at all); -1 with errno set (EINTR: a signal came, EINVAL: more than 256).
 * Pipes, pseudo-terminals, the terminal (a byte typed: a line read still
 * waits for its Enter) and sockets can wait; files and devices never do. */
int poll(struct pollfd *fds, nfds_t count, int timeout);

/* (43a-4) ppoll: poll with a timeout as a timespec (0: none) and, when
 * mask is not 0, that set of blocked signals while it waits (set, then
 * waited, then put back: a signal that comes just before the wait is
 * handled then, not by waking it). */
#include <signal.h>
#include <time.h>
int ppoll(struct pollfd *fds, nfds_t count, const struct timespec *timeout, const sigset_t *mask);

#ifdef __cplusplus
}
#endif

#endif

/* Waiting for several descriptors at once (52d). */
#ifndef RITTEROS_POLL_H
#define RITTEROS_POLL_H

#include "ritteros.h"           /* POLLIN, POLLOUT, ... (common/syscalls.h) */

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

#endif

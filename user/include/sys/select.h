/* select (53c): waiting for descriptors by sets, over poll. fd_set holds
 * FD_SETSIZE (256: a program's descriptors) bits. A descriptor is readable
 * with data, the end, or an error; writable with room or an error; the
 * exceptions set never fills (there is no out-of-band data). */
#ifndef RITTEROS_SYS_SELECT_H
#define RITTEROS_SYS_SELECT_H

#include <stdint.h>
#include <sys/time.h>
#include "ritteros.h"

#define FD_SETSIZE      FD_MAX

typedef struct {
    uint64_t bits[FD_SETSIZE / 64];
} fd_set;

#define FD_ZERO(set)    do { for (int i_ = 0; i_ < FD_SETSIZE / 64; i_++) (set)->bits[i_] = 0; } while (0)
#define FD_SET(fd, set) ((set)->bits[(fd) / 64] |= (uint64_t)1 << ((fd) % 64))
#define FD_CLR(fd, set) ((set)->bits[(fd) / 64] &= ~((uint64_t)1 << ((fd) % 64)))
#define FD_ISSET(fd, set) (((set)->bits[(fd) / 64] >> ((fd) % 64)) & 1)

/* How many descriptors below count are ready (each set keeps only those),
 * waiting up to timeout (0: as long as it takes; it is left as it was);
 * -1 with errno set (EBADF: one is not open, EINVAL, EINTR). */
int select(int count, fd_set *read, fd_set *write, fd_set *except, struct timeval *timeout);

#endif

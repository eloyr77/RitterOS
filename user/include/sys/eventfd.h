/*
 * sys/eventfd.h (43b-4): an event counter as a descriptor, as Linux's.
 * read takes the count (8 bytes, a uint64_t; with EFD_SEMAPHORE just 1),
 * waiting while it is 0; write adds a uint64_t (not 2^64 - 1: EINVAL),
 * waiting while the count would pass 2^64 - 2; poll: POLLIN while above
 * 0, POLLOUT while 1 more fits. EFD_NONBLOCK: EAGAIN instead of waiting.
 */
#ifndef RITTEROS_SYS_EVENTFD_H
#define RITTEROS_SYS_EVENTFD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "../ritteros.h"

typedef uint64_t eventfd_t;
#define EFD_SEMAPHORE   1
#define EFD_CLOEXEC     OPEN_CLOEXEC
#define EFD_NONBLOCK    OPEN_NONBLOCK

int eventfd(unsigned int initial, int flags);
int eventfd_read(int fd, eventfd_t *value);
int eventfd_write(int fd, eventfd_t value);

#ifdef __cplusplus
}
#endif

#endif

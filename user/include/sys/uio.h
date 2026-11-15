/*
 * sys/uio.h (43b-1): reading and writing several buffers in one call.
 * writev's buffers are gathered and written by one write (a pipe's writes
 * up to PIPE_BUF stay whole); readv reads once and spreads; preadv and
 * pwritev at an offset.
 */
#ifndef RITTEROS_SYS_UIO_H
#define RITTEROS_SYS_UIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

struct iovec {
    void *iov_base;
    size_t iov_len;
};
ssize_t readv(int fd, const struct iovec *parts, int count);
ssize_t writev(int fd, const struct iovec *parts, int count);
ssize_t preadv(int fd, const struct iovec *parts, int count, off_t offset);
ssize_t pwritev(int fd, const struct iovec *parts, int count, off_t offset);

#ifdef __cplusplus
}
#endif

#endif

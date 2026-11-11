/* Opening files. */
#ifndef RITTEROS_FCNTL_H
#define RITTEROS_FCNTL_H

#include "ritteros.h"

#define O_RDONLY        OPEN_READ
#define O_WRONLY        OPEN_WRITE
#define O_RDWR          OPEN_READ_WRITE
#define O_CREAT         OPEN_CREATE
#define O_TRUNC         OPEN_TRUNCATE
#define O_APPEND        OPEN_APPEND
#define O_EXCL          OPEN_EXCLUSIVE  /* (52j) with O_CREAT: fail (EEXIST) if it exists */
#define O_NONBLOCK      OPEN_NONBLOCK   /* (53b) calls on it never wait: EAGAIN */
#define O_ACCMODE       OPEN_ACCESS

/* (53b) fcntl's commands: F_DUPFD (the lowest free descriptor from arg
 * on), F_DUPFD_CLOEXEC, F_GETFD/F_SETFD (FD_CLOEXEC: kept, though no
 * program yet replaces itself), F_GETFL (the access mode, O_APPEND,
 * O_NONBLOCK), F_SETFL (O_APPEND, O_NONBLOCK: for the open file, which
 * dup'd descriptors and started programs share). With O_NONBLOCK, reads,
 * writes, accept and connect never wait: EAGAIN, or what was written so
 * far; connect EINPROGRESS (poll for POLLOUT, then SO_ERROR); a line read
 * at the terminal still waits for its Enter once a key is typed. */
#define F_DUPFD         FCNTL_DUPFD
#define F_GETFD         FCNTL_GETFD
#define F_SETFD         FCNTL_SETFD
#define F_GETFL         FCNTL_GETFL
#define F_SETFL         FCNTL_SETFL
#define F_DUPFD_CLOEXEC FCNTL_DUPFD_CLOEXEC
#define FD_CLOEXEC      FCNTL_CLOEXEC
int fcntl(int fd, int command, ...);

/* (63m) For utimensat: the current folder (relative paths start there;
 * other folders' descriptors are not taken yet: ENOTSUP), and a symbolic
 * link itself instead of what it names. */
#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100

/* A new file descriptor for path, or -1 with errno set. A file written
 * through it is written to the disk when it is closed (or the program
 * ends). (52b) With O_CREAT a third argument is the new file's mode
 * (e.g. 0666; the umask's bits are taken away, 022 at the start), as POSIX's. */
int open(const char *path, int flags, ...);
int creat(const char *path, mode_t mode);

#endif

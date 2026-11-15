/*
 * sys/file.h (43b-4): flock, a whole-file lock held by the open file (its
 * descriptors: dup's, a fork's, one passed over a socket) until LOCK_UN
 * or its last close: LOCK_SH shared, LOCK_EX exclusive, waiting while
 * another holds one in the way unless LOCK_NB (EWOULDBLOCK). Advisory:
 * reads and writes never look at it. fcntl's record locks are another
 * kind (fcntl.h); the two never meet, as on Linux.
 */
#ifndef RITTEROS_SYS_FILE_H
#define RITTEROS_SYS_FILE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <fcntl.h>

#define LOCK_SH     1
#define LOCK_EX     2
#define LOCK_NB     4
#define LOCK_UN     8

int flock(int fd, int op);

#ifdef __cplusplus
}
#endif

#endif

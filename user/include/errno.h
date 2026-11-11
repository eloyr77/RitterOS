/* Error numbers: set by the system call wrappers when they return -1. */
#ifndef RITTEROS_ERRNO_H
#define RITTEROS_ERRNO_H

#include "ritteros.h"

/* errno is the place __errno() returns: a function rather than a variable
 * since step 22c, because a shared library exports functions only (a
 * variable's layout would be copied into every program). Use errno as ever. */
int *__errno(void);
#define errno (*__errno())

#define EPERM           ERR_PERM
#define ENOENT          ERR_NOENT
#define EACCES          ERR_ACCES
#define ESRCH           ERR_SRCH
#define EAGAIN          ERR_AGAIN
#define EINTR           ERR_INTR
#define EIO             ERR_IO
#define E2BIG           ERR_2BIG
#define ENOEXEC         ERR_NOEXEC
#define EBADF           ERR_BADF
#define ECHILD          ERR_CHILD
#define ENOMEM          ERR_NOMEM
#define EFAULT          ERR_FAULT
#define EEXIST          ERR_EXIST
#define ENODEV          ERR_NODEV
#define ENXIO           ERR_NXIO
#define ENOTTY          ERR_NOTTY
#define ENOTSOCK        ERR_NOTSOCK     /* (53a: the sockets') */
#define ELOOP           ERR_LOOP        /* (57a) too many symbolic links in a path */
#define EDESTADDRREQ    ERR_DESTADDRREQ
#define EMSGSIZE        ERR_MSGSIZE
#define ENOPROTOOPT     ERR_NOPROTOOPT
#define EPROTONOSUPPORT ERR_PROTONOSUPPORT
#define ESOCKTNOSUPPORT ERR_SOCKTNOSUPPORT
#define EAFNOSUPPORT    ERR_AFNOSUPPORT
#define EADDRNOTAVAIL   ERR_ADDRNOTAVAIL
#define ECONNABORTED    ERR_CONNABORTED
#define EISCONN         ERR_ISCONN
#define EALREADY        ERR_ALREADY     /* (53b) */
#define EDEADLK         ERR_DEADLK      /* (54a) */
#define EINPROGRESS     ERR_INPROGRESS
#define EOPNOTSUPP      ENOTSUP
#define EWOULDBLOCK     EAGAIN
#define ENOTDIR         ERR_NOTDIR
#define EISDIR          ERR_ISDIR
#define EINVAL          ERR_INVAL
#define EMFILE          ERR_MFILE
#define EROFS           ERR_ROFS
#define ENAMETOOLONG    ERR_NAMETOOLONG
#define ENOSYS          ERR_NOSYS
#define ENOTEMPTY       ERR_NOTEMPTY
#define ENOSPC          ERR_NOSPC
#define ENODATA         ERR_NODATA
#define EBUSY           ERR_BUSY
#define EXDEV           ERR_XDEV
#define ENOTSUP         ERR_NOTSUP
#define ENETUNREACH     ERR_NETUNREACH
#define ETIMEDOUT       ERR_TIMEDOUT
#define EHOSTUNREACH    ERR_HOSTUNREACH
#define EPIPE           ERR_PIPE
#define EADDRINUSE      ERR_ADDRINUSE
#define ECONNRESET      ERR_CONNRESET
#define ENOTCONN        ERR_NOTCONN
#define ECONNREFUSED    ERR_CONNREFUSED
#define ENONAME         ERR_NONAME
#define ERANGE          34              /* a number out of range (strtol, 22m; the library's own) */
#define EILSEQ          84      /* (22p: a byte or character no character set has; Linux's number) */

#endif

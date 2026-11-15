/* Opening files. */
#ifndef RITTEROS_FCNTL_H
#define RITTEROS_FCNTL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include "../../common/syscalls.h"  /* (46b) not ritteros.h: POSIX names only */

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
/* (43b-3) A memory file's seals (memfd_create with MFD_ALLOW_SEALING):
 * F_ADD_SEALS adds them (EPERM once F_SEAL_SEAL is there; EBUSY for
 * F_SEAL_WRITE while a program maps it writable), F_GET_SEALS reads them. */
#define F_ADD_SEALS     FCNTL_ADD_SEALS
#define F_GET_SEALS     FCNTL_GET_SEALS
#define F_SEAL_SEAL         0x0001      /* no more seals */
#define F_SEAL_SHRINK       0x0002      /* never shorter */
#define F_SEAL_GROW         0x0004      /* never longer */
#define F_SEAL_WRITE        0x0008      /* never written again */
#define F_SEAL_FUTURE_WRITE 0x0010      /* written only through the writable mappings made before */
/* (43b-4) Record locks (POSIX's): F_SETLK sets or (F_UNLCK) lets go of a
 * lock on [l_start, l_start + l_len) (from l_whence's place; l_len 0: to
 * the end, however long the file gets; negative: the bytes before),
 * EAGAIN if another process holds one in the way; F_SETLKW waits instead
 * (EINTR on a signal; no deadlock detection); F_GETLK the first lock in
 * the way (l_whence SEEK_SET, its holder in l_pid) or l_type F_UNLCK. A
 * process's locks on a file go when it closes any descriptor of it, or
 * ends; a read lock needs the file open for reading, a write lock for
 * writing (EBADF). RitterFS files and memory files only (EINVAL). */
#define F_GETLK     FCNTL_GETLK
#define F_SETLK     FCNTL_SETLK
#define F_SETLKW    FCNTL_SETLKW
#define F_RDLCK     FCNTL_RDLCK
#define F_WRLCK     FCNTL_WRLCK
#define F_UNLCK     FCNTL_UNLCK
struct flock {
    short l_type;
    short l_whence;
    off_t l_start;
    off_t l_len;
    pid_t l_pid;
};
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

/* (43b-1) More of open's flags: O_CLOEXEC (the descriptor closes at
 * exec, set as it is made), O_DIRECTORY (ENOTDIR unless a folder),
 * O_NOFOLLOW (ELOOP when path is a symbolic link), and flags that change
 * nothing here (O_NOCTTY, O_SYNC, O_DSYNC, O_LARGEFILE, O_NOATIME).
 * openat and the other *at calls take a path relative to a folder's
 * descriptor (dirfd; AT_FDCWD: the current folder); AT_REMOVEDIR makes
 * unlinkat rmdir, AT_EACCESS makes faccessat use the effective user,
 * AT_EMPTY_PATH with "" means dirfd's own file. posix_fadvise: taken;
 * posix_fallocate: the file made at least offset + length long. */
#define O_CLOEXEC       OPEN_CLOEXEC
#define O_DIRECTORY     0x10000
#define O_NOFOLLOW      0x20000
#define O_NOCTTY        0
#define O_SYNC          0
#define O_DSYNC         0
#define O_RSYNC         0
#define O_LARGEFILE     0
#define O_NOATIME       0
#define O_NDELAY        O_NONBLOCK
#define AT_REMOVEDIR    0x200
#define AT_SYMLINK_FOLLOW 0x400
#define AT_EACCESS      0x200
#define AT_EMPTY_PATH   0x1000
#define POSIX_FADV_NORMAL       0
#define POSIX_FADV_RANDOM       1
#define POSIX_FADV_SEQUENTIAL   2
#define POSIX_FADV_WILLNEED     3
#define POSIX_FADV_DONTNEED     4
#define POSIX_FADV_NOREUSE      5
int openat(int dirfd, const char *path, int flags, ...);
int posix_fadvise(int fd, off_t offset, off_t length, int advice);
int posix_fallocate(int fd, off_t offset, off_t length);

#ifdef __cplusplus
}
#endif

#endif

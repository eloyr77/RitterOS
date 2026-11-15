/*
 * (43b-1) Files for ported programs: pread/pwrite, truncate, fsync, fchmod
 * and fchown, statvfs, readv/writev, pipe2, dup3, and the *at calls. A
 * path relative to a folder's descriptor is made whole from the path that
 * folder was opened by (SYS_FDPATH): a folder renamed since keeps its old
 * name there, a difference from Linux's, which follows the folder itself.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <unistd.h>
#include "cancel.h"
#include "ritteros.h"

static int64_t check(int64_t r)
{
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

/* path relative to dirfd, whole (into out, PATH_MAX); 0, or -1 with errno.
 * An absolute path or AT_FDCWD: path itself. */
static int whole(int dirfd, const char *path, char *out)
{
    if (!path) {
        errno = EFAULT;
        return -1;
    }
    size_t n = strlen(path);
    if (path[0] == '/' || dirfd == AT_FDCWD) {
        if (n >= PATH_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(out, path, n + 1);
        return 0;
    }
    int64_t k = check(syscall6(SYS_FDPATH, dirfd, (int64_t)out, PATH_MAX, 0, 0, 0));
    if (k < 0)
        return -1;
    if (!n)
        return 0;                               /* (AT_EMPTY_PATH: the descriptor's own) */
    if ((size_t)k + 1 + n >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    out[k] = '/';
    memcpy(out + k + 1, path, n + 1);
    return 0;
}

ssize_t pread(int fd, void *buffer, size_t size, off_t offset)
{
    cancel_point();
    return (ssize_t)check(syscall6(SYS_PREAD, fd, (int64_t)buffer, (int64_t)size, offset, 0, 0));
}

ssize_t pwrite(int fd, const void *buffer, size_t size, off_t offset)
{
    cancel_point();
    return (ssize_t)check(syscall6(SYS_PWRITE, fd, (int64_t)buffer, (int64_t)size, offset, 0, 0));
}

int truncate(const char *path, off_t length)
{
    return (int)check(syscall6(SYS_TRUNCATE, (int64_t)path, length, 0, 0, 0, 0));
}

int ftruncate(int fd, off_t length)
{
    return (int)check(syscall6(SYS_FTRUNCATE, fd, length, 0, 0, 0, 0));
}

int fsync(int fd)
{
    cancel_point();
    return (int)check(syscall6(SYS_FSYNC, fd, 0, 0, 0, 0, 0));
}

int fdatasync(int fd)
{
    return fsync(fd);
}

void sync(void)
{
    /* (a file is committed at its close: nothing else waits in memory) */
}

int fchmod(int fd, mode_t mode)
{
    return (int)check(syscall6(SYS_FCHMOD, fd, mode, 0, 0, 0, 0));
}

int fchown(int fd, uid_t uid, gid_t gid)
{
    return (int)check(syscall6(SYS_FCHOWN, fd, uid, gid, 0, 0, 0));
}

/* chown of a symbolic link itself: links have no owner of their own here
 * but the link's object's; not a link: chown. */
int lchown(const char *path, uid_t uid, gid_t gid)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return -1;
    if (S_ISLNK(st.st_mode)) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return chown(path, uid, gid);
}

int fchownat(int dirfd, const char *path, uid_t uid, gid_t gid, int flags)
{
    char p[PATH_MAX];
    if ((flags & AT_EMPTY_PATH) && path && !*path)
        return fchown(dirfd, uid, gid);
    if (whole(dirfd, path, p) < 0)
        return -1;
    return flags & AT_SYMLINK_NOFOLLOW ? lchown(p, uid, gid) : chown(p, uid, gid);
}

int fchmodat(int dirfd, const char *path, mode_t mode, int flags)
{
    char p[PATH_MAX];
    if (whole(dirfd, path, p) < 0)
        return -1;
    if (flags & AT_SYMLINK_NOFOLLOW) {
        struct stat st;
        if (lstat(p, &st) < 0)
            return -1;
        if (S_ISLNK(st.st_mode)) {
            errno = EOPNOTSUPP;                 /* (as Linux: a link's mode is not set) */
            return -1;
        }
    }
    return chmod(p, mode);
}

int fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
    if ((flags & AT_EMPTY_PATH) && path && !*path)
        return fstat(dirfd, st);
    char p[PATH_MAX];
    if (whole(dirfd, path, p) < 0)
        return -1;
    return flags & AT_SYMLINK_NOFOLLOW ? lstat(p, st) : stat(p, st);
}

int mkdirat(int dirfd, const char *path, mode_t mode)
{
    char p[PATH_MAX];
    return whole(dirfd, path, p) < 0 ? -1 : mkdir(p, mode);
}

int mkfifo(const char *path, mode_t mode)
{
    (void)path, (void)mode;
    errno = ENOSYS;
    return -1;
}

int fchdir(int fd)
{
    char p[PATH_MAX];
    if (check(syscall6(SYS_FDPATH, fd, (int64_t)p, sizeof(p), 0, 0, 0)) < 0)
        return -1;
    return chdir(p);
}

int dup3(int fd, int newfd, int flags)
{
    if (fd == newfd || (flags & ~O_CLOEXEC)) {
        errno = EINVAL;
        return -1;
    }
    if (dup2(fd, newfd) < 0)
        return -1;
    if (flags & O_CLOEXEC)
        fcntl(newfd, F_SETFD, FD_CLOEXEC);
    return newfd;
}

int pipe2(int fds[2], int flags)
{
    return (int)check(syscall6(SYS_PIPE, (int64_t)fds, flags & (O_CLOEXEC | O_NONBLOCK), 0, 0, 0, 0));
}

int faccessat(int dirfd, const char *path, int mode, int flags)
{
    (void)flags;                                /* (AT_EACCESS: a program runs as its real user's
                                                 * group, and access asks the real user) */
    char p[PATH_MAX];
    if (whole(dirfd, path, p) < 0)
        return -1;
    return access(p, mode);
}

ssize_t readlinkat(int dirfd, const char *path, char *buffer, size_t size)
{
    char p[PATH_MAX];
    return whole(dirfd, path, p) < 0 ? -1 : readlink(p, buffer, size);
}

int unlinkat(int dirfd, const char *path, int flags)
{
    char p[PATH_MAX];
    if (whole(dirfd, path, p) < 0)
        return -1;
    return flags & AT_REMOVEDIR ? rmdir(p) : unlink(p);
}

int linkat(int fromfd, const char *from, int tofd, const char *to, int flags)
{
    (void)flags;
    char a[PATH_MAX], b[PATH_MAX];
    if (whole(fromfd, from, a) < 0 || whole(tofd, to, b) < 0)
        return -1;
    return link(a, b);
}

int symlinkat(const char *target, int dirfd, const char *path)
{
    char p[PATH_MAX];
    return whole(dirfd, path, p) < 0 ? -1 : symlink(target, p);
}

int renameat(int fromfd, const char *from, int tofd, const char *to)
{
    char a[PATH_MAX], b[PATH_MAX];
    if (whole(fromfd, from, a) < 0 || whole(tofd, to, b) < 0)
        return -1;
    return rename(a, b);
}

int openat(int dirfd, const char *path, int flags, ...)
{
    unsigned mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, unsigned);
        va_end(ap);
    }
    char p[PATH_MAX];
    if (whole(dirfd, path, p) < 0)
        return -1;
    return open(p, flags, mode);
}

int posix_fadvise(int fd, off_t offset, off_t length, int advice)
{
    (void)fd, (void)offset, (void)length;
    return advice < POSIX_FADV_NORMAL || advice > POSIX_FADV_NOREUSE ? EINVAL : 0;
}

int posix_fallocate(int fd, off_t offset, off_t length)
{
    struct stat st;
    if (offset < 0 || length <= 0)
        return EINVAL;
    int saved = errno;
    int r = 0;
    if (fstat(fd, &st) < 0)
        r = errno;
    else if (st.st_size < offset + length && ftruncate(fd, offset + length) < 0)
        r = errno;
    errno = saved;
    return r;
}

/* The parts' bytes in one buffer, so one call moves them all. */
static void *gathered(const struct iovec *parts, int count, size_t *total)
{
    size_t n = 0;
    for (int i = 0; i < count; i++) {
        if (parts[i].iov_len > SSIZE_MAX - n) {
            errno = EINVAL;
            return 0;
        }
        n += parts[i].iov_len;
    }
    *total = n;
    char *buf = malloc(n ? n : 1);
    if (!buf)
        errno = ENOMEM;
    return buf;
}

static ssize_t write_parts(int fd, const struct iovec *parts, int count, off_t offset, int at)
{
    if (count < 0 || count > IOV_MAX) {
        errno = EINVAL;
        return -1;
    }
    size_t total;
    char *buf = gathered(parts, count, &total);
    if (!buf)
        return -1;
    for (int i = 0, k = 0; i < count; k += (int)parts[i].iov_len, i++)
        memcpy(buf + k, parts[i].iov_base, parts[i].iov_len);
    ssize_t r = at ? pwrite(fd, buf, total, offset) : write(fd, buf, total);
    free(buf);
    return r;
}

static ssize_t read_parts(int fd, const struct iovec *parts, int count, off_t offset, int at)
{
    if (count < 0 || count > IOV_MAX) {
        errno = EINVAL;
        return -1;
    }
    size_t total;
    char *buf = gathered(parts, count, &total);
    if (!buf)
        return -1;
    ssize_t r = at ? pread(fd, buf, total, offset) : read(fd, buf, total);
    for (int i = 0, k = 0; i < count && k < r; k += (int)parts[i].iov_len, i++) {
        size_t n = (size_t)(r - k) < parts[i].iov_len ? (size_t)(r - k) : parts[i].iov_len;
        memcpy(parts[i].iov_base, buf + k, n);
    }
    free(buf);
    return r;
}

ssize_t writev(int fd, const struct iovec *parts, int count) { return write_parts(fd, parts, count, 0, 0); }
ssize_t readv(int fd, const struct iovec *parts, int count) { return read_parts(fd, parts, count, 0, 0); }

ssize_t pwritev(int fd, const struct iovec *parts, int count, off_t offset)
{
    return write_parts(fd, parts, count, offset, 1);
}

ssize_t preadv(int fd, const struct iovec *parts, int count, off_t offset)
{
    return read_parts(fd, parts, count, offset, 1);
}

int statvfs(const char *path, struct statvfs *out)
{
    struct sys_statvfs s;
    if (check(syscall6(SYS_STATVFS, (int64_t)path, (int64_t)&s, 0, 0, 0, 0)) < 0)
        return -1;
    memset(out, 0, sizeof(*out));
    out->f_bsize = out->f_frsize = s.block_size;
    out->f_blocks = s.blocks;
    out->f_bfree = out->f_bavail = s.free_blocks;
    out->f_fsid = s.device;
    out->f_flag = s.read_only ? ST_RDONLY : 0;
    out->f_namemax = s.name_max;
    return 0;
}

int fstatvfs(int fd, struct statvfs *out)
{
    char p[PATH_MAX];
    if (check(syscall6(SYS_FDPATH, fd, (int64_t)p, sizeof(p), 0, 0, 0)) < 0)
        return -1;
    return statvfs(p, out);
}

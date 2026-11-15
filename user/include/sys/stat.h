/* File information. */
#ifndef RITTEROS_SYS_STAT_H
#define RITTEROS_SYS_STAT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#define S_IFMT          0170000
#define S_IFDIR         0040000
#define S_IFREG         0100000
#define S_IFBLK         0060000         /* a disk or partition (/dev) */
#define S_IFCHR         0020000         /* (52b) a terminal, a sound device */
#define S_IFIFO         0010000         /* (52b) a pipe */
#define S_IFSOCK        0140000         /* (52b) a socket */
#define S_IFLNK         0120000         /* (57a) a symbolic link (lstat) */
#define S_ISDIR(mode)   (((mode) & S_IFMT) == S_IFDIR)
#define S_ISREG(mode)   (((mode) & S_IFMT) == S_IFREG)
#define S_ISBLK(mode)   (((mode) & S_IFMT) == S_IFBLK)
#define S_ISCHR(mode)   (((mode) & S_IFMT) == S_IFCHR)
#define S_ISFIFO(mode)  (((mode) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(mode)  (((mode) & S_IFMT) == S_IFSOCK)
#define S_ISLNK(mode)   (((mode) & S_IFMT) == S_IFLNK)
#define S_ISUID         0004000         /* setuid: the program runs as its file's owner (50b) */
#define S_ISGID         0002000         /* (43a) setgid: the program runs as its file's group */
#define S_IRWXU         0000700         /* (43a) the permission bits by name: the owner's, */
#define S_IRUSR         0000400
#define S_IWUSR         0000200
#define S_IXUSR         0000100
#define S_IRWXG         0000070         /* the group's, */
#define S_IRGRP         0000040
#define S_IWGRP         0000020
#define S_IXGRP         0000010
#define S_IRWXO         0000007         /* everyone else's */
#define S_IROTH         0000004
#define S_IWOTH         0000002
#define S_IXOTH         0000001
#define S_IREAD         S_IRUSR         /* (the older BSD names) */
#define S_IWRITE        S_IWUSR
#define S_IEXEC         S_IXUSR
#define ACCESSPERMS     0000777
#define ALLPERMS        0007777
#define DEFFILEMODE     0000666
#define S_ISVTX         0001000         /* (57b) sticky: in this folder only an entry's owner
                                         * (or the folder's) removes or renames it */

/* (43a) POSIX's whole struct stat. RitterFS keeps no access times:
 * st_atim is the last change of the contents, as st_mtim; st_ctim is when
 * the file was made. st_blocks counts 512-byte units of the 4 KiB blocks
 * the size takes, st_blksize the best size to read and write at once;
 * st_rdev is 0. st_atime, st_mtime and st_ctime are the seconds. Programs
 * built before 43a had a shorter struct stat: the library keeps their
 * stat, fstat and lstat, and these declarations name the new ones (the
 * __asm__ names), so both kinds of program keep working. */
struct stat {
    dev_t st_dev;               /* the file system (a mount point differs from its parent) */
    ino_t st_ino;               /* RitterFS object ID */
    mode_t st_mode;             /* the kind (S_IFDIR, S_IFREG, ...) and the rwxrwxrwx bits */
    nlink_t st_nlink;
    uid_t st_uid;               /* owner */
    gid_t st_gid;
    dev_t st_rdev;
    off_t st_size;
    blksize_t st_blksize;
    blkcnt_t st_blocks;
    struct timespec st_atim, st_mtim, st_ctim;
};
#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec

/* Information about the file at path; 0, or -1 with errno set. */
int stat(const char *path, struct stat *st) __asm__("__ritteros_stat2");
/* (52b) The same for an open descriptor (a file as it is now; a terminal,
 * pipe or socket by its kind: S_ISCHR, S_ISFIFO, S_ISSOCK). */
int fstat(int fd, struct stat *st) __asm__("__ritteros_fstat2");
/* (57a) stat of the name itself: a symbolic link is S_ISLNK, its size the
 * length of the path it names, its mode lrwxrwxrwx; st_nlink counts a
 * file's names (hard links). */
int lstat(const char *path, struct stat *st) __asm__("__ritteros_lstat2");

/* Create a directory with these permissions (e.g. 0755); 0, or -1 with
 * errno set. */
int mkdir(const char *path, unsigned int mode);

/* (52h) The bits taken off the modes of the files and directories this
 * program makes (open with O_CREAT, mkdir): 022 at the start, so 0666
 * gives 0644; passed on to the programs it starts. Sets mask, returns the
 * one before. */
mode_t umask(mode_t mask);

/* Permissions (the owner or root; S_ISUID too) and owner (root only; it
 * clears S_ISUID) of a file or directory; 0, or -1 with errno set. */
int chmod(const char *path, mode_t mode);
int chown(const char *path, uid_t uid, gid_t gid);

/* (63m) A file's times: [0] the last access (taken, not kept: RitterOS
 * keeps none), [1] the last modification; tv_nsec UTIME_NOW for now,
 * UTIME_OMIT to leave it; times 0: both now. utimensat names the file by
 * path (dirfd AT_FDCWD, or any with an absolute path; flags 0 or
 * AT_SYMLINK_NOFOLLOW: a link itself), futimens by an open descriptor.
 * Now needs the owner, write permission or root (EACCES); any other time
 * the owner or root (EPERM). RitterFS keeps nanoseconds, exFAT 10 ms, FAT
 * 2 seconds. 0, or -1 with errno set. */
#define UTIME_NOW       ((1L << 30) - 1)
#define UTIME_OMIT      ((1L << 30) - 2)
int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags);
int futimens(int fd, const struct timespec times[2]);

/* (43b-1) fchmod: chmod of an open file; the *at forms (a path relative
 * to a folder's descriptor, AT_SYMLINK_NOFOLLOW, AT_EMPTY_PATH). mkfifo:
 * there are no named pipes (ENOSYS). */
int fchmod(int fd, mode_t mode);
int fchmodat(int dirfd, const char *path, mode_t mode, int flags);
int fstatat(int dirfd, const char *path, struct stat *st, int flags);
int mkdirat(int dirfd, const char *path, mode_t mode);
int mkfifo(const char *path, mode_t mode);

#ifdef __cplusplus
}
#endif

#endif

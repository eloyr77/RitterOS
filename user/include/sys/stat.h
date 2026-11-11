/* File information. */
#ifndef RITTEROS_SYS_STAT_H
#define RITTEROS_SYS_STAT_H

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
#define S_ISVTX         0001000         /* (57b) sticky: in this folder only an entry's owner
                                         * (or the folder's) removes or renames it */

struct stat {
    uint32_t st_dev;            /* the file system (a mount point differs from its parent) */
    uint64_t st_ino;            /* RitterFS object ID */
    unsigned int st_mode;       /* S_IFDIR or S_IFREG, and the rwxrwxrwx bits */
    uid_t st_uid;               /* owner */
    gid_t st_gid;
    unsigned int st_nlink;
    int64_t st_size;
    int64_t st_ctime, st_mtime; /* seconds since 1970 (RitterFS: created, modified) */
};

/* Information about the file at path; 0, or -1 with errno set. */
int stat(const char *path, struct stat *st);
/* (52b) The same for an open descriptor (a file as it is now; a terminal,
 * pipe or socket by its kind: S_ISCHR, S_ISFIFO, S_ISSOCK). */
int fstat(int fd, struct stat *st);
/* (57a) stat of the name itself: a symbolic link is S_ISLNK, its size the
 * length of the path it names, its mode lrwxrwxrwx; st_nlink counts a
 * file's names (hard links). */
int lstat(const char *path, struct stat *st);

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

#endif

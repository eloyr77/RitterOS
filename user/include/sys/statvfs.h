/*
 * sys/statvfs.h (43b-1): a file system's sizes, of the one path (or an
 * open file, fstatvfs) is on: f_bsize/f_frsize its block, f_blocks,
 * f_bfree and f_bavail (blocks, free ones; on FAT and exFAT not told
 * yet: 0), f_namemax, f_flag ST_RDONLY for a read-only one.
 */
#ifndef RITTEROS_SYS_STATVFS_H
#define RITTEROS_SYS_STATVFS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

struct statvfs {
    unsigned long f_bsize, f_frsize;
    fsblkcnt_t f_blocks, f_bfree, f_bavail;
    fsfilcnt_t f_files, f_ffree, f_favail;
    unsigned long f_fsid, f_flag, f_namemax;
};
#define ST_RDONLY       1
#define ST_NOSUID       2
int statvfs(const char *path, struct statvfs *out);
int fstatvfs(int fd, struct statvfs *out);

#ifdef __cplusplus
}
#endif

#endif

/* Reading directories: opendir, then readdir until it returns 0. */
#ifndef RITTEROS_DIRENT_H
#define RITTEROS_DIRENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define DT_BLK  6
#define DT_DIR  4
#define DT_REG  8
#define DT_LNK  10              /* (57a) a symbolic link */
#define DT_UNKNOWN 0            /* (43c) the rest of the BSDs' and Linux's: never given here */
#define DT_FIFO 1
#define DT_CHR  2
#define DT_SOCK 12

struct dirent {
    uint64_t d_ino;             /* RitterFS object ID */
    unsigned char d_type;       /* DT_DIR, DT_REG, DT_BLK or DT_LNK */
    char d_name[256];
};

typedef struct DIR DIR;         /* (as many open as descriptors: 63k) */

DIR *opendir(const char *path);
struct dirent *readdir(DIR *dir);       /* 0 at the end (or on an error: errno) */
int closedir(DIR *dir);

/* (43b-1) fdopendir: a DIR on a folder's descriptor (closedir closes
 * it); dirfd: a DIR's descriptor; rewinddir, telldir, seekdir: its place;
 * readdir_r: readdir into the caller's entry; scandir: the entries that
 * filter keeps (all when 0), sorted by compare (alphasort: strcoll's
 * order; versionsort: strverscmp's), into a malloc'd list. */
#include <sys/types.h>
DIR *fdopendir(int fd);
int dirfd(DIR *dir);
void rewinddir(DIR *dir);
long telldir(DIR *dir);
void seekdir(DIR *dir, long place);
int readdir_r(DIR *dir, struct dirent *entry, struct dirent **result);
int scandir(const char *path, struct dirent ***list, int (*filter)(const struct dirent *),
            int (*compare)(const struct dirent **, const struct dirent **));
int alphasort(const struct dirent **a, const struct dirent **b);
int versionsort(const struct dirent **a, const struct dirent **b);

#ifdef __cplusplus
}
#endif

#endif

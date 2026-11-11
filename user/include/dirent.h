/* Reading directories: opendir, then readdir until it returns 0. */
#ifndef RITTEROS_DIRENT_H
#define RITTEROS_DIRENT_H

#include <stdint.h>

#define DT_BLK  6
#define DT_DIR  4
#define DT_REG  8
#define DT_LNK  10              /* (57a) a symbolic link */

struct dirent {
    uint64_t d_ino;             /* RitterFS object ID */
    unsigned char d_type;       /* DT_DIR, DT_REG, DT_BLK or DT_LNK */
    char d_name[256];
};

typedef struct DIR DIR;         /* (as many open as descriptors: 63k) */

DIR *opendir(const char *path);
struct dirent *readdir(DIR *dir);       /* 0 at the end (or on an error: errno) */
int closedir(DIR *dir);

#endif

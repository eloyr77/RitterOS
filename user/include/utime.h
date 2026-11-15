/* A file's times in seconds (63m): utimensat's oldest form (sys/stat.h). */
#ifndef RITTEROS_UTIME_H
#define RITTEROS_UTIME_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <time.h>

struct utimbuf {
    time_t actime;              /* the last access (taken, not kept) */
    time_t modtime;             /* the last modification */
};

/* times 0: both now. 0, or -1 with errno set. */
int utime(const char *path, const struct utimbuf *times);

#ifdef __cplusplus
}
#endif

#endif

/* Times of day in seconds and microseconds (53a): what socket timeouts
 * and select take. */
#ifndef RITTEROS_SYS_TIME_H
#define RITTEROS_SYS_TIME_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <time.h>

#include <sys/types.h>

struct timeval {
    time_t tv_sec;
    suseconds_t tv_usec;        /* 0 to 999999 */
};

/* The time now since 1970 (UTC); zone must be 0. 0, or -1 with errno set. */
int gettimeofday(struct timeval *tv, void *zone);
/* (68l) The clock set to *tv (clock_settime's: root only, the firmware's
 * clock too); zone is not used. 0, or -1 with errno set. */
int settimeofday(const struct timeval *tv, const void *zone);

/* (63m) A file's times in microseconds ([0] access, [1] modification; 0:
 * both now): utimensat's older form (sys/stat.h). */
int utimes(const char *path, const struct timeval times[2]);

#ifdef __cplusplus
}
#endif

/* (46b) POSIX's sys/time.h also gives fd_set, the FD_ macros and select. */
#include <sys/select.h>

#endif

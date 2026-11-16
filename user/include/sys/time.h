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

/* (71a) BSD's timeval arithmetic, as glibc, musl, the BSDs and the Mac
 * have it (ported code counts on it: Fontconfig's timercmp). CMP is a
 * comparison operator other than == and != for timercmp's seconds, any for
 * the microseconds. The results stay normalized (0-999999 microseconds). */
#define timerisset(tv)  ((tv)->tv_sec || (tv)->tv_usec)
#define timerclear(tv)  ((tv)->tv_sec = (tv)->tv_usec = 0)
#define timercmp(a, b, CMP) \
    (((a)->tv_sec == (b)->tv_sec) ? ((a)->tv_usec CMP (b)->tv_usec) : ((a)->tv_sec CMP (b)->tv_sec))
#define timeradd(a, b, result) \
    do { \
        (result)->tv_sec = (a)->tv_sec + (b)->tv_sec; \
        (result)->tv_usec = (a)->tv_usec + (b)->tv_usec; \
        if ((result)->tv_usec >= 1000000) { \
            (result)->tv_sec++; \
            (result)->tv_usec -= 1000000; \
        } \
    } while (0)
#define timersub(a, b, result) \
    do { \
        (result)->tv_sec = (a)->tv_sec - (b)->tv_sec; \
        (result)->tv_usec = (a)->tv_usec - (b)->tv_usec; \
        if ((result)->tv_usec < 0) { \
            (result)->tv_sec--; \
            (result)->tv_usec += 1000000; \
        } \
    } while (0)
#define TIMEVAL_TO_TIMESPEC(tv, ts) \
    do { (ts)->tv_sec = (tv)->tv_sec; (ts)->tv_nsec = (tv)->tv_usec * 1000; } while (0)
#define TIMESPEC_TO_TIMEVAL(tv, ts) \
    do { (tv)->tv_sec = (ts)->tv_sec; (tv)->tv_usec = (ts)->tv_nsec / 1000; } while (0)

/* (63m) A file's times in microseconds ([0] access, [1] modification; 0:
 * both now): utimensat's older form (sys/stat.h). */
int utimes(const char *path, const struct timeval times[2]);

#ifdef __cplusplus
}
#endif

/* (46b) POSIX's sys/time.h also gives fd_set, the FD_ macros and select. */
#include <sys/select.h>

#endif

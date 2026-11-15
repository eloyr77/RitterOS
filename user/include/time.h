/* Time (step 22o): the clock (seconds since 1970-01-01 UTC), the CPU time
 * a program used, dates in the calendar and dates as text. Local time is
 * UTC plus the fixed offset in /system/config/time ("zone = -05:00"; none:
 * UTC); there are no daylight saving rules, so tm_isdst is always 0. */
#ifndef RITTEROS_TIME_H
#define RITTEROS_TIME_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#ifndef RITTEROS_TIME_TYPES
#define RITTEROS_TIME_TYPES
typedef int64_t time_t;
typedef long clock_t;
#endif

#define CLOCKS_PER_SEC 1000000L

/* A date: as C's struct, with the two fields BSD and Linux add. */
struct tm {
    int tm_sec;                 /* 0-60 */
    int tm_min;                 /* 0-59 */
    int tm_hour;                /* 0-23 */
    int tm_mday;                /* 1-31 */
    int tm_mon;                 /* 0-11 */
    int tm_year;                /* years since 1900 */
    int tm_wday;                /* 0-6, Sunday 0 */
    int tm_yday;                /* 0-365 */
    int tm_isdst;               /* 0 (no daylight saving here) */
    long tm_gmtoff;             /* seconds east of UTC */
    const char *tm_zone;        /* "UTC" or "UTC-05:00" */
};

/* Seconds since 1970 (also into *t), or -1 when the machine's clock is
 * unknown. */
time_t time(time_t *t);
/* The CPU time the program has used, in CLOCKS_PER_SEC (all its threads,
 * the kernel's work for it included); -1 if not known. */
clock_t clock(void);
double difftime(time_t end, time_t start);
/* A local date into seconds: out-of-range fields are carried (minute 75
 * is 1:15), and *tm is filled in again with the date found. */
time_t mktime(struct tm *tm);
/* Seconds into a date: in UTC or local time. The plain forms share one
 * result (until the next call); the _r forms fill in result. */
struct tm *gmtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *result);
struct tm *localtime(const time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *result);
/* A date as text into s (max bytes with the 0), by format's conversions
 * (C99's, the "C" locale's names: %a %A %b %B %c %C %d %D %e %F %g %G %h
 * %H %I %j %m %M %n %p %r %R %S %t %T %u %U %V %w %W %x %X %y %Y %z %Z
 * %%; E and O are ignored): the length, or 0 if it did not fit. */
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm);
/* "Sun Sep 16 01:03:52 1973\n" (a result of their own, until the next call). */
char *asctime(const struct tm *tm);
char *ctime(const time_t *t);

/* (22r) Seconds and nanoseconds. */
struct timespec {
    time_t tv_sec;
    long tv_nsec;
};
/* C11's: the time now in base TIME_UTC (since 1970); TIME_UTC, or 0. */
#define TIME_UTC 1
int timespec_get(struct timespec *ts, int base);
/* POSIX's (22r): CLOCK_REALTIME the date's clock, CLOCK_MONOTONIC one that
 * never goes back (the CPU's counter since the start), and the CPU time the
 * program has used; 0, or -1 (EINVAL: another clock). nanosleep waits at
 * least req (in the timer's ticks of 10 ms); interrupted by a signal: -1,
 * EINTR, and what was left into *rem. */
typedef int clockid_t;
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
int clock_gettime(clockid_t clock, struct timespec *ts);
/* (68l) The clock set (CLOCK_REALTIME only; root only: EPERM), the
 * firmware's too, so the next start has it: 0, or -1 with errno set
 * (EINVAL: another clock, a time before 1970 or nanoseconds out of range). */
int clock_settime(clockid_t clock, const struct timespec *ts);
int nanosleep(const struct timespec *req, struct timespec *rem);

/* (43a) POSIX's beside C's:
 * - timegm: mktime for a date in UTC; tzset: takes /system/config/time's
 *   zone again (it is read at every call anyway);
 * - asctime_r, ctime_r: into buf (26 bytes or more);
 * - strptime: text to a date by a format of strftime's conversions (the
 *   C locale's names; %z +hhmm, %s seconds since 1970): the place after
 *   what was read, or 0;
 * - clock_getres: how fine a clock is; clock_nanosleep: nanosleep on a
 *   clock, TIMER_ABSTIME to sleep until a time; an error number, not -1;
 * - more clocks under Linux's names: CLOCK_MONOTONIC_RAW, _COARSE and
 *   CLOCK_BOOTTIME are CLOCK_MONOTONIC here, CLOCK_REALTIME_COARSE is
 *   CLOCK_REALTIME; (43b-6) CLOCK_THREAD_CPUTIME_ID the calling thread's
 *   CPU time (pthread_getcpuclockid gives it for the calling thread only). */
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7
#define TIMER_ABSTIME            1
time_t timegm(struct tm *tm);
void tzset(void);
char *asctime_r(const struct tm *tm, char *buf);
char *ctime_r(const time_t *t, char *buf);
char *strptime(const char *text, const char *format, struct tm *tm);
int clock_getres(clockid_t clock, struct timespec *resolution);
int clock_nanosleep(clockid_t clock, int flags, const struct timespec *request, struct timespec *remain);

/* (43b-4) A timer's setting (timerfd_settime): the first expiry, then
 * the time between (0: once). */
struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};

#ifndef RITTEROS_LOCALE_T
#define RITTEROS_LOCALE_T
typedef struct __locale_struct *locale_t;
#endif
size_t strftime_l(char *s, size_t max, const char *format, const struct tm *tm, locale_t l);

#ifdef __cplusplus
}
#endif

#endif

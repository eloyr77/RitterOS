/*
 * time.h (step 22o): the clock from SYS_TIME, the CPU time from SYS_CPUTIME
 * (the kernel counts each thread's), the calendar (proleptic Gregorian,
 * days counted from 1970-01-01 with floor division, so dates before 1970
 * work too) and strftime in the "C" locale. Local time is UTC plus the
 * offset of /system/config/time's "zone" (+HH:MM or -HH:MM), read at every
 * call (it is a small file, and a change takes effect at once). Since 22r
 * also timespec_get, clock_gettime and nanosleep.
 */
#include "cancel.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ritteros.h"

#define TIME_FILE CONFIG_DIR "/time"

time_t time(time_t *t)
{
    uint64_t ns = ritteros_time();
    time_t now = ns ? (time_t)(ns / 1000000000) : -1;
    if (t)
        *t = now;
    return now;
}

clock_t clock(void)
{
    int64_t ns = syscall6(SYS_CPUTIME, 0, 0, 0, 0, 0, 0);
    return ns < 0 ? -1 : (clock_t)(ns / (1000000000 / CLOCKS_PER_SEC));
}

double difftime(time_t end, time_t start)
{
    return (double)(end - start);
}

/* --- The calendar --- */

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

static bool leap(int64_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static const int month_start[2][13] = {
    { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 },
    { 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 },
};

/* Days from 1970-01-01 to January 1st of year y. */
static int64_t days_to_year(int64_t y)
{
    int64_t n = y - 1;                      /* (leap days up to year y - 1) */
    int64_t leaps = floor_div(n, 4) - floor_div(n, 100) + floor_div(n, 400);
    int64_t leaps1970 = 1969 / 4 - 1969 / 100 + 1969 / 400;
    return (y - 1970) * 365 + leaps - leaps1970;
}

/* Seconds into a date, the zone's offset added already. */
static void fill_date(int64_t t, long offset, const char *zone, struct tm *tm)
{
    int64_t days = floor_div(t, 86400), secs = t - days * 86400;
    tm->tm_hour = (int)(secs / 3600);
    tm->tm_min = (int)(secs / 60 % 60);
    tm->tm_sec = (int)(secs % 60);
    tm->tm_wday = (int)((days % 7 + 11) % 7);          /* 1970-01-01 was a Thursday */
    int64_t y = 1970 + floor_div(days, 365);
    while (days_to_year(y) > days)
        y--;
    while (days_to_year(y + 1) <= days)
        y++;
    int yday = (int)(days - days_to_year(y));
    const int *start = month_start[leap(y)];
    int m = 0;
    while (yday >= start[m + 1])
        m++;
    tm->tm_year = (int)(y - 1900);
    tm->tm_yday = yday;
    tm->tm_mon = m;
    tm->tm_mday = yday - start[m] + 1;
    tm->tm_isdst = 0;
    tm->tm_gmtoff = offset;
    tm->tm_zone = zone;
}

/* The local zone: its offset in seconds and its name ("UTC-05:00"). */
static long zone_offset(const char **name)
{
    static char text[16];
    char value[32];
    long offset = 0;
    if (config_get(TIME_FILE, "zone", value, sizeof(value)) > 0) {
        const char *p = value;
        int sign = *p == '-' ? -1 : 1;
        if (*p == '+' || *p == '-')
            p++;
        char *end;
        long h = strtol(p, &end, 10), m = 0;
        if (end != p && *end == ':')
            m = strtol(end + 1, &end, 10);
        if (end != p && *end == 0 && h <= 14 && m < 60)
            offset = sign * (h * 3600 + m * 60);
    }
    if (offset == 0) {
        *name = "UTC";
    } else {
        long a = offset < 0 ? -offset : offset;
        snprintf(text, sizeof(text), "UTC%c%02ld:%02ld", offset < 0 ? '-' : '+', a / 3600,
                 a / 60 % 60);
        *name = text;
    }
    return offset;
}

struct tm *gmtime_r(const time_t *t, struct tm *result)
{
    fill_date(*t, 0, "UTC", result);
    return result;
}

struct tm *gmtime(const time_t *t)
{
    static struct tm shared;
    return gmtime_r(t, &shared);
}

struct tm *localtime_r(const time_t *t, struct tm *result)
{
    const char *name;
    long offset = zone_offset(&name);
    fill_date(*t + offset, offset, name, result);
    return result;
}

struct tm *localtime(const time_t *t)
{
    static struct tm shared;
    return localtime_r(t, &shared);
}

time_t mktime(struct tm *tm)
{
    /* Carry each field into the next, months into years. */
    int64_t y = (int64_t)tm->tm_year + 1900 + floor_div(tm->tm_mon, 12);
    int64_t mon = tm->tm_mon - floor_div(tm->tm_mon, 12) * 12;
    int64_t days = days_to_year(y) + month_start[leap(y)][mon] + tm->tm_mday - 1;
    int64_t t = days * 86400 + (int64_t)tm->tm_hour * 3600 + (int64_t)tm->tm_min * 60 + tm->tm_sec;
    const char *name;
    long offset = zone_offset(&name);
    t -= offset;
    fill_date(t + offset, offset, name, tm);
    return (time_t)t;
}

/* --- Dates as text --- */

static const char *const day_names[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
                                         "Friday", "Saturday" };
static const char *const month_names[] = { "January", "February", "March", "April", "May",
                                           "June", "July", "August", "September", "October",
                                           "November", "December" };

/* ISO 8601's week-based year and week (%G, %V): weeks begin on Monday,
 * week 1 holds the year's first Thursday. */
static void iso_week(const struct tm *tm, int64_t *year, int *week)
{
    int64_t y = (int64_t)tm->tm_year + 1900;
    int wday = (tm->tm_wday + 6) % 7;                  /* Monday 0 */
    int w = (tm->tm_yday - wday + 10) / 7;
    if (w < 1) {
        y--;
        int last = leap(y) ? 366 : 365;
        w = (tm->tm_yday + last - wday + 10) / 7;
    } else if (w == 53) {
        int days = leap(y) ? 366 : 365;
        if (tm->tm_yday - wday + 3 >= days) {          /* its Thursday is next year's */
            y++;
            w = 1;
        }
    }
    *year = y;
    *week = w;
}

struct out {
    char *s;
    size_t max, n;
    bool full;
};

static void put(struct out *o, const char *text)
{
    for (; *text; text++) {
        if (o->n + 1 >= o->max) {
            o->full = true;
            return;
        }
        o->s[o->n++] = *text;
    }
}

static void put_number(struct out *o, const char *format, int64_t v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), format, (long long)v);
    put(o, buf);
}

static void format_into(struct out *o, const char *format, const struct tm *tm);

static void conversion(struct out *o, char c, const struct tm *tm)
{
    int64_t year = (int64_t)tm->tm_year + 1900, iy;
    int week, hour12 = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12;
    int wday = tm->tm_wday >= 0 && tm->tm_wday < 7 ? tm->tm_wday : 0;
    int mon = tm->tm_mon >= 0 && tm->tm_mon < 12 ? tm->tm_mon : 0;
    char small[4];
    switch (c) {
    case 'a': snprintf(small, sizeof(small), "%.3s", day_names[wday]); put(o, small); break;
    case 'A': put(o, day_names[wday]); break;
    case 'b':
    case 'h': snprintf(small, sizeof(small), "%.3s", month_names[mon]); put(o, small); break;
    case 'B': put(o, month_names[mon]); break;
    case 'c': format_into(o, "%a %b %e %H:%M:%S %Y", tm); break;
    case 'C': put_number(o, "%02lld", floor_div(year, 100)); break;
    case 'd': put_number(o, "%02lld", tm->tm_mday); break;
    case 'D': format_into(o, "%m/%d/%y", tm); break;
    case 'e': put_number(o, "%2lld", tm->tm_mday); break;
    case 'F': format_into(o, "%Y-%m-%d", tm); break;
    case 'g': iso_week(tm, &iy, &week); put_number(o, "%02lld", (iy % 100 + 100) % 100); break;
    case 'G': iso_week(tm, &iy, &week); put_number(o, "%lld", iy); break;
    case 'H': put_number(o, "%02lld", tm->tm_hour); break;
    case 'I': put_number(o, "%02lld", hour12); break;
    case 'j': put_number(o, "%03lld", tm->tm_yday + 1); break;
    case 'm': put_number(o, "%02lld", tm->tm_mon + 1); break;
    case 'M': put_number(o, "%02lld", tm->tm_min); break;
    case 'n': put(o, "\n"); break;
    case 'p': put(o, tm->tm_hour < 12 ? "AM" : "PM"); break;
    case 'r': format_into(o, "%I:%M:%S %p", tm); break;
    case 'R': format_into(o, "%H:%M", tm); break;
    case 'S': put_number(o, "%02lld", tm->tm_sec); break;
    case 't': put(o, "\t"); break;
    case 'T': format_into(o, "%H:%M:%S", tm); break;
    case 'u': put_number(o, "%lld", wday ? wday : 7); break;
    case 'U': put_number(o, "%02lld", (tm->tm_yday + 7 - wday) / 7); break;
    case 'V': iso_week(tm, &iy, &week); put_number(o, "%02lld", week); break;
    case 'w': put_number(o, "%lld", wday); break;
    case 'W': put_number(o, "%02lld", (tm->tm_yday + 7 - (wday + 6) % 7) / 7); break;
    case 'x': format_into(o, "%m/%d/%y", tm); break;
    case 'X': format_into(o, "%H:%M:%S", tm); break;
    case 'y': put_number(o, "%02lld", (year % 100 + 100) % 100); break;
    case 'Y': put_number(o, "%lld", year); break;
    case 'z': {
        long off = tm->tm_gmtoff, a = off < 0 ? -off : off;
        char z[8];
        snprintf(z, sizeof(z), "%c%02ld%02ld", off < 0 ? '-' : '+', a / 3600, a / 60 % 60);
        put(o, z);
        break;
    }
    case 'Z': put(o, tm->tm_zone ? tm->tm_zone : "UTC"); break;
    case '%': put(o, "%"); break;
    default: {                                  /* not a conversion: as it is */
        char raw[3] = { '%', c, 0 };
        put(o, raw);
    }
    }
}

static void format_into(struct out *o, const char *format, const struct tm *tm)
{
    for (const char *p = format; *p && !o->full; p++) {
        if (*p != '%' || !p[1]) {
            char one[2] = { *p, 0 };
            put(o, one);
            continue;
        }
        p++;
        if ((*p == 'E' || *p == 'O') && p[1])
            p++;                                /* (the "C" locale has no alternatives) */
        conversion(o, *p, tm);
    }
}

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm)
{
    struct out o = { s, max, 0, false };
    if (max == 0)
        return 0;
    format_into(&o, format, tm);
    s[o.n] = 0;
    return o.full ? 0 : o.n;
}

char *asctime(const struct tm *tm)
{
    static char text[64];
    int wday = tm->tm_wday >= 0 && tm->tm_wday < 7 ? tm->tm_wday : 0;
    int mon = tm->tm_mon >= 0 && tm->tm_mon < 12 ? tm->tm_mon : 0;
    snprintf(text, sizeof(text), "%.3s %.3s%3d %.2d:%.2d:%.2d %d\n", day_names[wday],
             month_names[mon], tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
             tm->tm_year + 1900);
    return text;
}

char *ctime(const time_t *t)
{
    return asctime(localtime(t));
}

/* --- 22r: timespecs --- */

static void to_timespec(uint64_t ns, struct timespec *ts)
{
    ts->tv_sec = (time_t)(ns / 1000000000);
    ts->tv_nsec = (long)(ns % 1000000000);
}

/* Nanoseconds that only go forward: the date's clock (the CPU's counter,
 * set from the firmware once), or the timer's ticks without one. */
static uint64_t monotonic_ns(void)
{
    uint64_t ns = ritteros_time();
    if (ns)
        return ns;
    struct sys_info info;
    if (syscall6(SYS_SYSINFO, (int64_t)(uintptr_t)&info, 0, 0, 0, 0, 0) < 0 || !info.ticks_per_second)
        return 0;
    return info.ticks * (1000000000 / info.ticks_per_second);
}

int timespec_get(struct timespec *ts, int base)
{
    uint64_t ns = ritteros_time();
    if (base != TIME_UTC || !ns)
        return 0;
    to_timespec(ns, ts);
    return base;
}

int clock_gettime(clockid_t clock, struct timespec *ts)
{
    int64_t ns;
    if (clock == CLOCK_REALTIME)
        ns = (int64_t)ritteros_time();
    else if (clock == CLOCK_MONOTONIC)
        ns = (int64_t)monotonic_ns();
    else if (clock == CLOCK_PROCESS_CPUTIME_ID)
        ns = syscall6(SYS_CPUTIME, 0, 0, 0, 0, 0, 0);
    else
        ns = -1;
    if (ns < 0 || (ns == 0 && clock == CLOCK_REALTIME)) {
        errno = EINVAL;
        return -1;
    }
    to_timespec((uint64_t)ns, ts);
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (req->tv_nsec < 0 || req->tv_nsec >= 1000000000 || req->tv_sec < 0) {
        errno = EINVAL;
        return -1;
    }
    uint64_t want = (uint64_t)req->tv_sec * 1000000000 + (uint64_t)req->tv_nsec;
    uint64_t start = monotonic_ns();
    cancel_point();                             /* (54c) */
    int64_t r = cancel_after(syscall6(SYS_SLEEP, (int64_t)((want + 999999) / 1000000), 0, 0, 0, 0, 0));
    if (r >= 0)
        return 0;
    if (rem) {
        uint64_t spent = monotonic_ns() - start;
        to_timespec(spent < want ? want - spent : 0, rem);
    }
    errno = (int)-r;
    return -1;
}

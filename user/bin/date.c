/*
 * date [-u] [-d @SECONDS] [-s TIME] [+FORMAT] (51d): the date and time now,
 * or of SECONDS since 1970 (-d @SECONDS, a common extra), in local time
 * (the zone of /system/config/time) or UTC (-u), as FORMAT says
 * (strftime's conversions; by default "%a %b %e %H:%M:%S %Z %Y"). (68l)
 * -s TIME sets the clock (root: clock_settime, the firmware's clock too)
 * and prints the new time: TIME is @SECONDS or "YYYY-MM-DD HH:MM[:SS]" (or
 * a date alone, at midnight), local time, or UTC with -u, as Linux's date.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ritteros.h"

PROGRAM_HELP("date [-u] [-d @SECONDS] [-s TIME] [+FORMAT]", "the date and time (local or -u UTC); -s sets the clock");

/* -s's TIME: @SECONDS, or a date and time (local, or UTC with utc). */
static bool parse_time(const char *text, bool utc, time_t *when)
{
    if (text[0] == '@') {
        char *end;
        long long v = strtoll(text + 1, &end, 10);
        if (!text[1] || *end || v < 0)
            return false;
        *when = (time_t)v;
        return true;
    }
    static const char *const formats[] = { "%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%d" };
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        struct tm tm = { 0 };
        const char *end = strptime(text, formats[i], &tm);
        if (end && !*end) {
            tm.tm_isdst = -1;
            *when = utc ? timegm(&tm) : mktime(&tm);
            return *when >= 0;
        }
    }
    return false;
}

int main(int argc, char **argv)
{
    bool utc = false;
    time_t when = time(0);
    const char *format = "%a %b %e %H:%M:%S %Z %Y", *set = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-u")) {
            utc = true;
        } else if (!strcmp(argv[i], "-d") && i + 1 < argc && argv[i + 1][0] == '@') {
            char *end;
            when = (time_t)strtoll(argv[++i] + 1, &end, 10);
            if (*end) {
                fprintf(stderr, "date: %s: not @SECONDS\n", argv[i]);
                return 1;
            }
        } else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            set = argv[++i];
        } else if (argv[i][0] == '+') {
            format = argv[i] + 1;
        } else {
            fprintf(stderr, "usage: date [-u] [-d @SECONDS] [-s TIME] [+FORMAT]\n");
            return 1;
        }
    }
    if (set) {
        if (!parse_time(set, utc, &when)) {
            fprintf(stderr, "date: %s: not @SECONDS or YYYY-MM-DD HH:MM[:SS]\n", set);
            return 1;
        }
        struct timespec ts = { when, 0 };
        if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
            fprintf(stderr, "date: cannot set the clock: %s\n", strerror(errno));
            return 1;
        }
    }
    struct tm tm;
    if (utc)
        gmtime_r(&when, &tm);
    else
        localtime_r(&when, &tm);
    static char text[4096];
    size_t n = strftime(text, sizeof(text), format, &tm);
    if (n == 0 && *format) {
        fprintf(stderr, "date: the text is too long\n");
        return 1;
    }
    printf("%s\n", text);
    return 0;
}

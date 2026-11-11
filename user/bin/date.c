/*
 * date [-u] [-d @SECONDS] [+FORMAT] (51d): the date and time now, or of
 * SECONDS since 1970 (-d @SECONDS, a common extra), in local time (the zone
 * of /system/config/time) or UTC (-u), as FORMAT says (strftime's
 * conversions; by default "%a %b %e %H:%M:%S %Z %Y"). Setting the clock
 * is not done here.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ritteros.h"

PROGRAM_HELP("date [-u] [-d @SECONDS] [+FORMAT]", "the date and time (local or -u UTC)");

int main(int argc, char **argv)
{
    bool utc = false;
    time_t when = time(0);
    const char *format = "%a %b %e %H:%M:%S %Z %Y";
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
        } else if (argv[i][0] == '+') {
            format = argv[i] + 1;
        } else {
            fprintf(stderr, "usage: date [-u] [-d @SECONDS] [+FORMAT]\n");
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

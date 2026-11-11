/*
 * sleep SECONDS... (51d, POSIX; fractions such as 0.5 and the suffixes s m
 * h d are common extras): wait that long (several: their sum).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ritteros.h"

PROGRAM_HELP("sleep SECONDS...", "wait (fractions like 0.5; s, m, h, d)");

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: sleep SECONDS...\n");
        return 2;
    }
    double total = 0;
    for (int i = 1; i < argc; i++) {
        char *end;
        double v = strtod(argv[i], &end);
        double unit = *end == 'm' ? 60 : *end == 'h' ? 3600 : *end == 'd' ? 86400 : 1;
        if (*end && strchr("smhd", *end))
            end++;
        if (end == argv[i] || *end || v < 0) {
            fprintf(stderr, "sleep: %s: not a time\n", argv[i]);
            return 2;
        }
        total += v * unit;
    }
    struct timespec t = { (time_t)total, (long)((total - (double)(time_t)total) * 1e9) };
    while (nanosleep(&t, &t) < 0 && errno == EINTR)
        ;
    return 0;
}

/*
 * touch [-acm] [-r REFERENCE | -t TIME] FILE... (POSIX's; since 63m with
 * the system's call for a file's times, utimensat): each FILE gets the
 * time now, REFERENCE's time (-r), or TIME (-t [[CC]YY]MMDDhhmm[.SS], the
 * machine's local time; YY 69-99 is 19YY, 00-68 20YY; without them this
 * year). A missing FILE is made empty, unless -c. -a sets the last access
 * time and -m the last modification (both without either); RitterOS keeps
 * no access time, so -a alone changes nothing kept. Folders too. The time
 * now needs the right to write the file (or owning it); any other time
 * owning it (or root).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("touch [-acm] [-r REFERENCE | -t [[CC]YY]MMDDhhmm[.SS]] FILE...",
             "make empty files, or give files the time now (or another)");

static void usage(void)
{
    dprintf(STDERR_FILENO, "usage: touch [-acm] [-r REFERENCE | -t [[CC]YY]MMDDhhmm[.SS]] FILE...\n");
    exit(2);
}

/* -t's [[CC]YY]MMDDhhmm[.SS] as seconds since 1970 (local time), or -1. */
static time_t parse_stamp(const char *s)
{
    const char *dot = strchr(s, '.');
    size_t n = dot ? (size_t)(dot - s) : strlen(s);
    if ((n != 8 && n != 10 && n != 12) || (dot && strlen(dot + 1) != 2))
        return -1;
    for (const char *p = s; *p; p++)
        if (p != dot && (*p < '0' || *p > '9'))
            return -1;
    int v[6], k = 0;
    for (size_t i = 0; i < n; i += 2)
        v[k++] = (s[i] - '0') * 10 + (s[i + 1] - '0');
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    time_t now = time(0);
    struct tm *today = localtime(&now);
    int at = 0;
    if (n == 12)
        tm.tm_year = v[0] * 100 + v[1] - 1900, at = 2;
    else if (n == 10)
        tm.tm_year = (v[0] < 69 ? 2000 + v[0] : 1900 + v[0]) - 1900, at = 1;
    else
        tm.tm_year = today ? today->tm_year : 70;
    tm.tm_mon = v[at] - 1;
    tm.tm_mday = v[at + 1];
    tm.tm_hour = v[at + 2];
    tm.tm_min = v[at + 3];
    tm.tm_sec = dot ? (dot[1] - '0') * 10 + (dot[2] - '0') : 0;
    tm.tm_isdst = -1;
    if (tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31 || tm.tm_hour > 23 || tm.tm_min > 59 ||
        tm.tm_sec > 60)
        return -1;
    return mktime(&tm);
}

int main(int argc, char **argv)
{
    bool access = false, modification = false, create = true;
    const char *reference = 0, *stamp = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for (const char *k = argv[i] + 1; *k; k++) {
            if (*k == 'a')
                access = true;
            else if (*k == 'm')
                modification = true;
            else if (*k == 'c')
                create = false;
            else if (*k == 'r' || *k == 't') {
                const char *value = k[1] ? k + 1 : i + 1 < argc ? argv[++i] : 0;
                if (!value)
                    usage();
                if (*k == 'r')
                    reference = value;
                else
                    stamp = value;
                break;
            } else
                usage();
        }
    }
    if (i == argc || (reference && stamp))
        usage();
    if (!access && !modification)
        access = modification = true;
    struct timespec when[2] = { { 0, UTIME_NOW }, { 0, UTIME_NOW } };
    if (reference) {
        struct stat st;
        if (stat(reference, &st) < 0) {
            dprintf(STDERR_FILENO, "touch: %s: %s\n", reference, strerror(errno));
            return 1;
        }
        when[0].tv_sec = when[1].tv_sec = st.st_mtime;      /* (no access time is kept) */
        when[0].tv_nsec = when[1].tv_nsec = 0;
    } else if (stamp) {
        time_t t = parse_stamp(stamp);
        if (t < 0) {
            dprintf(STDERR_FILENO, "touch: %s: not a time as [[CC]YY]MMDDhhmm[.SS]\n", stamp);
            return 2;
        }
        when[0].tv_sec = when[1].tv_sec = t;
        when[0].tv_nsec = when[1].tv_nsec = 0;
    }
    if (!access)
        when[0].tv_nsec = UTIME_OMIT;
    if (!modification)
        when[1].tv_nsec = UTIME_OMIT;
    int status = 0;
    for (; i < argc; i++) {
        const char *name = argv[i];
        struct stat st;
        if (stat(name, &st) < 0) {
            if (!create)
                continue;                           /* (-c: a missing one stays missing) */
            int fd = open(name, O_WRONLY | O_CREAT, 0666);
            if (fd < 0 || close(fd) < 0) {
                dprintf(STDERR_FILENO, "touch: %s: %s\n", name, strerror(errno));
                status = 1;
                continue;
            }
            if (!reference && !stamp)
                continue;                           /* (made now: its time is now) */
        }
        if (utimensat(AT_FDCWD, name, when, 0) < 0) {
            dprintf(STDERR_FILENO, "touch: %s: %s\n", name, strerror(errno));
            status = 1;
        }
    }
    return status;
}

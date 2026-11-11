/*
 * basename STRING [SUFFIX] (51d, POSIX): STRING without its directories
 * (and without SUFFIX, unless that is all of it). Trailing slashes do not
 * count; "/" stays "/".
 */
#include <stdio.h>
#include <string.h>
#include "ritteros.h"

PROGRAM_HELP("basename STRING [SUFFIX]", "a path's last part (without SUFFIX)");

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: basename STRING [SUFFIX]\n");
        return 2;
    }
    char *s = argv[1];
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/')
        n--;                                    /* (trailing slashes) */
    if (n == 1 && s[0] == '/') {
        printf("/\n");
        return 0;
    }
    size_t start = n;
    while (start > 0 && s[start - 1] != '/')
        start--;
    size_t len = n - start;
    if (argc == 3) {
        size_t k = strlen(argv[2]);
        if (k < len && !strncmp(s + start + len - k, argv[2], k))
            len -= k;
    }
    printf("%.*s\n", (int)len, s + start);
    return 0;
}

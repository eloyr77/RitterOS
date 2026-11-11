/*
 * dirname STRING (51d, POSIX): the directory part of a path: "." for a
 * name without one, "/" for one in the root.
 */
#include <stdio.h>
#include <string.h>
#include "ritteros.h"

PROGRAM_HELP("dirname STRING", "a path without its last part");

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: dirname STRING\n");
        return 2;
    }
    char *s = argv[1];
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/')
        n--;                                    /* (trailing slashes) */
    while (n > 0 && s[n - 1] != '/')
        n--;                                    /* (the last part) */
    if (n == 0) {
        printf(".\n");
        return 0;
    }
    while (n > 1 && s[n - 1] == '/')
        n--;                                    /* (the slashes before it) */
    printf("%.*s\n", (int)n, s);
    return 0;
}

/*
 * head [-n N | -N] [-c N] [FILE]... (51e, POSIX; -c and the traditional
 * -N are common extras): the first N lines (10 by default) or N bytes of each file or of the input;
 * with several files each under a "==> NAME <==" line.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ritteros.h"

PROGRAM_HELP("head [-n N | -N] [-c N] [FILE]...", "the first lines (or bytes) of files");

static long long count = 10;
static bool bytes;

static int head(FILE *f)
{
    long long left = count;
    int c;
    while (left > 0 && (c = fgetc(f)) != EOF) {
        putc(c, stdout);
        if (bytes || c == '\n')
            left--;
    }
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, 0, _IOFBF, BUFSIZ);         /* (a write per buffer, not per byte) */
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if (argv[i][1] >= '0' && argv[i][1] <= '9') {
            count = atoll(argv[i] + 1);         /* (63d) -N: the traditional -n N */
            bytes = false;
            continue;
        }
        char opt = argv[i][1];
        const char *v = argv[i][2] ? argv[i] + 2 : i + 1 < argc ? argv[++i] : 0;
        if ((opt != 'n' && opt != 'c') || !v || atoll(v) < 0) {
            fprintf(stderr, "usage: head [-n N | -N] [-c N] [FILE]...\n");
            return 2;
        }
        count = atoll(v);
        bytes = opt == 'c';
    }
    if (i == argc)
        return head(stdin);
    int status = 0;
    for (int k = i; k < argc; k++) {
        FILE *f = fopen(argv[k], "r");
        if (!f) {
            perror(argv[k]);
            status = 1;
            continue;
        }
        if (argc - i > 1)
            fprintf(stdout, "%s==> %s <==\n", k > i ? "\n" : "", argv[k]);
        head(f);
        fclose(f);
    }
    fflush(stdout);
    return status;
}

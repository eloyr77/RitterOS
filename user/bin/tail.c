/*
 * tail [-f] [-n [+]N] [-c [+]N] [FILE]... (51e, POSIX; -c is a common extra):
 * the last N lines (10 by default) or bytes of a file or of the input; +N
 * from the Nth on. Several FILEs (50k, as BSD's and GNU's) each under a
 * "==> FILE <==" heading. -f: then keep following the files, printing
 * what is added (every second) until Ctrl-C.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("tail [-f] [-n [+]N | -N] [-c [+]N] [FILE]...", "the last lines (or bytes) of a file; -f: follow it");

/* The asked-for part of f to the output; the bytes read, or -1 (memory). */
static long long show(FILE *f, long long count, bool bytes, bool from_start)
{
    /* All of it into memory, then the part asked for. */
    size_t size = 65536, used = 0;
    char *text = malloc(size);
    int c;
    while (text && (c = fgetc(f)) != EOF) {
        if (used == size) {
            char *bigger = realloc(text, size * 2);
            if (!bigger) {
                free(text);
                text = 0;
                break;
            }
            text = bigger;
            size *= 2;
        }
        text[used++] = (char)c;
    }
    if (!text)
        return -1;
    size_t start = 0;
    if (bytes) {
        if (from_start)
            start = count > 0 ? (size_t)count - 1 : 0;
        else
            start = (size_t)count < used ? used - (size_t)count : 0;
    } else if (from_start) {
        long long line = 1;
        while (start < used && line < count)
            if (text[start++] == '\n')
                line++;
    } else {
        start = used;
        long long lines = 0;
        if (start > 0 && text[start - 1] == '\n')
            start--;                            /* (the last line's break) */
        while (start > 0 && !(text[start - 1] == '\n' && ++lines == count))
            start--;
        if (count == 0)
            start = used;
    }
    if (start > used)
        start = used;
    fwrite(text + start, 1, used - start, stdout);
    free(text);
    return (long long)used;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, 0, _IOFBF, BUFSIZ);         /* (a write per buffer, not per byte) */
    long long count = 10;
    bool bytes = false, from_start = false, follow = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if (!strcmp(argv[i], "-f")) {
            follow = true;
            continue;
        }
        if (argv[i][1] >= '0' && argv[i][1] <= '9') {
            count = atoll(argv[i] + 1);         /* (63c) -N: the traditional -n N */
            from_start = bytes = false;
            continue;
        }
        char opt = argv[i][1];
        const char *v = argv[i][2] ? argv[i] + 2 : i + 1 < argc ? argv[++i] : 0;
        if ((opt != 'n' && opt != 'c') || !v) {
            fprintf(stderr, "usage: tail [-f] [-n [+]N | -N] [-c [+]N] [FILE]...\n");
            return 2;
        }
        from_start = v[0] == '+';
        count = llabs(atoll(v));
        bytes = opt == 'c';
    }
    /* (50k) Several FILEs, as BSD's and GNU's tail: each under a heading. */
    int files = argc - i, status = 0;
    bool shown_one = false;                     /* a blank line goes between files shown */
    long long *at = calloc(files > 0 ? (size_t)files : 1, sizeof(long long));
    for (int k = 0; k < (files > 0 ? files : 1); k++) {
        const char *name = files > 0 ? argv[i + k] : 0;
        FILE *f = name ? fopen(name, "r") : stdin;
        if (!f) {
            int e = errno;
            fflush(stdout);                     /* (in order with what came before) */
            fprintf(stderr, "tail: %s: %s\n", name, strerror(e));
            status = 1;
            at[k] = -1;
            continue;
        }
        if (files > 1)
            fprintf(stdout, "%s==> %s <==\n", shown_one ? "\n" : "", name);
        shown_one = true;
        long long used = show(f, count, bytes, from_start);
        if (name)
            fclose(f);
        if (used < 0) {
            fprintf(stderr, "tail: out of memory\n");
            return 1;
        }
        at[k] = used;
    }
    fflush(stdout);
    int shown = files - 1;                      /* the file whose heading came last */
    while (follow && files > 0) {               /* (until Ctrl-C ends it) */
        sleep(1);
        for (int k = 0; k < files; k++) {
            struct stat st;
            if (at[k] < 0 || stat(argv[i + k], &st) != 0)
                continue;
            if (st.st_size < at[k])
                at[k] = st.st_size;             /* (it was cut: from its new end) */
            if (st.st_size <= at[k])
                continue;
            FILE *g = fopen(argv[i + k], "r");
            if (!g)
                continue;
            if (files > 1 && shown != k)
                fprintf(stdout, "\n==> %s <==\n", argv[i + k]);
            shown = k;
            fseek(g, (long)at[k], SEEK_SET);
            int c;
            while ((c = fgetc(g)) != EOF) {
                putc(c, stdout);
                at[k]++;
            }
            fclose(g);
            fflush(stdout);
        }
    }
    return status;
}

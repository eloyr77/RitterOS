/*
 * cut -b LIST | -c LIST | -f LIST [-d C] [-s] [FILE]... (51e, POSIX): parts
 * of each line. LIST is numbers and ranges (1-based) like 1,3-5,7-; -b
 * bytes, -c characters (a byte each: the "C" locale), -f fields between
 * tabs or -d C; lines without the delimiter are printed whole unless -s.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ritteros.h"

PROGRAM_HELP("cut -b LIST [-n] | -c LIST | -f LIST [-d C] [-s] [FILE]...",
             "parts of each line");

#define RANGES 64

static struct { long from, to; } ranges[RANGES];   /* to 0: to the end */
static int range_count;
static char mode, delim = '\t';
static bool only_delimited;

static bool parse_list(const char *s)
{
    while (*s) {
        if (range_count == RANGES)
            return false;
        long from = 1, to;
        char *end;
        if (*s != '-') {
            from = strtol(s, &end, 10);
            if (end == s || from < 1)
                return false;
            s = end;
        }
        to = from;
        if (*s == '-') {
            s++;
            to = 0;
            if (*s >= '0' && *s <= '9') {
                to = strtol(s, &end, 10);
                s = end;
                if (to < from)
                    return false;
            }
        }
        ranges[range_count].from = from;
        ranges[range_count++].to = to;
        if (*s == ',')
            s++;
        else if (*s)
            return false;
    }
    return range_count > 0;
}

static bool chosen(long n)
{
    for (int k = 0; k < range_count; k++)
        if (n >= ranges[k].from && (!ranges[k].to || n <= ranges[k].to))
            return true;
    return false;
}

static void cut_line(const char *s, size_t len)
{
    if (mode != 'f') {
        for (size_t k = 0; k < len; k++)
            if (chosen((long)k + 1))
                putc(s[k], stdout);
        putc('\n', stdout);
        return;
    }
    if (!memchr(s, delim, len)) {
        if (!only_delimited) {
            fwrite(s, 1, len, stdout);
            putc('\n', stdout);
        }
        return;
    }
    long field = 1;
    bool first = true;
    size_t start = 0;
    for (size_t k = 0; k <= len; k++) {
        if (k < len && s[k] != delim)
            continue;
        if (chosen(field)) {
            if (!first)
                putc(delim, stdout);
            fwrite(s + start, 1, k - start, stdout);
            first = false;
        }
        field++;
        start = k + 1;
    }
    putc('\n', stdout);
}

static void cut_file(FILE *f)
{
    size_t size = 256, n = 0;
    char *s = malloc(size);
    int c;
    while (s && (c = fgetc(f)) != EOF) {
        if (c == '\n') {
            cut_line(s, n);
            n = 0;
            continue;
        }
        if (n == size) {
            char *bigger = realloc(s, size *= 2);
            if (!bigger)
                break;
            s = bigger;
        }
        s[n++] = (char)c;
    }
    if (s && n)
        cut_line(s, n);
    free(s);
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
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 's') {
                only_delimited = true;
                continue;
            }
            if (*o != 'b' && *o != 'c' && *o != 'f' && *o != 'd' && *o != 'n')
                goto usage;
            if (*o == 'n')                      /* (POSIX: -n with -b, ignored) */
                continue;
            const char *v = o[1] ? o + 1 : i + 1 < argc ? argv[++i] : 0;
            if (!v)
                goto usage;
            if (*o == 'd') {
                delim = v[0];
            } else {
                if (mode || !parse_list(v))
                    goto usage;
                mode = *o;
            }
            break;
        }
    }
    if (!mode)
        goto usage;
    int status = 0;
    if (i == argc)
        cut_file(stdin);
    for (int k = i; k < argc; k++) {
        FILE *f = strcmp(argv[k], "-") ? fopen(argv[k], "r") : stdin;
        if (!f) {
            perror(argv[k]);
            status = 1;
            continue;
        }
        cut_file(f);
        if (f != stdin)
            fclose(f);
    }
    return status;
usage:
    fprintf(stderr, "usage: cut -b LIST | -c LIST | -f LIST [-d C] [-s] [FILE]...\n");
    return 1;
}

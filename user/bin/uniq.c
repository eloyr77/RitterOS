/*
 * uniq [-c] [-d | -u] [-i] [-f N] [-s N] [INPUT [OUTPUT]] (51e, POSIX; -i is
 * a common extra): the input with each run of equal lines once. -c puts
 * the run's length before it (4 columns, as BSD), -d prints only repeated
 * lines, -u only unrepeated ones; -f N skips N fields (blanks and then
 * non-blanks) and -s N characters before comparing, -i ignores case.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ritteros.h"

PROGRAM_HELP("uniq [-c] [-d | -u] [-i] [-f N] [-s N] [INPUT [OUTPUT]]",
             "runs of equal lines once");

static bool counts, repeated, unrepeated, nocase;
static int skip_fields, skip_chars;

static char *line_of(FILE *f)
{
    size_t size = 128, n = 0;
    char *s = malloc(size);
    int c = 0;
    while (s && (c = fgetc(f)) != EOF && c != '\n') {
        if (n + 1 == size) {
            char *bigger = realloc(s, size *= 2);
            if (!bigger) {
                free(s);
                return 0;
            }
            s = bigger;
        }
        s[n++] = (char)c;
    }
    if (!s || (c == EOF && n == 0)) {
        free(s);
        return 0;
    }
    s[n] = 0;
    return s;
}

static const char *key(const char *s)
{
    for (int k = 0; k < skip_fields; k++) {
        while (*s == ' ' || *s == '\t')
            s++;
        while (*s && *s != ' ' && *s != '\t')
            s++;
    }
    for (int k = 0; k < skip_chars && *s; k++)
        s++;
    return s;
}

static bool same(const char *a, const char *b)
{
    a = key(a), b = key(b);
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (nocase) {
            x = x >= 'A' && x <= 'Z' ? x + 32 : x;
            y = y >= 'A' && y <= 'Z' ? y + 32 : y;
        }
        if (x != y)
            return false;
    }
    return *a == *b;
}

static void emit(FILE *out, const char *line, long n)
{
    if ((repeated && n < 2) || (unrepeated && n > 1))
        return;
    if (counts)
        fprintf(out, "%4ld %s\n", n, line);
    else
        fprintf(out, "%s\n", line);
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
            if (*o == 'c')
                counts = true;
            else if (*o == 'd')
                repeated = true;
            else if (*o == 'u')
                unrepeated = true;
            else if (*o == 'i')
                nocase = true;
            else if (*o == 'f' || *o == 's') {
                const char *v = o[1] ? o + 1 : i + 1 < argc ? argv[++i] : 0;
                if (!v)
                    goto usage;
                *(*o == 'f' ? &skip_fields : &skip_chars) = atoi(v);
                break;
            } else
                goto usage;
        }
    }
    if (argc - i > 2)
        goto usage;
    FILE *in = i < argc && strcmp(argv[i], "-") ? fopen(argv[i], "r") : stdin;
    if (!in) {
        perror(argv[i]);
        return 1;
    }
    FILE *out = i + 1 < argc ? fopen(argv[i + 1], "w") : stdout;
    if (!out) {
        perror(argv[i + 1]);
        return 1;
    }
    char *last = 0, *s;
    long n = 0;
    while ((s = line_of(in))) {
        if (last && same(last, s)) {
            n++;
            free(s);
            continue;
        }
        if (last)
            emit(out, last, n);
        free(last);
        last = s, n = 1;
    }
    if (last)
        emit(out, last, n);
    if (out != stdout)
        fclose(out);
    return 0;
usage:
    fprintf(stderr, "usage: uniq [-c] [-d|-u] [-i] [-f N] [-s N] [INPUT [OUTPUT]]\n");
    return 1;
}

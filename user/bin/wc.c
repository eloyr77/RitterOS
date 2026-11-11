/*
 * wc [-c|-m] [-l] [-w] [FILE]... (51e, POSIX): the lines, words and bytes
 * of each file or of the input (all three unless some are asked for;
 * characters with -m: here bytes, the "C" locale's), with a total for
 * several. Each count in 8 columns, as BSD's wc.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "ritteros.h"

PROGRAM_HELP("wc [-l] [-w] [-c | -m] [FILE]...",
             "count lines, words and bytes");

static bool lines, words, chars;

static void show(long long l, long long w, long long c, const char *name)
{
    if (lines)
        printf("%8lld", l);
    if (words)
        printf("%8lld", w);
    if (chars)
        printf("%8lld", c);
    printf("%s%s\n", name ? " " : "", name ? name : "");
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'l')
                lines = true;
            else if (*o == 'w')
                words = true;
            else if (*o == 'c' || *o == 'm')
                chars = true;
            else {
                fprintf(stderr, "usage: wc [-l] [-w] [-c] [FILE]...\n");
                return 2;
            }
        }
    }
    if (!lines && !words && !chars)
        lines = words = chars = true;
    long long tl = 0, tw = 0, tc = 0;
    int status = 0, files = argc - i;
    for (int k = i; k < argc || (files == 0 && k == i); k++) {
        FILE *f = files ? fopen(argv[k], "r") : stdin;
        if (!f) {
            perror(argv[k]);
            status = 1;
            continue;
        }
        long long l = 0, w = 0, c = 0;
        bool in_word = false;
        int ch;
        while ((ch = fgetc(f)) != EOF) {
            c++;
            if (ch == '\n')
                l++;
            bool space = ch == ' ' || (ch >= '\t' && ch <= '\r');
            if (!space && !in_word)
                w++;
            in_word = !space;
        }
        if (files)
            fclose(f);
        show(l, w, c, files ? argv[k] : 0);
        tl += l, tw += w, tc += c;
    }
    if (files > 1)
        show(tl, tw, tc, "total");
    return status;
}

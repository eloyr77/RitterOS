/*
 * tee [-a] [-i] [FILE]... (51e, POSIX): the input to the output and into
 * each FILE (-a: added at their ends). -i: Ctrl-C does not end it.
 */
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("tee [-a] [-i] [FILE]...", "copy the input to the output and into files");

int main(int argc, char **argv)
{
    bool append = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-a"))
            append = true;
        else if (!strcmp(argv[i], "-i"))
            signal(SIGINT, SIG_IGN);
        else if (!strcmp(argv[i], "--")) {
            i++;
            break;
        } else {
            fprintf(stderr, "usage: tee [-a] [-i] [FILE]...\n");
            return 2;
        }
    }
    FILE *files[64];
    int n = 0, status = 0;
    for (; i < argc && n < 64; i++) {
        FILE *f = fopen(argv[i], append ? "a" : "w");
        if (!f) {
            perror(argv[i]);
            status = 1;
            continue;
        }
        files[n++] = f;
    }
    char buf[4096];
    ssize_t got;
    while ((got = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        write(STDOUT_FILENO, buf, (size_t)got);
        for (int k = 0; k < n; k++)
            fwrite(buf, 1, (size_t)got, files[k]);
    }
    for (int k = 0; k < n; k++)
        if (fclose(files[k]) != 0)
            status = 1;
    return status;
}

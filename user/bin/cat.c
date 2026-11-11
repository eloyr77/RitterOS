/*
 * cat [FILE]...: copy files (or, without arguments, what is typed until
 * Ctrl-D) to standard output. A file with a zero byte in it is binary: shown
 * on the terminal it would garble the screen, so cat names it instead -- but
 * only when standard output is a terminal; redirected to a file or a pipe
 * (e.g. over ssh), binary passes through unchanged.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("cat [FILE]...", "show files, or what is typed until Ctrl-D");

static char buffer[4096];
static bool to_terminal;                /* refuse binary only when it would reach the screen */

static int copy(int fd, const char *name)
{
    for (;;) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n == 0)
            return 0;
        if (n < 0) {
            dprintf(STDERR_FILENO, "cat: %s: %s\n", name, strerror(errno));
            return 1;
        }
        if (to_terminal && memchr(buffer, 0, (size_t)n)) {
            dprintf(STDERR_FILENO, "cat: %s: binary file\n", name);
            return 1;
        }
        write(STDOUT_FILENO, buffer, (size_t)n);
    }
}

int main(int argc, char **argv)
{
    to_terminal = isatty(STDOUT_FILENO);
    if (argc == 1)
        return copy(STDIN_FILENO, "standard input");
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "cat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        status |= copy(fd, argv[i]);
        close(fd);
    }
    return status;
}

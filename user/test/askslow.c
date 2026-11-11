/* Test: askslow PROMPT PAUSE_MS: print PROMPT (and a space, no line
 * break), wait, then read a line and say what came. A message that comes
 * during the wait, before the read, must not take the prompt away for
 * good: the terminal shows it again below the message (like login's
 * prompt among the elves' first lines at the boot). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        dprintf(STDERR_FILENO, "usage: askslow PROMPT PAUSE_MS\n");
        return 2;
    }
    int pause = 0;
    for (const char *s = argv[2]; *s >= '0' && *s <= '9'; s++)
        pause = pause * 10 + (*s - '0');
    printf("%s ", argv[1]);
    usleep((useconds_t)pause * 1000);
    char line[80];
    ssize_t n = read(STDIN_FILENO, line, sizeof(line) - 1);
    if (n < 0)
        n = 0;
    line[n] = 0;
    if (n && line[n - 1] == '\n')
        line[n - 1] = 0;
    printf("askslow: got '%s'\n", line);
    return 0;
}

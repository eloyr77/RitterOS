/* Test: counter NAME COUNT PAUSE_MS: print COUNT numbered lines with a pause
 * between them, so that other programs run in between. */
#include <stdio.h>
#include <unistd.h>

static int number(const char *s)
{
    int n = 0;
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return n;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        dprintf(STDERR_FILENO, "usage: counter NAME COUNT PAUSE_MS\n");
        return 2;
    }
    int count = number(argv[2]), pause = number(argv[3]);
    for (int i = 1; i <= count; i++) {
        printf("counter %s: %d\n", argv[1], i);
        if (i < count)
            usleep((useconds_t)pause * 1000);   /* none after the last line */
    }
    return 0;
}

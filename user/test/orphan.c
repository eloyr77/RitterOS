/* Test: a parent that ends while its child still runs. The child becomes an
 * orphan, goes on, and is cleaned up when it ends. */
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    (void)argc;
    char path[160];
    size_t n = 0;
    snprintf(path, sizeof(path), "%s", argv[0]);
    for (size_t i = 0; path[i]; i++)
        if (path[i] == '/')
            n = i + 1;
    snprintf(path + n, sizeof(path) - n, "counter");
    char *child[] = { "counter", "orphan", "3", "200", 0 };
    printf("orphan: started child %d, ending now\n", spawn(path, child));
    return 0;
}

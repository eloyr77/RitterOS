/* Test: a job of two processes, parent and child, both computing; Ctrl-C
 * stops the whole job (its process group). */
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    (void)argc;
    char path[160];
    snprintf(path, sizeof(path), "%s", argv[0]);
    /* the loop program lives next to this one */
    size_t n = 0;
    for (size_t i = 0; path[i]; i++)
        if (path[i] == '/')
            n = i + 1;
    snprintf(path + n, sizeof(path) - n, "loop");
    char *child[] = { "loop", 0 };
    pid_t pid = spawn(path, child);
    printf("tree: parent %d and child %d running until Ctrl-C\n", getpid(), pid);
    for (volatile int i = 0;; i++)
        ;
}

/*
 * Test: the dynamic loader and the shared libraries (steps 22b-22c). Built
 * twice: ldtest like every program (dynamically linked) and ldtest-static
 * (with libritteros.a, as before 22b). Both check that errno is per thread
 * (since 22i: a thread's errno is at another address than the main
 * thread's, in each thread's own data), and that errno, malloc, a thread
 * (whose start is the library's) and a file work. (Until 22i errno's place
 * also told the two apart: a library's data or the program's.)
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread.h>
#include <unistd.h>
#include "ritteros.h"

static int failures;

static void result(const char *name, int ok)
{
    printf("ldtest: %s: %s\n", name, ok ? "ok" : "FAILED");
    failures += !ok;
}

static int *worker_errno;

static int worker(void *arg)
{
    worker_errno = &errno;
    return (int)(long)arg * 2;
}

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    int first = thread_create(worker, (void *)1L);
    result("errno is per thread", first > 0 && thread_join(first, 0) == 0 && worker_errno &&
                                  worker_errno != &errno);
    errno = 0;
    result("errno", open("/no/such/file", O_RDONLY) < 0 && errno == ENOENT);
    char *m = malloc(1 << 20);
    int ok = m != 0;
    if (m) {
        memset(m, 7, 1 << 20);
        ok = m[12345] == 7;
        free(m);
    }
    result("malloc", ok);
    int t = thread_create(worker, (void *)21L), value = 0;
    result("a thread", t > 0 && thread_join(t, &value) == 0 && value == 42);
    int fd = open("/system/README", O_RDONLY);
    char buf[16];
    result("a file", fd < 0 || (read(fd, buf, sizeof(buf)) >= 0 && close(fd) == 0));
    printf("ldtest: %s\n", failures ? "FAILED" : "all ok");
    return failures != 0;
}

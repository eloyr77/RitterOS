/*
 * Test: per-thread data (step 22i). Built twice, like ldtest: tlstest
 * (dynamic: the program's block and libritteros's, laid out by ld.so) and
 * tlstest-static (with libritteros.a: one block, from the program's start
 * code). Checks: the initial values (data with a value, data zeroed) in the
 * main thread; four threads each starting from the initial values, not the
 * main thread's changes, and keeping their own through each other's
 * changes; a 64-byte aligned variable aligned in every thread; the main
 * thread's own values kept; errno per thread (two threads failing with
 * different errors at once, each seeing its own).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <thread.h>
#include <unistd.h>

static __thread int counter = 1234;             /* .tdata */
static __thread char scratch[100];              /* .tbss */
static __thread long wide __attribute__((aligned(64)));

static int failures;

static void result(const char *name, int ok)
{
    printf("tlstest: %s: %s\n", name, ok ? "ok" : "FAILED");
    failures += !ok;
}

static int own(void *arg)
{
    int id = (int)(intptr_t)arg, ok = counter == 1234 && scratch[0] == 0 && wide == 0 &&
                                      ((uintptr_t)&wide & 63) == 0;
    counter = id * 10;
    memset(scratch, id, sizeof(scratch));
    wide = id;
    for (int i = 0; i < 20; i++) {
        usleep(2000);                            /* let the others change theirs */
        ok = ok && counter == id * 10 && scratch[0] == id && scratch[99] == id && wide == id;
    }
    return ok;
}

static int fails_enoent(void *arg)
{
    (void)arg;
    int ok = 1;
    for (int i = 0; i < 30; i++) {
        ok = ok && open("/no/such/file", O_RDONLY) < 0;
        usleep(1000);
        ok = ok && errno == ENOENT;
    }
    return ok;
}

static int fails_ebadf(void *arg)
{
    (void)arg;
    int ok = 1;
    for (int i = 0; i < 30; i++) {
        ok = ok && close(-1) < 0;
        usleep(1000);
        ok = ok && errno == EBADF;
    }
    return ok;
}

int main(void)
{
    result("initial values", counter == 1234 && scratch[0] == 0 && scratch[99] == 0 && wide == 0);
    counter = 1;                                 /* the main thread's own change */
    scratch[5] = 'm';
    int t[4], all = 1;
    for (int i = 0; i < 4; i++)
        t[i] = thread_create(own, (void *)(intptr_t)(i + 1));
    for (int i = 0; i < 4; i++) {
        int r = 0;
        all = all && t[i] > 0 && thread_join(t[i], &r) == 0 && r == 1;
    }
    result("each thread its own", all);
    result("the main thread's kept", counter == 1 && scratch[5] == 'm');
    int a = thread_create(fails_enoent, 0), b = thread_create(fails_ebadf, 0), ra = 0, rb = 0;
    result("errno per thread", a > 0 && b > 0 && thread_join(a, &ra) == 0 &&
                               thread_join(b, &rb) == 0 && ra == 1 && rb == 1);
    printf("tlstest: %s\n", failures ? "FAILED" : "all ok");
    return failures != 0;
}

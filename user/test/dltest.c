/*
 * Test: dlopen, dlsym, dlclose, dlerror (step 22k). `dltest LIBRARY` loads
 * the test library (a path) while it runs: its constructor has run, a
 * function and a call into libritteros work, its per-thread data is each
 * thread's own (two threads counting from the initial value), opening it
 * again gives the same handle, dlclose works, a missing symbol and a missing
 * file fail with dlerror's message (and dlerror once only), and dlsym on
 * RTLD_DEFAULT finds a libritteros function. Built statically too
 * (dltest-static): there dlopen fails, "no dynamic loader".
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <thread.h>

static int failures;
static int (*count)(void);

static void result(const char *name, int ok, const char *detail)
{
    printf("dltest: %s: %s%s%s\n", name, ok ? "ok" : "FAILED", detail[0] ? " " : "", detail);
    failures += !ok;
}

static int counting(void *arg)
{
    (void)arg;
    int a = count(), b = count(), c = count();
    return a == 6 && b == 7 && c == 8;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: dltest LIBRARY\n");
        return 2;
    }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) {
        char *e = dlerror();
        printf("dltest: dlopen failed: %s\n", e ? e : "(no message)");
        return 1;
    }
    int (*ready)(void) = (int (*)(void))dlsym(h, "plug_ready");
    int (*add)(int, int) = (int (*)(int, int))dlsym(h, "plug_add");
    const char *(*name)(char *, int) = (const char *(*)(char *, int))dlsym(h, "plug_name");
    count = (int (*)(void))dlsym(h, "plug_count");
    result("loaded, its constructor ran", ready && ready() == 1, "");
    char buf[32];
    result("a function, and libritteros through it", add && add(20, 22) == 42 && name &&
           strcmp(name(buf, sizeof(buf)), "plug 22") == 0, "");
    int t1 = thread_create(counting, 0), t2 = thread_create(counting, 0), r1 = 0, r2 = 0;
    thread_join(t1, &r1);
    thread_join(t2, &r2);
    result("its per-thread data", count && r1 == 1 && r2 == 1 && count() == 6, "(each thread from 5)");
    result("opened again: the same handle", dlopen(argv[1], RTLD_NOW) == h, "");
    void *nothing = dlsym(h, "plug_nothing");
    char *e = dlerror();
    char detail[160];
    snprintf(detail, sizeof(detail), "(%s)", e ? e : "no message");
    result("a missing symbol", !nothing && e && strstr(e, "plug_nothing") && !dlerror(), detail);
    void *gone = dlopen("/no/such/libnone.so.1", RTLD_NOW);
    e = dlerror();
    snprintf(detail, sizeof(detail), "(%s)", e ? e : "no message");
    result("a missing file", !gone && e != 0, detail);
    result("RTLD_DEFAULT finds snprintf", dlsym(RTLD_DEFAULT, "snprintf") != 0, "");
    result("dlclose", dlclose(h) == 0 && dlclose(h) == 0, "");
    printf("dltest: %s\n", failures ? "FAILED" : "all ok");
    return failures != 0;
}

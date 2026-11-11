/*
 * Test: pthread_cancel (54c). Plain POSIX: the test builds it on the Mac
 * too and compares the "canceltest:" lines; "canceltest-own:" lines are
 * RitterOS's alone (asynchronous cancellation of a busy loop, ESRCH for a
 * thread that has ended). A thread waiting in read, one in
 * pthread_cond_wait (its cleanup handler unlocks the mutex), one in
 * sleep, one that disables cancellation for a while, cleanup handlers
 * popped with and without running, pthread_exit running them, a key's
 * destructor after a cancel.
 * Usage: canceltest [own]
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int cleaned;

static void clean(void *arg)
{
    cleaned += (int)(long)arg;
}

static int pipe_fds[2];

static void *in_read(void *arg)
{
    (void)arg;
    char c;
    pthread_cleanup_push(clean, (void *)1);
    read(pipe_fds[0], &c, 1);                   /* (nothing ever comes) */
    pthread_cleanup_pop(0);
    return 0;
}

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c = PTHREAD_COND_INITIALIZER;
static int waiting;

static void unlock(void *arg)
{
    pthread_mutex_unlock(arg);
    cleaned += 10;
}

static void *in_wait(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&m);
    pthread_cleanup_push(unlock, &m);
    waiting = 1;
    pthread_cond_signal(&c);
    for (;;)
        pthread_cond_wait(&c, &m);              /* (nobody signals it again) */
    pthread_cleanup_pop(1);
    return 0;
}

static void *in_sleep(void *arg)
{
    (void)arg;
    sleep(10);
    return (void *)7;
}

static volatile int ready, slept_through;

static void *disabled(void *arg)
{
    (void)arg;
    int old = -1;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old);
    ready = 1;
    usleep(200000);                             /* (a cancel comes meanwhile: kept) */
    slept_through = old == PTHREAD_CANCEL_ENABLE;
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, 0);
    pthread_testcancel();
    return (void *)5;
}

static void *pops(void *arg)
{
    (void)arg;
    pthread_cleanup_push(clean, (void *)100);
    pthread_cleanup_push(clean, (void *)1000);
    pthread_cleanup_pop(0);
    pthread_cleanup_pop(1);
    pthread_cleanup_push(clean, (void *)10000);
    pthread_exit((void *)3);
    pthread_cleanup_pop(0);
    return 0;
}

static pthread_key_t key;
static int destroyed;

static void destroy(void *v)
{
    destroyed += (int)(long)v;
}

static void *with_key(void *arg)
{
    (void)arg;
    pthread_setspecific(key, (void *)9);
    for (;;)
        usleep(10000);
    return 0;
}

static long ms_since(const struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (long)((t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000);
}

static const char *what(void *v)
{
    return v == PTHREAD_CANCELED ? "PTHREAD_CANCELED" : v == 0 ? "0" : "a value";
}

#ifndef __APPLE__
static volatile long spins;

static void *busy(void *arg)
{
    (void)arg;
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, 0);
    pthread_cleanup_push(clean, (void *)100000);
    ready = 1;
    for (;;)
        spins++;                                /* (no cancellation point anywhere) */
    pthread_cleanup_pop(0);
    return 0;
}

static void own(void)
{
    pthread_t t;
    void *v = 0;
    cleaned = 0;
    ready = 0;
    pthread_create(&t, 0, busy, 0);
    while (!ready)
        usleep(1000);
    usleep(50000);
    int r = pthread_cancel(t);
    pthread_join(t, &v);
    printf("canceltest-own: an asynchronous cancel of a busy loop: %d, %s, cleanup %d, it ran: %s\n",
           r, what(v), cleaned, spins > 0 ? "yes" : "no");
    printf("canceltest-own: a thread that has ended: %s\n",
           pthread_cancel(t) == ESRCH ? "ESRCH" : "another");
}
#endif

int main(int argc, char **argv)
{
    pthread_t t;
    void *v = 0;
    struct timespec t0;

    pipe(pipe_fds);
    pthread_create(&t, 0, in_read, 0);
    usleep(50000);
    int r = pthread_cancel(t);
    pthread_join(t, &v);
    printf("canceltest: a thread in read: cancel %d, joined %s, cleanup %d\n", r, what(v), cleaned);

    cleaned = 0;
    pthread_mutex_lock(&m);
    pthread_create(&t, 0, in_wait, 0);
    while (!waiting)
        pthread_cond_wait(&c, &m);
    pthread_mutex_unlock(&m);
    pthread_cancel(t);
    pthread_join(t, &v);
    int free_ = pthread_mutex_trylock(&m);
    if (!free_)
        pthread_mutex_unlock(&m);
    printf("canceltest: a thread in pthread_cond_wait: joined %s, cleanup %d, the mutex free: %s\n",
           what(v), cleaned, free_ == 0 ? "yes" : "no");

    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_create(&t, 0, in_sleep, 0);
    usleep(50000);
    pthread_cancel(t);
    pthread_join(t, &v);
    printf("canceltest: a thread in sleep(10): joined %s %s\n", what(v),
           ms_since(&t0) < 3000 ? "soon" : "late");

    pthread_create(&t, 0, disabled, 0);
    while (!ready)
        usleep(1000);
    pthread_cancel(t);
    pthread_join(t, &v);
    printf("canceltest: cancellation disabled for a while: slept through %s, then joined %s\n",
           slept_through ? "yes" : "no", what(v));

    cleaned = 0;
    pthread_create(&t, 0, pops, 0);
    pthread_join(t, &v);
    printf("canceltest: cleanup pop(0), pop(1), pthread_exit: cleanup %d, value %ld\n", cleaned,
           (long)v);

    pthread_key_create(&key, destroy);
    pthread_create(&t, 0, with_key, 0);
    usleep(50000);
    pthread_cancel(t);
    pthread_join(t, &v);
    printf("canceltest: a key's destructor after a cancel: %d, joined %s\n", destroyed, what(v));

    int old_state = -1, old_type = -1;
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &old_state);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &old_type);
    printf("canceltest: at first enabled %s, deferred %s; a wrong state %s\n",
           old_state == PTHREAD_CANCEL_ENABLE ? "yes" : "no",
           old_type == PTHREAD_CANCEL_DEFERRED ? "yes" : "no",
           pthread_setcancelstate(7, 0) == EINVAL ? "EINVAL" : "another");
#ifndef __APPLE__
    if (argc == 2 && !strcmp(argv[1], "own"))
        own();
#else
    (void)argc, (void)argv;
#endif
    printf("canceltest: done\n");
    return 0;
}

/*
 * Test: (45a) how precisely timed waits end, the ones a browser's timers
 * and frames use: nanosleep, pthread_cond_timedwait (the futex), poll's
 * timeout (select's too) and a periodic timerfd, each asked for 1 ms, 50
 * times. Each line gives the mean and the longest wait in ms and whether
 * the mean stayed under 3 ms (until 45a they waited for the next 10 ms
 * tick: 10 to 20 ms).
 *   timertest      the four lines
 *   timertest clock    (68p) the clock read from the kernel's clock page
 *                  (no system call): the page is there; on two threads at
 *                  once, 200,000 readings each never go back and stay
 *                  between SYS_TIME's around them; a reading costs less
 *                  than half a system call; the clock set an hour back
 *                  (clock_settime, root) moves CLOCK_REALTIME and not
 *                  CLOCK_MONOTONIC; a CLOCK_MONOTONIC timerfd at an
 *                  absolute time 30 ms on waits 30 ms
 */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include "ritteros.h"

#define T "timertest: "
#define ROUNDS 50

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void report(const char *what, const double *took, int n)
{
    double sum = 0, longest = 0;
    for (int i = 0; i < n; i++) {
        sum += took[i];
        if (took[i] > longest)
            longest = took[i];
    }
    double mean = n ? sum / n : 0;
    printf(T "%s: mean %.2f ms, longest %.2f ms; mean under 3 ms %d\n", what, mean, longest,
           n == ROUNDS && mean >= 1.0 && mean < 3.0);
}

static uint64_t ns_of(clockid_t clock)
{
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t kernel_time(void)
{
    return (uint64_t)syscall6(SYS_TIME, 0, 0, 0, 0, 0, 0);
}

#define READINGS 200000

/* A thread's share: the library's readings never go back and lie between
 * the kernel's (SYS_TIME) around them; the result is the number of faults. */
static void *readings(void *arg)
{
    (void)arg;
    uintptr_t bad = 0;
    uint64_t last_real = 0, last_mono = 0;
    for (int i = 0; i < READINGS; i++) {
        uint64_t mono = ns_of(CLOCK_MONOTONIC);
        uint64_t real = ns_of(CLOCK_REALTIME);
        if (mono < last_mono || real < last_real)
            bad++;
        last_mono = mono, last_real = real;
        if (i % 100 == 0) {
            uint64_t before = kernel_time();
            uint64_t ours = ns_of(CLOCK_REALTIME);
            uint64_t after = kernel_time();
            if (ours < before || ours > after)
                bad++;
        }
    }
    return (void *)bad;
}

static int clock_test(void)
{
    printf(T "clock page %s\n", getauxval(AT_RITTEROS_CLOCK) ? "there" : "missing");

    pthread_t other;
    void *bad_other = (void *)1;
    int made = pthread_create(&other, 0, readings, 0) == 0;
    uintptr_t bad = (uintptr_t)readings(0);
    if (made)
        pthread_join(other, &bad_other);
    printf(T "clock readings: %d threads, %lu out of order or outside the kernel's\n",
           made ? 2 : 1, (unsigned long)(bad + (uintptr_t)bad_other));

    enum { COST = 20000 };
    uint64_t t0 = ns_of(CLOCK_MONOTONIC);
    for (int i = 0; i < COST; i++)
        ns_of(CLOCK_REALTIME);
    uint64_t t1 = ns_of(CLOCK_MONOTONIC);
    for (int i = 0; i < COST; i++)
        kernel_time();
    uint64_t t2 = ns_of(CLOCK_MONOTONIC);
    uint64_t ours = (t1 - t0) / COST, call = (t2 - t1) / COST;
    printf(T "clock reading %lu ns, a system call %lu ns; under half %d\n", (unsigned long)ours,
           (unsigned long)call, ours * 2 < call);

    struct timespec real;
    clock_gettime(CLOCK_REALTIME, &real);
    uint64_t mono_before = ns_of(CLOCK_MONOTONIC), real_before = ns_of(CLOCK_REALTIME);
    struct timespec back = real;
    back.tv_sec -= 3600;
    int set = clock_settime(CLOCK_REALTIME, &back) == 0;
    uint64_t mono_after = ns_of(CLOCK_MONOTONIC), real_after = ns_of(CLOCK_REALTIME);
    if (set) {
        struct timespec again;
        clock_gettime(CLOCK_REALTIME, &again);
        again.tv_sec += 3600;
        clock_settime(CLOCK_REALTIME, &again);
    }
    printf(T "clock set an hour back %d: realtime back %d, monotonic on %d\n", set,
           real_after + 3500ULL * 1000000000ULL < real_before && real_after + 3700ULL * 1000000000ULL > real_before,
           mono_after >= mono_before && mono_after - mono_before < 1000000000ULL);

    int fd = timerfd_create(CLOCK_MONOTONIC, 0);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct itimerspec at = { { 0, 0 }, now };
    at.it_value.tv_nsec += 30000000;
    if (at.it_value.tv_nsec >= 1000000000) {
        at.it_value.tv_sec++;
        at.it_value.tv_nsec -= 1000000000;
    }
    double waited = -1;
    uint64_t expired;
    double w0 = now_ms();
    if (fd >= 0 && timerfd_settime(fd, TFD_TIMER_ABSTIME, &at, 0) == 0 &&
        read(fd, &expired, sizeof(expired)) == (ssize_t)sizeof(expired))
        waited = now_ms() - w0;
    if (fd >= 0)
        close(fd);
    printf(T "monotonic timerfd at an absolute time 30 ms on: %.1f ms; 25-60 ms %d\n", waited,
           waited >= 25.0 && waited < 60.0);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "clock"))
        return clock_test();
    double took[ROUNDS];
    int n = 0;
    for (int i = 0; i < ROUNDS; i++) {
        struct timespec t = { 0, 1000000 };
        double t0 = now_ms();
        if (nanosleep(&t, 0) == 0)
            took[n++] = now_ms() - t0;
    }
    report("nanosleep 1 ms", took, n);

    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t c;
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&c, &ca);
    n = 0;
    pthread_mutex_lock(&m);
    for (int i = 0; i < ROUNDS; i++) {
        struct timespec until;
        clock_gettime(CLOCK_MONOTONIC, &until);
        until.tv_nsec += 1000000;
        if (until.tv_nsec >= 1000000000) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000;
        }
        double t0 = now_ms();
        if (pthread_cond_timedwait(&c, &m, &until) == ETIMEDOUT)
            took[n++] = now_ms() - t0;
    }
    pthread_mutex_unlock(&m);
    report("pthread_cond_timedwait 1 ms", took, n);

    n = 0;
    for (int i = 0; i < ROUNDS; i++) {
        double t0 = now_ms();
        if (poll(0, 0, 1) == 0)
            took[n++] = now_ms() - t0;
    }
    report("poll timeout 1 ms", took, n);

    int fd = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec every = { { 0, 1000000 }, { 0, 1000000 } };
    n = 0;
    if (fd >= 0 && timerfd_settime(fd, 0, &every, 0) == 0) {
        double t0 = now_ms();
        for (int i = 0; i < ROUNDS; i++) {
            uint64_t expired;
            if (read(fd, &expired, sizeof(expired)) != (ssize_t)sizeof(expired))
                break;
            double t1 = now_ms();
            took[n++] = t1 - t0;
            t0 = t1;
        }
        close(fd);
    }
    report("timerfd every 1 ms", took, n);
    return 0;
}

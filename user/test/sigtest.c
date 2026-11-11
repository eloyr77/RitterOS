/*
 * Test: signal handlers that come at any moment (22r), which the libc test
 * cannot show (there every signal comes from raise, a call).
 *   sigtest fp      floating-point work runs while a second process sends
 *                   SIGUSR1 every 10 ms; the handler does floating-point
 *                   work of its own; each run must give exactly the result
 *                   of a run without signals (the FP/SIMD registers kept)
 *   sigtest sleep   nanosleep of 3 s, a SIGUSR1 after 0.3 s: -1, EINTR,
 *                   the time left, and the handler ran
 *   sigtest int     waits for Ctrl-C with a SIGINT handler (the test types it)
 *   sigtest send PID COUNT MS   (the second process) COUNT SIGUSR1s, MS apart
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "ritteros.h"

#define T "sigtest: "

static volatile sig_atomic_t handled;
static volatile double sink;

/* Floating-point work of its own, so that the registers do change. */
static void on_usr1(int sig)
{
    (void)sig;
    handled++;
    double v[12];
    for (int i = 0; i < 12; i++)
        v[i] = i * 1.25 + handled;
    double t = 0;
    for (int i = 0; i < 12; i++)
        t += v[i] * v[(i + 5) % 12] / (v[i] + 1);
    sink = t;
}

static volatile sig_atomic_t interrupted;

static void on_int(int sig)
{
    interrupted = sig == SIGINT;
}

/* Work kept in registers for its whole loop (no calls inside). */
static double work(long n)
{
    double a = 1.0, b = 0.5, c = 0.25, d = 2.0, e = 3.0, f = 0.125;
    for (long i = 0; i < n; i++) {
        a = a * 1.0000001 + b * 1e-9;
        b = b * 0.9999999 + c * 1e-9;
        c = c + d * 1e-12 - e * 1e-13;
        d = d * 1.00000001 - a * 1e-12;
        e = e * 0.99999999 + f * 1e-10;
        f = f + a * 1e-14;
    }
    return a + b + c + d + e + f;
}

static double seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* The second process: sigtest send PID COUNT MS. */
static int sender(char *self, int count, int ms)
{
    char pid[16], n[16], gap[16];
    snprintf(pid, sizeof(pid), "%d", (int)getpid());
    snprintf(n, sizeof(n), "%d", count);
    snprintf(gap, sizeof(gap), "%d", ms);
    char *argv[] = { self, "send", pid, n, gap, 0 };
    int fds[3] = { -1, -1, -1 };                /* (the caller's own 0, 1 and 2) */
    return ritteros_spawn(self, argv, 0, fds);
}

int main(int argc, char **argv)
{
    if (argc >= 5 && !strcmp(argv[1], "send")) {
        int target = atoi(argv[2]), count = atoi(argv[3]), ms = atoi(argv[4]);
        for (int i = 0; i < count; i++) {
            usleep((useconds_t)ms * 1000);
            if (kill(target, SIGUSR1) < 0)
                return 1;
        }
        return 0;
    }
    if (argc < 2) {
        printf(T "usage: sigtest fp|sleep|int\n");
        return 2;
    }
    signal(SIGUSR1, on_usr1);
    if (!strcmp(argv[1], "fp")) {
        /* A loop of about 0.3 s, measured. */
        long n = 1000000;
        double t0 = seconds();
        sink = work(n);                         /* (used, or the call goes away) */
        double took = seconds() - t0;
        if (took > 0.001)
            n = (long)(n * (0.3 / took));
        if (n > 200000000)
            n = 200000000;
        double want = work(n);
        int child = sender(argv[0], 80, 10);
        int runs = 0, same = 0;
        double end = seconds() + 1.2;
        while (seconds() < end && runs < 50) {
            double got = work(n);
            runs++;
            same += got == want;
        }
        int status;
        waitpid(child, &status, 0);
        printf(T "fp: %d runs, %d exactly as without signals, %d signals handled (some %d)\n",
               runs, same, (int)handled, handled > 0);
        printf(T "fp: intact %d\n", same == runs && runs > 1);
        return 0;
    }
    if (!strcmp(argv[1], "sleep")) {
        int child = sender(argv[0], 1, 300);
        struct timespec nap = { 3, 0 }, left = { 0, 0 };
        errno = 0;
        int r = nanosleep(&nap, &left);
        int eintr = errno == EINTR;
        int status;
        waitpid(child, &status, 0);
        printf(T "sleep: %d EINTR %d, left over 2 s %d (under 3 %d), handler %d\n", r, eintr,
               left.tv_sec >= 2, left.tv_sec < 3, (int)handled);
        return 0;
    }
    if (!strcmp(argv[1], "int")) {
        signal(SIGINT, on_int);
        printf(T "waiting for Ctrl-C\n");
        for (int i = 0; i < 100 && !interrupted; i++)
            usleep(100000);
        printf(T "int: %s\n", interrupted ? "SIGINT handled" : "no Ctrl-C (timed out)");
        return 0;
    }
    printf(T "unknown test %s\n", argv[1]);
    return 2;
}

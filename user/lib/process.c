/*
 * (43a-3) Process calls ported programs expect: alarm and ualarm (a
 * thread of the library sleeps until the time and sends SIGALRM to the
 * program; it blocks every signal itself, so the signal goes to the
 * program's own threads), pause, daemon, vfork, setpgrp, killpg,
 * psignal, GNU's sigset operations, wait3 and wait4.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread.h>
#include <time.h>
#include <unistd.h>
#include "ritteros.h"

/* The alarm: its deadline (monotonic nanoseconds, 0 none) and interval,
 * under a futex word the thread waits on. */
static int alarm_word;                  /* changes at every new alarm: the thread looks again */
static uint64_t alarm_at, alarm_every;
static int alarm_lock;
static bool alarm_thread_made;

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}

static void lock(void)
{
    while (__atomic_exchange_n(&alarm_lock, 1, __ATOMIC_ACQUIRE))
        syscall6(SYS_FUTEX, (int64_t)&alarm_lock, FUTEX_WAIT, 1, 0, 0, 0);
}

static void unlock(void)
{
    __atomic_store_n(&alarm_lock, 0, __ATOMIC_RELEASE);
    syscall6(SYS_FUTEX, (int64_t)&alarm_lock, FUTEX_WAKE, 1, 0, 0, 0);
}

static int alarm_thread(void *unused)
{
    (void)unused;
    sigset_t all;
    sigfillset(&all);
    sigprocmask(SIG_BLOCK, &all, 0);
    for (;;) {
        lock();
        int word = alarm_word;
        uint64_t at = alarm_at, now = now_ns();
        bool due = at && now >= at;
        if (due)
            alarm_at = alarm_every ? now + alarm_every : 0;
        unlock();
        if (due) {
            kill(getpid(), SIGALRM);
            continue;
        }
        /* until the deadline, or a change of the alarm (the word moves) */
        uint64_t ns = at ? at - now : 0;
        syscall6(SYS_FUTEX, (int64_t)&alarm_word, FUTEX_WAIT, word, at ? (int64_t)(ns ? ns : 1) : 0, 0, 0);
    }
    return 0;
}

/* Set the alarm (0: none); the time left of the one before, in ns. */
static uint64_t set_alarm(uint64_t after, uint64_t every)
{
    lock();
    uint64_t now = now_ns(), left = alarm_at > now ? alarm_at - now : alarm_at ? 1 : 0;
    alarm_at = after ? now + after : 0;
    alarm_every = every;
    alarm_word++;
    bool make = after && !alarm_thread_made;
    if (make)
        alarm_thread_made = true;
    unlock();
    if (make)
        thread_create(alarm_thread, 0);
    else
        syscall6(SYS_FUTEX, (int64_t)&alarm_word, FUTEX_WAKE, 1, 0, 0, 0);
    return left;
}

unsigned int alarm(unsigned int seconds)
{
    uint64_t left = set_alarm((uint64_t)seconds * 1000000000, 0);
    return (unsigned int)((left + 999999999) / 1000000000);
}

useconds_t ualarm(useconds_t microseconds, useconds_t interval)
{
    uint64_t left = set_alarm((uint64_t)microseconds * 1000, (uint64_t)interval * 1000);
    return (useconds_t)(left / 1000);
}

int pause(void)
{
    sigset_t now;
    sigprocmask(SIG_BLOCK, 0, &now);
    return sigsuspend(&now);
}

pid_t vfork(void)
{
    return fork();
}

int setpgrp(void)
{
    return setpgid(0, 0);
}

int daemon(int nochdir, int noclose)
{
    pid_t child = fork();
    if (child < 0)
        return -1;
    if (child > 0)
        _exit(0);
    if (setsid() < 0)
        return -1;
    if (!nochdir)
        chdir("/");
    if (!noclose) {
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2)
                close(fd);
        }
    }
    return 0;
}

int killpg(pid_t group, int signal)
{
    if (group < 0) {
        errno = EINVAL;
        return -1;
    }
    return kill(-group, signal);
}

void psignal(int signal, const char *s)
{
    if (s && *s)
        fprintf(stderr, "%s: %s\n", s, strsignal(signal));
    else
        fprintf(stderr, "%s\n", strsignal(signal));
}

int sigisemptyset(const sigset_t *set)
{
    return *set == 0;
}

int sigandset(sigset_t *dst, const sigset_t *a, const sigset_t *b)
{
    *dst = *a & *b;
    return 0;
}

int sigorset(sigset_t *dst, const sigset_t *a, const sigset_t *b)
{
    *dst = *a | *b;
    return 0;
}

pid_t wait4(pid_t pid, int *status, int options, struct rusage *usage)
{
    pid_t r = waitpid(pid, status, options);
    if (r > 0 && usage)
        memset(usage, 0, sizeof(*usage));       /* (a child's use is not kept) */
    return r;
}

pid_t wait3(int *status, int options, struct rusage *usage)
{
    return wait4(-1, status, options, usage);
}

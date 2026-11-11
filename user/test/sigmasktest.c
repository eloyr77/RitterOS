/*
 * Test: sigprocmask (52f, SYS_SIGPROCMASK). Each check prints
 * "sigmasktest: NAME ok" or "sigmasktest: FAIL ...":
 *  - held: a handled signal sent while blocked waits; its handler runs as
 *    the unblocking call returns.
 *  - not interrupted: a blocked signal sent by another thread during a
 *    sleep does not cut the sleep short (both threads block it: the new
 *    one took its creator's set).
 *  - another thread: a signal the main thread blocks is handled by a
 *    thread that does not block it.
 *  - sigkill and how: SIGKILL cannot be blocked, a wrong how is EINVAL.
 *  - default action: in a child, SIGTERM (no handler) waits while blocked
 *    and ends the child once let through.
 *  - pending (52i): sigpending shows a blocked signal sent, and not after
 *    it was let through.
 *  - suspend (52i): sigsuspend with nothing blocked waits for a signal
 *    another thread sends, past an ignored one; the handler has run when
 *    it returns EINTR, and the set from before is back.
 *  - spawn (52i): a program started while SIGTERM is blocked blocks it
 *    too.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <thread.h>
#include <unistd.h>
#include "ritteros.h"

#if defined(__x86_64__)
#define ARCH "x86_64"
#else
#define ARCH "aarch64"
#endif

static int failed;
static volatile sig_atomic_t usr1, usr2_thread = -1;

static void result(const char *name, int ok)
{
    if (ok)
        printf("sigmasktest: %s ok\n", name);
    else {
        printf("sigmasktest: FAIL %s (errno %d)\n", name, errno);
        failed = 1;
    }
}

static long ms_since(uint64_t start)
{
    return (long)((ritteros_time() - start) / 1000000);
}

static void on_usr1(int s)
{
    (void)s;
    usr1++;
}

static void on_usr2(int s)
{
    (void)s;
    usr2_thread = thread_self();
}

static int sender(void *arg)
{
    (void)arg;
    sigset_t now;
    sigprocmask(SIG_BLOCK, 0, &now);
    usleep(100000);
    kill(getpid(), SIGUSR1);
    return sigismember(&now, SIGUSR1) == 1;     /* (it took its creator's set) */
}

static volatile int sleeper_go = 1;

static int sleeper(void *arg)
{
    (void)arg;
    while (sleeper_go)
        usleep(20000);
    return 0;
}

static int late_signals(void *arg)
{
    (void)arg;
    usleep(100000);
    kill(getpid(), SIGUSR2);                    /* (ignored: the wait goes on) */
    usleep(200000);
    kill(getpid(), SIGUSR1);
    return 0;
}

static int child(void)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigprocmask(SIG_BLOCK, &set, 0);
    kill(getpid(), SIGTERM);
    usleep(100000);
    printf("sigmasktest: SIGTERM held while blocked\n");
    sigprocmask(SIG_UNBLOCK, &set, 0);
    printf("sigmasktest: FAIL still here after unblocking\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "child"))
        return child();
    if (argc == 2 && !strcmp(argv[1], "showmask")) {
        sigset_t now;
        sigprocmask(SIG_BLOCK, 0, &now);
        printf("sigmasktest: the child %s SIGTERM\n", sigismember(&now, SIGTERM) ? "blocks" : "lets through");
        return 0;
    }
    signal(SIGUSR1, on_usr1);
    signal(SIGUSR2, on_usr2);
    sigset_t one, old;
    sigemptyset(&one);
    sigaddset(&one, SIGUSR1);

    int ok = sigprocmask(SIG_BLOCK, &one, &old) == 0 && old == 0;
    kill(getpid(), SIGUSR1);
    ok &= usr1 == 0;
    sigprocmask(SIG_UNBLOCK, &one, 0);
    ok &= usr1 == 1;
    result("held", ok);

    usr1 = 0;
    sigprocmask(SIG_BLOCK, &one, 0);
    int t = thread_create(sender, 0);
    uint64_t start = ritteros_time();
    usleep(400000);
    long slept = ms_since(start);
    int inherited = 0;
    thread_join(t, &inherited);
    ok = slept >= 380 && usr1 == 0 && inherited;
    sigprocmask(SIG_UNBLOCK, &one, 0);
    ok &= usr1 == 1;
    printf("sigmasktest: a blocked signal during a 400 ms sleep: %s\n",
           slept >= 380 ? "slept on" : "cut short");
    result("not interrupted", ok);

    t = thread_create(sleeper, 0);
    usleep(50000);
    sigset_t two;
    sigemptyset(&two);
    sigaddset(&two, SIGUSR2);
    sigprocmask(SIG_BLOCK, &two, 0);
    kill(getpid(), SIGUSR2);
    for (int i = 0; i < 100 && usr2_thread < 0; i++)
        usleep(10000);
    sleeper_go = 0;
    thread_join(t, 0);
    result("another thread", usr2_thread == t);
    sigprocmask(SIG_UNBLOCK, &two, 0);

    sigset_t k, now;
    sigemptyset(&k);
    sigaddset(&k, SIGKILL);
    sigprocmask(SIG_BLOCK, &k, 0);
    sigprocmask(SIG_BLOCK, 0, &now);
    errno = 0;
    ok = !sigismember(&now, SIGKILL) && sigprocmask(7, &k, 0) == -1 && errno == EINVAL;
    result("sigkill and how", ok);

    char *cargv[] = { "sigmasktest", "child", 0 };
    int none[3] = { -1, -1, -1 };
    pid_t pid = ritteros_spawn("/system/" ARCH "/test/sigmasktest", cargv, 0, none);
    int status = 0;
    waitpid(pid, &status, 0);
    result("default action", pid > 0 && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);

    sigset_t waiting;
    usr1 = 0;
    sigprocmask(SIG_BLOCK, &one, 0);
    kill(getpid(), SIGUSR1);
    ok = sigpending(&waiting) == 0 && sigismember(&waiting, SIGUSR1) == 1 && usr1 == 0;
    sigprocmask(SIG_UNBLOCK, &one, 0);
    ok &= usr1 == 1 && sigpending(&waiting) == 0 && sigismember(&waiting, SIGUSR1) == 0;
    result("pending", ok);

    usr1 = 0;
    signal(SIGUSR2, SIG_IGN);
    sigprocmask(SIG_BLOCK, &one, 0);
    t = thread_create(late_signals, 0);
    sigset_t nothing;
    sigemptyset(&nothing);
    start = ritteros_time();
    errno = 0;
    int r = sigsuspend(&nothing);
    long waited = ms_since(start);
    ok = r == -1 && errno == EINTR && usr1 == 1 && waited >= 250 && waited < 3000;
    thread_join(t, 0);
    sigprocmask(SIG_BLOCK, 0, &now);
    ok &= sigismember(&now, SIGUSR1) == 1;
    sigprocmask(SIG_UNBLOCK, &one, 0);
    printf("sigmasktest: sigsuspend waited %s\n", waited >= 250 ? "past the ignored signal" : "too little");
    result("suspend", ok);

    sigset_t term;
    sigemptyset(&term);
    sigaddset(&term, SIGTERM);
    sigprocmask(SIG_BLOCK, &term, 0);
    char *sargv[] = { "sigmasktest", "showmask", 0 };
    pid = ritteros_spawn("/system/" ARCH "/test/sigmasktest", sargv, 0, none);
    waitpid(pid, &status, 0);
    sigprocmask(SIG_UNBLOCK, &term, 0);
    result("spawn", pid > 0 && WIFEXITED(status));

    printf(failed ? "sigmasktest: FAIL (see above)\n" : "sigmasktest: all ok\n");
    return failed;
}

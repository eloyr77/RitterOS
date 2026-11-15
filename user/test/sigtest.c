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
 *   sigtest context (43b-5) a handler's ucontext_t: a fault's PC (inside the
 *                   function that faulted) and address, the blocked set, no
 *                   alternate stack, the FP state's record; the PC changed
 *                   to a function that takes the program on (as JSC does);
 *                   the register with raise's result changed: raise gives
 *                   that value
 *   sigtest stop    (44a) stopping and continuing: children (one of two
 *                   threads counting) stopped by SIGSTOP and SIGTSTP stay
 *                   still, waitpid's WUNTRACED and WCONTINUED and SIGCHLD's
 *                   CLD_STOPPED/CLD_CONTINUED report each once, SIGKILL ends
 *                   a stopped one; a handled or ignored SIGTSTP stops
 *                   nothing, a blocked one stops at its unblocking; a read
 *                   and a nanosleep the stop interrupted go on after
 *                   SIGCONT (no EINTR; the sleep keeps its end, the time
 *                   stopped counting); SA_NOCLDSTOP
 *   sigtest job     (44b) job control at the terminal it runs on, as a shell
 *                   would do it: Ctrl-Z (typed when it says so) stops the
 *                   foreground job by SIGTSTP; continued in the background
 *                   its read stops it by SIGTTIN, and in the foreground again
 *                   the read gets the line typed; with TOSTOP a background
 *                   write stops it by SIGTTOU (without, it goes out); a
 *                   background tcsetattr stops it by SIGTTOU (goes on with
 *                   SIGTTOU ignored); a read with SIGTTIN ignored is EIO; an
 *                   orphaned group's stopped member gets SIGHUP and SIGCONT,
 *                   and its read is EIO
 *   sigtest jobpty  the same on a pseudo-terminal of its own: `sigtest job`
 *                   started there in a session of its own, what it prints
 *                   passed on, Ctrl-Z and the line typed by this side
 *   sigtest send PID COUNT MS   (the second process) COUNT SIGUSR1s, MS apart
 */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <termios.h>
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

/* --- (43b-5) the context --- */
static sigjmp_buf resumed;
static volatile uintptr_t seen_pc, seen_addr;
static volatile int seen_masked, seen_stack, seen_fp;

static void __attribute__((noinline)) resume_here(void)
{
    siglongjmp(resumed, 1);
}

static void __attribute__((noinline)) faulter(volatile int *p)
{
    *p = 1;
}

static void on_fault(int sig, siginfo_t *info, void *context)
{
    (void)sig;
    (void)info;
    ucontext_t *uc = context;
    seen_masked = (uc->uc_sigmask >> SIGUSR2) & 1;
    seen_stack = (uc->uc_stack.ss_flags & SS_DISABLE) != 0;
#if defined(__x86_64__)
    seen_pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    seen_addr = (uintptr_t)uc->uc_mcontext.gregs[REG_CR2];
    seen_fp = uc->uc_mcontext.fpregs && (uc->uc_mcontext.fpregs->mxcsr & 0x1F80) == 0x1F80;
    uc->uc_mcontext.gregs[REG_RSP] -= 8;            /* (as if called) */
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)resume_here;
#elif defined(__aarch64__)
    seen_pc = (uintptr_t)uc->uc_mcontext.pc;
    seen_addr = (uintptr_t)uc->uc_mcontext.fault_address;
    struct fpsimd_context *fp = (struct fpsimd_context *)uc->uc_mcontext.__reserved;
    struct _aarch64_ctx *end = (struct _aarch64_ctx *)((char *)fp + fp->head.size);
    seen_fp = fp->head.magic == FPSIMD_MAGIC && fp->head.size == 528 && end->magic == 0;
    uc->uc_mcontext.pc = (uintptr_t)resume_here;
#endif
}

static void on_usr2_result(int sig, siginfo_t *info, void *context)
{
    (void)sig;
    (void)info;
    ucontext_t *uc = context;
#if defined(__x86_64__)
    uc->uc_mcontext.gregs[REG_RAX] = 4242;          /* (the system call's result) */
#elif defined(__aarch64__)
    uc->uc_mcontext.regs[0] = 4242;
#endif
}

static int context_test(void)
{
    char *page = mmap(0, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, 0);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR2);
    sigprocmask(SIG_BLOCK, &mask, 0);
    int came_back = 0;
    if (!sigsetjmp(resumed, 1))
        faulter((volatile int *)(page + 100));
    else
        came_back = 1;
    sigprocmask(SIG_UNBLOCK, &mask, 0);
    uintptr_t f = (uintptr_t)faulter;
    int pc_inside = seen_pc >= f && seen_pc < f + 64;
    printf(T "context: pc in the faulting function %d, address %d, blocked set %d, no alternate stack %d,"
           " FP state %d, resumed where the handler said %d\n", pc_inside,
           seen_addr == (uintptr_t)(page + 100), seen_masked, seen_stack, seen_fp, came_back);
    signal(SIGSEGV, SIG_DFL);
    sa.sa_sigaction = on_usr2_result;
    sigaction(SIGUSR2, &sa, 0);
    int r = raise(SIGUSR2);
    printf(T "context: raise gave %d (the handler's register)\n", r);
    return 0;
}

/* --- (44a) stop ----------------------------------------------------------- */

static volatile long *counters;                 /* shared with the child: its two threads' */
static volatile sig_atomic_t chld_code, chld_status, chld_count;

static void on_chld(int sig, siginfo_t *info, void *context)
{
    (void)sig, (void)context;
    chld_code = info->si_code;
    chld_status = info->si_status;
    chld_count++;
}

static void *count_second(void *arg)
{
    (void)arg;
    for (;;)
        counters[1]++;
    return 0;
}

static void ms_sleep(int ms)
{
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000 };
    while (nanosleep(&t, &t) < 0 && errno == EINTR) {
    }
}

static volatile sig_atomic_t tstp_handled;
static void on_tstp(int sig)
{
    (void)sig;
    tstp_handled = 1;
}

/* waitpid and read again after EINTR (the SIGCHLD handler may run in them). */
static pid_t wait_child(pid_t pid, int *st, int flags)
{
    pid_t w;
    while ((w = waitpid(pid, st, flags)) < 0 && errno == EINTR) {
    }
    return w;
}

static ssize_t read_again(int fd, void *buf, size_t size)
{
    ssize_t n;
    while ((n = read(fd, buf, size)) < 0 && errno == EINTR) {
    }
    return n;
}

/* waitpid with WUNTRACED|WNOHANG: whether child shows as stopped now. */
static int shows_stopped(pid_t child)
{
    int st = 0;
    return waitpid(child, &st, WUNTRACED | WNOHANG) == child && WIFSTOPPED(st);
}

static int stop_test(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_chld;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGCHLD, &sa, 0);
    counters = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    /* 1. Two threads counting, stopped and continued. */
    pid_t child = fork();
    if (child == 0) {
        pthread_t th;
        pthread_create(&th, 0, count_second, 0);
        for (;;)
            counters[0]++;
    }
    ms_sleep(200);
    int st = 0;
    kill(child, SIGSTOP);
    pid_t w = wait_child(child, &st, WUNTRACED);
    int stopped = w == child && WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP && !WIFSIGNALED(st) && !WIFEXITED(st);
    ms_sleep(50);
    long c0 = counters[0], c1 = counters[1];
    ms_sleep(300);
    int still = counters[0] == c0 && counters[1] == c1 && c0 > 0 && c1 > 0;
    int told = chld_code == CLD_STOPPED && chld_status == SIGSTOP;
    int once = wait_child(child, &st, WUNTRACED | WNOHANG) == 0;
    printf(T "stop: SIGSTOP reported %d (WIFSTOPPED, WSTOPSIG), both threads still %d, "
           "SIGCHLD CLD_STOPPED %d, reported once %d\n", stopped, still, told, once);
    kill(child, SIGCONT);
    w = wait_child(child, &st, WCONTINUED);
    int cont = w == child && WIFCONTINUED(st) && !WIFSTOPPED(st) && !WIFSIGNALED(st);
    ms_sleep(100);
    int going = counters[0] > c0 && counters[1] > c1;
    told = chld_code == CLD_CONTINUED && chld_status == SIGCONT;
    printf(T "stop: SIGCONT reported %d (WIFCONTINUED), CLD_CONTINUED %d, both threads going %d\n",
           cont, told, going);
    kill(child, SIGTSTP);
    w = wait_child(child, &st, WUNTRACED);
    int tstp = w == child && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTSTP;
    kill(child, SIGKILL);
    w = wait_child(child, &st, 0);
    printf(T "stop: SIGTSTP stops by default %d, SIGKILL ends a stopped one %d\n", tstp,
           w == child && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
    /* 2. A handled SIGTSTP and an ignored SIGTTOU stop nothing. */
    int ready[2];
    pipe(ready);
    child = fork();
    if (child == 0) {
        signal(SIGTSTP, on_tstp);
        signal(SIGTTOU, SIG_IGN);
        write(ready[1], "r", 1);
        while (!tstp_handled)
            ms_sleep(10);
        write(ready[1], "h", 1);
        for (;;)
            ms_sleep(100);
    }
    char c;
    read_again(ready[0], &c, 1);
    kill(child, SIGTSTP);
    int handled_ran = read_again(ready[0], &c, 1) == 1 && c == 'h';
    kill(child, SIGTTOU);
    ms_sleep(200);
    int not_stopped = !shows_stopped(child);
    printf(T "stop: a handled SIGTSTP runs its handler %d, it and an ignored SIGTTOU stop nothing %d\n",
           handled_ran, not_stopped);
    kill(child, SIGKILL);
    wait_child(child, &st, 0);
    /* 3. A blocked SIGTSTP stops it when unblocked. */
    child = fork();
    if (child == 0) {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGTSTP);
        sigprocmask(SIG_BLOCK, &set, 0);
        write(ready[1], "r", 1);
        ms_sleep(600);
        sigprocmask(SIG_UNBLOCK, &set, 0);
        for (;;)
            ms_sleep(100);
    }
    read_again(ready[0], &c, 1);
    kill(child, SIGTSTP);
    ms_sleep(200);
    int waits = !shows_stopped(child);
    w = wait_child(child, &st, WUNTRACED);
    printf(T "stop: a blocked SIGTSTP waits %d, stops it once unblocked %d\n", waits,
           w == child && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTSTP);
    kill(child, SIGKILL);
    wait_child(child, &st, 0);
    /* 4. A read the stop interrupted goes on after SIGCONT, without EINTR. */
    int data[2], result[2];
    pipe(data);
    pipe(result);
    child = fork();
    if (child == 0) {
        char buf[16];
        write(ready[1], "r", 1);
        ssize_t n = read(data[0], buf, sizeof(buf));
        int e = n < 0 ? errno : 0;
        char line[64];
        int len = snprintf(line, sizeof(line), "%zd %d", n, e);
        write(result[1], line, (size_t)len);
        _exit(0);
    }
    read_again(ready[0], &c, 1);
    ms_sleep(100);                              /* (in its read) */
    kill(child, SIGSTOP);
    wait_child(child, &st, WUNTRACED);
    write(data[1], "data", 4);
    struct pollfd pf = { result[0], POLLIN, 0 };
    int quiet = poll(&pf, 1, 300) == 0;         /* stopped: it reads nothing */
    kill(child, SIGCONT);
    char got[64] = "";
    ssize_t n = read_again(result[0], got, sizeof(got) - 1);
    if (n > 0)
        got[n] = 0;
    wait_child(child, &st, 0);
    printf(T "stop: a stopped reader reads nothing %d, then its read gives \"%s\" (4 bytes, no error)\n",
           quiet, got);
    /* 5. A nanosleep the stop interrupted keeps its end (Linux's: the
     * time stopped counts): 2 s, stopped 1 s after 0.5 s, ends 2 s after
     * its start (a sleep made again whole: 3.5 s). */
    child = fork();
    if (child == 0) {
        write(ready[1], "r", 1);
        double t0 = seconds();
        struct timespec t = { 2, 0 };
        int r = nanosleep(&t, 0);
        int e = r < 0 ? errno : 0;
        double took = seconds() - t0;
        char line[64];
        int len = snprintf(line, sizeof(line), "%d %d %d", r, e, took >= 1.95 && took < 2.5);
        write(result[1], line, (size_t)len);
        _exit(0);
    }
    read_again(ready[0], &c, 1);
    ms_sleep(500);
    kill(child, SIGSTOP);
    wait_child(child, &st, WUNTRACED);
    ms_sleep(1000);
    kill(child, SIGCONT);
    n = read_again(result[0], got, sizeof(got) - 1);
    if (n > 0)
        got[n] = 0;
    wait_child(child, &st, 0);
    printf(T "stop: a stopped nanosleep gives \"%s\" (0, no error, ended at its end: 1.95-2.5 s)\n", got);
    /* 6. SA_NOCLDSTOP: no SIGCHLD for the stop, waitpid still reports it. */
    sa.sa_flags = SA_SIGINFO | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, 0);
    child = fork();
    if (child == 0)
        for (;;)
            ms_sleep(100);
    int before = chld_count;
    kill(child, SIGSTOP);
    w = wait_child(child, &st, WUNTRACED);
    ms_sleep(100);
    printf(T "stop: SA_NOCLDSTOP: no SIGCHLD %d, waitpid reports it %d\n", chld_count == before,
           w == child && WIFSTOPPED(st));
    kill(child, SIGKILL);
    wait_child(child, &st, 0);
    return 0;
}

/* --- (44b) job: job control at the terminal ------------------------------ */

/* A child in a group of its own (both sides set it, as a shell does),
 * SIGTTOU back to its default (the leader ignores it). */
static pid_t job_fork(void)
{
    pid_t c = fork();
    if (c == 0) {
        setpgid(0, 0);
        signal(SIGTTOU, SIG_DFL);
    } else if (c > 0) {
        setpgid(c, c);
    }
    return c;
}

/* Whether child stopped by signal (waits for its report). */
static int stopped_by(pid_t child, int signal)
{
    int st = 0;
    return wait_child(child, &st, WUNTRACED) == child && WIFSTOPPED(st) && WSTOPSIG(st) == signal;
}

static int ended_well(pid_t child)
{
    int st = 0;
    return wait_child(child, &st, 0) == child && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static void end_job(pid_t child)
{
    int st;
    kill(child, SIGKILL);
    wait_child(child, &st, 0);
}

static volatile sig_atomic_t hup_seen;
static void on_hup(int sig)
{
    (void)sig;
    hup_seen = 1;
}

static int job_test(void)
{
    signal(SIGTTOU, SIG_IGN);                   /* (as a shell: it takes the terminal back) */
    pid_t me = getpgrp();
    /* Ctrl-Z stops the foreground job; in the background its read stops it
     * again (SIGTTIN); in the foreground it reads the line. */
    pid_t c = job_fork();
    if (c == 0) {
        char line[64];
        ssize_t n = read(0, line, sizeof(line) - 1);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            n--;
        line[n > 0 ? n : 0] = 0;
        printf(T "job: the job read '%s' after SIGCONT\n", line);
        _exit(0);
    }
    tcsetpgrp(0, c);
    ms_sleep(300);                              /* (in its read) */
    printf(T "job: type Ctrl-Z now\n");
    int tstp = stopped_by(c, SIGTSTP);
    tcsetpgrp(0, me);
    printf(T "job: Ctrl-Z stopped the foreground job by SIGTSTP %d\n", tstp);
    kill(-c, SIGCONT);
    int ttin = stopped_by(c, SIGTTIN);
    printf(T "job: continued in the background, its read stopped it by SIGTTIN %d\n", ttin);
    tcsetpgrp(0, c);
    kill(-c, SIGCONT);
    ms_sleep(300);
    printf(T "job: type a line now\n");
    int read_ok = ended_well(c);
    tcsetpgrp(0, me);
    printf(T "job: in the foreground again it read and ended %d\n", read_ok);
    /* TOSTOP: a background write stops it by SIGTTOU; without, it goes out. */
    struct termios mode, saved;
    tcgetattr(0, &saved);
    mode = saved;
    mode.c_lflag |= TOSTOP;
    tcsetattr(0, TCSANOW, &mode);
    c = job_fork();
    if (c == 0) {
        ms_sleep(100);
        printf(T "job: a background write with TOSTOP went out (wrong)\n");
        _exit(0);
    }
    int ttou = stopped_by(c, SIGTTOU);
    end_job(c);
    tcsetattr(0, TCSANOW, &saved);
    c = job_fork();
    if (c == 0) {
        ms_sleep(100);
        printf(T "job: a background write without TOSTOP goes out\n");
        _exit(0);
    }
    int out = ended_well(c);
    printf(T "job: with TOSTOP a background write stopped it by SIGTTOU %d, without it went out %d\n",
           ttou, out);
    /* A background tcsetattr: SIGTTOU; ignored, it goes on. */
    c = job_fork();
    if (c == 0) {
        struct termios m;
        tcgetattr(0, &m);
        tcsetattr(0, TCSANOW, &m);
        _exit(0);
    }
    int set_stops = stopped_by(c, SIGTTOU);
    end_job(c);
    c = job_fork();
    if (c == 0) {
        struct termios m;
        signal(SIGTTOU, SIG_IGN);
        tcgetattr(0, &m);
        _exit(tcsetattr(0, TCSANOW, &m) == 0 ? 0 : 1);
    }
    printf(T "job: a background tcsetattr stopped it by SIGTTOU %d, with SIGTTOU ignored it went on %d\n",
           set_stops, ended_well(c));
    /* SIGTTIN ignored: a background read is EIO. */
    c = job_fork();
    if (c == 0) {
        signal(SIGTTIN, SIG_IGN);
        char b;
        _exit(read(0, &b, 1) < 0 && errno == EIO ? 0 : 1);
    }
    printf(T "job: with SIGTTIN ignored a background read is EIO %d\n", ended_well(c));
    /* An orphaned group: its stopped member gets SIGHUP and SIGCONT when
     * the group's last link to the session goes; its read is EIO. */
    int report[2];
    pipe(report);
    c = job_fork();
    if (c == 0) {
        pid_t g = fork();                       /* (in its parent's group) */
        if (g == 0) {
            signal(SIGHUP, on_hup);
            kill(getpid(), SIGSTOP);
            char b, got[2] = { hup_seen ? 'h' : '-', 0 };
            got[1] = read(0, &b, 1) < 0 && errno == EIO ? 'e' : '-';
            write(report[1], got, 2);
            _exit(0);
        }
        int st;
        wait_child(g, &st, WUNTRACED);          /* (stopped: then this one goes) */
        _exit(0);
    }
    close(report[1]);
    int gone = ended_well(c);
    char got[3] = "";
    ssize_t n = read_again(report[0], got, 2);
    got[n > 0 ? n : 0] = 0;
    printf(T "job: an orphaned group's stopped member: its parent gone %d, SIGHUP and SIGCONT, then EIO: \"%s\"\n",
           gone, got);
    return 0;
}

/* sigtest job on a pseudo-terminal of its own: its output passed on, Ctrl-Z
 * and a line typed when it asks. */
static int jobpty_test(char *self)
{
    int fds[2];
    if (ritteros_pty_open(fds) < 0) {
        printf(T "jobpty: no pseudo-terminal: %s\n", strerror(errno));
        return 1;
    }
    int slave[3] = { fds[1], fds[1], fds[1] };
    char *argv[] = { self, "job", 0 };
    int pid = ritteros_spawn(self, argv, SPAWN_NEW_GROUP | SPAWN_NEW_SESSION, slave);
    close(fds[1]);
    if (pid < 0) {
        printf(T "jobpty: cannot start: %s\n", strerror(errno));
        return 1;
    }
    char seen[8192];
    size_t have = 0;
    int typed_z = 0, typed_line = 0;
    for (;;) {
        char buf[512];
        ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n <= 0)
            break;
        for (ssize_t k = 0; k < n; k++)
            if (buf[k] != '\r')
                putchar(buf[k]);
        if (have + (size_t)n < sizeof(seen)) {
            memcpy(seen + have, buf, (size_t)n);
            have += (size_t)n;
            seen[have] = 0;
        }
        if (!typed_z && strstr(seen, "type Ctrl-Z now")) {
            typed_z = 1;
            write(fds[0], "\x1a", 1);
        }
        if (!typed_line && strstr(seen, "type a line now")) {
            typed_line = 1;
            write(fds[0], "hello\r", 6);
        }
    }
    int st = 0;
    wait_child(pid, &st, 0);
    printf(T "jobpty: it ended %d\n", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    return 0;
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
        printf(T "usage: sigtest fp|sleep|int|context|stop|job|jobpty\n");
        return 2;
    }
    signal(SIGUSR1, on_usr1);
    if (!strcmp(argv[1], "context"))
        return context_test();
    if (!strcmp(argv[1], "stop"))
        return stop_test();
    if (!strcmp(argv[1], "job"))
        return job_test();
    if (!strcmp(argv[1], "jobpty"))
        return jobpty_test(argv[0]);
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

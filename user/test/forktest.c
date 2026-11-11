/*
 * Test: fork (56a). Lines "forktest: ...".
 *   forktest DIR [own]    DIR: a folder for its files
 * Plain POSIX: the test builds it on the Mac too and compares the
 * "forktest:" lines; "forktest-own:" lines (with "own", not on the Mac)
 * measure RitterOS's copy-on-write (free pages) and its thread numbers.
 * A child reports through its exit code or a pipe.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef __APPLE__
#include "ritteros.h"
#endif

#define BIG (8 << 20)

static int global = 1;
static char *big;
static volatile sig_atomic_t hits;

static int code_of(pid_t pid)
{
    int status = -1;
    if (pid < 0 || waitpid(pid, &status, 0) != pid)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static void on_usr1(int s)
{
    (void)s;
    hits++;
}

static void read_line(int fd, char *out, size_t size)
{
    size_t n = 0;
    ssize_t r;
    while (n < size - 1 && (r = read(fd, out + n, size - 1 - n)) > 0)
        n += (size_t)r;
    out[n] = 0;
}

static volatile int keep_going = 1;

static void *sleeper(void *arg)
{
    (void)arg;
    while (keep_going)
        usleep(1000);
    return 0;
}

static void *allocator(void *arg)
{
    (void)arg;
    while (keep_going) {
        void *a = malloc(100), *b = malloc(5000);
        free(a);
        free(b);
    }
    return 0;
}

static pthread_t forking_self;

static void *fork_from_thread(void *arg)
{
    (void)arg;
    forking_self = pthread_self();
    pid_t pid = fork();
    if (pid == 0)
        _exit(pthread_equal(forking_self, pthread_self()) ? 5 : 1);
    return (void *)(long)code_of(pid);
}

static char order[64];
static int recording;
static void add(const char *s)
{
    if (recording)
        strcat(order, s);                           /* (only around the one fork) */
}
static void prep_a(void) { add("prepare-A "); }
static void prep_b(void) { add("prepare-B "); }
static void parent_a(void) { add("parent-A "); }
static void parent_b(void) { add("parent-B "); }
static void child_a(void) { add("child-A "); }
static void child_b(void) { add("child-B "); }

#ifndef __APPLE__
static long free_pages(void)
{
    struct sys_info info;
    return ritteros_sysinfo(&info) == 0 ? (long)info.free_pages : -1;
}
#endif

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: forktest DIR [own]\n");
        return 2;
    }
    setvbuf(stdout, 0, _IOLBF, 0);                  /* (a child's copy must not print it again) */
    const char *dir = argv[1];
    char path[300], text[300];
    pid_t me = getpid();

    pid_t pid = fork();
    if (pid == 0)
        _exit(getpid() != me ? 3 : 99);
    printf("forktest: a child: %s, exit %d\n", pid > 0 && pid != me ? "another process" : "?",
           code_of(pid));

    /* Memory: the child sees the parent's, and each one's writes stay its own. */
    int local = 1;
    big = malloc(BIG);
    memset(big, 'a', BIG);
    int go[2], back[2];
    pipe(go);
    pipe(back);
    pid = fork();
    if (pid == 0) {
        int ok = global == 1 && local == 1 && big[0] == 'a' && big[BIG - 1] == 'a';
        global = 2;
        local = 2;
        memset(big, 'b', 1 << 20);
        char c;
        read(go[0], &c, 1);                         /* the parent has written meanwhile */
        ok = ok && big[BIG - 1] == 'a' && big[0] == 'b' && global == 2;
        _exit(ok ? 0 : 1);
    }
    big[BIG - 1] = 'z';
    write(go[1], "x", 1);
    int code = code_of(pid);
    printf("forktest: the child saw the parent's memory and kept its own: exit %d; the parent's "
           "after: %d %d %c %c\n", code, global, local, big[0], big[BIG - 1]);

    pid = fork();
    if (pid == 0) {
        close(back[0]);
        write(back[1], "hello from the child", 20);
        _exit(0);
    }
    close(back[1]);
    read_line(back[0], text, sizeof(text));
    close(back[0]);
    printf("forktest: through a pipe: %s, exit %d\n", text, code_of(pid));

    snprintf(path, sizeof(path), "%s/fork-offset.txt", dir);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    pid = fork();
    if (pid == 0) {
        write(fd, "child|", 6);
        _exit(0);
    }
    code_of(pid);
    write(fd, "parent", 6);
    lseek(fd, 0, SEEK_SET);
    read_line(fd, text, sizeof(text));
    close(fd);
    printf("forktest: one file offset for both: %s\n", text);

    /* MAP_SHARED anonymous memory stays shared; MAP_PRIVATE does not. */
    long page = 16384;                              /* (a page or more on both systems) */
    int *shared = mmap(0, 2 * page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    int *private = mmap(0, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    shared[0] = 1;                                  /* (the second page: never touched before) */
    private[0] = 1;
    pid = fork();
    if (pid == 0) {
        shared[0] = 42;
        shared[page / sizeof(int)] = 43;
        private[0] = 7;
        _exit(0);
    }
    code_of(pid);
    printf("forktest: MAP_SHARED memory: %d %d; MAP_PRIVATE: %d\n", shared[0],
           shared[page / sizeof(int)], private[0]);

    /* Signals: handlers and ignored ones as here, the blocked set too, no
     * pending one. */
    signal(SIGUSR1, on_usr1);
    signal(SIGUSR2, SIG_IGN);
    sigset_t s, pending;
    sigemptyset(&s);
    sigaddset(&s, SIGTERM);
    sigprocmask(SIG_BLOCK, &s, 0);
    raise(SIGTERM);                                 /* pending here, not in the child */
    pid = fork();
    if (pid == 0) {
        raise(SIGUSR1);
        raise(SIGUSR2);
        sigset_t now;
        sigprocmask(SIG_BLOCK, 0, &now);
        sigpending(&pending);
        _exit((hits == 1) | (sigismember(&now, SIGTERM) ? 2 : 0) |
              (sigismember(&pending, SIGTERM) ? 0 : 4));
    }
    code = code_of(pid);
    printf("forktest: signals: its handler ran %s, an ignored one ignored %s, the blocked set %s, "
           "the pending one not passed on %s; the parent's handler ran %d time(s)\n",
           code & 1 ? "yes" : "no", code >= 0 && code < 8 ? "yes" : "no", code & 2 ? "yes" : "no",
           code & 4 ? "yes" : "no", (int)hits);
    signal(SIGTERM, SIG_IGN);
    sigprocmask(SIG_UNBLOCK, &s, 0);                /* (the pending SIGTERM: thrown away) */
    signal(SIGTERM, SIG_DFL);
    signal(SIGUSR1, SIG_DFL);
    signal(SIGUSR2, SIG_DFL);

    snprintf(path, sizeof(path), "%s/fork-cloexec.txt", dir);
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    pid = fork();
    if (pid == 0)
        _exit(fcntl(fd, F_GETFD) == FD_CLOEXEC ? 0 : 1);
    printf("forktest: FD_CLOEXEC passed on: exit %d\n", code_of(pid));
    close(fd);

    /* Threads: only the calling one goes on in the child. */
    pthread_t t;
    pthread_create(&t, 0, sleeper, 0);
    pid = fork();
    if (pid == 0) {
        pthread_t u;
        void *r = (void *)1;
        int made = pthread_create(&u, 0, sleeper, 0) == 0;
        keep_going = 0;
        int joined = made && pthread_join(u, &r) == 0 && r == 0;
        _exit(joined ? 0 : 1);
    }
    printf("forktest: a child of a program with two threads makes and joins one: exit %d\n",
           code_of(pid));
    pthread_t f;
    void *r;
    pthread_create(&f, 0, fork_from_thread, 0);
    pthread_join(f, &r);
    printf("forktest: forked from a second thread, the child goes on in it: exit %d\n", (int)(long)r);
    keep_going = 0;
    pthread_join(t, 0);

    int sum = 0;
    for (int i = 0; i < 30; i++) {
        pid = fork();
        if (pid == 0)
            _exit(i);
        sum += code_of(pid);
    }
    pid = fork();
    if (pid == 0) {
        pid_t g = fork();
        if (g == 0)
            _exit(4);
        _exit(code_of(g) + 1);
    }
    printf("forktest: 30 children in turn: exit codes add up to %d; a grandchild's code + 1: %d\n",
           sum, code_of(pid));

    pid = fork();
    if (pid == 0) {
        execlp("sh", "sh", "-c", "exit 6", (char *)0);
        _exit(99);
    }
    printf("forktest: exec in the child: exit %d\n", code_of(pid));

    pthread_atfork(prep_a, parent_a, child_a);
    pthread_atfork(prep_b, parent_b, child_b);
    pipe(back);
    recording = 1;
    pid = fork();
    recording = 0;
    if (pid == 0) {
        write(back[1], order, strlen(order));
        _exit(0);
    }
    close(back[1]);
    read_line(back[0], text, sizeof(text));
    close(back[0]);
    code_of(pid);
    printf("forktest: pthread_atfork: the parent's %s; the child's %s\n", order, text);

    keep_going = 1;
    pthread_create(&t, 0, allocator, 0);
    int well = 0;
    for (int i = 0; i < 20; i++) {
        pid = fork();
        if (pid == 0) {
            char *m = malloc(3000);
            free(m);
            _exit(m ? 0 : 1);
        }
        well += code_of(pid) == 0;
    }
    keep_going = 0;
    pthread_join(t, 0);
    printf("forktest: malloc in 20 children while a thread allocates: %d well\n", well);

    /* (56b) getppid, setpgid, getpgid, setsid. */
    pid_t parent = getpid();
    pid = fork();
    if (pid == 0) {
        int bits = getppid() == parent ? 1 : 0;
        bits |= setpgid(0, 0) == 0 && getpgrp() == getpid() ? 2 : 0;
        bits |= getpgid(0) == getpid() ? 4 : 0;
        errno = 0;
        bits |= setsid() < 0 && errno == EPERM ? 8 : 0;    /* (it leads a group now) */
        _exit(bits);
    }
    code = code_of(pid);
    printf("forktest: getppid %s; setpgid(0, 0) %s, getpgid %s; setsid of a group's leader EPERM %s\n",
           code & 1 ? "yes" : "no", code & 2 ? "yes" : "no", code & 4 ? "yes" : "no",
           code & 8 ? "yes" : "no");
    pid = fork();
    if (pid == 0)
        _exit(setsid() == getpid() && getpgrp() == getpid() ? 0 : 1);
    printf("forktest: setsid in a child: exit %d\n", code_of(pid));
    pipe(go);
    pid = fork();
    if (pid == 0) {
        char c;
        close(go[1]);
        read(go[0], &c, 1);
        _exit(getpgrp() == getpid() ? 0 : 1);
    }
    close(go[0]);
    int moved = setpgid(pid, pid);
    pid_t its = getpgid(pid);
    write(go[1], "x", 1);
    close(go[1]);
    printf("forktest: the parent moves a child to a group of its own: %d, getpgid %s, exit %d\n",
           moved, its == pid ? "its own" : "another", code_of(pid));
    errno = 0;
    printf("forktest: getpgid of no process: %s\n", getpgid(999999) < 0 && errno == ESRCH ? "ESRCH" : "?");
    pid = fork();
    if (pid == 0) {
        execlp("sleep", "sleep", "1", (char *)0);
        _exit(99);
    }
    usleep(500000);
    errno = 0;
    printf("forktest: setpgid of a child that ran exec: %s\n",
           setpgid(pid, pid) < 0 && errno == EACCES ? "EACCES" : "?");
    code_of(pid);

#ifndef __APPLE__
    if (argc == 3 && !strcmp(argv[2], "own")) {
        /* Copy-on-write: a child that writes little costs little. */
        long before = free_pages();
        pipe(go);
        pid = fork();
        if (pid == 0) {
            char c;
            big[0] = 'c';
            read(go[0], &c, 1);
            _exit(0);
        }
        usleep(200000);
        long during = free_pages();
        write(go[1], "x", 1);
        code_of(pid);
        printf("forktest-own: a child of a program with 8 MiB written, writing one page itself, "
               "costs fewer than 200 pages: %s\n", before - during < 200 ? "yes" : "no");
        printf("forktest-info: that child cost %ld pages (the 8 MiB are %d)\n", before - during,
               BIG / 4096);
        before = free_pages();
        for (int i = 0; i < 10; i++) {
            pid = fork();
            if (pid == 0) {
                memset(big, 'd', 1 << 20);
                _exit(0);
            }
            code_of(pid);
        }
        long after = free_pages();
        printf("forktest-own: 10 children that wrote 1 MiB each, ended: free pages back within "
               "8: %s\n", after - before >= -8 && after - before <= 8 ? "yes" : "no");
        printf("forktest-info: free pages before %ld, after %ld\n", before, after);
        pthread_create(&t, 0, sleeper, 0);
        keep_going = 1;
        pid = fork();
        if (pid == 0)
            _exit(pthread_join(t, 0) == ESRCH ? 0 : 1);
        printf("forktest-own: the other thread is not in the child (join: ESRCH): exit %d\n",
               code_of(pid));
        keep_going = 0;
        pthread_join(t, 0);
    }
#endif
    printf("forktest: done\n");
    return 0;
}

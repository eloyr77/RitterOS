/*
 * Test: execve and the exec family (55b). Lines "exectest: ...".
 *   exectest DIR         the checks (DIR: a folder for files and scripts)
 * It spawns a child ("chain") that sets up what exec keeps or not -- a
 * plain descriptor and an FD_CLOEXEC one, a handled, an ignored and a
 * blocked signal, a umask, a folder, two more threads -- and execs this
 * program again ("after") from one of those threads, with an environment
 * of its own; the new program looks at what it got and ends with exit
 * code 7, which the parent sees for the same process number. Then the
 * errors (no file, not runnable, not a program), a #! script, one
 * without #! through execvp, and execvp by name.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread.h>
#include <unistd.h>
#include "ritteros.h"

#if defined(__x86_64__)
#define ARCH "x86_64"
#else
#define ARCH "aarch64"
#endif
#define SELF "/system/" ARCH "/test/exectest"

static const char *err(int e)
{
    return e == 0 ? "0" : e == ENOENT ? "ENOENT" : e == EACCES ? "EACCES" : e == ENOEXEC ? "ENOEXEC"
         : e == EBADF ? "EBADF" : e == ESRCH ? "ESRCH" : "another";
}

static void on_usr1(int s)
{
    (void)s;
}

static int sleeper(void *arg)
{
    (void)arg;
    for (;;)
        sleep(10);
    return 0;
}

static char pid_text[16], dir_[200];

static int exec_from_thread(void *arg)
{
    (void)arg;
    char *envp[] = { "EXECTEST=from the exec", 0 };
    execle(SELF, "exectest", "after", pid_text, dir_, (char *)0, envp);
    printf("exectest: FAIL execle: %s\n", strerror(errno));
    exit(1);
}

static int chain(const char *dir)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/exec-kept.txt", dir);
    int kept = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    snprintf(path, sizeof(path), "%s/exec-closed.txt", dir);
    int closed = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(kept, 5);
    dup2(closed, 6);
    close(kept);
    close(closed);
    fcntl(6, F_SETFD, FD_CLOEXEC);
    signal(SIGUSR1, on_usr1);
    signal(SIGUSR2, SIG_IGN);
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGTERM);
    sigprocmask(SIG_BLOCK, &s, 0);
    umask(027);
    chdir(dir);
    snprintf(pid_text, sizeof(pid_text), "%d", getpid());
    snprintf(dir_, sizeof(dir_), "%s", dir);
    thread_create(sleeper, 0);
    thread_create(sleeper, 0);
    int t = thread_create(exec_from_thread, 0);
    thread_join(t, 0);                          /* (never: the exec ends this program) */
    return 1;
}

static int after(const char *old_pid, const char *dir)
{
    struct stat st;
    printf("exectest: after exec: the same process %s, thread %d, argv[0] %s\n",
           atoi(old_pid) == getpid() ? "yes" : "no", thread_self(), "exectest");
    printf("exectest: fd 5 kept %s, FD_CLOEXEC fd 6 %s\n", fstat(5, &st) == 0 ? "yes" : "no",
           fstat(6, &st) < 0 && errno == EBADF ? "closed" : "open");
    struct sigaction a;
    sigaction(SIGUSR1, 0, &a);
    int handled_default = a.sa_handler == SIG_DFL;
    sigaction(SIGUSR2, 0, &a);
    int ignored_kept = a.sa_handler == SIG_IGN;
    sigset_t now;
    sigprocmask(SIG_BLOCK, 0, &now);
    printf("exectest: the handled signal back to SIG_DFL %s, the ignored one still ignored %s, "
           "SIGTERM still blocked %s\n", handled_default ? "yes" : "no", ignored_kept ? "yes" : "no",
           sigismember(&now, SIGTERM) ? "yes" : "no");
    mode_t m = umask(022);
    char cwd[256];
    getcwd(cwd, sizeof(cwd));
    printf("exectest: umask %03o, folder the same %s, environment %s\n", (unsigned)m,
           strcmp(cwd, dir) ? "no" : "yes", getenv("EXECTEST") ? getenv("EXECTEST") : "(none)");
    errno = 0;
    printf("exectest: the other threads are gone: joining 1 %s, 2 %s\n",
           thread_join(1, 0) < 0 && errno == ESRCH ? "ESRCH" : "joined",
           thread_join(2, 0) < 0 && errno == ESRCH ? "ESRCH" : "joined");
    return 7;
}

static void write_script(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    write(fd, text, strlen(text));
    close(fd);
}

/* Spawn this program in MODE ARG and wait: its exit code. */
static int run(const char *mode, const char *arg, pid_t *pid_out)
{
    char *argv[] = { "exectest", (char *)mode, (char *)arg, 0 };
    int none[3] = { -1, -1, -1 };
    pid_t pid = ritteros_spawn(SELF, argv, 0, none);
    int status = -1;
    if (pid > 0)
        waitpid(pid, &status, 0);
    if (pid_out)
        *pid_out = pid;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "chain"))
        return chain(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "after"))
        return after(argv[2], argv[3]);
    if (argc == 3 && !strcmp(argv[1], "script")) {
        char *a[] = { argv[2], "an argument", 0 };
        execvp(argv[2], a);
        printf("exectest: FAIL execvp %s: %s\n", argv[2], strerror(errno));
        return 1;
    }
    if (argc == 3 && !strcmp(argv[1], "byname")) {
        execlp(argv[2], argv[2], "found by name", (char *)0);
        printf("exectest: FAIL execlp: %s\n", strerror(errno));
        return 1;
    }
    if (argc != 2) {
        printf("usage: exectest DIR\n");
        return 2;
    }
    const char *dir = argv[1];
    pid_t pid;
    int code = run("chain", dir, &pid);
    printf("exectest: the child's exit code %d (from the program it became)\n", code);

    char *none[] = { "x", 0 };
    errno = 0;
    int r = execv("/nonexistent", none);
    printf("exectest: execv of nothing: %d %s, still here\n", r, err(errno));
    errno = 0;
    r = execv("/readme.txt", none);
    printf("exectest: execv of a file without x: %s\n", err(errno));
    char path[256], hashbang[256];
    snprintf(path, sizeof(path), "%s/exec-plain.sh", dir);
    write_script(path, "echo exectest: a plain script ran with $1\n");
    errno = 0;
    r = execv(path, none);
    printf("exectest: execv of a script without #!: %s\n", err(errno));
    printf("exectest: execvp runs it through sh: exit code %d\n", run("script", path, 0));
    snprintf(hashbang, sizeof(hashbang), "%s/exec-hash.sh", dir);
    write_script(hashbang, "#!/system/" ARCH "/bin/sh\necho exectest: a hash-bang script ran with $1\n");
    printf("exectest: execvp of a #! script: exit code %d\n", run("script", hashbang, 0));
    printf("exectest: execlp by name: exit code %d\n", run("byname", "echo", 0));
    printf("exectest: done\n");
    return 0;
}

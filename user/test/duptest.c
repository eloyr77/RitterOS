/*
 * Test: dup, dup2 and pipe (52a, SYS_DUP2). Each check prints a line the
 * suite looks for ("duptest: NAME ok", or "duptest: FAIL ..."):
 *  - copy: dup gives the lowest free number, and writing through it reaches
 *    the same place as the original.
 *  - shared: two descriptors for one file share its position (writes through
 *    both, and through the copy after the original is closed, land in turn).
 *  - stdout: dup2 onto 1 sends printf into a file; dup2 back restores it.
 *  - errors: a closed descriptor, numbers out of range, dup2(fd, fd), and
 *    descriptors running out (EMFILE).
 *  - pipe eof: a pipe's read end sees the end only after the last write end,
 *    a dup'd one included, is closed.
 *  - spawn: a pipe's read end put on 0 with dup2 is a spawned cat's input.
 * Usage: duptest DIR (a folder it may write files in).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ritteros.h"

#if defined(__x86_64__)
#define ARCH "x86_64"
#else
#define ARCH "aarch64"
#endif

static int failed;

static void result(const char *name, int ok, const char *detail)
{
    if (ok)
        printf("duptest: %s ok\n", name);
    else {
        printf("duptest: FAIL %s: %s (errno %d)\n", name, detail, errno);
        failed = 1;
    }
}

static int file_is(const char *path, const char *want)
{
    char buf[256];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0)
        return 0;
    buf[n] = 0;
    return strcmp(buf, want) == 0;
}

static void put(int fd, const char *s)
{
    write(fd, s, strlen(s));
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: duptest DIR\n");
        return 2;
    }
    char one[200], two[200];
    snprintf(one, sizeof(one), "%s/dup-shared.txt", argv[1]);
    snprintf(two, sizeof(two), "%s/dup-stdout.txt", argv[1]);

    int d = dup(1);
    put(d, "duptest: written through a copy of stdout\n");
    result("copy", d == 3, "dup(1) was not 3, the lowest free number");
    close(d);

    int a = open(one, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int b = dup(a);
    put(a, "one ");
    put(b, "two ");
    put(a, "three\n");
    close(a);
    put(b, "four\n");
    close(b);
    result("shared", a >= 0 && b >= 0 && file_is(one, "one two three\nfour\n"),
           "the file does not hold the four writes in turn");

    int saved = dup(1);
    int f = open(two, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int r = dup2(f, 1);
    close(f);
    printf("into the file\n");
    dup2(saved, 1);
    close(saved);
    result("stdout", r == 1 && file_is(two, "into the file\n"), "printf did not go into the file");

    int ok = 1;
    errno = 0;
    ok &= dup(9) == -1 && errno == EBADF;
    ok &= dup2(1, FD_MAX) == -1 && errno == EBADF;
    ok &= dup2(1, -1) == -1 && errno == EBADF;
    ok &= dup2(9, 4) == -1 && errno == EBADF;
    ok &= dup2(1, 1) == 1;
    static int fds[FD_MAX];
    int count = 0;
    while (count < FD_MAX && (fds[count] = dup(1)) >= 0)
        count++;
    ok &= count == FD_MAX - 3 && errno == EMFILE;   /* (0-2 open: 253 more of 256) */
    while (count > 0)
        close(fds[--count]);
    ok &= dup(1) == 3;
    close(3);
    result("errors", ok, "a wrong descriptor, a number out of range or EMFILE");

    int p[2];
    char c[4];
    ok = pipe(p) == 0;
    int w2 = dup(p[1]);
    close(p[1]);
    put(w2, "x");
    ok &= read(p[0], c, sizeof(c)) == 1 && c[0] == 'x';
    close(w2);
    ok &= read(p[0], c, sizeof(c)) == 0;
    close(p[0]);
    result("pipe eof", ok, "the end came early or never");

    int q[2];
    ok = pipe(q) == 0;
    put(q[1], "duptest: cat read this from a pipe on its input\n");
    close(q[1]);
    int saved0 = dup(0);
    dup2(q[0], 0);
    close(q[0]);
    char *cat[] = { "cat", 0 };
    int none[3] = { -1, -1, -1 };
    pid_t pid = ritteros_spawn("/system/" ARCH "/bin/cat", cat, 0, none);
    int status = -1;
    if (pid > 0)
        waitpid(pid, &status, 0);
    dup2(saved0, 0);
    close(saved0);
    result("spawn", ok && pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0, "cat did not run");

    printf(failed ? "duptest: FAIL (see above)\n" : "duptest: all ok\n");
    return failed;
}

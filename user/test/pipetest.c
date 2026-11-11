/*
 * Test: the pipe() primitive (SYS_PIPE). Three checks, each printing a line
 * the suite greps for:
 *  - stream: a large pattern through a pipe between two threads round-trips
 *    exactly (exercises blocking and the ring wrapping), and the reader sees
 *    end of file once the writer closes the write end.
 *  - spawn eof: a child spawned with the pipe's write end as its stdout; once
 *    the parent drops its own write end and the child exits, the read end is
 *    at end of file (proves fd inheritance refcounts the write end).
 *  - epipe: writing to a pipe whose read end is closed fails with EPIPE.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <thread.h>
#include "ritteros.h"

#if defined(__x86_64__)
#define ARCH "x86_64"
#elif defined(__aarch64__)
#define ARCH "aarch64"
#endif

#define N   200000              /* > one pipe buffer, so the writer blocks and the ring wraps */

static uint8_t pat(long i)
{
    return (uint8_t)(i * 31 + 7);
}

static int g_write_fd;

static int writer(void *arg)
{
    (void)arg;
    uint8_t buf[4096];
    long done = 0;
    while (done < N) {
        size_t n = N - done < (long)sizeof(buf) ? (size_t)(N - done) : sizeof(buf);
        for (size_t i = 0; i < n; i++)
            buf[i] = pat(done + (long)i);
        size_t off = 0;
        while (off < n) {
            ssize_t w = write(g_write_fd, buf + off, n - off);
            if (w <= 0)
                return 1;
            off += (size_t)w;
        }
        done += (long)n;
    }
    close(g_write_fd);          /* the reader now sees end of file */
    return 0;
}

static int test_stream(void)
{
    int fds[2];
    if (ritteros_pipe(fds) < 0) {
        printf("pipetest: FAIL pipe: %s\n", strerror(errno));
        return 1;
    }
    g_write_fd = fds[1];
    int t = thread_create(writer, 0);
    if (t < 0) {
        printf("pipetest: FAIL thread\n");
        return 1;
    }
    uint8_t buf[4096];
    long got = 0;
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++)
            if (buf[i] != pat(got + i)) {
                printf("pipetest: FAIL stream: byte %ld wrong\n", got + i);
                return 1;
            }
        got += n;
    }
    if (n < 0) {
        printf("pipetest: FAIL stream read: %s\n", strerror(errno));
        return 1;
    }
    close(fds[0]);
    int result = 1;
    thread_join(t, &result);
    if (got != N || result != 0) {
        printf("pipetest: FAIL stream: %ld of %d bytes, writer %d\n", got, N, result);
        return 1;
    }
    printf("pipetest: stream ok (%ld bytes)\n", got);
    return 0;
}

static int test_spawn_eof(void)
{
    int fds[2];
    if (ritteros_pipe(fds) < 0) {
        printf("pipetest: FAIL pipe: %s\n", strerror(errno));
        return 1;
    }
    char *argv[] = { "echo", "hello", 0 };
    int child[3] = { 0, fds[1], 2 };            /* the child's stdout is the write end */
    int pid = ritteros_spawn("/system/" ARCH "/bin/echo", argv, SPAWN_NEW_GROUP, child);
    close(fds[1]);                              /* the child is now the only writer */
    if (pid < 0) {
        printf("pipetest: FAIL spawn: %s\n", strerror(errno));
        return 1;
    }
    char out[64];
    size_t got = 0;
    ssize_t n;
    while ((n = read(fds[0], out + got, sizeof(out) - 1 - got)) > 0)
        got += (size_t)n;
    out[got] = 0;
    close(fds[0]);
    int status = -1;
    waitpid(pid, &status, 0);
    if (n != 0 || strcmp(out, "hello\n") != 0) {
        printf("pipetest: FAIL spawn eof: got \"%s\" (n=%ld)\n", out, (long)n);
        return 1;
    }
    printf("pipetest: spawn eof ok\n");
    return 0;
}

static int test_epipe(void)
{
    int fds[2];
    if (ritteros_pipe(fds) < 0) {
        printf("pipetest: FAIL pipe: %s\n", strerror(errno));
        return 1;
    }
    close(fds[0]);                              /* no reader left */
    errno = 0;
    ssize_t w = write(fds[1], "x", 1);
    int e = errno;
    close(fds[1]);
    if (w >= 0 || e != EPIPE) {
        printf("pipetest: FAIL epipe: write returned %ld, errno %d\n", (long)w, e);
        return 1;
    }
    printf("pipetest: epipe ok\n");
    return 0;
}

int main(void)
{
    if (test_stream() || test_spawn_eof() || test_epipe())
        return 1;
    printf("pipetest: all ok\n");
    return 0;
}

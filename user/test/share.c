/*
 * Test: shared buffers (step 15c). A client and a server (two threads of
 * one program, each its own local-socket endpoint) share a buffer:
 *   - the client makes a buffer, maps it, fills it with a pattern, and
 *     passes it to the server over a local socket;
 *   - the server receives the buffer, maps it, checks the pattern (proving
 *     the same memory), and changes the first byte;
 *   - the client sees the server's change (proving it is shared both ways).
 * Each prints what it found, for the test to compare.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <thread.h>
#include <unistd.h>
#include "ritteros.h"

#define PATH    "/run/share"
#define SIZE    8192                    /* two pages, to exercise more than one */
#define MARK    0xAA

static unsigned char pattern(size_t i)
{
    return (unsigned char)(i * 7 + 1);
}

static int fail(const char *what)
{
    printf("share: %s: %s\n", what, strerror(errno));
    return 1;
}

static int server(void *arg)
{
    (void)arg;
    int fd = ritteros_local_listen(PATH, 4);
    if (fd < 0)
        return fail("listen");
    int c = ritteros_accept(fd, 0, 0);
    if (c < 0)
        return fail("accept");
    char go[2];
    if (read(c, go, 2) < 0)                             /* wait until the buffer was sent */
        return fail("read");
    int bfd = -1;
    for (int i = 0; i < 20 && bfd < 0; i++) {
        bfd = ritteros_share_recv(c);
        if (bfd < 0 && errno == EAGAIN)
            usleep(5000);
        else
            break;
    }
    if (bfd < 0)
        return fail("recv");
    unsigned char *mem = ritteros_share_map(bfd);
    if (!mem)
        return fail("map");
    long size = ritteros_share_size(bfd);
    int ok = size == SIZE;
    for (long i = 0; i < size; i++)
        if (mem[i] != pattern((size_t)i))
            ok = 0;
    printf("server: pattern %s (%ld bytes)\n", ok ? "ok" : "mismatch", size);
    mem[0] = MARK;                                      /* the client should see this */
    write(c, "ok", 2);
    close(bfd);
    close(c);
    close(fd);
    return ok ? 0 : 1;
}

int main(void)
{
    int t = thread_create(server, 0);
    if (t < 0)
        return fail("thread");
    int sock = -1;
    for (int i = 0; i < 40 && sock < 0; i++) {          /* wait for the listener */
        sock = ritteros_local_connect(PATH);
        if (sock < 0 && errno == ECONNREFUSED)
            usleep(50000);
        else
            break;
    }
    if (sock < 0)
        return fail("connect");
    int bfd = ritteros_share_create(SIZE);
    if (bfd < 0)
        return fail("create");
    unsigned char *mem = ritteros_share_map(bfd);
    if (!mem)
        return fail("map");
    for (size_t i = 0; i < SIZE; i++)
        mem[i] = pattern(i);
    if (ritteros_share_send(sock, bfd) < 0)
        return fail("send");
    write(sock, "go", 2);
    char ok[2];
    if (read(sock, ok, 2) < 0)                          /* the server is done */
        return fail("read");
    printf("client: server's change %s\n", mem[0] == MARK ? "seen" : "not seen");
    int result = mem[0] == MARK ? 0 : 1;
    close(bfd);
    close(sock);
    int server_result = 0;
    thread_join(t, &server_result);
    return result || server_result;
}

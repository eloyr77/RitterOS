/*
 * Test: poll (52d, SYS_POLL). Each check prints "polltest: NAME ok" or
 * "polltest: FAIL ...":
 *  - nothing yet: an empty pipe's read end has nothing (timeout 0), its
 *    write end can be written.
 *  - data: a byte in the second of two pipes is reported there only.
 *  - timeout: an empty pipe for 300 ms returns 0 after about that long.
 *  - woken: a thread writing after 200 ms wakes a poll without a limit.
 *  - ends: a read end whose writers are gone is POLLHUP; a write end whose
 *    reader is gone is POLLERR.
 *  - bad and skipped: a closed descriptor is POLLNVAL, a negative one
 *    is skipped.
 *  - a file: a RitterFS file is readable and writable at once.
 *  - a pty: the master is readable once the slave wrote.
 *  - a udp socket: readable once a datagram to it arrived (127.0.0.1).
 *  - a local socket: the listener is readable with a connection waiting,
 *    the accepted end once the client wrote.
 *  - many (52k): 120 pipes (240 descriptors) at once, the last one's read
 *    end the only one with a byte.
 * Usage: polltest DIR (a folder it may write a file in).
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <thread.h>
#include <unistd.h>
#include "ritteros.h"

static int failed;

static void result(const char *name, int ok, const char *detail)
{
    if (ok)
        printf("polltest: %s ok\n", name);
    else {
        printf("polltest: FAIL %s: %s (errno %d)\n", name, detail, errno);
        failed = 1;
    }
}

static long ms_since(uint64_t start)
{
    return (long)((ritteros_time() - start) / 1000000);
}

static int late_fd;

static int late_writer(void *arg)
{
    (void)arg;
    usleep(200000);
    write(late_fd, "w", 1);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: polltest DIR\n");
        return 2;
    }
    int a[2], b[2];
    pipe(a);
    pipe(b);
    struct pollfd p[3];

    p[0] = (struct pollfd){ a[0], POLLIN, 0 };
    p[1] = (struct pollfd){ a[1], POLLOUT, 0 };
    int n = poll(p, 1, 0);
    int ok = n == 0 && p[0].revents == 0;
    n = poll(p + 1, 1, 0);
    ok &= n == 1 && p[1].revents == POLLOUT;
    result("nothing yet", ok, "an empty pipe had something, or its write end could not be written");

    write(b[1], "x", 1);
    p[0] = (struct pollfd){ a[0], POLLIN, 0 };
    p[1] = (struct pollfd){ b[0], POLLIN | POLLRDNORM, 0 };
    n = poll(p, 2, -1);
    ok = n == 1 && p[0].revents == 0 && p[1].revents == (POLLIN | POLLRDNORM);
    char c;
    read(b[0], &c, 1);
    result("data", ok, "the wrong pipe was reported");

    uint64_t start = ritteros_time();
    p[0] = (struct pollfd){ a[0], POLLIN, 0 };
    n = poll(p, 1, 300);
    long took = ms_since(start);
    printf("polltest: an empty pipe for 300 ms: %d after %s\n", n,
           took >= 280 && took < 2000 ? "about that long" : "a wrong time");
    result("timeout", n == 0 && took >= 280 && took < 2000, "not 0 after about 300 ms");

    late_fd = a[1];
    int t = thread_create(late_writer, 0);
    start = ritteros_time();
    p[0] = (struct pollfd){ a[0], POLLIN, 0 };
    n = poll(p, 1, -1);
    took = ms_since(start);
    thread_join(t, 0);
    read(a[0], &c, 1);
    result("woken", n == 1 && p[0].revents == POLLIN && took >= 150 && took < 2000,
           "a write by another thread did not end the wait in time");

    close(a[1]);
    p[0] = (struct pollfd){ a[0], POLLIN, 0 };
    n = poll(p, 1, 0);
    ok = n == 1 && p[0].revents == POLLHUP;
    close(b[0]);
    p[0] = (struct pollfd){ b[1], POLLOUT, 0 };
    n = poll(p, 1, 0);
    ok &= n == 1 && (p[0].revents & POLLERR);
    close(a[0]);
    close(b[1]);
    result("ends", ok, "a pipe without writers or readers was not reported");

    p[0] = (struct pollfd){ 9, POLLIN, 0 };
    p[1] = (struct pollfd){ -1, POLLIN, 77 };
    n = poll(p, 2, 0);
    result("bad and skipped", n == 1 && p[0].revents == POLLNVAL && p[1].revents == 0,
           "a closed descriptor was not POLLNVAL, or a negative one not skipped");

    char path[200];
    snprintf(path, sizeof(path), "%s/poll.txt", argv[1]);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    p[0] = (struct pollfd){ fd, POLLIN | POLLOUT, 0 };
    n = poll(p, 1, -1);
    close(fd);
    result("a file", n == 1 && p[0].revents == (POLLIN | POLLOUT), "a file is not ready at once");

    int pty[2];
    ok = ritteros_pty_open(pty) == 0;
    p[0] = (struct pollfd){ pty[0], POLLIN, 0 };
    ok &= poll(p, 1, 0) == 0;
    write(pty[1], "hi", 2);
    n = poll(p, 1, 1000);
    ok &= n == 1 && (p[0].revents & POLLIN);
    close(pty[1]);
    close(pty[0]);
    result("a pty", ok, "the master was not readable after the slave wrote");

    int u = ritteros_socket(SOCKET_UDP);
    int port = ritteros_bind(u, 0);
    p[0] = (struct pollfd){ u, POLLIN, 0 };
    ok = u >= 0 && port > 0 && poll(p, 1, 0) == 0;
    int s = ritteros_socket(SOCKET_UDP);
    ritteros_sendto(s, "dgram", 5, 0x7F000001, (uint16_t)port);
    n = poll(p, 1, 2000);
    ok &= n == 1 && (p[0].revents & POLLIN);
    close(s);
    close(u);
    result("a udp socket", ok, "no POLLIN after a datagram to it");

    int l = ritteros_local_listen("/run/polltest", 2);
    p[0] = (struct pollfd){ l, POLLIN, 0 };
    ok = l >= 0 && poll(p, 1, 0) == 0;
    int cl = ritteros_local_connect("/run/polltest");
    n = poll(p, 1, 1000);
    ok &= n == 1 && (p[0].revents & POLLIN);
    int srv = ritteros_accept(l, 0, 0);
    p[0] = (struct pollfd){ srv, POLLIN, 0 };
    ok &= srv >= 0 && poll(p, 1, 0) == 0;
    write(cl, "ping", 4);
    n = poll(p, 1, 1000);
    ok &= n == 1 && (p[0].revents & POLLIN);
    close(cl);
    close(srv);
    close(l);
    result("a local socket", ok, "a waiting connection or the client's bytes were not reported");

    static int many[120][2];
    static struct pollfd all[120];
    int made = 0;
    while (made < 120 && pipe(many[made]) == 0)
        made++;
    if (made == 120)
        write(many[119][1], "m", 1);
    for (int i = 0; i < made; i++)
        all[i] = (struct pollfd){ many[i][0], POLLIN, 0 };
    n = poll(all, (nfds_t)made, 1000);
    printf("polltest: %d pipes made, %d with something\n", made, n);
    result("many", made == 120 && n == 1 && all[119].revents == POLLIN, "240 descriptors at once");
    for (int i = 0; i < made; i++) {
        close(many[i][0]);
        close(many[i][1]);
    }

    printf(failed ? "polltest: FAIL (see above)\n" : "polltest: all ok\n");
    return failed;
}

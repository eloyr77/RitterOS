/*
 * Test: fcntl and O_NONBLOCK (53b). Plain POSIX: the test builds it on the
 * Mac too and compares the "nbtest:" lines (no sizes or ports in them).
 * Descriptor flags (F_GETFD/F_SETFD, F_DUPFD from a minimum, _CLOEXEC),
 * O_NONBLOCK from open and F_SETFL; a pipe read with nothing in it, a pipe
 * written until full (the last write partial or EAGAIN); TCP: accept with
 * nothing waiting, connect EINPROGRESS then POLLOUT and SO_ERROR 0, a read
 * with nothing there, a refused connect; MSG_DONTWAIT on UDP; a local
 * socket read; select (53c): an empty pipe's timeout, a readable and a
 * writable end, a listener with a connection, a closed descriptor.
 * Usage: nbtest DIR (a folder for a file and a local socket's name)
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static const char *err(int e)
{
    return e == 0 ? "0" : e == EAGAIN ? "EAGAIN" : e == EINPROGRESS ? "EINPROGRESS"
         : e == ECONNREFUSED ? "ECONNREFUSED" : e == EBADF ? "EBADF" : e == EINVAL ? "EINVAL"
         : "another";
}

static void nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static struct sockaddr_in loopback(unsigned short port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

static unsigned short port_of(int fd)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);
    getsockname(fd, (struct sockaddr *)&a, &n);
    return ntohs(a.sin_port);
}

static void flags(const char *dir)
{
    int p[2];
    pipe(p);
    printf("nbtest: a pipe's ends: F_GETFD %d, read end O_RDONLY %s, write end O_WRONLY %s\n",
           fcntl(p[0], F_GETFD), (fcntl(p[0], F_GETFL) & O_ACCMODE) == O_RDONLY ? "yes" : "no",
           (fcntl(p[1], F_GETFL) & O_ACCMODE) == O_WRONLY ? "yes" : "no");
    printf("nbtest: F_SETFD FD_CLOEXEC %d, then F_GETFD %d\n", fcntl(p[0], F_SETFD, FD_CLOEXEC),
           fcntl(p[0], F_GETFD));
    int d = fcntl(p[0], F_DUPFD, 10);
    int e = fcntl(p[0], F_DUPFD_CLOEXEC, 10);
    printf("nbtest: F_DUPFD from 10: %d, F_GETFD %d; F_DUPFD_CLOEXEC: %d, F_GETFD %d\n", d,
           fcntl(d, F_GETFD), e, fcntl(e, F_GETFD));
    close(d);
    close(e);
    errno = 0;
    int r = fcntl(77, F_GETFL);
    printf("nbtest: a closed descriptor: %d %s\n", r, err(errno));
    char path[200];
    snprintf(path, sizeof(path), "%s/nbtest.txt", dir);
    int f = open(path, O_RDWR | O_CREAT | O_TRUNC | O_NONBLOCK, 0644);
    printf("nbtest: open with O_NONBLOCK: %s, O_RDWR %s\n", fcntl(f, F_GETFL) & O_NONBLOCK ? "kept" : "lost",
           (fcntl(f, F_GETFL) & O_ACCMODE) == O_RDWR ? "yes" : "no");
    fcntl(f, F_SETFL, 0);
    printf("nbtest: F_SETFL 0 clears it: %s\n", fcntl(f, F_GETFL) & O_NONBLOCK ? "no" : "yes");
    close(f);
    unlink(path);

    nonblock(p[0]);
    char buf[64];
    errno = 0;
    ssize_t n = read(p[0], buf, sizeof(buf));
    printf("nbtest: an empty pipe: read %d %s\n", (int)n, err(errno));
    nonblock(p[1]);
    static char block[4096];
    long total = 0, last = 0;
    for (int i = 0; i < 1000; i++) {
        errno = 0;
        n = write(p[1], block, sizeof(block));
        if (n < 0)
            break;
        total += n;
        last = n;
    }
    printf("nbtest: a pipe written until full: then %s; something went in: %s\n", err(errno),
           total > 0 ? "yes" : "no");
    (void)last;
    n = read(p[0], buf, sizeof(buf));
    printf("nbtest: and read again: %d\n", (int)n);
    close(p[0]);
    close(p[1]);
}

static void tcp(void)
{
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = loopback(0);
    bind(l, (struct sockaddr *)&a, sizeof(a));
    listen(l, 4);
    nonblock(l);
    errno = 0;
    int r = accept(l, 0, 0);
    printf("nbtest: accept with nothing waiting: %d %s\n", r, err(errno));
    int c = socket(AF_INET, SOCK_STREAM, 0);
    nonblock(c);
    a = loopback(port_of(l));
    errno = 0;
    r = connect(c, (struct sockaddr *)&a, sizeof(a));
    int e = errno;
    printf("nbtest: connect without waiting: %s\n",
           r == 0 || e == EINPROGRESS ? "connected or EINPROGRESS" : err(e));
    struct pollfd pf = { c, POLLOUT, 0 };
    int ready = poll(&pf, 1, 2000);
    int soerr = -1;
    socklen_t len = sizeof(soerr);
    getsockopt(c, SOL_SOCKET, SO_ERROR, &soerr, &len);
    printf("nbtest: then POLLOUT %s, SO_ERROR %d\n", ready == 1 && (pf.revents & POLLOUT) ? "yes" : "no",
           soerr);
    struct pollfd pl = { l, POLLIN, 0 };
    poll(&pl, 1, 2000);
    int s = accept(l, 0, 0);
    printf("nbtest: accept once it is there: %s\n", s >= 0 ? "yes" : "no");
    nonblock(s);
    char buf[16];
    errno = 0;
    ssize_t n = recv(s, buf, sizeof(buf), 0);
    printf("nbtest: a read with nothing there: %d %s\n", (int)n, err(errno));
    send(c, "data", 4, 0);
    struct pollfd ps = { s, POLLIN, 0 };
    poll(&ps, 1, 2000);
    n = recv(s, buf, sizeof(buf), 0);
    printf("nbtest: then %d '%.*s'\n", (int)n, (int)(n > 0 ? n : 0), buf);
    close(c);
    close(s);
    unsigned short gone = port_of(l);
    close(l);
    int c2 = socket(AF_INET, SOCK_STREAM, 0);
    nonblock(c2);
    a = loopback(gone);
    errno = 0;
    r = connect(c2, (struct sockaddr *)&a, sizeof(a));
    e = errno;
    if (r < 0 && e == EINPROGRESS) {
        struct pollfd pr = { c2, POLLOUT, 0 };
        poll(&pr, 1, 2000);
        len = sizeof(soerr);
        getsockopt(c2, SOL_SOCKET, SO_ERROR, &soerr, &len);
        e = soerr;
    }
    printf("nbtest: a refused connect without waiting: %s\n", err(e));
    close(c2);
}

static void others(const char *dir)
{
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = loopback(0);
    bind(u, (struct sockaddr *)&a, sizeof(a));
    char buf[16];
    errno = 0;
    ssize_t n = recv(u, buf, sizeof(buf), MSG_DONTWAIT);
    printf("nbtest: udp MSG_DONTWAIT with nothing there: %d %s\n", (int)n, err(errno));
    errno = 0;
    n = recvfrom(u, buf, sizeof(buf), MSG_DONTWAIT, 0, 0);
    printf("nbtest: recvfrom MSG_DONTWAIT: %d %s; the descriptor still waits: %s\n", (int)n,
           err(errno), fcntl(u, F_GETFL) & O_NONBLOCK ? "no" : "yes");
    close(u);
    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof(un.sun_path), "%s/nbtest.sock", dir);
    unlink(un.sun_path);
    int l = socket(AF_UNIX, SOCK_STREAM, 0);
    bind(l, (struct sockaddr *)&un, sizeof(un));
    listen(l, 2);
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    connect(c, (struct sockaddr *)&un, sizeof(un));
    int s = accept(l, 0, 0);
    nonblock(s);
    errno = 0;
    n = read(s, buf, sizeof(buf));
    printf("nbtest: a local socket with nothing there: %d %s\n", (int)n, err(errno));
    close(c);
    close(s);
    close(l);
    unlink(un.sun_path);
}

static void selects(void)
{
    int p[2];
    pipe(p);
    fd_set r, w;
    FD_ZERO(&r);
    FD_SET(p[0], &r);
    struct timeval tv = { 0, 100000 }, t0, t1;
    gettimeofday(&t0, 0);
    int n = select(p[0] + 1, &r, 0, 0, &tv);
    gettimeofday(&t1, 0);
    long ms = (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000);
    printf("nbtest: select on an empty pipe for 100 ms: %d after %s, still set: %s\n", n,
           ms >= 80 && ms < 2000 ? "about that long" : "a wrong time", FD_ISSET(p[0], &r) ? "yes" : "no");
    write(p[1], "x", 1);
    FD_ZERO(&r);
    FD_ZERO(&w);
    FD_SET(p[0], &r);
    FD_SET(p[1], &w);
    FD_SET(p[1], &r);
    n = select(p[1] + 1, &r, &w, 0, 0);
    printf("nbtest: select with a byte in it: %d, read end readable %s, write end writable %s, "
           "write end readable %s\n", n, FD_ISSET(p[0], &r) ? "yes" : "no",
           FD_ISSET(p[1], &w) ? "yes" : "no", FD_ISSET(p[1], &r) ? "yes" : "no");
    close(p[1]);
    char c;
    read(p[0], &c, 1);
    FD_ZERO(&r);
    FD_SET(p[0], &r);
    tv = (struct timeval){ 0, 0 };
    n = select(p[0] + 1, &r, 0, 0, &tv);
    printf("nbtest: the end is readable: %d %s\n", n, FD_ISSET(p[0], &r) ? "yes" : "no");
    close(p[0]);
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = loopback(0);
    bind(l, (struct sockaddr *)&a, sizeof(a));
    listen(l, 2);
    int cl = socket(AF_INET, SOCK_STREAM, 0);
    a = loopback(port_of(l));
    connect(cl, (struct sockaddr *)&a, sizeof(a));
    FD_ZERO(&r);
    FD_SET(l, &r);
    tv = (struct timeval){ 2, 0 };
    n = select(l + 1, &r, 0, 0, &tv);
    printf("nbtest: a listener with a connection is readable: %d %s\n", n, FD_ISSET(l, &r) ? "yes" : "no");
    close(cl);
    close(l);
    FD_ZERO(&r);
    FD_SET(77, &r);
    tv = (struct timeval){ 0, 0 };
    errno = 0;
    n = select(78, &r, 0, 0, &tv);
    /* (POSIX: EBADF; the Mac's select gives 0: compared apart) */
    printf("nbtest-posix: select on a closed descriptor: %d %s\n", n, err(errno));
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: nbtest DIR\n");
        return 2;
    }
    flags(argv[1]);
    tcp();
    others(argv[1]);
    selects();
    printf("nbtest: done\n");
    return 0;
}

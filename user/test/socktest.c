/*
 * Test: BSD sockets (53a). Plain POSIX: the test builds it on the Mac too
 * and compares the "socktest:" lines of both (no ports or times in them).
 * Addresses as text, byte order, TCP over 127.0.0.1 (listen, connect,
 * accept, the names of both ends, a half close with shutdown), UDP
 * (sendto/recvfrom with addresses, a connected UDP socket), local
 * (AF_UNIX) sockets, options (SO_RCVTIMEO ending a wait with EAGAIN,
 * SO_TYPE, SO_REUSEADDR) and errors.
 * AF_INET6 (64d): TCP over ::1 (the names of both ends), an IPv4 client
 * of an IPv6 listener (::ffff:127.0.0.1), IPV6_V6ONLY, UDP over ::1.
 * Usage: socktest DIR (a folder for the local socket's name)
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static const char *err(int e)
{
    switch (e) {
    case 0: return "0";
    case EAGAIN: return "EAGAIN";
    case ECONNREFUSED: return "ECONNREFUSED";
    case EADDRINUSE: return "EADDRINUSE";
    case ENOTCONN: return "ENOTCONN";
    case ENOTSOCK: return "ENOTSOCK";
    case EAFNOSUPPORT: return "EAFNOSUPPORT";
    case EINVAL: return "EINVAL";
    case EBADF: return "EBADF";
    case ENOPROTOOPT: return "ENOPROTOOPT";
    case EPIPE: return "EPIPE";
    default: return "another";
    }
}

static void texts(void)
{
    static const char *const cases[] = { "127.0.0.1", "10.0.2.15", "255.255.255.255", "1.2.3",
                                         "0x7f.1", "256.1.1.1", "1.2.3.4.5", "1..2.3", "", "a.b.c.d",
                                         "1.2.3.4 " };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct in_addr a = { 0 }, b = { 0 };
        int p = inet_pton(AF_INET, cases[i], &a);
        int q = inet_aton(cases[i], &b);
        char text[INET_ADDRSTRLEN] = "-";
        if (p == 1)
            inet_ntop(AF_INET, &a, text, sizeof(text));
        printf("socktest: '%s': pton %d %s, aton %d %s, addr %s\n", cases[i], p, text, q,
               q ? inet_ntoa(b) : "-", inet_addr(cases[i]) == INADDR_NONE ? "none" : "an address");
    }
    /* IPv6 (64a); not the Mac's leniencies, which RFC 4291 and glibc
     * refuse (a "%card", five hex digits, a leading zero in a.b.c.d). */
    static const char *const six[] = { "::", "::1", "1::", "fe80::1", "FE80::ABCD", "1:2:3:4:5:6:7:8",
                                       "1:0:0:2:0:0:0:3", "0:0:1:0:0:0:0:0", "2001:db8::ff00:42:8329",
                                       "2001:0db8:0000:0000:0000:ff00:0042:8329", "::ffff:10.0.2.15",
                                       "::1.2.3.4", "1:2:3:4:5:6:1.2.3.4", "1::2::3", ":::", "1:",
                                       "1:2:3:4:5:6:7:8:9", "::ffff:1.2.3", "1:2:3:4:5:6:7:1.2.3.4",
                                       ":1::", "1:::2", "", "g::1", "1:2:3:4:5:6:7::", "::ffff:0:0" };
    for (size_t i = 0; i < sizeof(six) / sizeof(six[0]); i++) {
        struct in6_addr a6;
        char text[INET6_ADDRSTRLEN] = "-";
        int p = inet_pton(AF_INET6, six[i], &a6);
        if (p == 1)
            inet_ntop(AF_INET6, &a6, text, sizeof(text));
        printf("socktest: IPv6 '%s': pton %d %s\n", six[i], p, text);
    }
    struct in6_addr long6;
    char small6[16];
    inet_pton(AF_INET6, "2001:db8:1:2:3:4:5:6", &long6);
    errno = 0;
    const char *r6 = inet_ntop(AF_INET6, &long6, small6, sizeof(small6));
    printf("socktest: IPv6 ntop into 16 bytes: %s, %s\n", r6 ? r6 : "null", err(errno));
    char small[8];
    struct in_addr a;
    inet_pton(AF_INET, "192.168.100.200", &a);
    errno = 0;
    const char *r = inet_ntop(AF_INET, &a, small, sizeof(small));
    printf("socktest: ntop into 8 bytes: %s, %s\n", r ? r : "null", err(errno) );
    printf("socktest: htons(0x1234) 0x%04x, htonl(0x01020304) 0x%08x, ntohs(htons(80)) %u\n",
           htons(0x1234), (unsigned)htonl(0x01020304), ntohs(htons(80)));
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
    if (getsockname(fd, (struct sockaddr *)&a, &n) < 0)
        return 0;
    return ntohs(a.sin_port);
}

static void tcp(void)
{
    int l = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = loopback(0);
    int r = bind(l, (struct sockaddr *)&a, sizeof(a));
    unsigned short port = port_of(l);
    printf("socktest: tcp bind %d, a port chosen: %s, listen %d\n", r, port ? "yes" : "no",
           listen(l, 4));
    int c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    a = loopback(port);
    printf("socktest: tcp connect %d\n", connect(c, (struct sockaddr *)&a, sizeof(a)));
    struct sockaddr_in peer;
    socklen_t n = sizeof(peer);
    int s = accept(l, (struct sockaddr *)&peer, &n);
    char text[INET_ADDRSTRLEN];
    printf("socktest: accepted: %s, from %s, length %d, the client's own port: %s\n",
           s >= 0 ? "yes" : "no", inet_ntop(AF_INET, &peer.sin_addr, text, sizeof(text)), (int)n,
           ntohs(peer.sin_port) == port_of(c) ? "yes" : "no");
    n = sizeof(peer);
    getpeername(c, (struct sockaddr *)&peer, &n);
    printf("socktest: the client's peer: %s, the listener's port: %s\n",
           inet_ntop(AF_INET, &peer.sin_addr, text, sizeof(text)),
           ntohs(peer.sin_port) == port ? "yes" : "no");
    n = sizeof(peer);
    getsockname(s, (struct sockaddr *)&peer, &n);
    printf("socktest: the server end's own address %s, port the listener's: %s\n",
           inet_ntop(AF_INET, &peer.sin_addr, text, sizeof(text)),
           ntohs(peer.sin_port) == port ? "yes" : "no");
    char buf[32] = "";
    ssize_t k = send(c, "hello", 5, 0);
    ssize_t m = recv(s, buf, 5, MSG_WAITALL);
    printf("socktest: sent %d, received %d '%.*s'\n", (int)k, (int)m, (int)(m > 0 ? m : 0), buf);
    printf("socktest: client shutdown(SHUT_WR) %d\n", shutdown(c, SHUT_WR));
    m = recv(s, buf, sizeof(buf), 0);
    printf("socktest: the server reads the end: %d\n", (int)m);
    k = send(s, "bye", 3, 0);
    m = recv(c, buf, 3, MSG_WAITALL);
    printf("socktest: the half-closed client still reads: %d '%.*s'\n", (int)m,
           (int)(m > 0 ? m : 0), buf);
    int type = 0;
    n = sizeof(type);
    getsockopt(c, SOL_SOCKET, SO_TYPE, &type, &n);
    int nodelay = 1;
    printf("socktest: SO_TYPE %s, TCP_NODELAY %d\n", type == SOCK_STREAM ? "stream" : "other",
           setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)));
    close(c);
    close(s);

    int l2 = socket(AF_INET, SOCK_STREAM, 0);
    a = loopback(port);
    errno = 0;
    r = bind(l2, (struct sockaddr *)&a, sizeof(a));
    printf("socktest: a second listener on the port: bind %d %s\n", r, err(errno));
    close(l2);
    close(l);
    int c2 = socket(AF_INET, SOCK_STREAM, 0);
    a = loopback(port);
    errno = 0;
    r = connect(c2, (struct sockaddr *)&a, sizeof(a));
    printf("socktest: connect to a closed port: %d %s\n", r, err(errno));
    close(c2);
}

static void udp(void)
{
    int a = socket(AF_INET, SOCK_DGRAM, 0), b = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in x = loopback(0);
    bind(a, (struct sockaddr *)&x, sizeof(x));
    bind(b, (struct sockaddr *)&x, sizeof(x));
    struct sockaddr_in to = loopback(port_of(b)), from;
    ssize_t k = sendto(a, "datagram", 8, 0, (struct sockaddr *)&to, sizeof(to));
    char buf[32];
    socklen_t n = sizeof(from);
    ssize_t m = recvfrom(b, buf, sizeof(buf), 0, (struct sockaddr *)&from, &n);
    char text[INET_ADDRSTRLEN];
    printf("socktest: udp sent %d, received %d '%.*s' from %s, the sender's port: %s\n", (int)k,
           (int)m, (int)(m > 0 ? m : 0), buf, inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text)),
           ntohs(from.sin_port) == port_of(a) ? "yes" : "no");
    errno = 0;
    n = sizeof(from);
    int r = getpeername(a, (struct sockaddr *)&from, &n);
    printf("socktest: an unconnected udp socket's peer: %d %s\n", r, err(errno));
    to = loopback(port_of(b));
    printf("socktest: udp connect %d\n", connect(a, (struct sockaddr *)&to, sizeof(to)));
    k = send(a, "again", 5, 0);
    m = recv(b, buf, sizeof(buf), 0);
    printf("socktest: connected udp sent %d, received %d '%.*s'\n", (int)k, (int)m,
           (int)(m > 0 ? m : 0), buf);
    struct timeval tv = { 0, 200000 }, got = { 0, 0 };
    r = setsockopt(b, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    n = sizeof(got);
    getsockopt(b, SOL_SOCKET, SO_RCVTIMEO, &got, &n);
    struct timeval t0, t1;
    gettimeofday(&t0, 0);
    errno = 0;
    m = recv(b, buf, sizeof(buf), 0);
    int e = errno;
    gettimeofday(&t1, 0);
    long ms = (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000);
    printf("socktest: SO_RCVTIMEO %d (%ld.%06ld s): recv %d %s after %s\n", r, (long)got.tv_sec,
           (long)got.tv_usec, (int)m, err(e), ms >= 150 && ms < 2000 ? "about that long" : "a wrong time");
    int type = 0;
    n = sizeof(type);
    getsockopt(b, SOL_SOCKET, SO_TYPE, &type, &n);
    printf("socktest: udp SO_TYPE %s\n", type == SOCK_DGRAM ? "datagram" : "other");
    close(a);
    close(b);
}

static void local(const char *dir)
{
    struct sockaddr_un u, got;
    memset(&u, 0, sizeof(u));
    u.sun_family = AF_UNIX;
    snprintf(u.sun_path, sizeof(u.sun_path), "%s/socktest.sock", dir);
    unlink(u.sun_path);                         /* (the Mac makes a file of it) */
    int l = socket(AF_UNIX, SOCK_STREAM, 0);
    int r = bind(l, (struct sockaddr *)&u, sizeof(u));
    printf("socktest: local bind %d, listen %d\n", r, listen(l, 2));
    socklen_t n = sizeof(got);
    getsockname(l, (struct sockaddr *)&got, &n);
    printf("socktest: the listener's name is the path: %s\n", strcmp(got.sun_path, u.sun_path) ? "no" : "yes");
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    printf("socktest: local connect %d\n", connect(c, (struct sockaddr *)&u, sizeof(u)));
    int s = accept(l, 0, 0);
    n = sizeof(got);
    memset(&got, 0, sizeof(got));
    getpeername(c, (struct sockaddr *)&got, &n);
    printf("socktest: the client's peer is the path: %s, family AF_UNIX: %s\n",
           strcmp(got.sun_path, u.sun_path) ? "no" : "yes", got.sun_family == AF_UNIX ? "yes" : "no");
    char buf[16];
    ssize_t k = send(c, "local", 5, 0);
    ssize_t m = recv(s, buf, sizeof(buf), 0);
    printf("socktest: local sent %d, received %d '%.*s'\n", (int)k, (int)m, (int)(m > 0 ? m : 0), buf);
    shutdown(c, SHUT_WR);
    m = recv(s, buf, sizeof(buf), 0);
    printf("socktest: after the client's shutdown the server reads %d\n", (int)m);
    close(c);
    close(s);
    close(l);
    unlink(u.sun_path);
}

static void errors(const char *dir)
{
    int t = socket(AF_INET, SOCK_STREAM, 0);
    errno = 0;
    int r = shutdown(t, SHUT_RDWR);
    printf("socktest: shutdown unconnected: %d %s\n", r, err(errno));
    errno = 0;
    r = getsockopt(t, SOL_SOCKET, 12345, &r, &(socklen_t){ sizeof(int) });
    printf("socktest: an unknown option: %d %s\n", r, err(errno));
    close(t);
    char path[200];
    snprintf(path, sizeof(path), "%s/socktest.file", dir);
    int f = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    struct sockaddr_in a = loopback(80);
    errno = 0;
    r = connect(f, (struct sockaddr *)&a, sizeof(a));
    printf("socktest: connect on a file: %d %s\n", r, err(errno));
    close(f);
    unlink(path);
    errno = 0;
    r = bind(77, (struct sockaddr *)&a, sizeof(a));
    printf("socktest: bind on a closed descriptor: %d %s\n", r, err(errno));
}

/* An AF_INET6 address as text: "::1 port P?" without the port (random). */
static const char *six_text(const struct sockaddr_in6 *a, char *out, size_t size)
{
    char t[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &a->sin6_addr, t, sizeof(t));
    snprintf(out, size, "%s (family %s)", t, a->sin6_family == AF_INET6 ? "AF_INET6" : "another");
    return out;
}

static void six(void)
{
    /* TCP over ::1: an IPv6 listener on any address, its names. */
    int l = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_any;
    int r = bind(l, (struct sockaddr *)&a, sizeof(a));
    listen(l, 4);
    socklen_t n = sizeof(a);
    getsockname(l, (struct sockaddr *)&a, &n);
    char t[96], u[96];
    printf("socktest: six: listener %d, bind %d, its name %s, length %d, a port %s\n", l >= 0, r,
           six_text(&a, t, sizeof(t)), (int)n, a.sin6_port ? "yes" : "no");
    unsigned short port = a.sin6_port;
    int c = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 to;
    memset(&to, 0, sizeof(to));
    to.sin6_family = AF_INET6;
    to.sin6_port = port;
    to.sin6_addr = in6addr_loopback;
    r = connect(c, (struct sockaddr *)&to, sizeof(to));
    struct sockaddr_in6 peer;
    n = sizeof(peer);
    int s = accept(l, (struct sockaddr *)&peer, &n);
    printf("socktest: six: connect %d, accept %s from %s\n", r, s >= 0 ? "yes" : "no",
           six_text(&peer, t, sizeof(t)));
    struct sockaddr_in6 own, other;
    n = sizeof(own);
    getsockname(c, (struct sockaddr *)&own, &n);
    socklen_t m = sizeof(other);
    getpeername(c, (struct sockaddr *)&other, &m);
    printf("socktest: six: the client's own %s, its peer %s, the listener's port %s\n",
           six_text(&own, t, sizeof(t)), six_text(&other, u, sizeof(u)),
           other.sin6_port == port ? "yes" : "no");
    write(c, "over six", 8);
    char buffer[32] = "";
    ssize_t got = recv(s, buffer, sizeof(buffer), 0);
    printf("socktest: six: %d bytes '%.*s'\n", (int)got, got > 0 ? (int)got : 0, buffer);
    close(c);
    close(s);
    /* An IPv4 client of the IPv6 listener. */
    c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in four;
    memset(&four, 0, sizeof(four));
    four.sin_family = AF_INET;
    four.sin_port = port;
    inet_pton(AF_INET, "127.0.0.1", &four.sin_addr);
    r = connect(c, (struct sockaddr *)&four, sizeof(four));
    n = sizeof(peer);
    s = accept(l, (struct sockaddr *)&peer, &n);
    printf("socktest: six: an IPv4 client: connect %d, from %s, IN6_IS_ADDR_V4MAPPED %d\n", r,
           six_text(&peer, t, sizeof(t)), IN6_IS_ADDR_V4MAPPED(&peer.sin6_addr) ? 1 : 0);
    close(c);
    close(s);
    close(l);
    /* IPV6_V6ONLY: such a listener takes no IPv4 client. */
    l = socket(AF_INET6, SOCK_STREAM, 0);
    int on = 1, value = -1;
    socklen_t size = sizeof(value);
    r = setsockopt(l, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
    getsockopt(l, IPPROTO_IPV6, IPV6_V6ONLY, &value, &size);
    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    bind(l, (struct sockaddr *)&a, sizeof(a));
    listen(l, 4);
    n = sizeof(a);
    getsockname(l, (struct sockaddr *)&a, &n);
    c = socket(AF_INET, SOCK_STREAM, 0);
    four.sin_port = a.sin6_port;
    errno = 0;
    int refused = connect(c, (struct sockaddr *)&four, sizeof(four));
    printf("socktest: six: IPV6_V6ONLY set %d, reads %d, an IPv4 client %d %s\n", r, value != 0,
           refused, err(errno));
    close(c);
    close(l);
    /* UDP over ::1: sendto, recvfrom with the sender's address. */
    int x = socket(AF_INET6, SOCK_DGRAM, 0), y = socket(AF_INET6, SOCK_DGRAM, 0);
    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    bind(x, (struct sockaddr *)&a, sizeof(a));
    n = sizeof(a);
    getsockname(x, (struct sockaddr *)&a, &n);
    to.sin6_port = a.sin6_port;
    ssize_t w = sendto(y, "datagram six", 12, 0, (struct sockaddr *)&to, sizeof(to));
    struct sockaddr_in6 from;
    n = sizeof(from);
    struct timeval wait = { 2, 0 };
    setsockopt(x, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
    got = recvfrom(x, buffer, sizeof(buffer), 0, (struct sockaddr *)&from, &n);
    n = sizeof(own);
    getsockname(y, (struct sockaddr *)&own, &n);
    printf("socktest: six: udp sent %d, got %d '%.*s' from %s, the sender's port %s\n", (int)w,
           (int)got, got > 0 ? (int)got : 0, buffer, six_text(&from, t, sizeof(t)),
           from.sin6_port == own.sin6_port ? "yes" : "no");
    close(x);
    close(y);
    printf("socktest: six: sizeof(struct sockaddr_in6) %d\n", (int)sizeof(struct sockaddr_in6));
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: socktest DIR\n");
        return 2;
    }
    texts();
    tcp();
    udp();
    six();
    local(argv[1]);
    errors(argv[1]);
    printf("socktest: done\n");
    return 0;
}

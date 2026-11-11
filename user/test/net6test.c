/*
 * Test: UDP and TCP over IPv6 (64c), through the system calls themselves
 * (the BSD socket layer's AF_INET6 comes in 64d).
 *   net6test loop               on this machine: TCP over ::1 (300,000 bytes
 *                               there and back, the names of both ends), an
 *                               IPv4 client of an IPv6 listener, UDP over ::1,
 *                               an IPv4 socket refusing IPv6, the IPv4 calls on
 *                               an IPv6 socket
 *   net6test get ADDRESS PORT   TCP: connect, take everything until the end;
 *                               the bytes must be i % 251
 *   net6test send ADDRESS PORT N  TCP: connect, send N bytes (i % 251), close
 *   net6test udp ADDRESS PORT TEXT  one datagram, the answer within 2 s
 *   net6test udpserve PORT      UDP: answer every datagram "got N bytes, SUM"
 *   net6test banner ADDRESS PORT  (64d) TCP: the first line the server sends
 *   net6test dns ADDRESS NAME   (64d) one DNS question for NAME's A record over
 *                               UDP to ADDRESS port 53, and its answer
 *   net6test burst N NAME       (64j) N threads look NAME up at once (64q: %d in
 *                               NAME: each its own name, its number there)
 *                               (getaddrinfo, through dnsd): how many answered,
 *                               and why the others did not
 * ADDRESS is IPv6 (ADDRESS%CARD for link-local) or IPv4. Lines start with
 * "net6test:".
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <threads.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("net6test loop|get|send|udp|udpserve|banner|dns|burst ...", "test: UDP and TCP over IPv6 (64c, 64d, 64j)");

static const char *error_name(int64_t r)
{
    switch (-r) {
    case ERR_AFNOSUPPORT: return "EAFNOSUPPORT";
    case ERR_CONNREFUSED: return "ECONNREFUSED";
    case ERR_NETUNREACH: return "ENETUNREACH";
    case ERR_HOSTUNREACH: return "EHOSTUNREACH";
    case ERR_TIMEDOUT: return "ETIMEDOUT";
    case ERR_INVAL: return "EINVAL";
    case ERR_NOTCONN: return "ENOTCONN";
    default: return "another";
    }
}

static int64_t sys(int64_t n, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f)
{
    return syscall6(n, a, b, c, d, e, f);
}

/* "ADDRESS", "ADDRESS%CARD" or a.b.c.d with a port, as the kernel takes it. */
static int address(const char *text, uint16_t port, struct sys_inaddr *out)
{
    memset(out, 0, sizeof(*out));
    out->port = port;
    char a[INET6_ADDRSTRLEN];
    const char *card = strchr(text, '%');
    size_t n = card ? (size_t)(card - text) : strlen(text);
    if (n >= sizeof(a))
        return -1;
    memcpy(a, text, n);
    a[n] = 0;
    uint8_t v4[4];
    if (inet_pton(AF_INET, a, v4) == 1) {
        out->family = 2;
        out->address[10] = out->address[11] = 0xFF;
        memcpy(out->address + 12, v4, 4);
        return 0;
    }
    if (inet_pton(AF_INET6, a, out->address) != 1)
        return -1;
    out->family = 10;
    if (card) {
        struct sys_netinfo info;
        for (int i = 0; ritteros_netcard(i, &info) == 0; i++)
            if (strcmp(info.name, card + 1) == 0)
                out->scope = (uint32_t)i + 1;
        if (!out->scope)
            return -1;
    }
    return 0;
}

/* The address alone, and its family. */
static const char *text_of(const struct sys_inaddr *a, char *out, size_t size)
{
    char t[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, a->address, t, sizeof(t));
    snprintf(out, size, "%s (family %u)", t, a->family);
    return out;
}

/* --- loop --- */

static int listener;
static char accepted[96];               /* who the server took: printed after its join */

static int serve_one(void *arg)
{
    (void)arg;
    struct sys_inaddr from;
    int64_t c = sys(SYS_ACCEPT6, listener, (int64_t)&from, 0, 0, 0, 0);
    if (c < 0)
        return 1;
    text_of(&from, accepted, sizeof(accepted));
    static uint8_t buffer[16384];
    int64_t n;
    while ((n = read((int)c, buffer, sizeof(buffer))) > 0)
        for (int64_t done = 0; done < n;) {
            int64_t w = write((int)c, buffer + done, (size_t)(n - done));
            if (w <= 0)
                break;
            done += w;
        }
    close((int)c);
    return 0;
}

static int loop(void)
{
    /* TCP over ::1: an IPv6 listener, an echo of 300,000 bytes. */
    listener = (int)sys(SYS_SOCKET, SOCKET_TCP | SOCKET_INET6, 0, 0, 0, 0, 0);
    int64_t port = sys(SYS_BIND, listener, 6000, 0, 0, 0, 0);
    sys(SYS_LISTEN, listener, 4, 0, 0, 0, 0);
    struct sys_inaddr own;
    char t[96];
    sys(SYS_SOCKNAME6, listener, SOCKNAME_OWN, (int64_t)&own, 0, 0, 0);
    printf("net6test: listening on %s port %u (bind %ld)\n", text_of(&own, t, sizeof(t)), own.port,
           (long)port);
    thrd_t server;
    thrd_create(&server, serve_one, 0);
    int c = (int)sys(SYS_SOCKET, SOCKET_TCP | SOCKET_INET6, 0, 0, 0, 0, 0);
    struct sys_inaddr to;
    address("::1", 6000, &to);
    int64_t r = sys(SYS_CONNECT6, c, (int64_t)&to, 0, 0, 0, 0);
    printf("net6test: connect [::1]:6000: %ld\n", (long)r);
    struct sys_inaddr peer;
    sys(SYS_SOCKNAME6, c, SOCKNAME_PEER, (int64_t)&peer, 0, 0, 0);
    sys(SYS_SOCKNAME6, c, SOCKNAME_OWN, (int64_t)&own, 0, 0, 0);
    char u[96];
    printf("net6test: peer %s port %u, own %s\n", text_of(&peer, t, sizeof(t)), peer.port,
           text_of(&own, u, sizeof(u)));
    static uint8_t out[300000], back[300000];
    for (size_t i = 0; i < sizeof(out); i++)
        out[i] = (uint8_t)(i % 251);
    size_t sent = 0, got = 0;
    while (got < sizeof(back)) {
        if (sent < sizeof(out)) {
            int64_t w = write(c, out + sent, sizeof(out) - sent > 8192 ? 8192 : sizeof(out) - sent);
            if (w > 0)
                sent += (size_t)w;
        }
        int64_t n = read(c, back + got, sizeof(back) - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    printf("net6test: tcp ::1: %zu sent, %zu back, %s\n", sent, got,
           got == sizeof(out) && !memcmp(out, back, got) ? "the same" : "DIFFERENT");
    close(c);
    thrd_join(server, 0);
    printf("net6test: accepted from %s\n", accepted);
    /* An IPv4 client (the IPv4 calls) of the IPv6 listener: ::ffff:127.0.0.1. */
    thrd_create(&server, serve_one, 0);
    c = (int)sys(SYS_SOCKET, SOCKET_TCP, 0, 0, 0, 0, 0);
    r = sys(SYS_CONNECT, c, 0x7F000001, 6000, 0, 0, 0);
    printf("net6test: an IPv4 client of the IPv6 listener: %ld\n", (long)r);
    write(c, "four", 4);
    char four[4];
    printf("net6test: its echo: %ld\n", (long)read(c, four, 4));
    close(c);
    thrd_join(server, 0);
    printf("net6test: accepted from %s\n", accepted);
    close(listener);
    /* UDP over ::1. */
    int a = (int)sys(SYS_SOCKET, SOCKET_UDP | SOCKET_INET6, 0, 0, 0, 0, 0);
    int b = (int)sys(SYS_SOCKET, SOCKET_UDP | SOCKET_INET6, 0, 0, 0, 0, 0);
    sys(SYS_BIND, a, 6001, 0, 0, 0, 0);
    address("::1", 6001, &to);
    r = sys(SYS_SENDTO6, b, (int64_t)"hello six", 9, (int64_t)&to, 0, 0);
    char buffer[64];
    struct sys_inaddr from;
    int64_t n = sys(SYS_RECVFROM6, a, (int64_t)buffer, sizeof(buffer), (int64_t)&from, 2000, 0);
    struct sys_inaddr bname;
    sys(SYS_SOCKNAME6, b, SOCKNAME_OWN, (int64_t)&bname, 0, 0, 0);
    printf("net6test: udp ::1: sent %ld, got %ld '%.*s' from %s, the sender's port %s\n", (long)r,
           (long)n, n > 0 ? (int)n : 0, buffer, text_of(&from, t, sizeof(t)),
           from.port == bname.port ? "yes" : "no");
    /* The IPv4 calls on an IPv6 socket: an IPv6 sender's address is 0. */
    sys(SYS_SENDTO6, b, (int64_t)"again", 5, (int64_t)&to, 0, 0);
    uint64_t old = 1;
    n = sys(SYS_RECVFROM, a, (int64_t)buffer, sizeof(buffer), (int64_t)&old, 2000, 0);
    printf("net6test: the IPv4 recvfrom: %ld bytes, address %lu\n", (long)n,
           (unsigned long)(old >> 16));
    close(a);
    close(b);
    /* An IPv4 socket takes no IPv6 address. */
    int v4 = (int)sys(SYS_SOCKET, SOCKET_UDP, 0, 0, 0, 0, 0);
    r = sys(SYS_CONNECT6, v4, (int64_t)&to, 0, 0, 0, 0);
    printf("net6test: an IPv4 socket to ::1: %s\n", r < 0 ? error_name(r) : "accepted");
    close(v4);
    v4 = (int)sys(SYS_SOCKET, SOCKET_TCP, 0, 0, 0, 0, 0);
    r = sys(SYS_CONNECT6, v4, (int64_t)&to, 0, 0, 0, 0);
    printf("net6test: an IPv4 TCP socket to ::1: %s\n", r < 0 ? error_name(r) : "accepted");
    close(v4);
    /* Nobody on the port: refused. */
    c = (int)sys(SYS_SOCKET, SOCKET_TCP | SOCKET_INET6, 0, 0, 0, 0, 0);
    address("::1", 6999, &to);
    r = sys(SYS_CONNECT6, c, (int64_t)&to, 0, 0, 0, 0);
    printf("net6test: [::1]:6999: %s\n", r < 0 ? error_name(r) : "connected");
    close(c);
    return 0;
}

/* --- to and from other machines --- */

static int connect_to(const char *text, const char *port_text)
{
    struct sys_inaddr to;
    if (address(text, (uint16_t)atoi(port_text), &to) < 0) {
        printf("net6test: %s: not an address\n", text);
        return -1;
    }
    int c = (int)sys(SYS_SOCKET, SOCKET_TCP | SOCKET_INET6, 0, 0, 0, 0, 0);
    int64_t r = sys(SYS_CONNECT6, c, (int64_t)&to, 0, 0, 0, 0);
    if (r < 0) {
        printf("net6test: connect %s port %s: %s\n", text, port_text, error_name(r));
        close(c);
        return -1;
    }
    return c;
}

static int get(const char *text, const char *port)
{
    int c = connect_to(text, port);
    if (c < 0)
        return 1;
    static uint8_t buffer[16384];
    size_t total = 0;
    bool right = true;
    int64_t n;
    while ((n = read(c, buffer, sizeof(buffer))) > 0) {
        for (int64_t i = 0; i < n; i++)
            right &= buffer[i] == (uint8_t)((total + (size_t)i) % 251);
        total += (size_t)n;
    }
    printf("net6test: got %zu bytes, %s\n", total, right ? "as sent" : "NOT AS SENT");
    close(c);
    return 0;
}

static int send_n(const char *text, const char *port, const char *count)
{
    int c = connect_to(text, port);
    if (c < 0)
        return 1;
    size_t total = (size_t)atol(count), done = 0;
    static uint8_t buffer[16384];
    while (done < total) {
        size_t n = total - done < sizeof(buffer) ? total - done : sizeof(buffer);
        for (size_t i = 0; i < n; i++)
            buffer[i] = (uint8_t)((done + i) % 251);
        int64_t w = write(c, buffer, n);
        if (w <= 0)
            break;
        done += (size_t)w;
    }
    char end[8];
    sys(SYS_SHUTDOWN, c, SHUTDOWN_WRITE, 0, 0, 0, 0);
    read(c, end, sizeof(end));                      /* until the other side closes */
    printf("net6test: sent %zu bytes\n", done);
    close(c);
    return 0;
}

static int udp(const char *text, const char *port, const char *message)
{
    struct sys_inaddr to, from;
    if (address(text, (uint16_t)atoi(port), &to) < 0)
        return 1;
    int s = (int)sys(SYS_SOCKET, SOCKET_UDP | SOCKET_INET6, 0, 0, 0, 0, 0);
    int64_t r = sys(SYS_SENDTO6, s, (int64_t)message, (int64_t)strlen(message), (int64_t)&to, 0, 0);
    if (r < 0) {
        printf("net6test: udp %s: %s\n", text, error_name(r));
        return 1;
    }
    char buffer[1500];
    int64_t n = sys(SYS_RECVFROM6, s, (int64_t)buffer, sizeof(buffer), (int64_t)&from, 2000, 0);
    char t[96];
    if (n < 0)
        printf("net6test: udp %s: no answer (%s)\n", text, error_name(n));
    else
        printf("net6test: udp answer '%.*s' from %s port %u\n", (int)n, buffer,
               text_of(&from, t, sizeof(t)), from.port);
    close(s);
    return 0;
}

static int udpserve(const char *port)
{
    int s = (int)sys(SYS_SOCKET, SOCKET_UDP | SOCKET_INET6, 0, 0, 0, 0, 0);
    if (sys(SYS_BIND, s, atoi(port), 0, 0, 0, 0) < 0)
        return 1;
    static uint8_t buffer[9000];
    for (;;) {
        struct sys_inaddr from;
        int64_t n = sys(SYS_RECVFROM6, s, (int64_t)buffer, sizeof(buffer), (int64_t)&from, 0, 0);
        if (n < 0)
            return 1;
        uint32_t sum = 0;
        for (int64_t i = 0; i < n; i++)
            sum = sum * 31 + buffer[i];
        char answer[64];
        int k = snprintf(answer, sizeof(answer), "got %ld bytes, %08x", (long)n, sum);
        sys(SYS_SENDTO6, s, (int64_t)answer, k, (int64_t)&from, 0, 0);
    }
}

/* (64d) A connect that does not wait, then poll for its answer for at
 * most 5 s (iBrowser's way): what came of it. Through the BSD calls. */
static int nonblocking(const char *host, const char *port)
{
    struct addrinfo hints, *list;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    int r = getaddrinfo(host, port, &hints, &list);
    if (r) {
        printf("net6test: nb %s: %s\n", host, gai_strerror(r));
        return 1;
    }
    int fd = socket(list->ai_family, list->ai_socktype, list->ai_protocol);
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    errno = 0;
    r = connect(fd, list->ai_addr, list->ai_addrlen);
    int e = errno;
    struct pollfd p = { .fd = fd, .events = POLLOUT };
    uint64_t start = ritteros_time();
    int ready = poll(&p, 1, 5000);
    uint64_t took = (ritteros_time() - start) / 1000000;
    printf("net6test: nb %s: connect %d (%s), poll %d after %s, revents %#x\n", host, r,
           e == EINPROGRESS ? "EINPROGRESS" : e ? "another" : "0", ready,
           took >= 4900 ? "about 5 s" : took < 1000 ? "under 1 s" : "1-5 s", p.revents);
    close(fd);
    freeaddrinfo(list);
    return 0;
}

static int banner(const char *text, const char *port)
{
    int c = connect_to(text, port);
    if (c < 0)
        return 1;
    char line[128];
    size_t n = 0;
    while (n + 1 < sizeof(line) && read(c, line + n, 1) == 1 && line[n] != '\n')
        n++;
    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n'))
        n--;
    printf("net6test: %s says '%.*s'\n", text, (int)n, line);
    close(c);
    return 0;
}

static int dns(const char *text, const char *name)
{
    struct sys_inaddr to, from;
    if (address(text, 53, &to) < 0)
        return 1;
    uint8_t q[512], a[512];
    size_t length = dns_query(name, 0x6464, q, sizeof(q));
    int s = (int)sys(SYS_SOCKET, SOCKET_UDP | SOCKET_INET6, 0, 0, 0, 0, 0);
    sys(SYS_SENDTO6, s, (int64_t)q, (int64_t)length, (int64_t)&to, 0, 0);
    int64_t n = sys(SYS_RECVFROM6, s, (int64_t)a, sizeof(a), (int64_t)&from, 3000, 0);
    uint32_t ip = 0, ttl;
    int r = n > 0 ? dns_answer(a, (size_t)n, &ip, &ttl) : -1;
    char t[96];
    if (r)
        printf("net6test: dns %s: no answer\n", name);
    else
        printf("net6test: dns %s: %u.%u.%u.%u, from %s port %u\n", name, ip >> 24, ip >> 16 & 255,
               ip >> 8 & 255, ip & 255, text_of(&from, t, sizeof(t)), from.port);
    close(s);
    return 0;
}

#define BURST_MAX 64

struct lookup {
    const char *name;
    char own[256];                      /* (64q) NAME with %d: this thread's number in it */
    int result;                         /* getaddrinfo's: 0 or EAI_* */
};

static int look_up(void *arg)
{
    struct lookup *l = arg;
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *list = 0;
    l->result = getaddrinfo(l->name, "80", &hints, &list);
    if (!l->result)
        freeaddrinfo(list);
    return 0;
}

/* An IPv6 address beyond the link in use on some card (what makes a
 * lookup ask for AAAA records first, as getaddrinfo's own test). */
static bool global_ipv6(void)
{
    struct sys_netinfo6 info;
    for (int i = 0;; i++) {
        memset(&info, 0, sizeof(info));
        info.index = (uint16_t)i;
        if (ritteros_netinfo6(&info) < 0)
            return false;
        for (uint32_t k = 0; k < info.count && k < 4; k++)
            if (info.addresses[k].state == 2 && !(info.addresses[k].address[0] == 0xFE &&
                                                  (info.addresses[k].address[1] & 0xC0) == 0x80))
                return true;
    }
}

/* (64j) n lookups of name at the same moment, each on a thread: dnsd gets
 * their questions (AAAA and A each) in one burst. (64k) After waiting up
 * to 10 s for an IPv6 address beyond the link, so that they do ask AAAA
 * (on ARM the router's address came after the first bursts). (64q) A
 * NAME with %d in it is a name for each thread, its number there. */
static int burst(const char *count, const char *name)
{
    int n = atoi(count);
    if (n < 1 || n > BURST_MAX)
        return 2;
    int waited = 0;
    while (!global_ipv6() && waited < 100) {
        usleep(100000);
        waited++;
    }
    printf("net6test: burst: an IPv6 address beyond the link: %s\n", global_ipv6() ? "yes" : "no");
    static struct lookup l[BURST_MAX];
    static thrd_t t[BURST_MAX];
    bool started[BURST_MAX] = { false };
    for (int i = 0; i < n; i++) {
        l[i].name = name;
        if (strstr(name, "%d")) {
            snprintf(l[i].own, sizeof(l[i].own), name, i);
            l[i].name = l[i].own;
        }
        started[i] = thrd_create(&t[i], look_up, &l[i]) == thrd_success;
    }
    int answered = 0, failed = 0;
    for (int i = 0; i < n; i++) {
        if (started[i])
            thrd_join(t[i], 0);
        if (started[i] && !l[i].result) {
            answered++;
        } else {
            failed++;
            printf("net6test: burst: lookup %d: %s\n", i, started[i] ? gai_strerror(l[i].result)
                                                                   : "no thread");
        }
    }
    printf("net6test: burst: %d lookups of %s at once: %d answered, %d failed\n", n, name, answered,
           failed);
    return failed != 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, 0, _IONBF, 0);
    if (argc == 2 && strcmp(argv[1], "loop") == 0)
        return loop();
    if (argc == 4 && strcmp(argv[1], "get") == 0)
        return get(argv[2], argv[3]);
    if (argc == 5 && strcmp(argv[1], "send") == 0)
        return send_n(argv[2], argv[3], argv[4]);
    if (argc == 5 && strcmp(argv[1], "udp") == 0)
        return udp(argv[2], argv[3], argv[4]);
    if (argc == 3 && strcmp(argv[1], "udpserve") == 0)
        return udpserve(argv[2]);
    if (argc == 4 && strcmp(argv[1], "nb") == 0)
        return nonblocking(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "banner") == 0)
        return banner(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "dns") == 0)
        return dns(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "burst") == 0)
        return burst(argv[2], argv[3]);
    fprintf(stderr, "usage: net6test loop|get|send|udp|udpserve|banner|dns|burst ...\n");
    return 2;
}

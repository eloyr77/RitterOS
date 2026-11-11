/*
 * The network for programs: IPv4 addresses as text, and the system calls.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

static int64_t check(int64_t r)
{
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

/* A decimal number of at most max at *s; false if there is none. */
static bool number(const char **s, uint32_t max, uint32_t *out)
{
    uint32_t n = 0;
    const char *p = *s;
    if (*p < '0' || *p > '9')
        return false;
    while (*p >= '0' && *p <= '9') {
        n = n * 10 + (uint32_t)(*p++ - '0');
        if (n > max)
            return false;
    }
    *s = p;
    *out = n;
    return true;
}

bool ip_parse(const char *text, uint32_t *ip, uint32_t *prefix)
{
    uint32_t value = 0, part;
    const char *s = text;
    for (int i = 0; i < 4; i++) {
        if (!number(&s, 255, &part) || (i < 3 && *s++ != '.'))
            return false;
        value = value << 8 | part;
    }
    if (*s == '/' && prefix) {
        s++;
        if (!number(&s, 32, prefix))
            return false;
    } else if (prefix) {
        *prefix = 0;
    }
    if (*s)
        return false;
    *ip = value;
    return true;
}

void ip_format(uint32_t ip, char *out, size_t size)
{
    snprintf(out, size, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
}

int ritteros_netset(const struct sys_netset *settings)
{
    return (int)check(syscall6(SYS_NETSET, (int64_t)settings, 0, 0, 0, 0, 0));
}

int ritteros_netset6(const struct sys_netset6 *settings)
{
    return (int)check(syscall6(SYS_NETSET6, (int64_t)settings, 0, 0, 0, 0, 0));
}

int ritteros_netlease6(const struct sys_netlease6 *lease)
{
    return (int)check(syscall6(SYS_NETLEASE6, (int64_t)lease, 0, 0, 0, 0, 0));
}

int ritteros_netinfo6(struct sys_netinfo6 *info)
{
    return (int)check(syscall6(SYS_NETINFO6, (int64_t)info, 0, 0, 0, 0, 0));
}

int ritteros_netinfo(struct sys_netinfo *info)
{
    return (int)check(syscall6(SYS_NETINFO, (int64_t)info, 0, 0, 0, 0, 0));
}

int ritteros_peer_uid(int fd)
{
    return (int)check(syscall6(SYS_PEER_UID, fd, 0, 0, 0, 0, 0));
}

int ritteros_netcard(int index, struct sys_netinfo *info)
{
    if (index < 0 || index > 0xFFFF) {
        errno = ENODEV;
        return -1;
    }
    memset(info, 0, sizeof(*info));
    info->index = (uint16_t)index;
    return ritteros_netinfo(info);
}

int64_t ritteros_wlan(int op, const char *card, void *buffer, size_t length, uint64_t value)
{
    return check(syscall6(SYS_WLAN, op, (int64_t)card, (int64_t)buffer, (int64_t)length,
                          (int64_t)value, 0));
}

int ritteros_bindcard(int fd, const char *card)
{
    char name[8] = { 0 };
    for (int i = 0; i < 7 && card[i]; i++)
        name[i] = card[i];
    return (int)check(syscall6(SYS_BINDCARD, fd, (int64_t)name, 0, 0, 0, 0));
}

int ritteros_forward(int mode)
{
    return (int)check(syscall6(SYS_FORWARD, mode, 0, 0, 0, 0, 0));
}

int ritteros_masquerade(int op, const char *card)
{
    return (int)check(syscall6(SYS_MASQUERADE, op, (int64_t)card, 0, 0, 0, 0));
}

int ritteros_tap(int op, unsigned value)
{
    return (int)check(syscall6(SYS_TAP, op, value, 0, 0, 0, 0));
}

int ritteros_bridge(int op, int bridge, const char *card)
{
    char name[8] = { 0 };
    for (int i = 0; card && i < 7 && card[i]; i++)
        name[i] = card[i];
    return (int)check(syscall6(SYS_BRIDGE, op, bridge, card ? (int64_t)name : 0, 0, 0, 0));
}

int ritteros_default_route(int op, const char *card)
{
    return (int)check(syscall6(SYS_DEFAULT_ROUTE, op, (int64_t)card, 0, 0, 0, 0));
}

int64_t ritteros_ping(uint32_t ip, uint16_t id, uint16_t seq, size_t size, uint32_t timeout_ms)
{
    return check(syscall6(SYS_PING, ip, (int64_t)id << 16 | seq, (int64_t)size, timeout_ms, 0,
                          0));
}

int64_t ritteros_ping6(const struct sys_inaddr *to, uint16_t id, uint16_t seq, size_t size,
                       uint32_t timeout_ms)
{
    return check(syscall6(SYS_PING6, (int64_t)to, (int64_t)id << 16 | seq, (int64_t)size,
                          timeout_ms, 0, 0));
}

/* Is name "localhost" (in any case)? */
static bool is_localhost(const char *name)
{
    const char *l = "localhost";
    const char *c = name;
    while (*c && *l && (*c == *l || *c == *l - 32))
        c++, l++;
    return !*c && !*l;
}

/* Ask the elf dnsd, on this machine's port 53, for name's record of
 * `type` (1 A, 28 AAAA): 0 and its data into out, or an errno. */
static int ask_dnsd(const char *name, uint16_t type, uint8_t *out)
{
    static __thread uint8_t q[512], a[512];
    uint16_t id;
    ritteros_random(&id, sizeof(id));
    size_t length = dns_query_type(name, id, type, q, sizeof(q));
    if (!length)
        return EINVAL;
    int fd = ritteros_socket(SOCKET_UDP);
    if (fd < 0)
        return errno;
    int r = ETIMEDOUT;
    /* (64j) Asked twice: a datagram may be lost (UDP allows it, and dnsd's
     * queue overflowed in a burst of lookups), so after 2 s the question
     * goes again, and the answer may then take 4 s more (6 s in all, as
     * before). Both carry the same ID; either answer is taken. */
    bool answered = false;
    for (int attempt = 0; attempt < 2 && !answered; attempt++) {
        if (ritteros_sendto(fd, q, length, 0x7F000001, 53) < 0)
            break;
        for (;;) {
            uint32_t from;
            uint16_t port;
            ssize_t n = ritteros_recvfrom(fd, a, sizeof(a), &from, &port, attempt ? 4000 : 2000);
            if (n < 0) {
                r = errno == EINTR ? EINTR : ETIMEDOUT;
                answered = r == EINTR;              /* (interrupted: not asked again) */
                break;
            }
            if (n >= 12 && a[0] == q[0] && a[1] == q[1]) {
                answered = true;                    /* (a server failure too: not asked again) */
                uint32_t ttl, v4;
                if (type == 28) {
                    r = dns_answer_aaaa(a, (size_t)n, out, &ttl);
                } else if (!(r = dns_answer(a, (size_t)n, &v4, &ttl))) {
                    out[0] = (uint8_t)(v4 >> 24), out[1] = (uint8_t)(v4 >> 16);
                    out[2] = (uint8_t)(v4 >> 8), out[3] = (uint8_t)v4;
                }
                break;
            }
        }
    }
    close(fd);
    return r;
}

int ritteros_resolve(const char *name, uint32_t *ip)
{
    if (ip_parse(name, ip, 0))
        return 0;                                   /* written out */
    if (is_localhost(name)) {
        *ip = 0x7F000001;
        return 0;
    }
    uint8_t a[4];
    int r = ask_dnsd(name, 1, a);
    if (r) {
        errno = r;
        return -1;
    }
    *ip = (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3];
    return 0;
}

int ritteros_resolve6(const char *name, uint8_t address[16])
{
    if (inet_pton(AF_INET6, name, address) == 1)
        return 0;                                   /* written out */
    if (is_localhost(name)) {
        memset(address, 0, 16);
        address[15] = 1;                            /* ::1 */
        return 0;
    }
    int r = ask_dnsd(name, 28, address);
    if (r) {
        errno = r;
        return -1;
    }
    return 0;
}

int ritteros_socket(int type)
{
    return (int)check(syscall6(SYS_SOCKET, type, 0, 0, 0, 0, 0));
}

int ritteros_bind(int fd, uint16_t port)
{
    return (int)check(syscall6(SYS_BIND, fd, port, 0, 0, 0, 0));
}

int ritteros_connect(int fd, uint32_t ip, uint16_t port)
{
    return (int)check(syscall6(SYS_CONNECT, fd, ip, port, 0, 0, 0));
}

ssize_t ritteros_sendto(int fd, const void *data, size_t size, uint32_t ip, uint16_t port)
{
    return check(syscall6(SYS_SENDTO, fd, (int64_t)data, (int64_t)size, ip, port, 0));
}

ssize_t ritteros_recvfrom(int fd, void *data, size_t size, uint32_t *ip, uint16_t *port,
                          uint32_t timeout_ms)
{
    uint64_t from = 0;
    ssize_t r = check(syscall6(SYS_RECVFROM, fd, (int64_t)data, (int64_t)size, (int64_t)&from,
                               timeout_ms, 0));
    if (r >= 0) {
        if (ip)
            *ip = (uint32_t)(from >> 16);
        if (port)
            *port = (uint16_t)from;
    }
    return r;
}

int ritteros_listen(int fd, int backlog)
{
    return (int)check(syscall6(SYS_LISTEN, fd, backlog, 0, 0, 0, 0));
}

int ritteros_accept(int fd, uint32_t *ip, uint16_t *port)
{
    uint64_t from = 0;
    int r = (int)check(syscall6(SYS_ACCEPT, fd, (int64_t)&from, 0, 0, 0, 0));
    if (r >= 0) {
        if (ip)
            *ip = (uint32_t)(from >> 16);
        if (port)
            *port = (uint16_t)from;
    }
    return r;
}

/* Local (Unix-domain) sockets. bind/connect carry a name, not a port. */
int ritteros_bind_local(int fd, const char *path)
{
    return (int)check(syscall6(SYS_BIND, fd, (int64_t)path, (int64_t)strlen(path), 0, 0, 0));
}

int ritteros_connect_local(int fd, const char *path)
{
    return (int)check(syscall6(SYS_CONNECT, fd, (int64_t)path, (int64_t)strlen(path), 0, 0, 0));
}

int ritteros_local_listen(const char *path, int backlog)
{
    int fd = ritteros_socket(SOCKET_LOCAL);
    if (fd < 0)
        return -1;
    if (ritteros_bind_local(fd, path) < 0 || ritteros_listen(fd, backlog) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int ritteros_local_connect(const char *path)
{
    int fd = ritteros_socket(SOCKET_LOCAL);
    if (fd < 0)
        return -1;
    if (ritteros_connect_local(fd, path) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int ritteros_pty_open(int fds[2])
{
    return (int)check(syscall6(SYS_PTY, PTY_OPEN, (int64_t)fds, 0, 0, 0, 0));
}

int ritteros_pty_size(int fd, int rows, int cols)
{
    return (int)check(syscall6(SYS_PTY, PTY_SIZE, fd, rows, cols, 0, 0));
}

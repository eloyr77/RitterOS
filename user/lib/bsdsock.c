/*
 * BSD sockets (53a, sys/socket.h): the POSIX interface over RitterOS's
 * own socket calls (common/syscalls.h). The kind of a socket (TCP, UDP,
 * local) and its family (64d: IPv4 or IPv6, SYS_SOCKNAME6) come from the
 * kernel, so a descriptor passed to another program, or dup'd, keeps
 * working: nothing is kept here. Internet addresses go through the IPv6
 * calls (SYS_CONNECT6 ...), whose struct sys_inaddr holds either family.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include "../../common/ip6text.h"
#include "cancel.h"
#include "ritteros.h"

static int64_t check(int64_t r)
{
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

static int fail(int error)
{
    errno = error;
    return -1;
}

static const struct in6_addr any6 = IN6ADDR_ANY_INIT, loopback6 = IN6ADDR_LOOPBACK_INIT;

const struct in6_addr *__ritteros_in6addr_any(void)
{
    return &any6;
}

const struct in6_addr *__ritteros_in6addr_loopback(void)
{
    return &loopback6;
}

uint32_t htonl(uint32_t v)
{
    return __builtin_bswap32(v);                /* (both processors are little-endian) */
}

uint16_t htons(uint16_t v)
{
    return __builtin_bswap16(v);
}

uint32_t ntohl(uint32_t v)
{
    return __builtin_bswap32(v);
}

uint16_t ntohs(uint16_t v)
{
    return __builtin_bswap16(v);
}

/* The kernel's own and peer's address of fd. */
static int name_of(int fd, int which, struct sys_sockname *n)
{
    return (int)check(syscall6(SYS_SOCKNAME, fd, which, (int64_t)n, 0, 0, 0));
}

static int kind_of(int fd)
{
    struct sys_sockname n;
    return name_of(fd, SOCKNAME_OWN, &n) < 0 ? -1 : (int)n.type;
}

/* (64d) An Internet socket's address in either family (an IPv6 socket's
 * family is 10, an IPv4 one's 2). */
static int name6_of(int fd, int which, struct sys_inaddr *a)
{
    return (int)check(syscall6(SYS_SOCKNAME6, fd, which, (int64_t)a, 0, 0, 0));
}

/* Is fd an IPv6 socket? */
static bool six_of(int fd)
{
    struct sys_inaddr a;
    return name6_of(fd, SOCKNAME_OWN, &a) == 0 && a.family == 10;
}

/* An address out to the program's sockaddr: as much as fits, *length the
 * whole size (POSIX's truncation). */
static void give(struct sockaddr *to, socklen_t *length, const void *from, socklen_t size)
{
    if (!to || !length)
        return;
    memcpy(to, from, *length < size ? *length : size);
    *length = size;
}

static void give_local(struct sockaddr *to, socklen_t *length, const char *name)
{
    struct sockaddr_un u;
    memset(&u, 0, sizeof(u));
    u.sun_family = AF_UNIX;
    memcpy(u.sun_path, name, sizeof(u.sun_path) - 1);
    give(to, length, &u, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(u.sun_path) + 1));
}

/* The kernel's address as the socket's family has it: sockaddr_in6 for an
 * IPv6 socket (an IPv4 peer as ::ffff:a.b.c.d), sockaddr_in otherwise. */
static void give_inet(struct sockaddr *to, socklen_t *length, const struct sys_inaddr *a, bool six)
{
    if (six) {
        struct sockaddr_in6 in;
        memset(&in, 0, sizeof(in));
        in.sin6_family = AF_INET6;
        in.sin6_port = htons(a->port);
        memcpy(in.sin6_addr.s6_addr, a->address, 16);
        in.sin6_scope_id = a->scope;
        give(to, length, &in, sizeof(in));
        return;
    }
    struct sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_port = htons(a->port);
    memcpy(&in.sin_addr.s_addr, a->address + 12, 4);
    give(to, length, &in, sizeof(in));
}

int socket(int family, int type, int protocol)
{
    int kind;
    bool inet = family == AF_INET || family == AF_INET6;
    if (inet && type == SOCK_STREAM && (!protocol || protocol == IPPROTO_TCP))
        kind = SOCKET_TCP;
    else if (inet && type == SOCK_DGRAM && (!protocol || protocol == IPPROTO_UDP))
        kind = SOCKET_UDP;
    else if (family == AF_UNIX && type == SOCK_STREAM && !protocol)
        kind = SOCKET_LOCAL;
    else if (!inet && family != AF_UNIX)
        return fail(EAFNOSUPPORT);
    else if (type != SOCK_STREAM && type != SOCK_DGRAM)
        return fail(ESOCKTNOSUPPORT);
    else
        return fail(EPROTONOSUPPORT);
    return (int)check(syscall6(SYS_SOCKET, kind | (family == AF_INET6 ? SOCKET_INET6 : 0), 0, 0,
                               0, 0, 0));
}

/* A sockaddr for a socket of this kind: a local name, or an Internet
 * address of the socket's own family (sockaddr_in for AF_INET,
 * sockaddr_in6 for AF_INET6) into *a; the family's error otherwise. */
static int take(int kind, bool six, const struct sockaddr *s, socklen_t length,
                struct sys_inaddr *a, char *name)
{
    if (!s)
        return EFAULT;
    if (kind == SOCKET_LOCAL) {
        if (length < sizeof(sa_family_t) || s->sa_family != AF_UNIX)
            return EAFNOSUPPORT;
        const struct sockaddr_un *u = (const struct sockaddr_un *)s;
        size_t room = length - offsetof(struct sockaddr_un, sun_path);
        if (room > sizeof(u->sun_path))
            room = sizeof(u->sun_path);
        size_t n = strnlen(u->sun_path, room);
        if (n == 0 || n >= LOCAL_PATH_MAX)
            return n ? ENAMETOOLONG : EINVAL;
        memcpy(name, u->sun_path, n);
        name[n] = 0;
        return 0;
    }
    memset(a, 0, sizeof(*a));
    if (six) {
        if (length < sizeof(struct sockaddr_in6))
            return EINVAL;
        if (s->sa_family != AF_INET6)
            return EAFNOSUPPORT;
        const struct sockaddr_in6 *in = (const struct sockaddr_in6 *)s;
        a->family = 10;
        a->port = ntohs(in->sin6_port);
        a->scope = in->sin6_scope_id;
        memcpy(a->address, in->sin6_addr.s6_addr, 16);
        return 0;
    }
    if (length < sizeof(struct sockaddr_in))
        return EINVAL;
    if (s->sa_family != AF_INET)
        return EAFNOSUPPORT;
    const struct sockaddr_in *in = (const struct sockaddr_in *)s;
    a->family = 2;
    a->port = ntohs(in->sin_port);
    a->address[10] = a->address[11] = 0xFF;
    memcpy(a->address + 12, &in->sin_addr.s_addr, 4);
    return 0;
}

int bind(int fd, const struct sockaddr *address, socklen_t length)
{
    int kind = kind_of(fd);
    if (kind < 0)
        return -1;
    struct sys_inaddr a;
    char name[LOCAL_PATH_MAX];
    int e = take(kind, kind != SOCKET_LOCAL && six_of(fd), address, length, &a, name);
    if (e)
        return fail(e);
    if (kind == SOCKET_LOCAL)
        return (int)check(syscall6(SYS_BIND, fd, (int64_t)name, (int64_t)strlen(name), 0, 0, 0)) < 0
               ? -1 : 0;
    /* (the port only: the socket answers on every address of the machine) */
    return check(syscall6(SYS_BIND, fd, a.port, 0, 0, 0, 0)) < 0 ? -1 : 0;
}

int connect(int fd, const struct sockaddr *address, socklen_t length)
{
    int kind = kind_of(fd);
    if (kind < 0)
        return -1;
    struct sys_inaddr a;
    char name[LOCAL_PATH_MAX];
    int e = take(kind, kind != SOCKET_LOCAL && six_of(fd), address, length, &a, name);
    if (e)
        return fail(e);
    if (kind == SOCKET_LOCAL)
        return (int)check(syscall6(SYS_CONNECT, fd, (int64_t)name, (int64_t)strlen(name), 0, 0, 0));
    if (kind == SOCKET_TCP && !a.port)
        return fail(ECONNREFUSED);
    cancel_point();                             /* (54c) */
    return (int)check(cancel_after(syscall6(SYS_CONNECT6, fd, (int64_t)&a, 0, 0, 0, 0)));
}

int listen(int fd, int backlog)
{
    if (backlog <= 0)
        backlog = 1;
    if (backlog > SOMAXCONN)
        backlog = SOMAXCONN;
    return (int)check(syscall6(SYS_LISTEN, fd, backlog, 0, 0, 0, 0));
}

int accept(int fd, struct sockaddr *address, socklen_t *length)
{
    int kind = address ? kind_of(fd) : 0;
    if (kind < 0)
        return -1;
    bool six = kind && kind != SOCKET_LOCAL && six_of(fd);
    struct sys_inaddr from;
    memset(&from, 0, sizeof(from));
    cancel_point();                             /* (54c) */
    int r = (int)check(cancel_after(syscall6(kind == SOCKET_LOCAL ? SYS_ACCEPT : SYS_ACCEPT6, fd,
                                             kind == SOCKET_LOCAL ? 0 : (int64_t)&from, 0, 0, 0, 0)));
    if (r >= 0 && address) {
        if (kind == SOCKET_LOCAL)
            give_local(address, length, "");    /* (a local peer has no name) */
        else
            give_inet(address, length, &from, six);
    }
    return r;
}

/* Flags send and recv take; the rest is EOPNOTSUPP. */
static int flags_ok(int flags, int allowed)
{
    return (flags & ~allowed) == 0;
}

ssize_t send(int fd, const void *data, size_t size, int flags)
{
    if (!flags_ok(flags, MSG_NOSIGNAL | MSG_DONTWAIT))
        return fail(EOPNOTSUPP);
    if (flags & MSG_DONTWAIT)                   /* (53b: this call only) */
        return check(syscall6(SYS_SENDTO, fd, (int64_t)data, (int64_t)size, 0, 0, SOCKET_DONTWAIT));
    return write(fd, data, size);
}

ssize_t recv(int fd, void *data, size_t size, int flags)
{
    if (!flags_ok(flags, MSG_WAITALL | MSG_DONTWAIT) ||
        (flags & (MSG_WAITALL | MSG_DONTWAIT)) == (MSG_WAITALL | MSG_DONTWAIT))
        return fail(EOPNOTSUPP);
    if (flags & MSG_DONTWAIT)                   /* (53b: this call only) */
        return check(syscall6(SYS_RECVFROM, fd, (int64_t)data, (int64_t)size, 0, 0, SOCKET_DONTWAIT));
    if (!(flags & MSG_WAITALL))
        return read(fd, data, size);
    size_t done = 0;                            /* MSG_WAITALL: all of it, or the end */
    while (done < size) {
        ssize_t n = read(fd, (char *)data + done, size - done);
        if (n < 0)
            return done ? (ssize_t)done : -1;
        if (n == 0)
            break;
        done += (size_t)n;
    }
    return (ssize_t)done;
}

ssize_t sendto(int fd, const void *data, size_t size, int flags, const struct sockaddr *to,
               socklen_t length)
{
    if (!flags_ok(flags, MSG_NOSIGNAL | MSG_DONTWAIT))
        return fail(EOPNOTSUPP);
    int64_t wait = flags & MSG_DONTWAIT ? SOCKET_DONTWAIT : 0;
    if (!to)
        return check(syscall6(SYS_SENDTO, fd, (int64_t)data, (int64_t)size, 0, 0, wait));
    int kind = kind_of(fd);
    if (kind < 0)
        return -1;
    if (kind != SOCKET_UDP)
        return fail(EISCONN);                   /* (a stream: connect, then send) */
    struct sys_inaddr a;
    int e = take(kind, six_of(fd), to, length, &a, 0);
    if (e)
        return fail(e);
    if (!a.port)
        return fail(EINVAL);
    cancel_point();                             /* (54c) */
    return check(cancel_after(syscall6(SYS_SENDTO6, fd, (int64_t)data, (int64_t)size, (int64_t)&a, 0,
                                       wait)));
}

ssize_t recvfrom(int fd, void *data, size_t size, int flags, struct sockaddr *from,
                 socklen_t *length)
{
    if (!flags_ok(flags, MSG_DONTWAIT))
        return fail(EOPNOTSUPP);
    int kind = from ? kind_of(fd) : 0;
    if (kind < 0)
        return -1;
    struct sys_inaddr sender;
    memset(&sender, 0, sizeof(sender));
    bool inet = kind != SOCKET_LOCAL;
    cancel_point();                             /* (54c) */
    ssize_t r = check(cancel_after(syscall6(inet ? SYS_RECVFROM6 : SYS_RECVFROM, fd, (int64_t)data,
                                            (int64_t)size, inet && from ? (int64_t)&sender : 0, 0,
                                            flags & MSG_DONTWAIT ? SOCKET_DONTWAIT : 0)));
    if (r >= 0 && from) {
        if (inet)
            give_inet(from, length, &sender, six_of(fd));
        else
            give_local(from, length, "");
    }
    return r;
}

int shutdown(int fd, int how)
{
    return (int)check(syscall6(SYS_SHUTDOWN, fd, how, 0, 0, 0, 0));
}

/* A socket's own or peer's address: a local socket's name, or an
 * Internet address in the socket's family. */
static int name_for(int fd, int which, struct sockaddr *address, socklen_t *length)
{
    struct sys_sockname n;
    if (name_of(fd, which, &n) < 0)
        return -1;
    if (n.type == SOCKET_LOCAL) {
        give_local(address, length, n.name);
        return 0;
    }
    struct sys_inaddr a;
    if (name6_of(fd, which, &a) < 0)
        return -1;
    give_inet(address, length, &a, a.family == 10);
    return 0;
}

int getsockname(int fd, struct sockaddr *address, socklen_t *length)
{
    return name_for(fd, SOCKNAME_OWN, address, length);
}

int getpeername(int fd, struct sockaddr *address, socklen_t *length)
{
    return name_for(fd, SOCKNAME_PEER, address, length);
}

/* (level, option) to the kernel's SOCKOPT_*; 0 for none. */
static int kernel_option(int level, int option)
{
    if (level == IPPROTO_TCP)
        return option == TCP_NODELAY ? SOCKOPT_NODELAY : 0;
    if (level == IPPROTO_IPV6)
        return option == IPV6_V6ONLY ? SOCKOPT_V6ONLY : 0;     /* (64d) */
    if (level != SOL_SOCKET)
        return 0;
    switch (option) {
    case SO_RCVTIMEO: return SOCKOPT_RCVTIMEO;
    case SO_SNDTIMEO: return SOCKOPT_SNDTIMEO;
    case SO_REUSEADDR: return SOCKOPT_REUSEADDR;
    case SO_KEEPALIVE: return SOCKOPT_KEEPALIVE;
    case SO_BROADCAST: return SOCKOPT_BROADCAST;
    case SO_ERROR: return SOCKOPT_ERROR;
    case SO_TYPE: return SOCKOPT_TYPE;
    case SO_RCVBUF: return SOCKOPT_RCVBUF;
    case SO_SNDBUF: return SOCKOPT_SNDBUF;
    default: return 0;
    }
}

int setsockopt(int fd, int level, int option, const void *value, socklen_t length)
{
    int k = kernel_option(level, option);
    if (!k)
        return fail(ENOPROTOOPT);
    uint64_t v;
    if (k == SOCKOPT_RCVTIMEO || k == SOCKOPT_SNDTIMEO) {
        if (length < sizeof(struct timeval))
            return fail(EINVAL);
        const struct timeval *tv = value;
        if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000)
            return fail(EINVAL);
        v = (uint64_t)tv->tv_sec * 1000 + ((uint64_t)tv->tv_usec + 999) / 1000;
    } else {
        if (length < sizeof(int))
            return fail(EINVAL);
        v = (uint64_t)(int64_t)*(const int *)value;
    }
    return (int)check(syscall6(SYS_SOCKOPT, fd, SOCKOPT_SET, k, (int64_t)&v, 0, 0));
}

int getsockopt(int fd, int level, int option, void *value, socklen_t *length)
{
    int k = kernel_option(level, option);
    if (!k)
        return fail(ENOPROTOOPT);
    uint64_t v = 0;
    if (check(syscall6(SYS_SOCKOPT, fd, SOCKOPT_GET, k, (int64_t)&v, 0, 0)) < 0)
        return -1;
    if (k == SOCKOPT_RCVTIMEO || k == SOCKOPT_SNDTIMEO) {
        struct timeval tv = { (time_t)(v / 1000), (suseconds_t)(v % 1000 * 1000) };
        give((struct sockaddr *)value, length, &tv, sizeof(tv));
        return 0;
    }
    int i = (int)v;
    if (k == SOCKOPT_TYPE)
        i = v == SOCKET_UDP ? SOCK_DGRAM : SOCK_STREAM;
    give((struct sockaddr *)value, length, &i, sizeof(i));
    return 0;
}

/* --- addresses as text (arpa/inet.h) --- */

/* A number of the text form at *s (decimal, 0x hex or 0 octal, as BSD's
 * inet_aton; dotted-decimal only for inet_pton), moving *s on. */
static int number(const char **s, bool strict, uint32_t *out)
{
    const char *p = *s;
    uint64_t v = 0;
    int base = 10;
    if (!strict && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        base = 16, p += 2;
    else if (!strict && p[0] == '0' && p[1] >= '0' && p[1] <= '9')
        base = 8, p++;
    const char *start = p;
    for (;; p++) {
        int d = *p >= '0' && *p <= '9' ? *p - '0'
              : base == 16 && *p >= 'a' && *p <= 'f' ? *p - 'a' + 10
              : base == 16 && *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : -1;
        if (d < 0 || d >= base)
            break;
        v = v * (uint64_t)base + (uint64_t)d;
        if (v > 0xFFFFFFFFu)
            return 0;
    }
    if (p == start && base != 8)
        return 0;
    if (strict && (p - start > 3 || (p - start > 1 && start[0] == '0')))
        return 0;                               /* (inet_pton: no leading zeros) */
    *s = p;
    *out = (uint32_t)v;
    return 1;
}

int inet_aton(const char *text, struct in_addr *out)
{
    uint32_t part[4];
    int n = 0;
    const char *s = text;
    for (;;) {
        if (n == 4 || !number(&s, false, &part[n]))
            return 0;
        n++;
        if (*s != '.')
            break;
        s++;
    }
    if (*s && *s != ' ' && (*s < '\t' || *s > '\r'))
        return 0;                               /* (BSD's: it ends at a blank) */
    uint32_t v;
    switch (n) {                                /* a, a.b, a.b.c, a.b.c.d */
    case 1: v = part[0]; break;
    case 2: if (part[0] > 255 || part[1] > 0xFFFFFF) return 0;
            v = part[0] << 24 | part[1]; break;
    case 3: if (part[0] > 255 || part[1] > 255 || part[2] > 0xFFFF) return 0;
            v = part[0] << 24 | part[1] << 16 | part[2]; break;
    default: if (part[0] > 255 || part[1] > 255 || part[2] > 255 || part[3] > 255) return 0;
             v = part[0] << 24 | part[1] << 16 | part[2] << 8 | part[3]; break;
    }
    if (out)
        out->s_addr = htonl(v);
    return 1;
}

in_addr_t inet_addr(const char *text)
{
    struct in_addr a;
    return inet_aton(text, &a) ? a.s_addr : INADDR_NONE;
}

int inet_pton(int family, const char *text, void *out)
{
    if (family == AF_INET6)
        return ip6_parse(text, strlen(text), out) ? 1 : 0;
    if (family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    uint32_t v = 0, part;
    const char *s = text;
    for (int i = 0; i < 4; i++) {
        if (!number(&s, true, &part) || part > 255 || (i < 3 && *s++ != '.'))
            return 0;
        v = v << 8 | part;
    }
    if (*s)
        return 0;
    uint32_t net = htonl(v);
    memcpy(out, &net, sizeof(net));
    return 1;
}

const char *inet_ntop(int family, const void *address, char *out, socklen_t size)
{
    if (family == AF_INET6) {
        if (!ip6_format(address, out, size)) {
            errno = ENOSPC;
            return 0;
        }
        return out;
    }
    if (family != AF_INET) {
        errno = EAFNOSUPPORT;
        return 0;
    }
    uint32_t net;
    memcpy(&net, address, sizeof(net));
    uint32_t v = ntohl(net);
    char text[INET_ADDRSTRLEN];
    int n = snprintf(text, sizeof(text), "%u.%u.%u.%u", v >> 24, v >> 16 & 255, v >> 8 & 255,
                     v & 255);
    if ((socklen_t)n >= size) {
        errno = ENOSPC;
        return 0;
    }
    memcpy(out, text, (size_t)n + 1);
    return out;
}

char *inet_ntoa(struct in_addr address)
{
    static char text[INET_ADDRSTRLEN];
    return (char *)inet_ntop(AF_INET, &address.s_addr, text, sizeof(text));
}

/* --- sys/time.h --- */

int gettimeofday(struct timeval *tv, void *zone)
{
    if (zone)
        memset(zone, 0, 2 * sizeof(int));       /* (struct timezone: UTC, no daylight time) */
    uint64_t ns = ritteros_time();
    tv->tv_sec = (time_t)(ns / 1000000000);
    tv->tv_usec = (suseconds_t)(ns % 1000000000 / 1000);
    return 0;
}

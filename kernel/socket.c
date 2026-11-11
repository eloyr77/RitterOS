/*
 * Sockets: programs' ends of network conversations (see socket.h). A UDP
 * socket owns a port; datagrams for it wait in a queue of SOCKET_QUEUE
 * (more are dropped, as UDP allows) until the program takes them, each in
 * memory of its own (up to DATAGRAM_MAX: IPv6 puts fragmented ones
 * together, 64c). TCP sockets (kernel/tcp.c) keep a connection. An IPv6
 * socket (SOCKET_INET6) talks to IPv6 and IPv4 peers (the latter as
 * ::ffff:a.b.c.d), an IPv4 one to IPv4 peers only.
 */
#include "socket.h"
#include "heap.h"
#include "inet.h"
#include "ipv4.h"
#include "localsock.h"
#include "sched.h"
#include "string.h"
#include "sync.h"
#include "syscalls.h"
#include "tcp.h"
#include "timer.h"
#include "udp.h"

#define SOCKET_QUEUE    128     /* (64j; 16 until then: a burst of name lookups
                                 * overflowed dnsd's queue and was lost) */
#define DATAGRAM_MAX    (9000 - 8)      /* ipv6.c's FRAGMENTED_MAX less UDP's header */

/* (64j) Every datagram dropped by a full queue (or no memory for it), on
 * any socket: net's udp: line. */
static uint64_t udp_dropped;

uint64_t socket_udp_dropped(void)
{
    return udp_dropped;
}

struct datagram {
    struct inet_addr from;
    uint16_t from_port;
    uint16_t length;
    uint8_t *data;                      /* kmalloc'd, its length */
};

struct socket {
    int type;
    bool six;                           /* SOCKET_INET6: IPv6 peers too (64c) */
    bool v6only;                        /* (64d) IPV6_V6ONLY: IPv6 peers only */
    int refs;                           /* descriptors holding it */
    uint16_t port;                      /* bound (UDP) */
    struct inet_addr peer;              /* connect: the default destination (UDP) */
    uint16_t peer_port;
    struct netdev *card;                /* bindcard: that card's only (UDP) */
    struct spinlock lock;
    struct wait_queue waiters;
    struct datagram *queue;             /* SOCKET_QUEUE of them */
    unsigned head, count;
    uint64_t dropped;
    struct tcp *tcp;                    /* SOCKET_TCP */
    struct local *local;                /* SOCKET_LOCAL */
    uint32_t rcv_timeout_ms, snd_timeout_ms;    /* (53a) SO_RCVTIMEO, SO_SNDTIMEO (0: none) */
    uint8_t kept[SOCKOPT_BROADCAST + 1];        /* (53a) the options only kept */
    bool read_shut;                     /* (53a) UDP after shutdown(SHUT_RD) */
};

/* The families of peers the socket takes (64d). */
static unsigned families(const struct socket *s)
{
    return !s->six ? INET_V4 : s->v6only ? INET_V6 : INET_V4 | INET_V6;
}

static void udp_arrived(void *context, const struct inet_addr *src, uint16_t src_port,
                        const uint8_t *data, size_t length)
{
    struct socket *s = context;
    uint8_t *copy = length <= DATAGRAM_MAX ? kmalloc(length ? length : 1) : 0;
    if (copy)
        memcpy(copy, data, length);
    uint64_t state = spin_lock(&s->lock);
    if (s->count == SOCKET_QUEUE || !copy) {
        s->dropped++;
        udp_dropped++;
    } else {
        struct datagram *d = &s->queue[(s->head + s->count) % SOCKET_QUEUE];
        d->from = *src;
        d->from_port = src_port;
        d->length = (uint16_t)length;
        d->data = copy;
        copy = 0;
        s->count++;
    }
    spin_unlock(&s->lock, state);
    kfree(copy);
    wait_queue_wake_all(&s->waiters);
}

int socket_poll(struct socket *s)
{
    if (s->type == SOCKET_UDP)
        poll_watch(&s->waiters);            /* (65e) before the state is read */
    if (s->type == SOCKET_TCP)
        return s->tcp ? tcp_poll(s->tcp) : POLLOUT;
    if (s->type == SOCKET_LOCAL)
        return s->local ? local_poll(s->local) : 0;
    uint64_t state = spin_lock(&s->lock);
    int r = POLLOUT | (s->count ? POLLIN : 0);
    spin_unlock(&s->lock, state);
    return r;
}

struct socket *socket_new(int type)
{
    bool six = type & SOCKET_INET6;
    type &= ~SOCKET_INET6;
    if (type != SOCKET_UDP && type != SOCKET_TCP && (type != SOCKET_LOCAL || six))
        return 0;
    struct socket *s = kzalloc(sizeof(*s));
    if (!s)
        return 0;
    s->type = type;
    s->six = six;
    s->refs = 1;
    s->lock = (struct spinlock)SPINLOCK_INIT("socket");
    s->waiters.pollable = true;
    if (type == SOCKET_UDP && !(s->queue = kmalloc(SOCKET_QUEUE * sizeof(struct datagram)))) {
        kfree(s);
        return 0;
    }
    return s;
}

int socket_bind_local(struct socket *s, const char *name, int uid)
{
    return s->type == SOCKET_LOCAL ? local_bind(&s->local, name, uid) : ERR_NOTSUP;
}

int socket_connect_local(struct socket *s, const char *name, int uid)
{
    return s->type == SOCKET_LOCAL ? local_connect(&s->local, name, uid) : ERR_NOTSUP;
}

int socket_bind(struct socket *s, uint16_t port)
{
    if (s->type == SOCKET_TCP)
        return tcp_bind(&s->tcp, port, families(s));
    if (s->port)
        return ERR_INVAL;                           /* bound already */
    if (!port && !(port = udp_ephemeral_port()))
        return ERR_AGAIN;
    if (!udp_bind(port, s->card, families(s), udp_arrived, s))
        return ERR_ADDRINUSE;
    s->port = port;
    return 0;
}

int socket_bind_card(struct socket *s, struct netdev *card)
{
    if (s->type != SOCKET_UDP)
        return ERR_NOTSUP;
    if (s->port)
        return ERR_INVAL;                           /* bound already: too late */
    s->card = card;
    return 0;
}

int socket_connect(struct socket *s, const struct inet_addr *ip, uint16_t port)
{
    if (s->type == SOCKET_TCP)
        return tcp_connect(&s->tcp, ip, port, families(s));
    if (!(families(s) & inet_family(ip)))
        return ERR_AFNOSUPPORT;
    int r = s->port ? 0 : socket_bind(s, 0);
    if (r)
        return r;
    s->peer = *ip;
    s->peer_port = port;
    return 0;
}

int64_t socket_sendto(struct socket *s, const void *data, size_t length, const struct inet_addr *to,
                      uint16_t port)
{
    if (s->type == SOCKET_LOCAL)
        return s->local ? local_send(s->local, data, length, s->snd_timeout_ms) : -ERR_NOTCONN;
    if (s->type == SOCKET_TCP)
        return s->tcp ? tcp_send(s->tcp, data, length, s->snd_timeout_ms) : -ERR_NOTCONN;
    if (!to) {
        if (!s->peer_port)
            return -ERR_NOTCONN;
        to = &s->peer, port = s->peer_port;
    }
    if (!(families(s) & inet_family(to)))
        return -ERR_AFNOSUPPORT;
    int r = s->port ? 0 : socket_bind(s, 0);
    if (!r && s->card && inet_v4_of(to) == 0xFFFFFFFFu)
        r = udp_broadcast(s->card, s->port, port, data, length);   /* out of its card */
    else if (!r)
        r = udp_send(s->port, to, port, data, length);
    return r ? -r : (int64_t)length;
}

static int64_t receive(struct socket *s, void *data, size_t size, struct inet_addr *ip,
                       uint16_t *port, uint32_t timeout_ms);

/* (53a) With no time given, SO_RCVTIMEO's; running past that one is
 * ERR_AGAIN (POSIX's), past one given ERR_TIMEDOUT (as before). */
int64_t socket_recvfrom(struct socket *s, void *data, size_t size, struct inet_addr *ip,
                        uint16_t *port, uint32_t timeout_ms)
{
    bool by_option = !timeout_ms && s->rcv_timeout_ms;
    int64_t r = receive(s, data, size, ip, port, by_option ? s->rcv_timeout_ms : timeout_ms);
    return by_option && r == -ERR_TIMEDOUT ? -ERR_AGAIN : r;
}

static int64_t receive(struct socket *s, void *data, size_t size, struct inet_addr *ip,
                       uint16_t *port, uint32_t timeout_ms)
{
    if (s->type == SOCKET_LOCAL) {
        if (!s->local)
            return -ERR_NOTCONN;
        if (ip)
            *ip = inet_v4(0);
        if (port)
            *port = 0;
        return local_recv(s->local, data, size, timeout_ms);
    }
    if (s->type == SOCKET_TCP) {
        if (!s->tcp)
            return -ERR_NOTCONN;
        if (ip)
            *ip = tcp_peer(s->tcp, port);
        return tcp_receive(s->tcp, data, size, timeout_ms);
    }
    if (!s->port)
        return -ERR_NOTCONN;                        /* nothing can arrive */
    if (s->read_shut)
        return 0;
    if (!s->count && thread_current()->dont_wait)
        return -ERR_AGAIN;                          /* (53b) */
    uint64_t deadline = timeout_ms ? timer_ticks() + ((uint64_t)timeout_ms * TIMER_HZ + 999) / 1000
                                   : 0;
    bool ready = timeout_ms
        ? wait_event_until(&s->waiters, s->count || thread_current()->stop_requested, deadline)
        : wait_event_interruptible(&s->waiters, s->count);
    if (thread_current()->stop_requested)
        return -ERR_INTR;
    if (!ready || !s->count)
        return -ERR_TIMEDOUT;
    uint64_t state = spin_lock(&s->lock);
    struct datagram *d = &s->queue[s->head];
    size_t n = d->length < size ? d->length : size;  /* the rest of it is lost, as UDP's is */
    memcpy(data, d->data, n);
    uint8_t *taken = d->data;
    d->data = 0;
    if (ip)
        *ip = d->from;
    if (port)
        *port = d->from_port;
    s->head = (s->head + 1) % SOCKET_QUEUE;
    s->count--;
    spin_unlock(&s->lock, state);
    kfree(taken);
    return (int64_t)n;
}

int socket_listen(struct socket *s, int backlog)
{
    if (s->type == SOCKET_LOCAL)
        return local_listen(&s->local, backlog);
    return s->type == SOCKET_TCP ? tcp_listen(&s->tcp, backlog, families(s)) : ERR_NOTSUP;
}

struct socket *socket_accept(struct socket *s, int *error)
{
    if (s->type == SOCKET_LOCAL) {
        if (!s->local) {
            *error = ERR_INVAL;
            return 0;
        }
        struct local *c = 0;
        *error = local_accept(s->local, &c);
        if (*error)
            return 0;
        struct socket *n = socket_new(SOCKET_LOCAL);
        if (!n) {
            local_close(c);
            *error = ERR_NOMEM;
            return 0;
        }
        n->local = c;
        return n;
    }
    if (s->type != SOCKET_TCP || !s->tcp) {
        *error = ERR_INVAL;
        return 0;
    }
    struct tcp *c = 0;
    *error = tcp_accept(s->tcp, &c);
    if (*error)
        return 0;
    struct socket *n = socket_new(SOCKET_TCP | (s->six ? SOCKET_INET6 : 0));
    if (!n) {
        tcp_close(c);
        *error = ERR_NOMEM;
        return 0;
    }
    n->v6only = s->v6only;
    n->tcp = c;
    return n;
}

void socket_hold(struct socket *s)
{
    __atomic_add_fetch(&s->refs, 1, __ATOMIC_SEQ_CST);
}

int socket_peer_uid(struct socket *s)
{
    return s->type == SOCKET_LOCAL ? local_peer_uid(s->local) : -1;
}

int socket_type(struct socket *s)
{
    return s->type;
}

int socket_send_buffer(struct socket *s, struct shbuf *b)
{
    return s->type == SOCKET_LOCAL && s->local ? local_send_buffer(s->local, b) : ERR_NOTSUP;
}

struct shbuf *socket_recv_buffer(struct socket *s)
{
    return s->type == SOCKET_LOCAL && s->local ? local_recv_buffer(s->local) : 0;
}

void socket_close(struct socket *s)
{
    if (__atomic_sub_fetch(&s->refs, 1, __ATOMIC_SEQ_CST) > 0)
        return;
    if (s->type == SOCKET_UDP) {
        if (s->port)
            udp_unbind(s->port, s->card);           /* no delivery to it any more */
        for (unsigned i = 0; i < s->count; i++)
            kfree(s->queue[(s->head + i) % SOCKET_QUEUE].data);
        kfree(s->queue);
    } else if (s->type == SOCKET_LOCAL) {
        if (s->local)
            local_close(s->local);
    } else if (s->tcp) {
        tcp_close(s->tcp);                          /* the connection ends on its own */
    }
    wait_queue_detach(&s->waiters);                 /* (65e) no poller left on it */
    kfree(s);
}

int socket_shutdown(struct socket *s, int how)
{
    if (how < SHUTDOWN_READ || how > SHUTDOWN_BOTH)
        return ERR_INVAL;
    bool read = how != SHUTDOWN_WRITE, write = how != SHUTDOWN_READ;
    if (s->type == SOCKET_TCP) {
        uint16_t peer_port = 0;
        if (s->tcp)
            tcp_peer(s->tcp, &peer_port);
        if (!peer_port)
            return ERR_NOTCONN;                     /* (not connected, or listening) */
        tcp_shutdown(s->tcp, read, write);
    } else if (s->type == SOCKET_LOCAL) {
        if (!s->local)
            return ERR_NOTCONN;
        local_shutdown(s->local, read, write);
    } else {
        if (!s->peer_port)
            return ERR_NOTCONN;
        if (read) {
            s->read_shut = true;
            wait_queue_wake_all(&s->waiters);
        }
    }
    return 0;
}

int socket_name(struct socket *s, bool peer, struct sys_sockname *out)
{
    memset(out, 0, sizeof(*out));
    out->type = (uint32_t)s->type;
    if (s->type == SOCKET_LOCAL)
        return s->local ? local_sock_name(s->local, peer, out->name, sizeof(out->name))
                        : peer ? ERR_NOTCONN : 0;
    struct inet_addr a;
    int r = socket_name6(s, peer, &a, &out->port);
    out->address = inet_v4_of(&a);                  /* (an IPv6 one: 0, as "none") */
    return r;
}

int socket_name6(struct socket *s, bool peer, struct inet_addr *address, uint16_t *port)
{
    *address = inet_v4(0);
    *port = 0;
    if (s->type == SOCKET_LOCAL)
        return peer && !s->local ? ERR_NOTCONN : 0;
    if (peer) {
        *address = socket_peer(s, port);
        return *port ? 0 : ERR_NOTCONN;
    }
    *port = socket_port(s);
    if (s->type == SOCKET_TCP && s->tcp)
        *address = tcp_local_address(s->tcp);
    if (s->six && inet_none(address))
        memset(address->b, 0, 16);                  /* an IPv6 socket's "any": :: */
    return 0;
}

bool socket_six(struct socket *s)
{
    return s->six;
}

int socket_option(struct socket *s, bool set, int option, uint64_t *value)
{
    switch (option) {
    case SOCKOPT_RCVTIMEO:
    case SOCKOPT_SNDTIMEO: {
        uint32_t *t = option == SOCKOPT_RCVTIMEO ? &s->rcv_timeout_ms : &s->snd_timeout_ms;
        if (set)
            *t = *value > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)*value;
        else
            *value = *t;
        return 0;
    }
    case SOCKOPT_REUSEADDR:
    case SOCKOPT_KEEPALIVE:
    case SOCKOPT_NODELAY:
    case SOCKOPT_BROADCAST:
        if (option == SOCKOPT_NODELAY && s->type != SOCKET_TCP)
            return ERR_NOTSUP;
        if (set)
            s->kept[option] = *value != 0;
        else
            *value = s->kept[option];
        return 0;
    case SOCKOPT_ERROR:
    case SOCKOPT_TYPE:
        if (set)
            return ERR_INVAL;                       /* (read only) */
        *value = option == SOCKOPT_TYPE ? (uint64_t)s->type
               : s->type == SOCKET_TCP && s->tcp ? (uint64_t)tcp_error(s->tcp) : 0;
        return 0;
    case SOCKOPT_V6ONLY:                            /* (64d) before the socket is bound */
        if (!s->six || s->type == SOCKET_LOCAL)
            return ERR_NOPROTOOPT;
        if (set && (s->port || s->tcp))
            return ERR_INVAL;
        if (set)
            s->v6only = *value != 0;
        else
            *value = s->v6only;
        return 0;
    case SOCKOPT_RCVBUF:
    case SOCKOPT_SNDBUF:
        if (!set)
            *value = 65536;                         /* (setting it changes nothing) */
        return 0;
    default:
        return ERR_NOTSUP;
    }
}

struct inet_addr socket_peer(struct socket *s, uint16_t *port)
{
    if (s->type == SOCKET_LOCAL) {
        *port = 0;
        return inet_v4(0);                          /* a local socket has no address */
    }
    if (s->type == SOCKET_TCP)
        return tcp_peer(s->tcp, port);
    *port = s->peer_port;
    return s->peer;
}

uint16_t socket_port(struct socket *s)
{
    if (s->type == SOCKET_LOCAL)
        return 0;
    return s->type == SOCKET_TCP ? tcp_local_port(s->tcp) : s->port;
}

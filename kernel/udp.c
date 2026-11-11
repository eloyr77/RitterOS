/*
 * UDP (see udp.h): an 8-byte header (source port, destination port,
 * length, checksum) in front of the data. The checksum covers a
 * "pseudo-header" of the IP addresses too, so a datagram delivered to the
 * wrong address is noticed (IPv6 requires it: a datagram without one is
 * dropped). Ports are bound by programs' sockets; a binding may belong to
 * one card (a DHCP client's, one per card on the same port). One port is
 * one binding for both families (64c): an IPv6 socket's takes both.
 */
#include "udp.h"
#include "socket.h"
#include "ipv4.h"
#include "inet.h"
#include "kprintf.h"
#include "net.h"
#include "string.h"
#include "sync.h"
#include "syscalls.h"
#include "random.h"
#include "timer.h"

#define BINDINGS 128            /* (64j; 16 until then: every name lookup binds a port while
                                 * it waits, and a burst of them ran out) */

static struct {
    uint16_t port;
    struct netdev *card;                /* 0: what arrives on any card */
    unsigned families;                  /* INET_V4, INET_V6: the datagrams it takes */
    udp_handler handler;
    void *context;
    int calls;                          /* deliveries in progress (under the lock) */
} bindings[BINDINGS];
static struct spinlock lock = SPINLOCK_INIT("udp");
static uint16_t next_ephemeral = 49152;

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v;
}

bool udp_bind(uint16_t port, struct netdev *card, unsigned families, udp_handler handler,
              void *context)
{
    bool ok = false;
    uint64_t state = spin_lock(&lock);
    for (int i = 0; i < BINDINGS && !ok; i++)
        if (bindings[i].port == port && (!card || !bindings[i].card || bindings[i].card == card))
            goto out;                               /* taken */
    for (int i = 0; i < BINDINGS && !ok; i++) {
        if (!bindings[i].port) {
            bindings[i].port = port;
            bindings[i].card = card;
            bindings[i].families = families;
            bindings[i].handler = handler;
            bindings[i].context = context;
            ok = true;
        }
    }
out:
    spin_unlock(&lock, state);
    return ok;
}

void udp_unbind(uint16_t port, struct netdev *card)
{
    /* Unbound only when no delivery to it is still going on: afterwards
     * its owner may free the context. */
    for (;;) {
        uint64_t state = spin_lock(&lock);
        bool busy = false;
        for (int i = 0; i < BINDINGS; i++) {
            if (bindings[i].port != port || bindings[i].card != card)
                continue;
            busy = bindings[i].calls > 0;
            if (!busy)
                bindings[i].port = 0;
        }
        spin_unlock(&lock, state);
        if (!busy)
            return;
        timer_sleep_ms(1);
    }
}

uint16_t udp_ephemeral_port(void)
{
    uint64_t state = spin_lock(&lock);
    uint16_t port = 0;
    uint16_t r;
    random_bytes(&r, sizeof(r));
    next_ephemeral = (uint16_t)(49152 + r % (65536 - 49152));   /* a random start */
    for (int tries = 0; tries < 16384 && !port; tries++) {
        uint16_t candidate = next_ephemeral;
        next_ephemeral = next_ephemeral == 65535 ? 49152 : next_ephemeral + 1;
        bool taken = false;
        for (int i = 0; i < BINDINGS; i++)
            taken |= bindings[i].port == candidate;
        if (!taken)
            port = candidate;
    }
    spin_unlock(&lock, state);
    return port;
}

/* Send a datagram: on card d as a broadcast (IPv4), else (d 0) by its route. */
static int send(struct netdev *d, uint16_t src_port, const struct inet_addr *dst, uint16_t dst_port,
                const void *data, size_t length)
{
    if (length > inet_payload_max(dst) - 8)
        return ERR_INVAL;
    uint8_t datagram[IP_PAYLOAD_MAX];
    put16(datagram, src_port);
    put16(datagram + 2, dst_port);
    put16(datagram + 4, (uint16_t)(8 + length));
    put16(datagram + 6, 0);
    memcpy(datagram + 8, data, length);
    struct inet_addr src = inet_v4(d ? d->address : 0);
    if (!d && !inet_source(dst, &src) && !inet_is_v4(dst))
        return ERR_NETUNREACH;                      /* (IPv6: no route; IPv4 says so on sending) */
    uint16_t sum = ip_checksum_fold(ip_checksum_add(inet_pseudo(&src, dst, IP_PROTO_UDP, 8 + length),
                                                    datagram, 8 + length));
    put16(datagram + 6, sum ? sum : 0xFFFF);        /* 0 would mean "no checksum" */
    if (d)
        return ip_broadcast(d, IP_PROTO_UDP, datagram, 8 + length);
    return inet_send(&src, dst, IP_PROTO_UDP, datagram, 8 + length);
}

int udp_send(uint16_t src_port, const struct inet_addr *dst, uint16_t dst_port, const void *data,
             size_t length)
{
    return send(0, src_port, dst, dst_port, data, length);
}

int udp_broadcast(struct netdev *d, uint16_t src_port, uint16_t dst_port, const void *data,
                  size_t length)
{
    struct inet_addr all = inet_v4(0xFFFFFFFFu);
    return send(d, src_port, &all, dst_port, data, length);
}

void udp_input(struct netdev *d, const struct inet_addr *src, const struct inet_addr *dst,
               const uint8_t *p, size_t length)
{
    if (length < 8)
        return;
    size_t total = get16(p + 4);
    bool six = !inet_is_v4(src);
    if (total < 8 || total > length || (six && !get16(p + 6)))
        return;                                     /* (IPv6: a checksum is a must, RFC 8200) */
    if (get16(p + 6) &&
        ip_checksum_fold(ip_checksum_add(inet_pseudo(src, dst, IP_PROTO_UDP, total), p, total)) != 0)
        return;                                     /* damaged */
    uint16_t port = get16(p + 2);
    int slot = -1;
    uint64_t state = spin_lock(&lock);
    for (int i = 0; i < BINDINGS && slot < 0; i++)
        if (bindings[i].port == port && (!bindings[i].card || bindings[i].card == d) &&
            (bindings[i].families & inet_family(src)))
            slot = i;
    if (slot >= 0)
        bindings[slot].calls++;
    spin_unlock(&lock, state);
    if (slot < 0)
        return;
    bindings[slot].handler(bindings[slot].context, src, get16(p), p + 8, total - 8);
    state = spin_lock(&lock);
    bindings[slot].calls--;
    spin_unlock(&lock, state);
}

void udp_print(void)
{
    uint64_t state = spin_lock(&lock);
    int bound = 0;
    for (int i = 0; i < BINDINGS; i++)
        bound += bindings[i].port != 0;
    spin_unlock(&lock, state);
    kprintf("udp: %d of %d port(s) bound; %lu datagram(s) dropped by a full queue\n", bound,
            BINDINGS, socket_udp_dropped());
}

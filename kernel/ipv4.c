/*
 * IPv4 over Ethernet (see ipv4.h).
 *
 * Ethernet: 6 bytes destination MAC, 6 source, 2 type (0x0806 ARP, 0x0800
 * IPv4), then the payload. ARP asks "who has IP x?" by broadcast; the
 * owner answers with its MAC; answers are kept for a few minutes. IPv4:
 * a 20-byte header (version, length, TTL, protocol, a checksum of the
 * header, source and destination) and the payload; packets larger than
 * the link (1500 bytes) would need fragments, which are not supported
 * (TCP keeps its segments small enough). ICMP echo request and reply are
 * ping.
 *
 * Routes come from the cards' settings: 127.0.0.0/8 on lo, each card's
 * own network, a default route through the first card with a gateway;
 * the longest matching prefix wins. With forwarding switched on, a packet
 * for someone else is passed on to the card its route names, its TTL one
 * less (ICMP "time exceeded" when none is left, "network unreachable"
 * without a route). There is no NAT: the networks on both sides must know
 * their way back through us.
 */
#include "ipv4.h"
#include "ipv6.h"
#include "inet.h"
#include "guard.h"
#include "arch.h"
#include "heap.h"
#include "kprintf.h"
#include "net.h"
#include "random.h"
#include "sched.h"
#include "string.h"
#include "sync.h"
#include "syscalls.h"
#include "tcp.h"
#include "udp.h"
#include "timer.h"

#define ETH_TYPE_ARP    0x0806
#define ETH_TYPE_IP     0x0800
#define ETH_TYPE_IP6    0x86DD
#define ARP_ENTRIES     16
#define ARP_LIFETIME    (300 * TIMER_HZ)    /* five minutes */

static const uint8_t broadcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)(v >> 16)), put16(p + 2, (uint16_t)v);
}

void ip_text(uint32_t ip, char *out, size_t size)
{
    ksnprintf(out, size, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
}

uint32_t ip_checksum_add(uint32_t sum, const void *data, size_t length)
{
    const uint8_t *p = data;
    for (size_t i = 0; i + 1 < length; i += 2)
        sum += get16(p + i);
    if (length & 1)
        sum += (uint32_t)p[length - 1] << 8;
    return sum;
}

uint16_t ip_checksum_fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

struct netdev *ip_interface(void)
{
    for (int i = 0; i < net_count(); i++)
        if (net_at(i)->address)
            return net_at(i);
    return net_count() ? net_at(0) : 0;
}

static uint32_t mask(uint32_t prefix)
{
    return prefix ? 0xFFFFFFFFu << (32 - prefix) : 0;
}

/* --- ARP ------------------------------------------------------------------------ */

static struct {
    uint32_t ip;
    uint8_t mac[6];
    uint64_t tick;
} arp[ARP_ENTRIES];
static struct spinlock arp_lock = SPINLOCK_INIT("arp");
static struct wait_queue arp_waiters;

static bool arp_lookup(uint32_t ip, uint8_t *mac)
{
    bool found = false;
    uint64_t state = spin_lock(&arp_lock);
    for (int i = 0; i < ARP_ENTRIES && !found; i++) {
        if (arp[i].ip == ip && timer_ticks() - arp[i].tick < ARP_LIFETIME) {
            memcpy(mac, arp[i].mac, 6);
            found = true;
        }
    }
    spin_unlock(&arp_lock, state);
    return found;
}

static void release_held(uint32_t hop, const uint8_t *mac);
static void hold(struct netdev *d, uint32_t hop, const uint8_t *packet, size_t length);

static void arp_learn(uint32_t ip, const uint8_t *mac)
{
    uint8_t kept[6];
    uint64_t state = spin_lock(&arp_lock);
    int slot = 0, found = -1;
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (arp[i].ip == ip) {
            found = i;
            break;
        }
        if (arp[i].tick < arp[slot].tick)
            slot = i;                               /* the oldest goes */
    }
    uint64_t now = timer_ticks() ? timer_ticks() : 1;
    if (found >= 0 && timer_ticks() - arp[found].tick < ARP_LIFETIME &&
        memcmp(arp[found].mac, mac, 6) != 0) {
        /* Sticky (49g): a fresh entry is not overwritten by a different MAC
         * (a gratuitous-ARP or flood MITM, or a spoof learned passively);
         * the first owner holds until it ages out. */
    } else if (found >= 0) {
        memcpy(arp[found].mac, mac, 6);             /* same MAC, or the old one expired */
        arp[found].tick = now;
    } else {
        arp[slot].ip = ip;
        memcpy(arp[slot].mac, mac, 6);
        arp[slot].tick = now;
        found = slot;
    }
    memcpy(kept, arp[found].mac, 6);
    spin_unlock(&arp_lock, state);
    wait_queue_wake_all(&arp_waiters);
    release_held(ip, kept);                         /* the MAC we kept, not necessarily this one */
}

bool arp_entry(int index, uint32_t *ip, uint8_t *mac)
{
    int seen = 0;
    bool found = false;
    uint64_t state = spin_lock(&arp_lock);
    for (int i = 0; i < ARP_ENTRIES && !found; i++) {
        if (!arp[i].ip || timer_ticks() - arp[i].tick >= ARP_LIFETIME)
            continue;
        if (seen++ == index) {
            *ip = arp[i].ip;
            memcpy(mac, arp[i].mac, 6);
            found = true;
        }
    }
    spin_unlock(&arp_lock, state);
    return found;
}

static void send_frame(struct netdev *d, const uint8_t *to, uint16_t type, const void *payload,
                       size_t length)
{
    uint8_t frame[ETH_FRAME_MAX];
    memcpy(frame, to, 6);
    memcpy(frame + 6, d->mac, 6);
    put16(frame + 12, type);
    memcpy(frame + ETH_HEADER, payload, length);
    d->send(d, frame, ETH_HEADER + length);
}

static void arp_send(struct netdev *d, uint16_t op, const uint8_t *to_mac, uint32_t to_ip)
{
    uint8_t p[28];
    put16(p, 1);                                    /* Ethernet */
    put16(p + 2, ETH_TYPE_IP);
    p[4] = 6, p[5] = 4;
    put16(p + 6, op);
    memcpy(p + 8, d->mac, 6);
    put32(p + 14, d->address);
    memcpy(p + 18, op == 1 ? (const uint8_t[6]){ 0 } : to_mac, 6);
    put32(p + 24, to_ip);
    send_frame(d, op == 1 ? broadcast_mac : to_mac, ETH_TYPE_ARP, p, sizeof(p));
}

static void arp_input(struct netdev *d, const uint8_t *p, size_t length)
{
    if (length < 28 || get16(p) != 1 || get16(p + 2) != ETH_TYPE_IP || p[4] != 6 || p[5] != 4)
        return;
    uint16_t op = get16(p + 6);
    uint32_t sender = get32(p + 14), target = get32(p + 24);
    if (sender)
        arp_learn(sender, p + 8);
    if (op == 1 && d->address && target == d->address)
        arp_send(d, 2, p + 8, sender);              /* "that is me" */
}

/* The MAC address for ip (asked by ARP: three times, half a second each). */
static bool arp_resolve(struct netdev *d, uint32_t ip, uint8_t *mac)
{
    uint32_t net_mask = mask(d->prefix);
    if (ip == 0xFFFFFFFFu || (d->prefix && (ip & ~net_mask) == ~net_mask)) {
        memcpy(mac, broadcast_mac, 6);
        return true;
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        if (arp_lookup(ip, mac))
            return true;
        arp_send(d, 1, 0, ip);
        wait_event_until(&arp_waiters, arp_lookup(ip, mac) || thread_current()->stop_requested,
                         timer_ticks() + TIMER_HZ / 2);
        if (thread_current()->stop_requested)
            return false;
    }
    return arp_lookup(ip, mac);
}

/* --- IPv4 ---------------------------------------------------------------------- */


/* src_mac: the Ethernet source of the frame p arrived in (0 for loopback
 * and other internally generated packets). */
static void ip_input(struct netdev *d, const uint8_t *src_mac, const uint8_t *p, size_t length);

/* The loopback interface lo: 127.0.0.1/8, there with or without a card
 * and always up. It has no MAC address and no driver: what it "sends"
 * goes into the loopback queue below; its counters are packets. */
static struct netdev lo = {
    .name = "lo",
    .driver = "loopback",
    .address = 0x7F000001,
    .prefix = 8,
};

struct netdev *ip_loopback(void)
{
    return &lo;
}

/* Is dst this machine: 127.0.0.0/8 or the address of one of its cards? */
static bool local(uint32_t dst)
{
    if ((dst >> 24) == 127)
        return true;
    for (int i = 0; i < net_count(); i++)
        if (net_at(i)->address && net_at(i)->address == dst)
            return true;
    return false;
}

/* The loopback queue: packets to ourselves wait there for the thread
 * "loopback", which delivers them as a card's thread delivers what
 * arrived. Delivered at once, inside ip_send, an answer would come back
 * inside its sender (TCP: data, its ACK, more data, ...) and the calls
 * would nest until the stack ran out. A full queue drops the packet, as a
 * full card's ring does. */
#define LOOP_QUEUE 128

static struct {
    uint16_t length;
    uint8_t data[1500];
} *loop_queue;
static unsigned loop_head, loop_count;
static struct spinlock loop_lock = SPINLOCK_INIT("loopback");
static struct wait_queue loop_waiters;
static struct mutex loop_start = MUTEX_INIT("loopback start");

static void loop_thread(void *context)
{
    (void)context;
    uint8_t *packet = kmalloc(sizeof(loop_queue[0].data));
    for (;;) {
        wait_event(&loop_waiters, loop_count > 0);
        uint64_t state = spin_lock(&loop_lock);
        size_t length = loop_queue[loop_head].length;
        memcpy(packet, loop_queue[loop_head].data, length);
        loop_head = (loop_head + 1) % LOOP_QUEUE;
        loop_count--;
        spin_unlock(&loop_lock, state);
        lo.rx_frames++;
        lo.rx_bytes += length;
        if (packet[0] >> 4 == 6)
            ip6_input(&lo, 0, packet, length);      /* ::1 and our own IPv6 addresses (64a) */
        else
            ip_input(&lo, 0, packet, length);
    }
}

void ip_loop_send(const uint8_t *packet, size_t length)
{
    if (!loop_queue) {
        mutex_lock(&loop_start);
        if (!loop_queue) {
            void *q = kmalloc(LOOP_QUEUE * sizeof(loop_queue[0]));
            if (q && thread_create("loopback", loop_thread, 0))
                loop_queue = q;
            else
                kfree(q);
        }
        mutex_unlock(&loop_start);
        if (!loop_queue) {
            lo.tx_dropped++;
            return;
        }
    }
    uint64_t state = spin_lock(&loop_lock);
    if (loop_count == LOOP_QUEUE) {
        lo.tx_dropped++;
    } else {
        lo.tx_frames++;
        lo.tx_bytes += length;
        unsigned at = (loop_head + loop_count) % LOOP_QUEUE;
        loop_queue[at].length = (uint16_t)length;
        memcpy(loop_queue[at].data, packet, length);
        loop_count++;
    }
    spin_unlock(&loop_lock, state);
    wait_queue_wake_all(&loop_waiters);
}

/* --- routes --------------------------------------------------------------- */

#define MAX_ROUTES (MAX_NETDEVS + 2)       /* lo, each card's own network, the default */

/* The default route's preferred cards (50g, `net default`), by name: a
 * card not plugged in now may be listed. */
static char preferred[MAX_NETDEVS][8];
static int preferred_count;
static struct spinlock preferred_lock = SPINLOCK_INIT("default route");

void ip_default_reset(void)
{
    uint64_t s = spin_lock(&preferred_lock);
    preferred_count = 0;
    spin_unlock(&preferred_lock, s);
}

int ip_default_add(const char *name)
{
    uint64_t s = spin_lock(&preferred_lock);
    int r = 0;
    bool there = false;
    for (int i = 0; i < preferred_count && !there; i++)
        there = !strcmp(preferred[i], name);
    if (!there && preferred_count < MAX_NETDEVS)
        memcpy(preferred[preferred_count++], name, 8);
    else if (!there)
        r = -1;
    spin_unlock(&preferred_lock, s);
    return r;
}

int ip_default_order(char (*names)[8], int max)
{
    uint64_t s = spin_lock(&preferred_lock);
    int n = preferred_count < max ? preferred_count : max;
    memcpy(names, preferred, (size_t)n * 8);
    spin_unlock(&preferred_lock, s);
    return n;
}

/* Can d carry the default route now? */
static bool gateway_card(struct netdev *d)
{
    return d->address && d->gateway && (!d->link_speed || net_link(d));
}

struct route {
    uint32_t net, prefix, gateway;      /* gateway 0: on the card's own network */
    struct netdev *d;
};

/* The routing table, made from the cards' settings. */
static int routes(struct route *r)
{
    int n = 0;
    r[n++] = (struct route){ 0x7F000000, 8, 0, &lo };
    for (int i = 0; i < net_count() && n < MAX_ROUTES - 1; i++) {
        struct netdev *d = net_at(i);
        if (d->address)
            r[n++] = (struct route){ d->address & mask(d->prefix), d->prefix, 0, d };
    }
    /* The default: the first preferred card that can carry it, else the
     * first card with a gateway and a link (the cable may be out). */
    struct netdev *chosen = 0;
    uint64_t s = spin_lock(&preferred_lock);
    for (int k = 0; k < preferred_count && !chosen; k++)
        for (int i = 0; i < net_count() && !chosen; i++)
            if (!strcmp(net_at(i)->name, preferred[k]) && gateway_card(net_at(i)))
                chosen = net_at(i);
    spin_unlock(&preferred_lock, s);
    for (int i = 0; i < net_count() && !chosen; i++)
        if (gateway_card(net_at(i)))
            chosen = net_at(i);
    if (chosen)
        r[n++] = (struct route){ 0, 0, chosen->gateway, chosen };
    return n;
}

/* The card a packet for dst leaves on and its next hop (dst itself on the
 * card's own network); 0 without a route. */
static struct netdev *route(uint32_t dst, uint32_t *hop)
{
    struct route r[MAX_ROUTES];
    int n = routes(r), best = -1;
    for (int i = 0; i < n; i++) {
        struct netdev *d = r[i].d;
        if (d->link_speed && !net_link(d))
            continue;                               /* no link: it cannot deliver (the cable
                                                     * is out); another card may */
        if ((dst & mask(r[i].prefix)) == r[i].net && (best < 0 || r[i].prefix > r[best].prefix))
            best = i;
    }
    if (best < 0)
        return 0;
    *hop = r[best].gateway ? r[best].gateway : dst;
    return r[best].d;
}

uint32_t ip_source(uint32_t dst)
{
    if (local(dst))
        return dst;
    uint32_t hop;
    struct netdev *d = route(dst, &hop);
    return d ? d->address : 0;
}

void ip_print_routes(void)
{
    struct route r[MAX_ROUTES];
    int n = routes(r);
    for (int i = 0; i < n; i++) {
        char net[16], gateway[16];
        ip_text(r[i].net, net, sizeof(net));
        ip_text(r[i].gateway, gateway, sizeof(gateway));
        if (r[i].prefix == 0)
            kprintf("route: default via %s on %s\n", gateway, r[i].d->name);
        else
            kprintf("route: %s/%u on %s\n", net, r[i].prefix, r[i].d->name);
    }
    /* (50g) The preference, when there is one: "default route: wlan0 eth1". */
    char list[MAX_NETDEVS * 9 + 1];
    size_t used = 0;
    uint64_t s = spin_lock(&preferred_lock);
    for (int i = 0; i < preferred_count; i++)
        used += (size_t)ksnprintf(list + used, sizeof(list) - used, "%s%s", i ? " " : "", preferred[i]);
    int count = preferred_count;
    spin_unlock(&preferred_lock, s);
    if (count)
        kprintf("default route: %s, then any card with a gateway\n", list);
}

/* --- sending --------------------------------------------------------------- */

static void put_header(uint8_t *packet, uint32_t src, uint32_t dst, uint8_t protocol, size_t length)
{
    packet[0] = 0x45;                               /* version 4, 20-byte header */
    packet[1] = 0;
    put16(packet + 2, (uint16_t)(20 + length));
    uint16_t id;
    random_bytes(&id, sizeof(id));
    put16(packet + 4, id);                          /* random: no idle scan, no DF leak */
    put16(packet + 6, 0x4000);                      /* don't fragment */
    packet[8] = 64;                                 /* time to live */
    packet[9] = protocol;
    put16(packet + 10, 0);
    put32(packet + 12, src);
    put32(packet + 16, dst);
    put16(packet + 10, ip_checksum_fold(ip_checksum_add(0, packet, 20)));
}

/* Send a packet from src (0: ip_source) to dst; a broadcast (to
 * 255.255.255.255) leaves on card d (0: the first with an address). */
static int send_on(struct netdev *d, uint32_t src, uint32_t dst, uint8_t protocol,
                   const void *payload, size_t length)
{
    if (length > IP_PAYLOAD_MAX)
        return ERR_INVAL;
    uint32_t hop = dst;
    bool broadcast = dst == 0xFFFFFFFFu, here = local(dst);
    if (broadcast)
        d = d ? d : ip_interface();
    else if (!here && !(d = route(dst, &hop)))
        return ERR_NETUNREACH;                      /* no route */
    if (!src)
        src = here ? dst : d ? d->address : 0;
    uint8_t packet[1500];
    put_header(packet, src, dst, protocol, length);
    memcpy(packet + 20, payload, length);
    if (here) {
        ip_loop_send(packet, 20 + length);             /* never leaves the machine */
        return 0;
    }
    if (!d || (!d->address && !broadcast) || !net_link(d))
        return ERR_NETUNREACH;                      /* a broadcast needs no address (DHCP) */
    if (!guard_check_out(d, packet, 20 + length))
        return ERR_PERM;                            /* the firewall's: not allowed out */
    uint8_t mac[6];
    uint32_t net_mask = mask(d->prefix);
    if (hop == 0xFFFFFFFFu || (d->prefix && (hop & ~net_mask) == ~net_mask)) {
        send_frame(d, broadcast_mac, ETH_TYPE_IP, packet, 20 + length);   /* a broadcast */
        return 0;
    }
    if (arp_lookup(hop, mac)) {
        send_frame(d, mac, ETH_TYPE_IP, packet, 20 + length);
        return 0;
    }
    /* No ARP entry. In a card's receive thread we must not wait for the
     * answer (the ring would overflow and the TCP mutex be held): queue the
     * packet and ask by ARP, as forwarding does; arp_learn sends it when the
     * reply comes (49c). Elsewhere (a program's thread, dhcp, the link
     * watcher) it is fine to wait. */
    if (thread_current()->net_rx) {
        hold(d, hop, packet, 20 + length);
        return 0;
    }
    if (!arp_resolve(d, hop, mac))
        return thread_current()->stop_requested ? ERR_INTR : ERR_HOSTUNREACH;
    send_frame(d, mac, ETH_TYPE_IP, packet, 20 + length);
    return 0;
}

int ip_send(uint32_t dst, uint8_t protocol, const void *payload, size_t length)
{
    return send_on(0, 0, dst, protocol, payload, length);
}

int ip_send_from(uint32_t src, uint32_t dst, uint8_t protocol, const void *payload, size_t length)
{
    return send_on(0, src, dst, protocol, payload, length);
}

int ip_broadcast(struct netdev *d, uint8_t protocol, const void *payload, size_t length)
{
    return send_on(d, 0, 0xFFFFFFFFu, protocol, payload, length);
}

/* --- forwarding ------------------------------------------------------------ */

static bool forwarding;
static struct {
    uint64_t forwarded, no_route, time_exceeded, no_arp;
} fwd_stats;

/* Packets to forward whose next hop's MAC address ARP is still asking
 * for (the card's thread must not wait for the answer): sent when it
 * comes, dropped after ARP_WAIT. */
#define HELD            8
#define ARP_WAIT        (TIMER_HZ * 3 / 2)

static struct held {
    struct netdev *d;
    uint32_t hop;
    uint64_t since;                     /* 0: a free slot */
    uint16_t length;
    uint8_t data[1500];
} *held;
static struct spinlock held_lock = SPINLOCK_INIT("forward");

static void release_held(uint32_t hop, const uint8_t *mac)
{
    if (!held)
        return;
    for (int i = 0; i < HELD; i++) {
        uint8_t packet[1500];
        struct netdev *d = 0;
        size_t length = 0;
        uint64_t state = spin_lock(&held_lock);
        if (held[i].since && held[i].hop == hop) {
            d = held[i].d, length = held[i].length;
            memcpy(packet, held[i].data, length);
            held[i].since = 0;
        }
        spin_unlock(&held_lock, state);
        if (d) {
            send_frame(d, mac, ETH_TYPE_IP, packet, length);
            fwd_stats.forwarded++;
        }
    }
}

static void hold(struct netdev *d, uint32_t hop, const uint8_t *packet, size_t length)
{
    if (!held) {
        void *h = kmalloc(HELD * sizeof(*held));
        if (!h)
            return;
        memset(h, 0, HELD * sizeof(*held));
        uint64_t state = spin_lock(&held_lock);
        if (!held)
            held = h, h = 0;
        spin_unlock(&held_lock, state);
        kfree(h);
    }
    int slot = -1;
    uint64_t now = timer_ticks() ? timer_ticks() : 1;
    uint64_t state = spin_lock(&held_lock);
    for (int i = 0; i < HELD; i++) {
        if (held[i].since && now - held[i].since > ARP_WAIT) {
            held[i].since = 0;                      /* nobody answered */
            fwd_stats.no_arp++;
        }
        if (!held[i].since && slot < 0)
            slot = i;
    }
    if (slot >= 0) {
        held[slot].d = d, held[slot].hop = hop, held[slot].since = now;
        held[slot].length = (uint16_t)length;
        memcpy(held[slot].data, packet, length);
    } else {
        fwd_stats.no_arp++;                         /* all slots wait already */
    }
    spin_unlock(&held_lock, state);
    if (slot >= 0)
        arp_send(d, 1, 0, hop);
}

/* --- masquerade: source NAT for forwarded IPv4 (49i) --------------------------
 *
 * IPv4 only. One or more cards are marked "private" (the setting names the
 * private networks, which may be several, not the public ones). When a
 * packet from a private card is forwarded out a card that is not private,
 * its source is rewritten to that outgoing card's address, so several inside
 * hosts can share it: TCP/UDP get a fresh source port, ICMP echo a fresh
 * identifier, kept in a table so the reply (addressed to that card) can be
 * rewritten back and forwarded to the inside host. Set by `net masquerade
 * ethN ...`. */
#define NAT_ENTRIES 512
#define NAT_LOW     20000                   /* the range of rewritten ports */
#define NAT_HIGH    60000

static struct netdev *private_cards[MAX_NETDEVS];   /* the marked private networks */
static int private_count;
static struct nat {
    uint64_t last;                          /* timer ticks; 0: free */
    uint32_t inside, remote, outside;       /* the inside host, the far side, the card's address used */
    uint16_t inside_port, remote_port;      /* ICMP echo: the identifier in inside_port */
    uint16_t assigned;                      /* the rewritten source port / identifier */
    uint8_t protocol;
} nat[NAT_ENTRIES];
static struct spinlock nat_lock = SPINLOCK_INIT("nat");
static struct { uint64_t out, in; } nat_stats;
static uint16_t nat_next = NAT_LOW;

void ip_masquerade_reset(void)
{
    private_count = 0;
}

int ip_masquerade_add(struct netdev *d)         /* mark a card private; 0, or -1 if full */
{
    for (int i = 0; i < private_count; i++)
        if (private_cards[i] == d)
            return 0;                           /* already marked */
    if (private_count >= MAX_NETDEVS)
        return -1;
    private_cards[private_count++] = d;
    return 0;
}

static bool masq_private(const struct netdev *d)
{
    for (int i = 0; i < private_count; i++)
        if (private_cards[i] == d)
            return true;
    return false;
}

bool ip_masquerade_on(void)
{
    return private_count > 0;
}

static uint64_t nat_idle(uint8_t protocol)
{
    return protocol == IP_PROTO_TCP ? (uint64_t)TIMER_HZ * 600
         : protocol == IP_PROTO_UDP ? (uint64_t)TIMER_HZ * 60 : (uint64_t)TIMER_HZ * 30;
}

static uint32_t pseudo4(uint32_t src, uint32_t dst, uint8_t protocol, size_t length)
{
    uint8_t h[12];
    put32(h, src);
    put32(h + 4, dst);
    h[8] = 0, h[9] = protocol;
    put16(h + 10, (uint16_t)length);
    return ip_checksum_add(0, h, sizeof(h));
}

/* The IP header checksum, and the transport checksum after a rewrite
 * (TCP, UDP with a checksum, or ICMP: no pseudo-header). */
static void fix_checksums(uint8_t *p, size_t total)
{
    size_t header = (size_t)(p[0] & 15) * 4;
    put16(p + 10, 0);
    put16(p + 10, ip_checksum_fold(ip_checksum_add(0, p, header)));
    uint8_t proto = p[9], *t = p + header;
    size_t n = total - header;
    if (proto == IP_PROTO_TCP && n >= 20) {
        put16(t + 16, 0);
        put16(t + 16, ip_checksum_fold(ip_checksum_add(pseudo4(get32(p + 12), get32(p + 16), proto, n),
                                                       t, n)));
    } else if (proto == IP_PROTO_UDP && n >= 8 && get16(t + 6) != 0) {
        put16(t + 6, 0);
        uint16_t c = ip_checksum_fold(ip_checksum_add(pseudo4(get32(p + 12), get32(p + 16), proto, n),
                                                      t, n));
        put16(t + 6, c ? c : 0xFFFF);           /* 0 would mean "no checksum" */
    } else if (proto == IP_PROTO_ICMP && n >= 8) {
        put16(t + 2, 0);
        put16(t + 2, ip_checksum_fold(ip_checksum_add(0, t, n)));
    }
}

/* Is this packet one masquerade can rewrite (TCP, UDP, or an ICMP echo)?
 * Fills the inside port/identifier and the remote port. */
static bool nat_ports(const uint8_t *p, size_t total, uint16_t *local_port, uint16_t *remote_port,
                      bool *portbased)
{
    size_t header = (size_t)(p[0] & 15) * 4, n = total - header;
    const uint8_t *t = p + header;
    if ((p[9] == IP_PROTO_TCP || p[9] == IP_PROTO_UDP) && n >= 4) {
        *local_port = get16(t), *remote_port = get16(t + 2), *portbased = true;
        return true;
    }
    if (p[9] == IP_PROTO_ICMP && n >= 8 && (t[0] == 8 || t[0] == 0)) {
        *local_port = *remote_port = get16(t + 4), *portbased = false;   /* the identifier */
        return true;
    }
    return false;
}

/* A packet from a private card being forwarded out card `out` (not
 * private): rewrite its source to out's address and its source
 * port/identifier to a table slot's. */
static void masq_outbound(uint8_t *p, size_t total, const struct netdev *out)
{
    uint16_t inside_port, remote_port;
    bool portbased;
    if (!nat_ports(p, total, &inside_port, &remote_port, &portbased))
        return;                                     /* leave what cannot be masqueraded */
    uint32_t inside = get32(p + 12), remote = get32(p + 16);
    uint8_t proto = p[9];
    uint64_t now = timer_ticks() ? timer_ticks() : 1;
    uint16_t assigned = 0;
    uint64_t s = spin_lock(&nat_lock);
    struct nat *slot = 0, *oldest = &nat[0];
    for (int k = 0; k < NAT_ENTRIES; k++) {
        struct nat *e = &nat[k];
        bool live = e->last && now - e->last <= nat_idle(e->protocol);
        if (live && e->protocol == proto && e->inside == inside && e->remote == remote &&
            e->inside_port == inside_port && e->remote_port == remote_port) {
            slot = e;
            break;
        }
        if (!e->last || e->last < oldest->last)
            oldest = e;
    }
    if (slot) {
        assigned = slot->assigned;
    } else {
        for (int tries = 0; tries < NAT_HIGH - NAT_LOW; tries++) {   /* a port free for this protocol */
            uint16_t cand = nat_next++;
            if (nat_next > NAT_HIGH)
                nat_next = NAT_LOW;
            bool taken = false;
            for (int k = 0; k < NAT_ENTRIES && !taken; k++)
                taken = nat[k].last && now - nat[k].last <= nat_idle(nat[k].protocol) &&
                        nat[k].protocol == proto && nat[k].assigned == cand;
            if (!taken) {
                assigned = cand;
                break;
            }
        }
        slot = oldest;
        slot->protocol = proto, slot->inside = inside, slot->remote = remote;
        slot->inside_port = inside_port, slot->remote_port = remote_port, slot->assigned = assigned;
    }
    slot->outside = out->address;
    slot->last = now;
    spin_unlock(&nat_lock, s);
    size_t header = (size_t)(p[0] & 15) * 4;
    put32(p + 12, out->address);
    if (portbased)
        put16(p + header, assigned);
    else
        put16(p + header + 4, assigned);
    fix_checksums(p, total);
    nat_stats.out++;
}

/* A packet arriving at a non-private card's own address: if it is the reply
 * to a masqueraded connection (rewritten to that address), rewrite the
 * destination back to the inside host (the caller then forwards it in). */
static bool masq_inbound(uint8_t *p, size_t total)
{
    uint16_t remote_port, assigned;
    bool portbased;
    if (!nat_ports(p, total, &remote_port, &assigned, &portbased))
        return false;
    uint32_t remote = get32(p + 12);                /* the far side: the reply's source */
    uint32_t to = get32(p + 16);                    /* the card address it was masqueraded to */
    uint8_t proto = p[9];
    uint64_t now = timer_ticks();
    uint32_t inside = 0;
    uint16_t inside_port = 0;
    uint64_t s = spin_lock(&nat_lock);
    for (int k = 0; k < NAT_ENTRIES; k++) {
        struct nat *e = &nat[k];
        if (!e->last || now - e->last > nat_idle(e->protocol) || e->protocol != proto)
            continue;
        if (e->assigned == assigned && e->remote == remote && e->outside == to &&
            (!portbased || e->remote_port == remote_port)) {
            e->last = now ? now : 1;
            inside = e->inside, inside_port = e->inside_port;
            break;
        }
    }
    spin_unlock(&nat_lock, s);
    if (!inside)
        return false;
    size_t header = (size_t)(p[0] & 15) * 4;
    put32(p + 16, inside);
    if (portbased)
        put16(p + header + 2, inside_port);
    else
        put16(p + header + 4, inside_port);
    fix_checksums(p, total);
    nat_stats.in++;
    return true;
}

/* An ICMP error about packet p (it came in on card d): type 11 (time
 * exceeded) or 3 (unreachable), with the packet's header and its first 8
 * bytes, from our address on that card. Never about an ICMP error. */
static void icmp_error(struct netdev *d, uint8_t type, uint8_t code, const uint8_t *p, size_t length)
{
    size_t header = (size_t)(p[0] & 15) * 4;
    if (p[9] == IP_PROTO_ICMP && length > header && p[header] != 8 && p[header] != 0)
        return;
    size_t quoted = header + 8 < length ? header + 8 : length;
    uint8_t m[8 + 60 + 8];
    memset(m, 0, 8);
    m[0] = type, m[1] = code;
    memcpy(m + 8, p, quoted);
    put16(m + 2, ip_checksum_fold(ip_checksum_add(0, m, 8 + quoted)));
    send_on(0, d->address, get32(p + 12), IP_PROTO_ICMP, m, 8 + quoted);
}

/* A packet that came in on card d for someone else. */
static void forward(struct netdev *d, const uint8_t *p, size_t total)
{
    uint32_t src = get32(p + 12), dst = get32(p + 16);
    if (!forwarding || d == &lo || !d->address || src == 0 || (src >> 24) == 127 ||
        (dst >> 28) >= 14 || (dst >> 24) == 127)
        return;                                     /* not to be passed on */
    for (int i = 0; i < net_count(); i++) {
        struct netdev *c = net_at(i);
        if (c->prefix && c->prefix < 32 && dst == (c->address | ~mask(c->prefix)))
            return;                                 /* a network's broadcast */
    }
    if (p[8] <= 1) {
        fwd_stats.time_exceeded++;
        icmp_error(d, 11, 0, p, total);
        return;
    }
    uint32_t hop;
    struct netdev *out = route(dst, &hop);
    if (!out || out == &lo || !net_link(out)) {
        fwd_stats.no_route++;
        icmp_error(d, 3, 0, p, total);              /* network unreachable */
        return;
    }
    if (!guard_check_forward(d, out, p, total))
        return;                                     /* the firewall's forward chain (49h) */
    uint8_t packet[1500];
    memcpy(packet, p, total);
    packet[8]--;
    size_t header = (size_t)(p[0] & 15) * 4;
    put16(packet + 10, 0);
    put16(packet + 10, ip_checksum_fold(ip_checksum_add(0, packet, header)));
    if (masq_private(d) && !masq_private(out) && out->address)
        masq_outbound(packet, total, out);          /* source NAT: private -> public (49i) */
    uint8_t mac[6];
    if (arp_lookup(hop, mac)) {
        send_frame(out, mac, ETH_TYPE_IP, packet, total);
        fwd_stats.forwarded++;
    } else {
        hold(out, hop, packet, total);
    }
}

void ip_set_forwarding(bool on)
{
    forwarding = on;
}

bool ip_forwarding(void)
{
    return forwarding;
}

void ip_print_forwarding(void)
{
    kprintf("forwarding: %s; %lu packets forwarded, not: %lu without a route, %lu out of time "
            "(TTL), %lu without an ARP answer\n", forwarding ? "on" : "off", fwd_stats.forwarded,
            fwd_stats.no_route, fwd_stats.time_exceeded, fwd_stats.no_arp);
    if (private_count) {
        char cards[64] = "";
        for (int i = 0; i < private_count; i++)
            ksnprintf(cards + strlen(cards), sizeof(cards) - strlen(cards), "%s%s",
                      i ? ", " : "", private_cards[i]->name);
        kprintf("masquerade: source NAT, private: %s; %lu rewritten out, %lu back\n", cards,
                nat_stats.out, nat_stats.in);
    } else {
        kprintf("masquerade: off\n");
    }
}

static void icmp_input(uint32_t src, uint32_t dst, const uint8_t *p, size_t length);

static void ip_input(struct netdev *d, const uint8_t *src_mac, const uint8_t *p, size_t length)
{
    if (length < 20 || p[0] >> 4 != 4)
        return;
    size_t header = (size_t)(p[0] & 15) * 4, total = get16(p + 2);
    if (header < 20 || total < header || total > length ||
        ip_checksum_fold(ip_checksum_add(0, p, header)) != 0)
        return;
    if (get16(p + 6) & 0x3FFF)
        return;                                     /* a fragment: not supported */
    uint32_t src = get32(p + 12), dst = get32(p + 16);
    if (d != &lo) {
        /* Martian sources from a network: 127.x, our own addresses (a spoof
         * reflecting our replies to ourselves: dnsd then looped on one
         * packet), the broadcast or a multicast/reserved address. 0.0.0.0
         * is allowed (a host with no address yet: DHCP). */
        if ((dst >> 24) == 127)
            return;                                 /* 127.x never comes from a network */
        if (src != 0 && ((src >> 24) == 127 || src == 0xFFFFFFFFu || (src >> 28) >= 0xE ||
                         local(src)))
            return;
        /* Learn an on-link sender's MAC from the frame it came in (49c), so
         * a reply to it finds an ARP entry and the receive thread need not
         * wait. Only for a source on this card's own network (one reached
         * through the gateway has the gateway's MAC, not its own). */
        if (src_mac && src != 0 && d->prefix &&
            (src & mask(d->prefix)) == (d->address & mask(d->prefix)))
            arp_learn(src, src_mac);
    }
    /* A reply to a masqueraded connection arrives at a non-private card's
     * own address (49i): rewrite the destination back to the inside host and
     * forward it in, before it would be taken as ours. */
    if (ip_masquerade_on() && d != &lo && !masq_private(d) && dst == d->address && total <= 1500) {
        uint8_t copy[1500];
        memcpy(copy, p, total);
        if (masq_inbound(copy, total)) {
            forward(d, copy, total);
            return;
        }
    }
    bool ours = local(dst) || dst == 0xFFFFFFFFu ||
                (d && d->prefix && dst == (d->address | ~mask(d->prefix)));
    if (!ours) {
        forward(d, p, total);
        return;
    }
    if (d != &lo && !guard_check_in(d, p, total))
        return;                                     /* the firewall's */
    struct inet_addr from = inet_v4(src), to = inet_v4(dst);
    if (p[9] == IP_PROTO_ICMP)
        icmp_input(src, dst, p + header, total - header);
    else if (p[9] == IP_PROTO_UDP)
        udp_input(d, &from, &to, p + header, total - header);
    else if (p[9] == IP_PROTO_TCP)
        tcp_input(&from, &to, p + header, total - header);
}

void ether_input(struct netdev *d, const uint8_t *frame, size_t length)
{
    if (length < ETH_HEADER)
        return;
    uint16_t type = get16(frame + 12);
    if (type == ETH_TYPE_ARP)
        arp_input(d, frame + ETH_HEADER, length - ETH_HEADER);
    else if (type == ETH_TYPE_IP)
        ip_input(d, frame + 6, frame + ETH_HEADER, length - ETH_HEADER);   /* frame+6: source MAC */
    else if (type == ETH_TYPE_IP6)
        ip6_input(d, frame + 6, frame + ETH_HEADER, length - ETH_HEADER);
}

/* --- ICMP echo --------------------------------------------------------------- */

#define PINGS 8

struct ping {
    bool waiting, answered;
    uint32_t from;
    uint16_t id, seq;
};
static struct ping pings[PINGS];
static struct spinlock ping_lock = SPINLOCK_INIT("ping");
static struct wait_queue ping_waiters;

static void icmp_input(uint32_t src, uint32_t dst, const uint8_t *p, size_t length)
{
    if (length < 8 || ip_checksum_fold(ip_checksum_add(0, p, length)) != 0)
        return;
    if (p[0] == 8 && length <= IP_PAYLOAD_MAX && local(dst)) {
        /* An echo request addressed to one of our own addresses: answer it,
         * from that address. A request to a broadcast or multicast address
         * is not answered (anti-smurf, 49g: we must not be an amplifier). */
        uint8_t reply[IP_PAYLOAD_MAX];
        memcpy(reply, p, length);
        reply[0] = 0;
        put16(reply + 2, 0);
        put16(reply + 2, ip_checksum_fold(ip_checksum_add(0, reply, length)));
        ip_send_from(dst, src, IP_PROTO_ICMP, reply, length);
    } else if (p[0] == 0) {                         /* echo reply: someone's ping */
        uint64_t state = spin_lock(&ping_lock);
        for (int i = 0; i < PINGS; i++)
            if (pings[i].waiting && pings[i].from == src && pings[i].id == get16(p + 4) &&
                pings[i].seq == get16(p + 6))
                pings[i].answered = true;
        spin_unlock(&ping_lock, state);
        wait_queue_wake_all(&ping_waiters);
    }
}

int64_t icmp_ping(uint32_t dst, uint16_t id, uint16_t seq, size_t size, uint32_t timeout_ms)
{
    if (size > IP_PAYLOAD_MAX - 8)
        return -ERR_INVAL;
    int slot = -1;
    uint64_t state = spin_lock(&ping_lock);
    for (int i = 0; i < PINGS && slot < 0; i++)
        if (!pings[i].waiting)
            slot = i;
    if (slot >= 0)
        pings[slot] = (struct ping){ true, false, dst, id, seq };
    spin_unlock(&ping_lock, state);
    if (slot < 0)
        return -ERR_AGAIN;
    uint8_t request[IP_PAYLOAD_MAX];
    request[0] = 8, request[1] = 0;
    put16(request + 2, 0);
    put16(request + 4, id);
    put16(request + 6, seq);
    for (size_t i = 0; i < size; i++)
        request[8 + i] = (uint8_t)i;
    put16(request + 2, ip_checksum_fold(ip_checksum_add(0, request, 8 + size)));
    uint64_t start = arch_counter();
    int r = ip_send(dst, IP_PROTO_ICMP, request, 8 + size);
    int64_t result;
    if (r) {
        result = -r;
    } else {
        uint64_t deadline = timer_ticks() + (timeout_ms * TIMER_HZ + 999) / 1000;
        wait_event_until(&ping_waiters, pings[slot].answered || thread_current()->stop_requested,
                         deadline);
        uint64_t hz = arch_counter_hz();
        result = pings[slot].answered ? (int64_t)((arch_counter() - start) * 1000000 / (hz ? hz : 1))
               : thread_current()->stop_requested ? -ERR_INTR : -ERR_TIMEDOUT;
    }
    state = spin_lock(&ping_lock);
    pings[slot].waiting = false;
    spin_unlock(&ping_lock, state);
    return result;
}

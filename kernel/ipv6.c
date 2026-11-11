/*
 * IPv6 over Ethernet (64a; see ipv6.h).
 *
 * An IPv6 packet: a 40-byte header (version 6, the payload's length, the
 * next header's kind, a hop limit, 16-byte source and destination), maybe
 * extension headers, then the payload; Ethernet type 0x86DD. There is no
 * broadcast: what IPv4 broadcasts goes to multicast groups (ff02::1 all
 * the link's machines; a machine's "solicited-node" group ff02::1:ffXX:XXXX
 * from its address's last 24 bits, so a question about one address wakes
 * few machines). A group's frames go to the MAC 33:33 + the group's last
 * 32 bits: each card is asked for its groups' MACs (net_set_multicast;
 * how it filters is its driver's), and ip6_input keeps only what is for
 * a group the card is in (a card may let more through).
 *
 * Neighbour discovery (RFC 4861) is IPv6's ARP, in ICMPv6: a neighbour
 * solicitation for an address goes to its solicited-node group with the
 * asker's MAC, the owner answers with a neighbour advertisement and its
 * MAC; such messages must arrive with hop limit 255 (nobody forwarded
 * them, they came from this link). A card's link-local address (fe80::
 * and its MAC made into 64 bits, EUI-64: the 7th bit flipped, ff:fe in the
 * middle) is checked first (duplicate address detection, RFC 4862): a
 * solicitation from :: for it after a random wait of up to a second; an
 * answer, or another machine's own check for the same address, within a
 * second makes it a duplicate, never used. Before it, an MLD report (RFC
 * 3810) tells switches that listen for them which groups the card is in,
 * or they may not pass the solicitations on.
 *
 * Addresses beyond the link (64b, SLAAC, RFC 4862): with its link-local
 * address in use a card asks the routers (a router solicitation to
 * ff02::2); their advertisements name the default router, the prefixes
 * on the link and those to make addresses in (the prefix and the card's
 * interface ID), the link's MTU and a DNS server, each for a lifetime.
 * A card set to a fixed address (SYS_NETSET6) takes no advertisement.
 * Routes: a prefix on a card's link goes there, the rest to the default
 * router (net default's order chooses the card, as for IPv4).
 *
 * UDP and TCP (64c) are udp.c's and tcp.c's for both families (struct
 * inet_addr, inet.h, whose senders are at the end of this file). A packet
 * that came in pieces (a fragment header) is put back together, up to
 * FRAGMENTED_MAX; RitterOS sends none (TCP's segments fit, a datagram
 * bigger than a packet is refused). An ICMPv6 "packet too big" makes the
 * TCP connection it names send smaller segments. The firewall checks
 * every packet as it does IPv4's (guard.c; until 64e's IPv6 rules, the
 * rules that name no address count, and neighbour discovery always
 * passes).
 */
#include "ipv6.h"
#include "ipv4.h"
#include "ip6text.h"
#include "kprintf.h"
#include "net.h"
#include "random.h"
#include "sched.h"
#include "string.h"
#include "sync.h"
#include "syscalls.h"
#include "timer.h"
#include "heap.h"
#include "arch.h"
#include "guard.h"
#include "inet.h"
#include "tcp.h"
#include "udp.h"

#define ETH_TYPE_IP6        0x86DD

#define NEXT_HOP_BY_HOP     0           /* extension headers and payloads (the next header) */
#define NEXT_TCP            6
#define NEXT_UDP            17
#define NEXT_ROUTING        43
#define NEXT_FRAGMENT       44
#define NEXT_ICMP6          58
#define NEXT_DESTINATION    60

#define ICMP6_TOO_BIG       2
#define ICMP6_ECHO_REQUEST  128
#define ICMP6_ECHO_REPLY    129
#define MLD_QUERY           130
#define ROUTER_SOLICIT      133
#define ROUTER_ADVERT       134
#define ND_SOLICIT          135
#define ND_ADVERT           136
#define MLD2_REPORT         143

#define MLD_JOINED          4           /* MLDv2 record types: CHANGE_TO_EXCLUDE (a group joined) */
#define MLD_IS_IN           2           /* MODE_IS_EXCLUDE (an answer to a query) */

#define NEIGHBOURS          32
#define NEIGHBOUR_LIFETIME  (300 * TIMER_HZ)    /* five minutes, as ARP's */
#define RETRANS             TIMER_HZ            /* RFC 4861's RetransTimer: a second */
#define SOLICITS            3                   /* questions before giving up */
#define ROUTER_SOLICITS     3                   /* RFC 4861's MAX_RTR_SOLICITATIONS */
#define ROUTER_INTERVAL     (4 * TIMER_HZ)      /* RTR_SOLICITATION_INTERVAL */
#define TWO_HOURS           (7200ull * TIMER_HZ)

static const uint8_t unspecified[16];
static const uint8_t loopback6[16] = { [15] = 1 };
static const uint8_t all_nodes[16] = { 0xFF, 0x02, [15] = 1 };
static const uint8_t mld_listeners[16] = { 0xFF, 0x02, [15] = 0x16 };     /* MLDv2 routers */
static const uint8_t all_routers[16] = { 0xFF, 0x02, [15] = 2 };

/* Every card's ip6[] and the neighbours. Never held while sending. */
static struct spinlock lock = SPINLOCK_INIT("ipv6");

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)get16(p) << 16 | get16(p + 2);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v;
}

static bool same(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 16) == 0;
}

static bool is_multicast(const uint8_t *a)
{
    return a[0] == 0xFF;
}

static bool is_link_local(const uint8_t *a)
{
    return a[0] == 0xFE && (a[1] & 0xC0) == 0x80;
}

/* ff02::1:ffXX:XXXX, the group of the addresses ending in a's last 24 bits. */
static void solicited_group(const uint8_t *a, uint8_t *out)
{
    static const uint8_t base[13] = { 0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xFF };
    memcpy(out, base, 13);
    memcpy(out + 13, a + 13, 3);
}

static bool is_solicited_group(const uint8_t *a)
{
    uint8_t g[16];
    solicited_group(a, g);
    return same(a, g);
}

/* fe80:: + the MAC as an EUI-64 (RFC 4291 appendix A). */
static void link_local_of_mac(const uint8_t *mac, uint8_t *out)
{
    memset(out, 0, 16);
    out[0] = 0xFE, out[1] = 0x80;
    out[8] = mac[0] ^ 2, out[9] = mac[1], out[10] = mac[2];
    out[11] = 0xFF, out[12] = 0xFE;
    out[13] = mac[3], out[14] = mac[4], out[15] = mac[5];
}

/* The slot of address a on card d, in state `state` (0: any), or -1 (lock held). */
static int find_address(struct netdev *d, const uint8_t *a, int state)
{
    for (int k = 0; k < IP6_ADDRESSES; k++)
        if (d->ip6[k].state && (!state || d->ip6[k].state == state) && same(d->ip6[k].address, a))
            return k;
    return -1;
}

static int address_state(struct netdev *d, const uint8_t *a)
{
    uint64_t s = spin_lock(&lock);
    int k = find_address(d, a, 0), state = k >= 0 ? d->ip6[k].state : 0;
    spin_unlock(&lock, s);
    return state;
}

/* Is a this machine: ::1 or a card's address in use? */
static bool local6(const uint8_t *a)
{
    if (same(a, loopback6))
        return true;
    bool found = false;
    uint64_t s = spin_lock(&lock);
    for (int i = 0; i < net_count() && !found; i++)
        found = find_address(net_at(i), a, IP6_PREFERRED) >= 0;
    spin_unlock(&lock, s);
    return found;
}

/* Is a packet to dst on card d for us: one of its addresses in use, or a
 * group it is in (all the link's machines, the solicited-node group of
 * an address it has or is checking)? */
static bool addressed(struct netdev *d, const uint8_t *dst)
{
    if (!is_multicast(dst))
        return local6(dst);                         /* any card's (as IPv4's local()) */
    if (same(dst, all_nodes))
        return true;
    bool found = false;
    uint64_t s = spin_lock(&lock);
    for (int k = 0; k < IP6_ADDRESSES && !found; k++) {
        uint8_t g[16];
        solicited_group(d->ip6[k].address, g);
        found = (d->ip6[k].state == IP6_TENTATIVE || d->ip6[k].state == IP6_PREFERRED) &&
                same(g, dst);
    }
    spin_unlock(&lock, s);
    return found;
}

/* The card's link-local address, if it is in use. */
static bool link_local_in_use(struct netdev *d, uint8_t *out)
{
    uint64_t s = spin_lock(&lock);
    bool found = d->ip6[0].state == IP6_PREFERRED;
    if (found)
        memcpy(out, d->ip6[0].address, 16);
    spin_unlock(&lock, s);
    return found;
}

/* --- neighbours ----------------------------------------------------------- */

static struct neighbour {
    struct netdev *d;                   /* 0: a free slot */
    uint8_t ip[16];
    uint8_t mac[6];
    uint64_t tick;
} neighbours[NEIGHBOURS];
static struct wait_queue nd_waiters;

static bool nd_lookup(struct netdev *d, const uint8_t *ip, uint8_t *mac)
{
    bool found = false;
    uint64_t s = spin_lock(&lock);
    for (int i = 0; i < NEIGHBOURS && !found; i++) {
        struct neighbour *n = &neighbours[i];
        if (n->d == d && same(n->ip, ip) && timer_ticks() - n->tick < NEIGHBOUR_LIFETIME) {
            memcpy(mac, n->mac, 6);
            found = true;
        }
    }
    spin_unlock(&lock, s);
    return found;
}

static void release_held(struct netdev *d, const uint8_t *hop, const uint8_t *mac);

/* As ARP's (49g): a fresh entry keeps its MAC until it ages out, so a
 * spoofed advertisement cannot take an address over while its owner is
 * known. */
static void nd_learn(struct netdev *d, const uint8_t *ip, const uint8_t *mac)
{
    uint8_t kept[6];
    uint64_t now = timer_ticks() ? timer_ticks() : 1;
    uint64_t s = spin_lock(&lock);
    int slot = 0, found = -1;
    for (int i = 0; i < NEIGHBOURS; i++) {
        if (neighbours[i].d == d && same(neighbours[i].ip, ip)) {
            found = i;
            break;
        }
        if (!neighbours[i].d ? neighbours[slot].d != 0
                             : neighbours[slot].d && neighbours[i].tick < neighbours[slot].tick)
            slot = i;                               /* a free slot, else the oldest */
    }
    if (found >= 0 && now - neighbours[found].tick < NEIGHBOUR_LIFETIME &&
        memcmp(neighbours[found].mac, mac, 6) != 0) {
        /* sticky: the first owner holds */
    } else if (found >= 0) {
        memcpy(neighbours[found].mac, mac, 6);
        neighbours[found].tick = now;
    } else {
        neighbours[slot].d = d;
        memcpy(neighbours[slot].ip, ip, 16);
        memcpy(neighbours[slot].mac, mac, 6);
        neighbours[slot].tick = now;
        found = slot;
    }
    memcpy(kept, neighbours[found].mac, 6);
    spin_unlock(&lock, s);
    wait_queue_wake_all(&nd_waiters);
    release_held(d, ip, kept);
}

static bool nd_known(struct netdev *d, const uint8_t *ip)
{
    uint8_t mac[6];
    return nd_lookup(d, ip, mac);
}

/* --- sending ------------------------------------------------------------- */

static void send_frame(struct netdev *d, const uint8_t *to, const uint8_t *packet, size_t length)
{
    uint8_t frame[ETH_FRAME_MAX];
    memcpy(frame, to, 6);
    memcpy(frame + 6, d->mac, 6);
    put16(frame + 12, ETH_TYPE_IP6);
    memcpy(frame + ETH_HEADER, packet, length);
    d->send(d, frame, ETH_HEADER + length);
}

static uint32_t pseudo(const uint8_t *src, const uint8_t *dst, uint8_t next, size_t length)
{
    uint8_t t[8] = { (uint8_t)(length >> 24), (uint8_t)(length >> 16), (uint8_t)(length >> 8),
                     (uint8_t)length, 0, 0, 0, next };
    return ip_checksum_add(ip_checksum_add(ip_checksum_add(0, src, 16), dst, 16), t, 8);
}

/* Packets waiting in a receive thread for a neighbour's MAC (as ipv4.c's
 * `held`, 49c: the card's thread must not wait for the answer): sent when
 * it comes, dropped after HOLD_WAIT. */
#define HELD        8
#define HOLD_WAIT   (SOLICITS * RETRANS)

static struct held {
    struct netdev *d;
    uint8_t hop[16];
    uint64_t since;                     /* 0: a free slot */
    uint16_t length;
    uint8_t data[1500];
} *held;
static struct spinlock held_lock = SPINLOCK_INIT("ipv6 held");

static void release_held(struct netdev *d, const uint8_t *hop, const uint8_t *mac)
{
    if (!held)
        return;
    for (int i = 0; i < HELD; i++) {
        uint8_t packet[1500];
        size_t length = 0;
        uint64_t s = spin_lock(&held_lock);
        if (held[i].since && held[i].d == d && same(held[i].hop, hop)) {
            length = held[i].length;
            memcpy(packet, held[i].data, length);
            held[i].since = 0;
        }
        spin_unlock(&held_lock, s);
        if (length)
            send_frame(d, mac, packet, length);
    }
}

static void solicit(struct netdev *d, const uint8_t *target, bool checking);

static void hold(struct netdev *d, const uint8_t *hop, const uint8_t *packet, size_t length)
{
    if (!held) {
        void *h = kmalloc(HELD * sizeof(*held));
        if (!h)
            return;
        memset(h, 0, HELD * sizeof(*held));
        uint64_t s = spin_lock(&held_lock);
        if (!held)
            held = h, h = 0;
        spin_unlock(&held_lock, s);
        kfree(h);
    }
    int slot = -1;
    uint64_t now = timer_ticks() ? timer_ticks() : 1;
    uint64_t s = spin_lock(&held_lock);
    for (int i = 0; i < HELD; i++) {
        if (held[i].since && now - held[i].since > HOLD_WAIT)
            held[i].since = 0;                      /* nobody answered */
        if (!held[i].since && slot < 0)
            slot = i;
    }
    if (slot >= 0) {
        held[slot].d = d, held[slot].since = now, held[slot].length = (uint16_t)length;
        memcpy(held[slot].hop, hop, 16);
        memcpy(held[slot].data, packet, length);
    }
    spin_unlock(&held_lock, s);
    if (slot >= 0)
        solicit(d, hop, false);
}

/* The MAC of neighbour ip on card d (asked SOLICITS times, a second each). */
static bool nd_resolve(struct netdev *d, const uint8_t *ip, uint8_t *mac)
{
    for (int attempt = 0; attempt < SOLICITS; attempt++) {
        if (nd_lookup(d, ip, mac))
            return true;
        solicit(d, ip, false);
        wait_event_until(&nd_waiters, nd_lookup(d, ip, mac) || thread_current()->stop_requested,
                         timer_ticks() + RETRANS);
        if (thread_current()->stop_requested)
            return false;
    }
    return nd_lookup(d, ip, mac);
}

/* --- routes ------------------------------------------------------------------ */

/* Does a begin with the length bits of prefix? */
static bool in_prefix(const uint8_t *a, const uint8_t *prefix, unsigned length)
{
    unsigned whole = length / 8, rest = length % 8;
    if (memcmp(a, prefix, whole))
        return false;
    return !rest || ((a[whole] ^ prefix[whole]) & (0xFF00 >> rest) & 0xFF) == 0;
}

/* Can card d carry IPv6 now (lock held)? */
static bool usable(struct netdev *d)
{
    return d->ip6_up && !d->gone && net_link(d);
}

/* A global address of d in use (lock held). */
static bool has_global(struct netdev *d)
{
    for (int k = 0; k < IP6_ADDRESSES; k++)
        if (d->ip6[k].state == IP6_PREFERRED && !is_link_local(d->ip6[k].address))
            return true;
    return false;
}

/* The router of d's default route, if it has one (lock held): the last
 * one a router advertisement named, or the fixed settings' gateway. */
static bool router_of(struct netdev *d, uint64_t now, uint8_t *router)
{
    struct ip6_link *l = &d->ip6_link;
    if ((l->mode == NETSET6_AUTO || l->mode == NETSET6_DHCP) && l->router_until > now) {
        memcpy(router, l->router, 16);
        return true;
    }
    if (l->mode == NETSET6_FIXED && !same(l->gateway, unspecified)) {
        memcpy(router, l->gateway, 16);
        return true;
    }
    return false;
}

/* The card of the default route and its router: the first card in net
 * default's order (50g) that has a router and a global address, else
 * the first such card (lock not held: it takes it). */
static struct netdev *default_route(uint8_t *router)
{
    char order[MAX_NETDEVS][8];
    int named = ip_default_order(order, MAX_NETDEVS);
    uint64_t now = timer_ticks();
    struct netdev *chosen = 0;
    uint64_t s = spin_lock(&lock);
    for (int k = 0; k <= named && !chosen; k++)
        for (int i = 0; i < net_count() && !chosen; i++) {
            struct netdev *d = net_at(i);
            if ((k == named || !strcmp(d->name, order[k])) && usable(d) && has_global(d) &&
                router_of(d, now, router))
                chosen = d;
        }
    spin_unlock(&lock, s);
    return chosen;
}

/* The card a packet for dst (not link-local, not a group) leaves on and
 * its next hop: dst itself for a prefix on a card's link (a router's,
 * or the fixed address's; the longest wins), else the default route's
 * router. 0: no route. */
static struct netdev *route6(const uint8_t *dst, uint8_t *hop)
{
    uint64_t now = timer_ticks();
    struct netdev *best = 0;
    int best_length = -1;
    uint64_t s = spin_lock(&lock);
    for (int i = 0; i < net_count(); i++) {
        struct netdev *d = net_at(i);
        struct ip6_link *l = &d->ip6_link;
        if (!usable(d))
            continue;
        for (int k = 0; k < IP6_PREFIXES; k++)
            if (l->on_link[k].until > now && l->on_link[k].length > best_length &&
                in_prefix(dst, l->on_link[k].prefix, l->on_link[k].length))
                best = d, best_length = l->on_link[k].length;
        if (l->mode == NETSET6_FIXED && find_address(d, l->fixed, IP6_PREFERRED) >= 0 &&
            l->fixed_prefix > best_length && in_prefix(dst, l->fixed, l->fixed_prefix))
            best = d, best_length = l->fixed_prefix;
    }
    spin_unlock(&lock, s);
    if (best) {
        memcpy(hop, dst, 16);
        return best;
    }
    return default_route(hop);
}

/* The source for dst on card d (RFC 6724, simply): the link-local address
 * for a link-local or group destination; else a global one in use, not
 * past its preferred lifetime if one is not, the most leading bits in
 * common with dst first. */
static bool source_for(struct netdev *d, const uint8_t *dst, uint8_t *out)
{
    if (is_link_local(dst) || is_multicast(dst))
        return link_local_in_use(d, out);
    uint64_t now = timer_ticks();
    int best = -1, best_score = -1;
    uint64_t s = spin_lock(&lock);
    for (int k = 0; k < IP6_ADDRESSES; k++) {
        struct ip6_address *a = &d->ip6[k];
        if (a->state != IP6_PREFERRED || is_link_local(a->address))
            continue;
        int common = 0;
        while (common < 128 && in_prefix(dst, a->address, (unsigned)common + 1))
            common++;
        int score = (a->preferred_until > now ? 256 : 0) + common;
        if (score > best_score)
            best = k, best_score = score;
    }
    if (best >= 0)
        memcpy(out, d->ip6[best].address, 16);
    spin_unlock(&lock, s);
    return best >= 0;
}

/* Send a packet from src (0: the card's address for dst) to dst, on card
 * d (needed for a link-local or multicast dst; any other goes by its
 * route; 0 for ourselves). ICMPv6's
 * checksum is filled in here (the source may be chosen here). A Router
 * Alert option goes before the payload when asked (MLD needs it). */
static int send6(struct netdev *d, const uint8_t *src, const uint8_t *dst, uint8_t next,
                 const void *payload, size_t length, uint8_t hops, bool router_alert)
{
    size_t header = IP6_HEADER + (router_alert ? 8 : 0);
    if (length > 1500 - header)
        return ERR_INVAL;
    bool here = local6(dst);
    uint8_t hop[16];
    memcpy(hop, dst, 16);
    if (!here && !is_multicast(dst) && !is_link_local(dst) && !(d = route6(dst, hop)))
        return ERR_NETUNREACH;                      /* no route */
    if (!here && !d)
        return ERR_INVAL;                           /* which card? */
    uint8_t source[16];
    if (src)
        memcpy(source, src, 16);
    else if (here)
        memcpy(source, dst, 16);
    else if (!source_for(d, dst, source))
        return ERR_NETUNREACH;                      /* no address yet (still being checked) */
    uint8_t packet[1500];
    packet[0] = 0x60, packet[1] = packet[2] = packet[3] = 0;
    put16(packet + 4, (uint16_t)(header - IP6_HEADER + length));
    packet[6] = router_alert ? NEXT_HOP_BY_HOP : next;
    packet[7] = hops;
    memcpy(packet + 8, source, 16);
    memcpy(packet + 24, dst, 16);
    if (router_alert) {
        static const uint8_t alert[7] = { 0, 5, 2, 0, 0, 1, 0 };    /* Router Alert (MLD), PadN */
        packet[IP6_HEADER] = next;
        memcpy(packet + IP6_HEADER + 1, alert, 7);
    }
    uint8_t *body = packet + header;
    memcpy(body, payload, length);
    if (next == NEXT_ICMP6 && length >= 4) {
        put16(body + 2, 0);
        put16(body + 2, ip_checksum_fold(ip_checksum_add(pseudo(source, dst, next, length), body,
                                                         length)));
    }
    size_t total = header + length;
    if (here) {
        ip_loop_send(packet, total);                /* never leaves the machine */
        return 0;
    }
    if (d->gone || !net_link(d))
        return ERR_NETUNREACH;
    if (!guard_check_out(d, packet, total))
        return ERR_PERM;                            /* the firewall's: not allowed out */
    uint8_t mac[6];
    if (is_multicast(dst)) {
        mac[0] = mac[1] = 0x33;
        memcpy(mac + 2, dst + 12, 4);
    } else if (!nd_lookup(d, hop, mac)) {
        if (thread_current()->net_rx) {
            hold(d, hop, packet, total);
            return 0;
        }
        if (!nd_resolve(d, hop, mac))
            return thread_current()->stop_requested ? ERR_INTR : ERR_HOSTUNREACH;
    }
    send_frame(d, mac, packet, total);
    return 0;
}

/* A neighbour solicitation for target: from :: without our MAC while
 * checking it for duplicates, else from our link-local address. */
static void solicit(struct netdev *d, const uint8_t *target, bool checking)
{
    uint8_t m[32] = { ND_SOLICIT }, group[16];
    memcpy(m + 8, target, 16);
    size_t n = 24;
    if (!checking) {
        m[24] = 1, m[25] = 1;                       /* source link-layer address */
        memcpy(m + 26, d->mac, 6);
        n = 32;
    }
    solicited_group(target, group);
    send6(d, checking ? unspecified : 0, group, NEXT_ICMP6, m, n, 255, false);
}

/* "target is at our MAC", to `to` (solicited: an answer to its question). */
static void advertise(struct netdev *d, const uint8_t *target, const uint8_t *to, bool solicited)
{
    uint8_t m[32] = { ND_ADVERT };
    m[4] = (uint8_t)((solicited ? 0x40 : 0) | 0x20);   /* S, O (override) */
    memcpy(m + 8, target, 16);
    m[24] = 2, m[25] = 1;                           /* target link-layer address */
    memcpy(m + 26, d->mac, 6);
    send6(d, target, to, NEXT_ICMP6, m, sizeof(m), 255, false);
}

/* "Routers, tell me about this link": to ff02::2 with our MAC. */
static void router_solicit(struct netdev *d)
{
    uint8_t m[16] = { ROUTER_SOLICIT };
    m[8] = 1, m[9] = 1;                             /* source link-layer address */
    memcpy(m + 10, d->mac, 6);
    send6(d, 0, all_routers, NEXT_ICMP6, m, sizeof(m), 255, false);
}

/* The groups card d is in (each address's solicited-node group; ff02::1
 * is never reported), as an MLDv2 report to the routers (ff02::16). */
static void mld_report(struct netdev *d, uint8_t kind)
{
    uint8_t m[8 + 20 * IP6_ADDRESSES], source[16];
    memset(m, 0, sizeof(m));
    m[0] = MLD2_REPORT;
    int records = 0;
    uint64_t s = spin_lock(&lock);
    for (int k = 0; k < IP6_ADDRESSES; k++) {
        if (d->ip6[k].state != IP6_TENTATIVE && d->ip6[k].state != IP6_PREFERRED)
            continue;
        uint8_t g[16];
        solicited_group(d->ip6[k].address, g);
        bool listed = false;
        for (int r = 0; r < records && !listed; r++)
            listed = same(m + 8 + 20 * r + 4, g);
        if (!listed) {
            m[8 + 20 * records] = kind;
            memcpy(m + 8 + 20 * records + 4, g, 16);
            records++;
        }
    }
    bool named = d->ip6[0].state == IP6_PREFERRED;
    memcpy(source, named ? d->ip6[0].address : unspecified, 16);
    spin_unlock(&lock, s);
    if (!records)
        return;
    put16(m + 6, (uint16_t)records);
    send6(d, source, mld_listeners, NEXT_ICMP6, m, 8 + 20 * (size_t)records, 1, true);
}

/* --- the cards' addresses ---------------------------------------------- */

/* The end of a lifetime of `seconds` from now (all ones: never). */
static uint64_t until_ticks(uint32_t seconds, uint64_t now)
{
    return seconds == 0xFFFFFFFFu ? UINT64_MAX : now + (uint64_t)seconds * TIMER_HZ;
}

/* A new address in a free slot of d, checked for duplicates from `next`
 * on (lock held): its slot, or -1 when all are taken. */
static int add_address(struct netdev *d, const uint8_t *a, uint8_t prefix, uint8_t origin,
                       uint64_t next)
{
    for (int k = 0; k < IP6_ADDRESSES; k++) {
        struct ip6_address *s = &d->ip6[k];
        if (s->state)
            continue;
        memset(s, 0, sizeof(*s));
        memcpy(s->address, a, 16);
        s->prefix = prefix, s->origin = origin, s->state = IP6_TENTATIVE, s->next = next;
        s->valid_until = s->preferred_until = UINT64_MAX;
        return k;
    }
    return -1;
}

/* Everything d had and learnt goes, its settings stay (lock held). */
static void forget(struct netdev *d)
{
    struct ip6_link *l = &d->ip6_link;
    memset(d->ip6, 0, sizeof(d->ip6));
    l->router_until = l->router_dns_until = l->dhcp_dns_until = 0;
    memset(l->on_link, 0, sizeof(l->on_link));
    l->mtu = 0, l->managed = l->other = false, l->solicits = 0, l->solicit_next = 0;
    for (int k = 0; k < NEIGHBOURS; k++)
        if (neighbours[k].d == d)
            neighbours[k].d = 0;
}

void ip6_tick(void)
{
    uint64_t now = timer_ticks();
    for (int i = 0; i < net_count(); i++) {
        struct netdev *d = net_at(i);
        struct ip6_link *l = &d->ip6_link;
        bool link = !d->gone && net_link(d) && l->mode != NETSET6_OFF;     /* (38b: not a port) */
        uint8_t check[IP6_ADDRESSES][16];
        int checks = 0;
        bool ask_routers = false;
        uint32_t wait = 0;
        if (link && !d->ip6_up)
            random_bytes(&wait, sizeof(wait));
        uint64_t s = spin_lock(&lock);
        if (link && !d->ip6_up) {
            /* The link came: the link-local address (and a fixed one),
             * checked after a random wait of up to a second (RFC 4862:
             * machines started together must not all ask at once). */
            d->ip6_up = true;
            forget(d);
            uint8_t local[16];
            link_local_of_mac(d->mac, local);
            add_address(d, local, 64, IP6_LINK_LOCAL, now + wait % TIMER_HZ);   /* slot 0 */
            if (l->mode == NETSET6_FIXED)
                add_address(d, l->fixed, l->fixed_prefix, IP6_FIXED, now + wait % TIMER_HZ);
        } else if (!link && d->ip6_up) {
            /* It went (or IPv6 was switched off): its addresses are checked
             * again when it comes back (another network, maybe), what its
             * routers said and its neighbours are forgotten. */
            d->ip6_up = false;
            forget(d);
        }
        /* Lifetimes: an address past its valid one goes (its group too). */
        for (int k = 0; k < IP6_ADDRESSES; k++)
            if (d->ip6[k].state && d->ip6[k].valid_until <= now)
                memset(&d->ip6[k], 0, sizeof(d->ip6[k]));
        for (int k = 0; k < IP6_PREFIXES; k++)
            if (l->on_link[k].until && l->on_link[k].until <= now)
                l->on_link[k].until = 0;
        if (l->router_until && l->router_until <= now)
            l->router_until = 0;
        if (l->router_dns_until && l->router_dns_until <= now)
            l->router_dns_until = 0;
        if (l->dhcp_dns_until && l->dhcp_dns_until <= now)
            l->dhcp_dns_until = 0;
        /* The groups' MACs for the card: every machine's (ff02::1) and
         * each address's solicited-node group, also while checking it
         * (the answer comes there), none without a link. */
        uint8_t macs[IP6_ADDRESSES + 1][6];
        int groups = 0;
        if (d->ip6_up) {
            static const uint8_t all_nodes_mac[6] = { 0x33, 0x33, 0, 0, 0, 1 };
            memcpy(macs[groups++], all_nodes_mac, 6);
        }
        for (int k = 0; k < IP6_ADDRESSES; k++) {
            if (d->ip6[k].state != IP6_TENTATIVE && d->ip6[k].state != IP6_PREFERRED)
                continue;
            uint8_t mac[6] = { 0x33, 0x33, 0xFF };
            memcpy(mac + 3, d->ip6[k].address + 13, 3);
            bool listed = false;
            for (int g = 0; g < groups && !listed; g++)
                listed = memcmp(macs[g], mac, 6) == 0;
            if (!listed)
                memcpy(macs[groups++], mac, 6);
        }
        for (int k = 0; k < IP6_ADDRESSES; k++) {
            struct ip6_address *a = &d->ip6[k];
            if (a->state != IP6_TENTATIVE || now < a->next)
                continue;
            if (a->probes == 0) {
                a->probes = 1;
                a->next = now + RETRANS;
                memcpy(check[checks++], a->address, 16);
            } else {
                a->state = IP6_PREFERRED;           /* nobody else has it */
            }
        }
        /* With the link-local address in use, ask the routers (64b):
         * three times, four seconds apart, until one answers. */
        if (d->ip6_up && (l->mode == NETSET6_AUTO || l->mode == NETSET6_DHCP) &&
            d->ip6[0].state == IP6_PREFERRED &&
            !l->router_until && l->solicits < ROUTER_SOLICITS && now >= l->solicit_next) {
            l->solicits++;
            l->solicit_next = now + ROUTER_INTERVAL;
            ask_routers = true;
        }
        spin_unlock(&lock, s);
        net_set_multicast(d, (const uint8_t (*)[6])macs, groups);  /* (the card is not touched
                                                                    * when nothing changed) */
        if (checks)
            mld_report(d, MLD_JOINED);              /* the group first, or a switch may not pass it */
        for (int k = 0; k < checks; k++)
            solicit(d, check[k], true);
        if (ask_routers)
            router_solicit(d);
    }
}

int ip6_configure(struct netdev *d, const struct sys_netset6 *settings)
{
    if (settings->mode > NETSET6_DHCP)
        return ERR_INVAL;
    if (settings->mode == NETSET6_FIXED &&
        (settings->prefix < 1 || settings->prefix > 128 || is_multicast(settings->address) ||
         is_link_local(settings->address) || same(settings->address, unspecified) ||
         same(settings->address, loopback6)))
        return ERR_INVAL;
    if (settings->dns_port > 65535)
        return ERR_INVAL;
    uint64_t s = spin_lock(&lock);
    struct ip6_link *l = &d->ip6_link;
    bool fixed = settings->mode == NETSET6_FIXED;
    bool again = l->mode != settings->mode ||
                 (fixed && (!same(l->fixed, settings->address) || l->fixed_prefix != settings->prefix));
    l->mode = (uint8_t)settings->mode;
    memcpy(l->fixed, fixed ? settings->address : unspecified, 16);
    l->fixed_prefix = fixed ? (uint8_t)settings->prefix : 0;
    memcpy(l->gateway, fixed ? settings->gateway : unspecified, 16);
    memcpy(l->dns, settings->dns, 16);
    l->dns_port = (uint16_t)settings->dns_port;
    if (again && d->ip6_up) {
        d->ip6_up = false;                          /* started again at the next look */
        forget(d);
    }
    spin_unlock(&lock, s);
    return 0;
}

/* Another machine has (or is checking) an address card d is checking. */
static void duplicate(struct netdev *d, const uint8_t *a, const uint8_t *mac)
{
    uint64_t s = spin_lock(&lock);
    int k = find_address(d, a, IP6_TENTATIVE);
    if (k >= 0)
        d->ip6[k].state = IP6_DUPLICATE;
    spin_unlock(&lock, s);
    if (k < 0)
        return;
    char text[IP6_TEXT_MAX];
    ip6_format(a, text, sizeof(text));
    kprintf("%s: IPv6 address %s is used by another machine (%02x:%02x:%02x:%02x:%02x:%02x); not used\n",
            d->name, text, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* --- receiving ------------------------------------------------------------ */

/* The link-layer address option of the given type (1 source, 2 target)
 * among an ND message's options (0 if there is none); false for options
 * that are not well formed (a length of 0 would never end). */
static bool nd_option(const uint8_t *o, size_t n, uint8_t type, const uint8_t **mac)
{
    *mac = 0;
    while (n >= 2) {
        size_t length = (size_t)o[1] * 8;
        if (length == 0 || length > n)
            return false;
        if (o[0] == type && length >= 8)
            *mac = o + 2;
        o += length, n -= length;
    }
    return true;
}

static void solicited(struct netdev *d, const uint8_t *src_mac, const uint8_t *src,
                      const uint8_t *dst, const uint8_t *m, size_t n)
{
    const uint8_t *target = m + 8, *mac;
    if (n < 24 || is_multicast(target) || !nd_option(m + 24, n - 24, 1, &mac))
        return;
    bool checking = same(src, unspecified);         /* the sender's duplicate check */
    if (checking && (mac || !is_solicited_group(dst)))
        return;
    int state = address_state(d, target);
    if (state == IP6_TENTATIVE) {
        if (checking)
            duplicate(d, target, src_mac);          /* both check it: neither may have it */
        return;
    }
    if (state != IP6_PREFERRED)
        return;
    if (checking) {
        advertise(d, target, all_nodes, false);     /* it is ours: tell everyone */
        return;
    }
    nd_learn(d, src, mac ? mac : src_mac);
    advertise(d, target, src, true);
}

static void advertised(struct netdev *d, const uint8_t *src_mac, const uint8_t *dst,
                       const uint8_t *m, size_t n)
{
    const uint8_t *target = m + 8, *mac;
    if (n < 24 || is_multicast(target) || !nd_option(m + 24, n - 24, 2, &mac))
        return;
    bool answer = m[4] & 0x40;
    if (is_multicast(dst) && answer)
        return;                                     /* RFC 4861 7.1.2: not to a group */
    int state = address_state(d, target);
    if (state == IP6_TENTATIVE) {
        duplicate(d, target, mac ? mac : src_mac);
        return;
    }
    if (state)
        return;                                     /* ours (in use or a duplicate): not learnt */
    /* Only an answer to a question, or news of a neighbour already known:
     * a stranger's advertisements do not fill the table. */
    if (answer || nd_known(d, target))
        nd_learn(d, target, mac ? mac : src_mac);
}

/* A prefix information option of a router advertisement (lock held):
 * a prefix on the link (L), and with A and 64 bits an address of ours
 * in it (SLAAC: the prefix and the card's interface ID, as the
 * link-local address's). An address there already keeps a valid
 * lifetime of over two hours unless the router says more (RFC 4862
 * 5.5.3 e: an advertisement nobody checks must not end it early). */
static void prefix_information(struct netdev *d, const uint8_t *o, uint64_t now)
{
    uint8_t length = o[2], flags = o[3];
    uint32_t valid = get32(o + 4), preferred = get32(o + 8);
    const uint8_t *prefix = o + 16;
    if (length > 128 || is_link_local(prefix) || is_multicast(prefix) || preferred > valid)
        return;
    struct ip6_link *l = &d->ip6_link;
    if (flags & 0x80) {
        int slot = -1;
        for (int k = 0; k < IP6_PREFIXES; k++)
            if (l->on_link[k].until && l->on_link[k].length == length &&
                in_prefix(prefix, l->on_link[k].prefix, length))
                slot = k;
        for (int k = 0; k < IP6_PREFIXES && slot < 0 && valid; k++)
            if (!l->on_link[k].until)
                slot = k;
        if (slot >= 0) {
            memset(l->on_link[slot].prefix, 0, 16);
            memcpy(l->on_link[slot].prefix, prefix, (size_t)(length + 7) / 8);
            if (length % 8)
                l->on_link[slot].prefix[length / 8] &= (uint8_t)(0xFF00 >> (length % 8));
            l->on_link[slot].length = length;
            l->on_link[slot].until = valid ? until_ticks(valid, now) : 0;
        }
    }
    if (!(flags & 0x40) || length != 64 || l->mode == NETSET6_DHCP)
        return;                                     /* (64g: DHCPv6's addresses instead) */
    uint8_t a[16];
    link_local_of_mac(d->mac, a);
    memcpy(a, prefix, 8);
    int k = find_address(d, a, 0);
    if (k < 0) {
        if (!valid || (k = add_address(d, a, 64, IP6_FROM_ROUTER, now)) < 0)
            return;
        d->ip6[k].valid_until = until_ticks(valid, now);
    } else if (d->ip6[k].origin == IP6_FROM_ROUTER) {
        uint64_t received = until_ticks(valid, now);
        if (valid > 7200 || received > d->ip6[k].valid_until)
            d->ip6[k].valid_until = received;
        else if (d->ip6[k].valid_until > now + TWO_HOURS)
            d->ip6[k].valid_until = now + TWO_HOURS;
    } else {
        return;                                     /* a fixed address: the settings' */
    }
    uint64_t until = until_ticks(preferred, now);
    d->ip6[k].preferred_until = until < d->ip6[k].valid_until ? until : d->ip6[k].valid_until;
}

/* A router advertisement (RFC 4861 6.1.2 checked by the caller: hop limit
 * 255, code 0): the default router, prefixes, the link's MTU and a DNS
 * server (RFC 8106). Only for a card on NETSET6_AUTO. */
static void router_advertised(struct netdev *d, const uint8_t *src, const uint8_t *m, size_t n)
{
    if (n < 16 || !is_link_local(src))
        return;
    for (size_t at = 16; at < n;) {                 /* every option well formed first */
        size_t size = (size_t)m[at + 1] * 8;
        if (n - at < 2 || size == 0 || size > n - at)
            return;
        at += size;
    }
    uint64_t now = timer_ticks();
    const uint8_t *mac = 0;
    uint64_t s = spin_lock(&lock);
    struct ip6_link *l = &d->ip6_link;
    if ((l->mode != NETSET6_AUTO && l->mode != NETSET6_DHCP) || !d->ip6_up) {
        spin_unlock(&lock, s);
        return;
    }
    l->managed = m[5] & 0x80, l->other = m[5] & 0x40;
    l->solicits = ROUTER_SOLICITS;                  /* a router spoke: no more asking */
    uint16_t lifetime = get16(m + 6);
    if (lifetime) {
        memcpy(l->router, src, 16);
        l->router_until = now + (uint64_t)lifetime * TIMER_HZ;
    } else if (same(l->router, src)) {
        l->router_until = 0;                        /* "no longer a default router" */
    }
    for (size_t at = 16; at < n; at += (size_t)m[at + 1] * 8) {
        const uint8_t *o = m + at;
        size_t size = (size_t)o[1] * 8;
        if (o[0] == 1 && size >= 8) {
            mac = o + 2;                            /* the router's MAC */
        } else if (o[0] == 3 && size == 32) {
            prefix_information(d, o, now);
        } else if (o[0] == 5 && size == 8) {
            uint32_t mtu = get32(o + 4);
            if (mtu >= 1280 && mtu <= 1500)
                l->mtu = (uint16_t)mtu;
        } else if (o[0] == 25 && size >= 24) {
            uint32_t seconds = get32(o + 4);
            if (seconds) {
                memcpy(l->router_dns, o + 8, 16);
                l->router_dns_until = until_ticks(seconds, now);
            } else if (same(l->router_dns, o + 8)) {
                l->router_dns_until = 0;
            }
        }
    }
    spin_unlock(&lock, s);
    if (mac)
        nd_learn(d, src, mac);
}

/* --- ICMPv6 echo --------------------------------------------------------- */

#define PINGS 8

static struct ping {
    bool waiting, answered, any;        /* any: to a group, answered by any member */
    uint8_t to[16];
    uint16_t id, seq;
} pings[PINGS];
static struct spinlock ping_lock = SPINLOCK_INIT("ping6");
static struct wait_queue ping_waiters;

static void icmp6_input(struct netdev *d, const uint8_t *src_mac, const uint8_t *ip,
                        const uint8_t *m, size_t n)
{
    const uint8_t *src = ip + 8, *dst = ip + 24;
    if (n < 4 || ip_checksum_fold(ip_checksum_add(pseudo(src, dst, NEXT_ICMP6, n), m, n)) != 0)
        return;
    bool card = d != ip_loopback();
    switch (m[0]) {
    case ICMP6_ECHO_REQUEST:
        /* Answered from the address it was sent to; a request to a group
         * is not answered (as IPv4's broadcasts, 49g: no amplifier). */
        if (n >= 8 && n <= IP6_PAYLOAD_MAX && !is_multicast(dst)) {
            uint8_t reply[IP6_PAYLOAD_MAX];
            memcpy(reply, m, n);
            reply[0] = ICMP6_ECHO_REPLY;
            send6(card ? d : 0, dst, src, NEXT_ICMP6, reply, n, 64, false);
        }
        break;
    case ICMP6_ECHO_REPLY: {
        if (n < 8)
            break;
        uint64_t s = spin_lock(&ping_lock);
        for (int i = 0; i < PINGS; i++)
            if (pings[i].waiting && (pings[i].any || same(pings[i].to, src)) &&
                pings[i].id == get16(m + 4) && pings[i].seq == get16(m + 6))
                pings[i].answered = true;
        spin_unlock(&ping_lock, s);
        wait_queue_wake_all(&ping_waiters);
        break;
    }
    case ND_SOLICIT:
        if (card && ip[7] == 255 && m[1] == 0)
            solicited(d, src_mac, src, dst, m, n);
        break;
    case ND_ADVERT:
        if (card && ip[7] == 255 && m[1] == 0)
            advertised(d, src_mac, dst, m, n);
        break;
    case ROUTER_ADVERT:
        if (card && ip[7] == 255 && m[1] == 0)
            router_advertised(d, src, m, n);
        break;
    case ICMP6_TOO_BIG:
        /* Our packet (quoted after the MTU) was too big for a link on its
         * way: a TCP segment's connection sends smaller ones. */
        if (n >= 8 + IP6_HEADER + 4 && m[8 + 6] == NEXT_TCP) {
            const uint8_t *q = m + 8;
            struct inet_addr local, remote;
            memset(&local, 0, sizeof(local));
            memset(&remote, 0, sizeof(remote));
            memcpy(local.b, q + 8, 16);
            memcpy(remote.b, q + 24, 16);
            tcp_path_mtu(&local, &remote, get16(q + IP6_HEADER), get16(q + IP6_HEADER + 2),
                         get32(m + 4));
        }
        break;
    case MLD_QUERY:
        /* A router asks which groups are wanted (hop limit 1, from its
         * link-local address): our report again. */
        if (card && ip[7] == 1 && n >= 24 && is_link_local(src))
            mld_report(d, MLD_IS_IN);
        break;
    }
}

/* --- fragments ------------------------------------------------------------------- */

/* A packet that came in pieces is put together in a slot of its own,
 * keyed by its addresses and identification, for at most REASSEMBLY_TIME;
 * pieces that overlap end it (RFC 5722: an attacker's way to change what
 * a firewall saw). */
#define REASSEMBLIES        4
#define FRAGMENTED_MAX      9000            /* the payload put back together */
#define REASSEMBLY_TIME     (30 * TIMER_HZ)

static struct reassembly {
    struct netdev *d;
    uint8_t src[16], dst[16];
    uint32_t id;
    uint64_t since;                         /* 0: a free slot */
    uint8_t next;                           /* the first piece's next header */
    uint32_t end;                           /* the payload's length (0: the last piece not yet) */
    uint8_t have[(FRAGMENTED_MAX / 8 + 7) / 8];     /* a bit per 8 bytes received */
    uint8_t *packet;                        /* IP6_HEADER + FRAGMENTED_MAX */
} reassemblies[REASSEMBLIES];
static struct spinlock reassembly_lock = SPINLOCK_INIT("ipv6 fragments");

static void deliver(struct netdev *d, const uint8_t *src_mac, const uint8_t *p, size_t total);

static bool have_block(const struct reassembly *r, uint32_t k)
{
    return r->have[k / 8] >> (k % 8) & 1;
}

/* A piece (its fragment header at `at`): kept; the whole delivered once
 * every piece is there. */
static void fragment(struct netdev *d, const uint8_t *src_mac, const uint8_t *p, size_t at,
                     size_t total)
{
    if (total - at < 8)
        return;
    const uint8_t *f = p + at;
    uint8_t next = f[0];
    uint32_t offset = get16(f + 2) & 0xFFF8u, id = get32(f + 4);
    bool more = f[3] & 1;
    const uint8_t *data = f + 8;
    size_t n = total - at - 8;
    if (offset == 0 && !more) {
        /* A whole packet with a fragment header (an "atomic fragment",
         * RFC 6946): as if it had none. */
        uint8_t *whole = kmalloc(IP6_HEADER + n);
        if (!whole)
            return;
        memcpy(whole, p, IP6_HEADER);
        whole[6] = next;
        put16(whole + 4, (uint16_t)n);
        memcpy(whole + IP6_HEADER, data, n);
        deliver(d, src_mac, whole, IP6_HEADER + n);
        kfree(whole);
        return;
    }
    if ((more && (n == 0 || n % 8)) || offset + n > FRAGMENTED_MAX)
        return;                                     /* pieces but the last are 8-byte multiples */
    uint8_t *done = 0, *spare = kmalloc(IP6_HEADER + FRAGMENTED_MAX);  /* (for a new slot) */
    size_t done_length = 0;
    uint64_t now = timer_ticks() ? timer_ticks() : 1;
    uint64_t s = spin_lock(&reassembly_lock);
    struct reassembly *r = 0, *oldest = &reassemblies[0];
    for (int k = 0; k < REASSEMBLIES && !r; k++) {
        struct reassembly *x = &reassemblies[k];
        if (x->since && now - x->since > REASSEMBLY_TIME)
            x->since = 0;                           /* given up: its pieces never all came */
        if (x->since && x->d == d && x->id == id && same(x->src, p + 8) && same(x->dst, p + 24))
            r = x;
        else if (!x->since || (oldest->since && x->since < oldest->since))
            oldest = x;
    }
    if (!r) {
        r = oldest;
        uint8_t *buffer = r->packet;
        memset(r, 0, sizeof(*r));
        r->packet = buffer;
        r->d = d, r->id = id, r->since = now;
        memcpy(r->src, p + 8, 16);
        memcpy(r->dst, p + 24, 16);
    }
    if (!r->packet)
        r->packet = spare, spare = 0;
    if (!r->packet) {
        r->since = 0;                               /* out of memory */
        spin_unlock(&reassembly_lock, s);
        return;
    }
    uint32_t first = offset / 8, last = (uint32_t)((offset + n + 7) / 8);
    bool bad = (!more && r->end && r->end != offset + n) || (r->end && offset + n > r->end);
    for (uint32_t k = first; k < last && !bad; k++)
        bad = have_block(r, k);                     /* overlapping pieces */
    if (!more)
        for (uint32_t k = last; k < (FRAGMENTED_MAX + 7) / 8 && !bad; k++)
            bad = have_block(r, k);                 /* something beyond the end */
    if (bad) {
        r->since = 0;
        spin_unlock(&reassembly_lock, s);
        kfree(spare);
        return;
    }
    memcpy(r->packet + IP6_HEADER + offset, data, n);
    for (uint32_t k = first; k < last; k++)
        r->have[k / 8] |= (uint8_t)(1 << (k % 8));
    if (offset == 0) {
        r->next = next;
        memcpy(r->packet, p, IP6_HEADER);           /* its header for the whole */
    }
    if (!more)
        r->end = (uint32_t)(offset + n);
    bool complete = r->end && have_block(r, 0);
    for (uint32_t k = 0; complete && k < (r->end + 7) / 8; k++)
        complete = have_block(r, k);
    if (complete) {
        done = r->packet, done_length = IP6_HEADER + r->end;
        done[6] = r->next;
        put16(done + 4, (uint16_t)r->end);
        r->packet = 0;                              /* the slot's buffer goes with it */
        r->since = 0;
    }
    spin_unlock(&reassembly_lock, s);
    kfree(spare);
    if (done) {
        deliver(d, src_mac, done, done_length);
        kfree(done);
    }
}

/* What a packet for us holds (its header checked): extension headers
 * (RFC 8200: hop-by-hop options first only; a routing header with
 * segments left is not for a host), then a fragment, ICMPv6, UDP or TCP,
 * the last two (and echo) past the firewall. */
static void deliver(struct netdev *d, const uint8_t *src_mac, const uint8_t *p, size_t total)
{
    uint8_t next = p[6];
    size_t at = IP6_HEADER;
    while (next == NEXT_HOP_BY_HOP || next == NEXT_DESTINATION || next == NEXT_ROUTING) {
        if ((next == NEXT_HOP_BY_HOP && at != IP6_HEADER) || at + 8 > total)
            return;
        size_t size = (size_t)(p[at + 1] + 1) * 8;
        if (size > total - at || (next == NEXT_ROUTING && p[at + 3] != 0))
            return;
        next = p[at];
        at += size;
    }
    if (next == NEXT_FRAGMENT) {
        fragment(d, src_mac, p, at, total);
        return;
    }
    bool card = d != ip_loopback();
    if (card && !guard_check_in(d, p, total))
        return;                                     /* the firewall's */
    struct inet_addr src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    memcpy(src.b, p + 8, 16);
    memcpy(dst.b, p + 24, 16);
    if (card && is_link_local(src.b))
        src.scope = dst.scope = (uint32_t)net_index(d) + 1;     /* replies leave by this card */
    if (next == NEXT_ICMP6)
        icmp6_input(d, src_mac, p, p + at, total - at);
    else if (next == NEXT_UDP)
        udp_input(d, &src, &dst, p + at, total - at);
    else if (next == NEXT_TCP)
        tcp_input(&src, &dst, p + at, total - at);
}

void ip6_input(struct netdev *d, const uint8_t *src_mac, const uint8_t *p, size_t length)
{
    if (length < IP6_HEADER || p[0] >> 4 != 6)
        return;
    size_t total = IP6_HEADER + get16(p + 4);
    if (total > length)
        return;
    const uint8_t *src = p + 8, *dst = p + 24;
    if (is_multicast(src))
        return;
    if (d != ip_loopback()) {
        /* Our own frame back (an access point repeats group frames to
         * every station, the sender too: our duplicate check would find
         * itself), ::1 from a network, or not for this card. */
        if (src_mac && memcmp(src_mac, d->mac, 6) == 0)
            return;
        if (same(src, loopback6) || same(dst, loopback6) || !addressed(d, dst))
            return;
    }
    deliver(d, src_mac, p, total);
}

int64_t icmp6_ping(const uint8_t dst[16], struct netdev *card, uint16_t id, uint16_t seq,
                   size_t size, uint32_t timeout_ms)
{
    if (size > IP6_PAYLOAD_MAX - 8)
        return -ERR_INVAL;
    if (!local6(dst) && (is_link_local(dst) || is_multicast(dst)) && !card)
        return -ERR_INVAL;                          /* which card? (fe80::1%eth0) */
    int slot = -1;
    uint64_t s = spin_lock(&ping_lock);
    for (int i = 0; i < PINGS && slot < 0; i++)
        if (!pings[i].waiting)
            slot = i;
    if (slot >= 0) {
        pings[slot] = (struct ping){ true, false, is_multicast(dst), { 0 }, id, seq };
        memcpy(pings[slot].to, dst, 16);
    }
    spin_unlock(&ping_lock, s);
    if (slot < 0)
        return -ERR_AGAIN;
    uint8_t request[IP6_PAYLOAD_MAX];
    memset(request, 0, 8);
    request[0] = ICMP6_ECHO_REQUEST;
    put16(request + 4, id);
    put16(request + 6, seq);
    for (size_t i = 0; i < size; i++)
        request[8 + i] = (uint8_t)i;
    uint64_t start = arch_counter();
    int r = send6(card, 0, dst, NEXT_ICMP6, request, 8 + size, 64, false);
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
    s = spin_lock(&ping_lock);
    pings[slot].waiting = false;
    spin_unlock(&ping_lock, s);
    return result;
}

/* --- net's report ------------------------------------------------------------ */

/* "for ever" or "N s" for the time until `until`. */
static void seconds_left(uint64_t until, uint64_t now, char *out, size_t size)
{
    if (until == UINT64_MAX)
        ksnprintf(out, size, "for ever");
    else
        ksnprintf(out, size, "%lu s", until > now ? (until - now) / TIMER_HZ : (uint64_t)0);
}

void ip6_print_card(struct netdev *d)
{
    struct ip6_address a[IP6_ADDRESSES];
    struct ip6_link l;
    uint64_t now = timer_ticks();
    uint64_t s = spin_lock(&lock);
    memcpy(a, d->ip6, sizeof(a));
    l = d->ip6_link;
    spin_unlock(&lock, s);
    if (l.mode == NETSET6_OFF) {
        kprintf("       IPv6 off\n");
        return;
    }
    for (int k = 0; k < IP6_ADDRESSES; k++) {
        if (!a[k].state)
            continue;
        char text[IP6_TEXT_MAX], origin[64], valid[16], preferred[16];
        ip6_format(a[k].address, text, sizeof(text));
        seconds_left(a[k].valid_until, now, valid, sizeof(valid));
        seconds_left(a[k].preferred_until, now, preferred, sizeof(preferred));
        if (a[k].origin == IP6_FROM_ROUTER || a[k].origin == IP6_FROM_DHCP)
            ksnprintf(origin, sizeof(origin), "from %s, valid %s, preferred %s",
                      a[k].origin == IP6_FROM_DHCP ? "DHCPv6" : "the router", valid, preferred);
        else
            ksnprintf(origin, sizeof(origin), "%s", a[k].origin == IP6_FIXED ? "fixed" : "link-local");
        kprintf("       IPv6 %s/%u (%s%s)\n", text, a[k].prefix, origin,
                a[k].state == IP6_TENTATIVE ? ", being checked for duplicates"
                : a[k].state == IP6_DUPLICATE ? ", used by another machine: not used"
                : a[k].preferred_until <= now ? ", deprecated: not for new conversations" : "");
    }
    /* The router (or gateway), the DNS server and the MTU, as far as known. */
    char line[160] = "", router[IP6_TEXT_MAX], dns[IP6_TEXT_MAX], left[16];
    size_t used = 0;
    if ((l.mode == NETSET6_AUTO || l.mode == NETSET6_DHCP) && l.router_until > now) {
        ip6_format(l.router, router, sizeof(router));
        seconds_left(l.router_until, now, left, sizeof(left));
        used += (size_t)ksnprintf(line + used, sizeof(line) - used, "router %s (%s)", router, left);
    } else if (l.mode == NETSET6_FIXED && !same(l.gateway, unspecified)) {
        ip6_format(l.gateway, router, sizeof(router));
        used += (size_t)ksnprintf(line + used, sizeof(line) - used, "gateway %s", router);
    }
    if (!same(l.dns, unspecified)) {
        ip6_format(l.dns, dns, sizeof(dns));
        used += (size_t)ksnprintf(line + used, sizeof(line) - used, "%sDNS %s", used ? ", " : "", dns);
        if (l.dns_port)
            used += (size_t)ksnprintf(line + used, sizeof(line) - used, " port %u", l.dns_port);
    } else if (l.dhcp_dns_until > now) {
        ip6_format(l.dhcp_dns, dns, sizeof(dns));
        seconds_left(l.dhcp_dns_until, now, left, sizeof(left));
        used += (size_t)ksnprintf(line + used, sizeof(line) - used, "%sDNS %s (DHCPv6, %s)",
                                  used ? ", " : "", dns, left);
    } else if (l.router_dns_until > now) {
        ip6_format(l.router_dns, dns, sizeof(dns));
        seconds_left(l.router_dns_until, now, left, sizeof(left));
        used += (size_t)ksnprintf(line + used, sizeof(line) - used, "%sDNS %s (%s)", used ? ", " : "",
                                  dns, left);
    }
    if (l.mtu)
        used += (size_t)ksnprintf(line + used, sizeof(line) - used, "%sMTU %u", used ? ", " : "",
                                  l.mtu);
    if (used)
        kprintf("       IPv6 %s\n", line);
}

void ip6_print_routes(void)
{
    uint64_t now = timer_ticks();
    for (int i = 0; i < net_count(); i++) {
        struct netdev *d = net_at(i);
        struct ip6_link l;
        bool fixed;
        uint64_t s = spin_lock(&lock);
        bool up = usable(d);
        l = d->ip6_link;
        fixed = l.mode == NETSET6_FIXED && find_address(d, l.fixed, IP6_PREFERRED) >= 0;
        spin_unlock(&lock, s);
        if (!up)
            continue;
        for (int k = 0; k <= IP6_PREFIXES; k++) {
            uint8_t net[16] = { 0 };
            unsigned length;
            if (k < IP6_PREFIXES && l.on_link[k].until > now) {
                memcpy(net, l.on_link[k].prefix, 16);
                length = l.on_link[k].length;
            } else if (k == IP6_PREFIXES && fixed) {
                length = l.fixed_prefix;
                memcpy(net, l.fixed, (length + 7) / 8);
                if (length % 8)
                    net[length / 8] &= (uint8_t)(0xFF00 >> (length % 8));
            } else {
                continue;
            }
            char text[IP6_TEXT_MAX];
            ip6_format(net, text, sizeof(text));
            kprintf("route6: %s/%u on %s\n", text, length, d->name);
        }
    }
    uint8_t router[16];
    struct netdev *d = default_route(router);
    if (d) {
        char text[IP6_TEXT_MAX];
        ip6_format(router, text, sizeof(text));
        kprintf("route6: default via %s on %s\n", text, d->name);
    }
}

void ip6_print_loopback(void)
{
    kprintf("       IPv6 ::1/128\n");
}

void ip6_print_neighbours(void)
{
    for (int i = 0; i < NEIGHBOURS; i++) {
        struct neighbour n;
        uint64_t s = spin_lock(&lock);
        n = neighbours[i];
        spin_unlock(&lock, s);
        if (!n.d || timer_ticks() - n.tick >= NEIGHBOUR_LIFETIME)
            continue;
        char text[IP6_TEXT_MAX + 8];
        ip6_format(n.ip, text, IP6_TEXT_MAX);
        if (is_link_local(n.ip))
            ksnprintf(text + strlen(text), 9, "%%%s", n.d->name);
        kprintf("ndp: %s is %02x:%02x:%02x:%02x:%02x:%02x\n", text, n.mac[0], n.mac[1], n.mac[2],
                n.mac[3], n.mac[4], n.mac[5]);
    }
}

/* --- both families: inet.h's senders ------------------------------------------ */

uint32_t inet_pseudo(const struct inet_addr *src, const struct inet_addr *dst, uint8_t protocol,
                     size_t length)
{
    if (inet_is_v4(src) && inet_is_v4(dst)) {
        uint8_t h[12];
        memcpy(h, src->b + 12, 4);
        memcpy(h + 4, dst->b + 12, 4);
        h[8] = 0, h[9] = protocol;
        put16(h + 10, (uint16_t)length);
        return ip_checksum_add(0, h, sizeof(h));
    }
    return pseudo(src->b, dst->b, protocol, length);
}

/* The card a link-local or group destination names by its scope. */
static struct netdev *scope_card(const struct inet_addr *a)
{
    return a->scope ? net_at((int)a->scope - 1) : 0;
}

bool inet_source(const struct inet_addr *dst, struct inet_addr *src)
{
    memset(src, 0, sizeof(*src));
    if (inet_is_v4(dst)) {
        *src = inet_v4(ip_source(inet_v4_of(dst)));
        return inet_v4_of(src) != 0;
    }
    if (local6(dst->b)) {
        *src = *dst;
        return true;
    }
    uint8_t hop[16];
    struct netdev *d = is_link_local(dst->b) || is_multicast(dst->b) ? scope_card(dst)
                                                                      : route6(dst->b, hop);
    if (!d || !source_for(d, dst->b, src->b))
        return false;
    if (is_link_local(src->b))
        src->scope = (uint32_t)net_index(d) + 1;
    return true;
}

int inet_send(const struct inet_addr *src, const struct inet_addr *dst, uint8_t protocol,
              const void *payload, size_t length)
{
    if (inet_is_v4(dst))
        return ip_send_from(inet_v4_of(src), inet_v4_of(dst), protocol, payload, length);
    return send6(scope_card(dst), src->b, dst->b, protocol, payload, length, 64, false);
}

uint32_t inet_mss(const struct inet_addr *dst)
{
    if (inet_is_v4(dst))
        return 1460;
    uint8_t hop[16];
    struct netdev *d = local6(dst->b) ? 0 : is_link_local(dst->b) ? scope_card(dst)
                                                                   : route6(dst->b, hop);
    uint16_t mtu = 0;
    if (d) {
        uint64_t s = spin_lock(&lock);
        mtu = d->ip6_link.mtu;
        spin_unlock(&lock, s);
    }
    return (mtu ? mtu : 1500) - IP6_HEADER - 20;
}

size_t inet_payload_max(const struct inet_addr *dst)
{
    return inet_is_v4(dst) ? IP_PAYLOAD_MAX : IP6_PAYLOAD_MAX;
}

/* --- DHCPv6's leases (64g) ---------------------------------------------------- */

int ip6_lease(struct netdev *d, const struct sys_netlease6 *lease)
{
    const uint8_t *a = lease->address;
    bool none = same(a, unspecified);
    if (lease->flags & ~(uint32_t)NETLEASE6_DROP)
        return ERR_INVAL;
    if (!none && (is_multicast(a) || is_link_local(a) || same(a, loopback6)))
        return ERR_INVAL;
    uint64_t now = timer_ticks();
    uint64_t s = spin_lock(&lock);
    struct ip6_link *l = &d->ip6_link;
    int r = 0;
    if (l->mode != NETSET6_AUTO && l->mode != NETSET6_DHCP) {
        r = ERR_INVAL;                              /* (off, or the settings' own address) */
    } else if (lease->flags & NETLEASE6_DROP) {
        for (int k = 0; k < IP6_ADDRESSES; k++)
            if (d->ip6[k].state && d->ip6[k].origin == IP6_FROM_DHCP &&
                (none || same(d->ip6[k].address, a)))
                memset(&d->ip6[k], 0, sizeof(d->ip6[k]));
        l->dhcp_dns_until = 0;
    } else if (!d->ip6_up) {
        r = ERR_NETUNREACH;                         /* no link: asked again when it comes */
    } else {
        if (!none) {
            int k = find_address(d, a, 0);
            if (k < 0 && (k = add_address(d, a, 128, IP6_FROM_DHCP, now)) < 0)
                r = ERR_NOSPC;
            if (k >= 0 && d->ip6[k].origin == IP6_FROM_DHCP) {
                d->ip6[k].valid_until = until_ticks(lease->valid, now);
                uint64_t until = until_ticks(lease->preferred, now);
                d->ip6[k].preferred_until = until < d->ip6[k].valid_until ? until
                                                                          : d->ip6[k].valid_until;
            }
        }
        if (!same(lease->dns, unspecified)) {
            memcpy(l->dhcp_dns, lease->dns, 16);
            l->dhcp_dns_until = none ? until_ticks(0xFFFFFFFFu, now) : until_ticks(lease->valid, now);
        }
    }
    spin_unlock(&lock, s);
    return r;
}

/* --- netinfo6 (64d) ----------------------------------------------------------- */

void ip6_info(struct netdev *d, struct sys_netinfo6 *info)
{
    uint64_t now = timer_ticks();
    uint64_t s = spin_lock(&lock);
    struct ip6_link *l = &d->ip6_link;
    info->mode = (uint16_t)(l->mode | (l->managed ? NETINFO6_MANAGED : 0) |
                            (l->other ? NETINFO6_OTHER : 0));
    info->count = 0;
    for (int k = 0; k < IP6_ADDRESSES && k < 4; k++) {
        struct ip6_address *a = &d->ip6[k];
        if (!a->state)
            continue;
        int i = (int)info->count++;
        memcpy(info->addresses[i].address, a->address, 16);
        info->addresses[i].prefix = a->prefix;
        info->addresses[i].state = a->state;
        info->addresses[i].origin = a->origin;
        info->addresses[i].valid = a->valid_until == UINT64_MAX ? 0xFFFFFFFFu
                                 : (uint32_t)(a->valid_until > now ? (a->valid_until - now) / TIMER_HZ : 0);
        info->addresses[i].preferred = a->preferred_until == UINT64_MAX ? 0xFFFFFFFFu
                                     : (uint32_t)(a->preferred_until > now
                                                  ? (a->preferred_until - now) / TIMER_HZ : 0);
    }
    if (!router_of(d, now, info->router))
        memset(info->router, 0, 16);
    if (!same(l->dns, unspecified)) {
        memcpy(info->dns, l->dns, 16);
        info->dns_port = l->dns_port;
    } else if (l->dhcp_dns_until > now) {
        memcpy(info->dns, l->dhcp_dns, 16);         /* (64g) DHCPv6's before the router's */
    } else if (l->router_dns_until > now) {
        memcpy(info->dns, l->router_dns, 16);
    }
    info->mtu = l->mtu;
    spin_unlock(&lock, s);
}

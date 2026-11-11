/*
 * The firewall ("guard", roadmap item 12, designed with the user after
 * step 44l): the checks of every packet that arrives for the machine on a
 * card or leaves it on one. The rules come from the program guardd
 * (/system/config/guard); the first one that matches a packet decides,
 * else the default of its direction (in = drop, out = allow on a new
 * system). A packet a rule allowed starts a connection: TCP and UDP by
 * their addresses and ports, ICMP echo by its identifier. The packets of
 * a connection pass both ways without the rules until it has been quiet
 * for a while, so the answers to what the machine asked (DNS, a web page,
 * a ping, DHCP's broadcast) come in through "in = drop", and the answers
 * of a server the rules let in go out through "out = drop". A connection
 * may leave from 0.0.0.0 (DHCP before an address: any local address
 * matches) or to 255.255.255.255 (any sender matches), and (64g) to an
 * IPv6 group (ff02::1:2: DHCPv6's servers answer from their own address).
 *
 * lo is never checked; forwarded packets (between cards) are not yet.
 *
 * IPv6 (64c, 64e): its packets share the connections (addresses are
 * struct inet_addr, IPv4 as ::ffff:a.b.c.d) and the defaults, and have a
 * table of their own (table6, GUARD_SET6: the rules file's rules without
 * an address and those with an IPv6 one, in the file's order). Until it
 * is set, IPv6 packets are judged by the IPv4 table's rules that name no
 * address (64c's interim). ICMPv6 counts as icmp. Neighbour discovery,
 * router advertisements, MLD and ICMPv6's error messages always pass:
 * IPv6 does not work without them (RFC 4890).
 * Before guardd has loaded rules (the boot), what comes in is dropped and
 * what goes out passes (opening connections, so DHCP's answers come in).
 *
 * "limit ... N/minute" lets at most N new connections a minute through,
 * counted per other side (the sender coming in, the receiver going out),
 * so one machine guessing passwords uses up its own allowance only; the
 * rest are dropped. A minute is counted from the first connection.
 */
#include "guard.h"
#include "inet.h"
#include "ipv4.h"
#include "string.h"
#include "sync.h"
#include "timer.h"
#include "uefi.h"

#define FLOWS       256
#define TCP_IDLE    (TIMER_HZ * 600)    /* how long a quiet connection is kept */
#define UDP_IDLE    (TIMER_HZ * 60)
#define ICMP_IDLE   (TIMER_HZ * 30)
#define LIMITS      64                  /* (rule, other side) pairs counted at once */
#define MINUTE      (TIMER_HZ * 60)

struct flow {
    uint64_t last;                      /* timer ticks of its last packet; 0: free */
    struct inet_addr local, remote;     /* local none: any; remote 255.255.255.255: any */
    uint16_t local_port, remote_port;   /* ICMP echo: the identifier in both */
    uint8_t protocol;
    bool inbound;                       /* opened by a packet coming in (49d) */
    bool forward;                       /* a forwarded connection, matched both ways (49h) */
};

/* What a check needs of a packet. */
struct packet {
    struct inet_addr src, dst;
    uint16_t sport, dport;              /* ICMP echo: the identifier in both */
    uint8_t protocol;                   /* (ICMPv6: IP_PROTO_ICMP) */
    bool connection;                    /* TCP, UDP or an ICMP echo: may start one */
    bool syn;                           /* a TCP packet that opens a connection (SYN, no ACK) */
    bool always;                        /* IPv6's own messages: never stopped */
};

/* Of FLOWS, kept free for the machine's own outbound connections: a flood
 * of inbound packets from many sources (or ICMP identifiers) must not evict
 * the flows that let the answers to what we asked come back (49d). */
#define OUT_RESERVE 64

static struct spinlock lock = SPINLOCK_INIT("guard");
/* Until guardd has loaded the rules, what comes in is dropped (the user's
 * choice: safe from the first packet); net up switches the checks off
 * when guardd's elf is off. */
static uint32_t state = GUARD_STATE_BOOT;
static struct sys_guard table;          /* the loaded rules, their counters, the drops */
static struct sys_guard6 table6;        /* (64e) IPv6's */
static bool loaded6;                    /* table6 was set (else 64c's interim) */
#define RULE6_ID(r) (GUARD_RULES + (r)) /* limits: IPv6 rule r's own counts */
static struct flow flows[FLOWS];

/* New connections a limit rule let through, per other side. */
static struct limit {
    uint64_t since;                     /* timer ticks: its minute began; 0: free */
    struct inet_addr address;
    uint16_t rule, count;
} limits[LIMITS];

/* May one more connection from (or to) address pass limit rule r? */
static bool within_limit(int r, const struct inet_addr *address)
{
    uint64_t now = timer_ticks();
    struct limit *slot = 0, *oldest = &limits[0];
    for (int k = 0; k < LIMITS; k++) {
        struct limit *l = &limits[k];
        if (l->since && l->rule == r && inet_same(&l->address, address)) {
            slot = l;
            break;
        }
        if (!l->since || l->since < oldest->since)
            oldest = l;
    }
    if (!slot) {
        slot = oldest;                              /* a free one, or the oldest goes */
        slot->since = 0;
    }
    if (!slot->since || now - slot->since >= MINUTE) {
        slot->since = now ? now : 1;
        slot->rule = (uint16_t)r;
        slot->address = *address;
        slot->count = 0;
    }
    uint32_t limit = r >= GUARD_RULES ? table6.rules[r - GUARD_RULES].limit : table.rules[r].limit;
    if (slot->count >= limit)
        return false;
    slot->count++;
    return true;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* An IPv6 packet: past its extension headers to the transport (64c). */
static bool parse6(const uint8_t *p, size_t length, struct packet *i)
{
    if (length < 40)
        return false;
    memcpy(i->src.b, p + 8, 16);
    memcpy(i->dst.b, p + 24, 16);
    uint8_t next = p[6];
    size_t at = 40;
    while ((next == 0 || next == 43 || next == 60) && at + 8 <= length) {
        next = p[at];
        at += (size_t)(p[at + 1] + 1) * 8;
    }
    if (at > length)
        return false;
    const uint8_t *t = p + at;
    size_t n = length - at;
    i->protocol = next;                             /* (ipv6.c puts pieces together first) */
    if ((next == IP_PROTO_TCP || next == IP_PROTO_UDP) && n >= 4) {
        i->sport = get16(t);
        i->dport = get16(t + 2);
        i->connection = true;
        i->syn = next == IP_PROTO_TCP && n >= 14 && (t[13] & 0x02) && !(t[13] & 0x10);
    } else if (next == 58 && n >= 4) {
        i->protocol = IP_PROTO_ICMP;
        if ((t[0] == 128 || t[0] == 129) && n >= 8) {
            i->sport = i->dport = get16(t + 4);
            i->connection = true;
        } else {
            i->always = t[0] < 128 || (t[0] >= 130 && t[0] <= 137) || t[0] == 143;
        }
    }
    return true;
}

static bool parse(const uint8_t *p, size_t length, struct packet *i)
{
    if (length < 20)
        return false;
    memset(i, 0, sizeof(*i));
    if (p[0] >> 4 == 6)
        return parse6(p, length, i);
    size_t header = (size_t)(p[0] & 15) * 4;
    if (header < 20 || header > length)
        return false;
    i->src = inet_v4(get32(p + 12));
    i->dst = inet_v4(get32(p + 16));
    i->protocol = p[9];
    const uint8_t *t = p + header;
    size_t n = length - header;
    if ((i->protocol == IP_PROTO_TCP || i->protocol == IP_PROTO_UDP) && n >= 4) {
        i->sport = get16(t);
        i->dport = get16(t + 2);
        i->connection = true;
        /* A TCP connection is opened by its first segment, a SYN without an
         * ACK (49d); a SYN+ACK and later segments match the flow that SYN
         * made. UDP and ICMP open on any packet. */
        i->syn = i->protocol == IP_PROTO_TCP && n >= 14 && (t[13] & 0x02) && !(t[13] & 0x10);
    } else if (i->protocol == IP_PROTO_ICMP && n >= 8 && (t[0] == 0 || t[0] == 8)) {
        i->sport = i->dport = get16(t + 4);
        i->connection = true;
    }
    return true;
}

/* The card's number as rules name it: N of ethN, GUARD_WLAN_CARD | N of
 * wlanN (50k: Wi-Fi cards could not be named before), (38b) N of tapN and
 * brN with their kind in *kind. GUARD_ANY_CARD: a name no rule can have. */
static int card_number(const struct netdev *d, uint8_t *kind)
{
    bool wireless = !strncmp(d->name, "wlan", 4), tap = !strncmp(d->name, "tap", 3);
    bool bridge = !strncmp(d->name, "br", 2);
    const char *digits = d->name + (wireless ? 4 : tap || !bridge ? 3 : 2);
    *kind = tap ? GUARD_KIND_TAP : bridge ? GUARD_KIND_BRIDGE : GUARD_KIND_ETH;
    if ((!wireless && !tap && !bridge && strncmp(d->name, "eth", 3) != 0) || *digits < '0' ||
        *digits > '9')
        return GUARD_ANY_CARD;
    int n = 0;
    for (const char *c = digits; *c >= '0' && *c <= '9'; c++)
        n = n * 10 + (*c - '0');
    return n < GUARD_WLAN_CARD ? (wireless ? GUARD_WLAN_CARD | n : n) : GUARD_ANY_CARD;
}

/* (38b) Does a rule's card byte and kind name card d? */
static bool names_card(uint8_t card, uint8_t kind, const struct netdev *d)
{
    uint8_t its;
    int n = card_number(d, &its);
    return n != GUARD_ANY_CARD && n == card && its == kind;
}

static bool in_net(const struct inet_addr *a, uint32_t net, uint32_t prefix)
{
    if (prefix == 0)
        return true;
    if (!inet_is_v4(a))
        return false;                               /* (a rule's IPv4 network: not IPv6's) */
    uint32_t address = inet_v4_of(a);
    uint32_t m = prefix >= 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> prefix);
    return (address & m) == (net & m);
}

static uint64_t idle(uint8_t protocol)
{
    return protocol == IP_PROTO_TCP ? TCP_IDLE : protocol == IP_PROTO_UDP ? UDP_IDLE : ICMP_IDLE;
}

/* The connection a packet belongs to, if one is open. */
static struct flow *find(const struct packet *i, bool in)
{
    const struct inet_addr *local = in ? &i->dst : &i->src, *remote = in ? &i->src : &i->dst;
    uint16_t lport = in ? i->dport : i->sport, rport = in ? i->sport : i->dport;
    uint64_t now = timer_ticks();
    for (int k = 0; k < FLOWS; k++) {
        struct flow *f = &flows[k];
        if (!f->last || f->forward || now - f->last > idle(f->protocol))
            continue;
        if (f->protocol == i->protocol && f->local_port == lport && f->remote_port == rport &&
            (inet_none(&f->local) || inet_same(&f->local, local)) &&
            (inet_v4_of(&f->remote) == 0xFFFFFFFFu || f->remote.b[0] == 0xFF ||
             inet_same(&f->remote, remote)))
            return f;
    }
    return 0;
}

/* A forwarded connection, matched in either direction (49h): the flow stores
 * the first packet's direction, so the return traffic (addresses and ports
 * swapped) passes without a mirror rule. */
static struct flow *find_forward(const struct packet *i)
{
    uint64_t now = timer_ticks();
    for (int k = 0; k < FLOWS; k++) {
        struct flow *f = &flows[k];
        if (!f->last || !f->forward || now - f->last > idle(f->protocol) || f->protocol != i->protocol)
            continue;
        if (inet_same(&f->local, &i->src) && inet_same(&f->remote, &i->dst) &&
            f->local_port == i->sport && f->remote_port == i->dport)
            return f;                               /* same direction as it opened */
        if (inet_same(&f->local, &i->dst) && inet_same(&f->remote, &i->src) &&
            f->local_port == i->dport && f->remote_port == i->sport)
            return f;                               /* the return direction */
    }
    return 0;
}

/* A connection starts. TCP opens only on its first SYN, so a flood of
 * non-SYN segments opens nothing. Room is made (49d) preferring a free or
 * expired slot, then the oldest inbound flow over any outbound one, and
 * inbound flows never take more than FLOWS - OUT_RESERVE slots, so a flood
 * of inbound packets cannot evict the machine's own outbound flows. */
static void open_flow(const struct packet *i, bool in)
{
    if (!i->connection)
        return;
    if (i->protocol == IP_PROTO_TCP && !i->syn)
        return;
    uint64_t now = timer_ticks();
    struct flow *freeslot = 0, *old_in = 0, *old_out = 0;
    int in_count = 0;
    for (int k = 0; k < FLOWS; k++) {
        struct flow *f = &flows[k];
        if (!f->last || now - f->last > idle(f->protocol)) {
            if (!freeslot)
                freeslot = f;
        } else if (f->inbound) {
            in_count++;
            if (!old_in || f->last < old_in->last)
                old_in = f;
        } else if (!old_out || f->last < old_out->last) {
            old_out = f;
        }
    }
    struct flow *slot;
    if (in && in_count >= FLOWS - OUT_RESERVE)
        slot = old_in;                      /* inbound at its cap: reuse the oldest inbound */
    else if (freeslot)
        slot = freeslot;
    else
        slot = old_in ? old_in : old_out;   /* prefer evicting an inbound flow */
    if (!slot)
        return;                             /* nothing to evict (inbound capped, none inbound) */
    slot->last = now ? now : 1;
    slot->protocol = i->protocol;
    slot->inbound = in;
    slot->forward = false;
    slot->local = in ? i->dst : i->src;
    slot->remote = in ? i->src : i->dst;
    slot->local_port = in ? i->dport : i->sport;
    slot->remote_port = in ? i->sport : i->dport;
}

/* A forwarded connection starts: reuse the oldest slot (forwarded flows do
 * not get the inbound/outbound reservation — they are transit). Stores the
 * first packet's direction for find_forward to match both ways. */
static void open_forward_flow(const struct packet *i)
{
    if (!i->connection || (i->protocol == IP_PROTO_TCP && !i->syn))
        return;
    uint64_t now = timer_ticks();
    struct flow *slot = &flows[0];
    for (int k = 0; k < FLOWS; k++) {
        struct flow *f = &flows[k];
        if (!f->last || now - f->last > idle(f->protocol)) {
            slot = f;
            break;
        }
        if (f->last < slot->last)
            slot = f;
    }
    slot->last = now ? now : 1;
    slot->protocol = i->protocol;
    slot->inbound = false;
    slot->forward = true;
    slot->local = i->src;
    slot->remote = i->dst;
    slot->local_port = i->sport;
    slot->remote_port = i->dport;
}

/* The rule that decides a packet, or -1. */
static int decide(int direction, const struct netdev *d, const struct packet *i)
{
    for (int r = 0; r < table.count; r++) {
        const struct sys_guard_rule *g = &table.rules[r];
        if (g->direction != direction || (g->protocol && g->protocol != i->protocol))
            continue;
        if ((g->port_low || g->port_high) &&
            ((i->protocol != IP_PROTO_TCP && i->protocol != IP_PROTO_UDP) ||
             i->dport < g->port_low || i->dport > g->port_high))
            continue;
        if (!in_net(&i->src, g->from, g->from_prefix) || !in_net(&i->dst, g->to, g->to_prefix))
            continue;
        if (g->card != GUARD_ANY_CARD && !names_card(g->card, g->kinds[0], d))
            continue;
        return r;
    }
    return -1;
}

/* (64e) Is a in the IPv6 network net/prefix? */
static bool in_net6(const struct inet_addr *a, const uint8_t *net, unsigned prefix)
{
    if (prefix == 0)
        return true;
    if (inet_is_v4(a))
        return false;
    unsigned whole = prefix / 8, rest = prefix % 8;
    if (memcmp(a->b, net, whole))
        return false;
    return !rest || ((a->b[whole] ^ net[whole]) & (0xFF00 >> rest) & 0xFF) == 0;
}

/* The IPv6 rule that decides a packet, or -1. */
static int decide6(int direction, const struct netdev *d, const struct packet *i)
{
    for (int r = 0; r < table6.count; r++) {
        const struct sys_guard6_rule *g = &table6.rules[r];
        if (g->direction != direction || (g->protocol && g->protocol != i->protocol))
            continue;
        if ((g->port_low || g->port_high) &&
            ((i->protocol != IP_PROTO_TCP && i->protocol != IP_PROTO_UDP) ||
             i->dport < g->port_low || i->dport > g->port_high))
            continue;
        if (!in_net6(&i->src, g->from, g->from_prefix) || !in_net6(&i->dst, g->to, g->to_prefix))
            continue;
        if (g->card != GUARD_ANY_CARD && !names_card(g->card, g->kinds[0], d))
            continue;
        return r;
    }
    return -1;
}

static void dropped6(int direction, const struct netdev *d, const struct packet *i, int rule)
{
    if (table6.drop_count < GUARD_DROPS)
        table6.drop_count++;
    memmove(&table6.drops[1], &table6.drops[0], sizeof(table6.drops[0]) * (GUARD_DROPS - 1));
    struct sys_guard6_drop *x = &table6.drops[0];
    memset(x, 0, sizeof(*x));
    x->time = uefi_clock_ns();
    memcpy(x->from, i->src.b, 16);
    memcpy(x->to, i->dst.b, 16);
    x->port = i->connection && i->protocol != IP_PROTO_ICMP ? i->dport : 0;
    x->direction = (uint8_t)direction;
    x->protocol = i->protocol;
    uint8_t kind;
    x->card = (uint8_t)card_number(d, &kind);
    x->card_kind = kind;
    x->rule = rule >= 0 ? table6.rules[rule].number : 0;
}

/* The forward rule that decides a transit packet, or -1. The destination
 * port is matched (the far side's service); from/to are the real addresses;
 * card is the incoming card, out_card the outgoing one. */
static int decide_forward(const struct netdev *in, const struct netdev *out, const struct packet *i)
{
    for (int r = 0; r < table.count; r++) {
        const struct sys_guard_rule *g = &table.rules[r];
        if (g->direction != GUARD_FORWARD || (g->protocol && g->protocol != i->protocol))
            continue;
        if ((g->port_low || g->port_high) &&
            ((i->protocol != IP_PROTO_TCP && i->protocol != IP_PROTO_UDP) ||
             i->dport < g->port_low || i->dport > g->port_high))
            continue;
        if (!in_net(&i->src, g->from, g->from_prefix) || !in_net(&i->dst, g->to, g->to_prefix))
            continue;
        if (g->card != GUARD_ANY_CARD && !names_card(g->card, g->kinds[0], in))
            continue;
        if (g->out_card != GUARD_ANY_CARD && !names_card(g->out_card, g->kinds[1], out))
            continue;
        return r;
    }
    return -1;
}

static void dropped(int direction, const struct netdev *d, const struct packet *i, int rule)
{
    if (table.drop_count < GUARD_DROPS)
        table.drop_count++;
    memmove(&table.drops[1], &table.drops[0], sizeof(table.drops[0]) * (GUARD_DROPS - 1));
    struct sys_guard_drop *x = &table.drops[0];
    memset(x, 0, sizeof(*x));
    x->time = uefi_clock_ns();
    x->from = inet_v4_of(&i->src);                  /* (an IPv6 one: 0 until 64e) */
    x->to = inet_v4_of(&i->dst);
    x->port = i->connection && i->protocol != IP_PROTO_ICMP ? i->dport : 0;
    x->direction = (uint8_t)direction;
    x->protocol = i->protocol;
    uint8_t kind;
    x->card = (uint8_t)card_number(d, &kind);
    x->card_kind = kind;
    x->rule = (uint8_t)(rule + 1);
}

static bool check(int direction, struct netdev *d, const uint8_t *p, size_t length)
{
    if (state == GUARD_STATE_OFF || !d || d == ip_loopback())
        return true;
    struct packet i;
    if (!parse(p, length, &i) || i.always)
        return true;                                /* (IPv4 drops it itself; IPv6's own) */
    bool in = direction == GUARD_IN, pass;
    uint64_t s = spin_lock(&lock);
    struct flow *f = find(&i, in);
    if (state == GUARD_STATE_OFF) {
        pass = true;
    } else if (f) {
        f->last = timer_ticks();
        table.connection_packets++;
        pass = true;
    } else if (state == GUARD_STATE_BOOT) {
        pass = !in;                                 /* nothing in before the rules */
        if (pass)
            open_flow(&i, in);
        else
            dropped(direction, d, &i, -1);
    } else if (loaded6 && !inet_is_v4(&i.src)) {
        /* (64e) IPv6's own table, the shared defaults. */
        int r = decide6(direction, d, &i);
        int action;
        if (r >= 0) {
            table6.rules[r].packets++;
            action = table6.rules[r].action;
        } else {
            action = in ? table.in_default : table.out_default;
        }
        pass = action == GUARD_ALLOW ||
               (action == GUARD_LIMIT && within_limit(RULE6_ID(r), in ? &i.src : &i.dst));
        if (r < 0) {
            uint64_t *count = in ? (pass ? &table6.in_allowed : &table6.in_dropped)
                                 : (pass ? &table6.out_allowed : &table6.out_dropped);
            (*count)++;
        }
        if (pass)
            open_flow(&i, in);
        else
            dropped6(direction, d, &i, r);
    } else {
        int r = decide(direction, d, &i);
        int action;
        if (r >= 0) {
            table.rules[r].packets++;
            action = table.rules[r].action;
        } else {
            action = in ? table.in_default : table.out_default;
        }
        pass = action == GUARD_ALLOW ||
               (action == GUARD_LIMIT && within_limit(r, in ? &i.src : &i.dst));
        if (r < 0) {
            uint64_t *count = in ? (pass ? &table.in_allowed : &table.in_dropped)
                                 : (pass ? &table.out_allowed : &table.out_dropped);
            (*count)++;
        }
        if (pass)
            open_flow(&i, in);
        else
            dropped(direction, d, &i, r);
    }
    spin_unlock(&lock, s);
    return pass;
}

bool guard_check_in(struct netdev *d, const uint8_t *packet, size_t length)
{
    return check(GUARD_IN, d, packet, length);
}

bool guard_check_out(struct netdev *d, const uint8_t *packet, size_t length)
{
    return check(GUARD_OUT, d, packet, length);
}

/* A packet passing between two cards (49h). lo is never a forwarding card.
 * A forwarded connection opens a flow matched both ways, so the return
 * traffic passes without a mirror rule. */
bool guard_check_forward(struct netdev *in, struct netdev *out, const uint8_t *p, size_t length)
{
    if (state == GUARD_STATE_OFF || !in || !out || in == ip_loopback() || out == ip_loopback())
        return true;
    struct packet i;
    if (!parse(p, length, &i))
        return true;
    bool pass;
    uint64_t s = spin_lock(&lock);
    struct flow *f = find_forward(&i);
    if (state == GUARD_STATE_OFF) {
        pass = true;
    } else if (f) {
        f->last = timer_ticks();
        table.connection_packets++;
        pass = true;
    } else if (state == GUARD_STATE_BOOT) {
        pass = false;                               /* nothing forwarded before the rules load */
        dropped(GUARD_FORWARD, in, &i, -1);
    } else {
        int r = decide_forward(in, out, &i);
        int action;
        if (r >= 0) {
            table.rules[r].packets++;
            action = table.rules[r].action;
        } else {
            action = table.forward_default;
        }
        pass = action == GUARD_ALLOW || (action == GUARD_LIMIT && within_limit(r, &i.src));
        if (r < 0) {
            uint64_t *count = pass ? &table.fwd_allowed : &table.fwd_dropped;
            (*count)++;
        }
        if (pass)
            open_forward_flow(&i);
        else
            dropped(GUARD_FORWARD, in, &i, r);
    }
    spin_unlock(&lock, s);
    return pass;
}

int guard_load(const struct sys_guard *g)
{
    if (g->count > GUARD_RULES ||
        (g->in_default != GUARD_ALLOW && g->in_default != GUARD_DROP) ||
        (g->out_default != GUARD_ALLOW && g->out_default != GUARD_DROP) ||
        (g->forward_default != GUARD_ALLOW && g->forward_default != GUARD_DROP))
        return ERR_INVAL;
    for (int r = 0; r < g->count; r++) {
        const struct sys_guard_rule *x = &g->rules[r];
        if (x->action < GUARD_ALLOW || x->action > GUARD_LIMIT ||
            (x->direction != GUARD_IN && x->direction != GUARD_OUT &&
             x->direction != GUARD_FORWARD) ||
            x->from_prefix > 32 || x->to_prefix > 32 || x->port_low > x->port_high ||
            (x->action == GUARD_LIMIT && (x->limit == 0 || x->limit > 65535)))
            return ERR_INVAL;
    }
    uint64_t s = spin_lock(&lock);
    memset(&table, 0, sizeof(table));
    table.in_default = g->in_default;
    table.out_default = g->out_default;
    table.forward_default = g->forward_default;
    table.count = g->count;
    memcpy(table.rules, g->rules, sizeof(g->rules[0]) * g->count);
    for (int r = 0; r < table.count; r++)
        table.rules[r].packets = 0;
    memset(limits, 0, sizeof(limits));              /* (rule numbers may have moved) */
    state = GUARD_STATE_ON;                         /* open connections stay open */
    spin_unlock(&lock, s);
    return 0;
}

int guard_set6(const struct sys_guard6 *g)
{
    if (g->count > GUARD_RULES)
        return ERR_INVAL;
    for (int r = 0; r < g->count; r++) {
        const struct sys_guard6_rule *x = &g->rules[r];
        if (x->action < GUARD_ALLOW || x->action > GUARD_LIMIT ||
            (x->direction != GUARD_IN && x->direction != GUARD_OUT &&
             x->direction != GUARD_FORWARD) ||
            x->from_prefix > 128 || x->to_prefix > 128 || x->port_low > x->port_high ||
            (x->action == GUARD_LIMIT && (x->limit == 0 || x->limit > 65535)))
            return ERR_INVAL;
    }
    uint64_t s = spin_lock(&lock);
    memset(&table6, 0, sizeof(table6));
    table6.count = g->count;
    memcpy(table6.rules, g->rules, sizeof(g->rules[0]) * g->count);
    for (int r = 0; r < table6.count; r++)
        table6.rules[r].packets = 0;
    for (int k = 0; k < LIMITS; k++)
        if (limits[k].rule >= GUARD_RULES)
            limits[k].since = 0;                    /* (its rule numbers may have moved) */
    loaded6 = true;
    spin_unlock(&lock, s);
    return 0;
}

void guard_get6(struct sys_guard6 *g)
{
    uint64_t s = spin_lock(&lock);
    memcpy(g, &table6, sizeof(*g));
    spin_unlock(&lock, s);
}

void guard_off(void)
{
    uint64_t s = spin_lock(&lock);
    state = GUARD_STATE_OFF;
    spin_unlock(&lock, s);
}

void guard_status(struct sys_guard *g)
{
    uint64_t s = spin_lock(&lock);
    memcpy(g, &table, sizeof(*g));
    g->state = state;
    spin_unlock(&lock, s);
}

/*
 * dhclient: the DHCP client, an elf. For every card whose settings file
 * (/system/config/network/ethN) says address = dhcp it asks the network for
 * an address (RFC 2131: DISCOVER, a server OFFERs, REQUEST, it ACKs, with the
 * network's mask, gateway, DNS server and the lease's time) and gives it to
 * the card (netset). Once a second it looks at each such card:
 *   - no address: it asks every 10 seconds, and at once when the link comes;
 *   - an address: it asks again at half the lease, every minute while no
 *     server answers, and gives the address up when the lease ends;
 *   - the link gone: the address goes too, until the link is back.
 * It talks through a UDP socket bound to the card (bindcard): broadcasts
 * from 0.0.0.0 to 255.255.255.255, port 68 to 67, the answer asked for as a
 * broadcast. net up makes its elf's file when a card uses DHCP.
 *
 * DHCPv6 (64g, RFC 8415) for every card on IPv6: with address6 = dhcp, or
 * with address6 = auto when the router's advertisement says "managed", it
 * asks for an address (Solicit, a server Advertises, Request, it Replies:
 * the address, its lifetimes, T1 and T2, a DNS server), renews it at T1
 * with that server (Renew), with any after T2 (Rebind), and gives it up at
 * its end; when the router says only "other", it asks for the DNS server
 * alone (Information-request), again every day. It talks from the card's
 * link-local address, port 546, to every server's group ff02::1:2, port
 * 547 (an IPv6 socket bound to the card); its identity (DUID) is the
 * card's MAC (DUID-LL). The kernel takes what it gets by netlease6.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("dhclient", "the DHCP client, an elf: addresses for the cards set to dhcp (and DHCPv6)");

#define MAX_CARDS       64              /* (the kernel keeps 64, MAX_NETDEVS) */
#define CLIENT_PORT     68
#define SERVER_PORT     67
#define COOKIE          0x63825363u
#define MSG_DISCOVER    1
#define MSG_OFFER       2
#define MSG_REQUEST     3
#define MSG_ACK         5
#define MSG_NAK         6
#define RETRY           10              /* seconds between tries without an address */
#define RENEW_RETRY     60              /* between renewals no server answered */

struct lease {
    uint32_t address, mask, gateway, dns, server, seconds;
};

/* (64g) A card's DHCPv6. */
#define CLIENT6_PORT    546
#define SERVER6_PORT    547
#define D6_SOLICIT      1
#define D6_ADVERTISE    2
#define D6_REQUEST      3
#define D6_RENEW        5
#define D6_REBIND       6
#define D6_REPLY        7
#define D6_INFO_REQUEST 11
#define O_CLIENTID      1
#define O_SERVERID      2
#define O_IA_NA         3
#define O_IAADDR        5
#define O_ORO           6
#define O_ELAPSED       8
#define O_STATUS        13
#define O_DNS           23
#define INFO_REFRESH    86400           /* RFC 8415's default for stateless settings */

struct six {
    int fd;                             /* -1: none yet */
    int doing;                          /* 0 nothing, 1 an address (stateful), 2 the DNS server */
    bool have, told_no_answer;
    uint8_t address[16], dns[16];
    uint32_t t1, t2, valid, preferred;
    uint8_t server_id[130];
    size_t server_id_length;
    uint8_t server[16];                 /* the server's link-local address */
    uint64_t next, rebind, end;
};

struct card {
    char name[8];
    struct six six;
    bool dhcp;                          /* its file says so */
    int fd;                             /* its socket, -1: none yet */
    uint8_t mac[6];
    bool had_link, told_no_answer;
    struct lease lease;                 /* seconds 0: no address */
    uint64_t next, end;                 /* the next try (or renewal); the lease's end */
};

static struct card cards[MAX_CARDS];    /* in the kernel's order; name "": free */
static uint64_t now;                    /* seconds: the clock's, or counted */
static uint8_t message[548];

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t prefix_of(uint32_t mask)
{
    uint32_t n = 0;
    while (mask & 0x80000000u)
        n++, mask <<= 1;
    return n;
}

/* A BOOTP message of the type; REQUEST names the address and server. */
static int send_message(struct card *c, uint8_t type, uint32_t xid, const struct lease *offer)
{
    uint8_t *m = message;
    memset(m, 0, sizeof(message));
    m[0] = 1, m[1] = 1, m[2] = 6;                   /* request, Ethernet, 6-byte addresses */
    put32(m + 4, xid);
    m[10] = 0x80;                                   /* answer by broadcast */
    memcpy(m + 28, c->mac, 6);
    put32(m + 236, COOKIE);
    size_t i = 240;
    m[i++] = 53, m[i++] = 1, m[i++] = type;
    if (type == MSG_REQUEST) {
        m[i++] = 50, m[i++] = 4;
        put32(m + i, offer->address), i += 4;
        m[i++] = 54, m[i++] = 4;
        put32(m + i, offer->server), i += 4;
    }
    m[i++] = 55, m[i++] = 4, m[i++] = 1, m[i++] = 3, m[i++] = 6, m[i++] = 51;
    m[i++] = 255;
    return ritteros_sendto(c->fd, m, sizeof(message), 0xFFFFFFFFu, SERVER_PORT) < 0 ? -1 : 0;
}

/* An answer to xid: its type (0: none), the lease it offers. */
static int parse(const uint8_t *m, size_t n, uint32_t xid, const struct card *c, struct lease *out)
{
    if (n < 240 || m[0] != 2 || get32(m + 4) != xid || memcmp(m + 28, c->mac, 6) ||
        get32(m + 236) != COOKIE)
        return 0;
    memset(out, 0, sizeof(*out));
    out->address = get32(m + 16);
    int type = 0;
    for (size_t i = 240; i + 1 < n && m[i] != 255;) {
        if (m[i] == 0) {
            i++;
            continue;
        }
        uint8_t code = m[i], length = m[i + 1];
        const uint8_t *v = m + i + 2;
        if (i + 2 + length > n)
            break;
        if (code == 53 && length >= 1)
            type = v[0];
        else if (code == 1 && length >= 4)
            out->mask = get32(v);
        else if (code == 3 && length >= 4)
            out->gateway = get32(v);
        else if (code == 6 && length >= 4)
            out->dns = get32(v);
        else if (code == 51 && length >= 4)
            out->seconds = get32(v);
        else if (code == 54 && length >= 4)
            out->server = get32(v);
        i += 2 + length;
    }
    return type;
}

/* Send type and wait for want (three times, a second each): 0, or -1. */
static int ask(struct card *c, uint8_t type, uint8_t want, uint32_t xid, const struct lease *offer,
               struct lease *out)
{
    static uint8_t answer[1500];
    for (int attempt = 0; attempt < 3; attempt++) {
        if (send_message(c, type, xid, offer) < 0)
            return -1;
        for (;;) {
            uint32_t from;
            uint16_t port;
            ssize_t n = ritteros_recvfrom(c->fd, answer, sizeof(answer), &from, &port, 1000);
            if (n < 0)
                break;                              /* a second without an answer */
            int got = parse(answer, (size_t)n, xid, c, out);
            if (got == MSG_NAK)
                return -1;
            if (got == want)
                return 0;
        }
    }
    return -1;
}

static void text(uint32_t ip, char *out, size_t size)
{
    ip_format(ip, out, size);
}

/* The card's settings from DHCP, with the file's dns if it has one: as
 * set (in s). */
static void apply(struct card *c, const struct lease *l, struct sys_netset *out)
{
    struct sys_netset s;
    memset(&s, 0, sizeof(s));
    snprintf(s.name, sizeof(s.name), "%s", c->name);
    s.address = l->address;
    s.prefix = l->address ? (l->mask ? prefix_of(l->mask) : 24) : 0;
    s.gateway = l->gateway;
    s.dns = l->dns;
    char path[64], value[64];
    snprintf(path, sizeof(path), "%s/%s", NETWORK_CONFIG, c->name);
    if (l->address && config_get(path, "dns", value, sizeof(value)) > 0) {
        char *colon = strchr(value, ':');           /* a server set by hand wins */
        if (colon) {
            *colon = 0;
            for (const char *p = colon + 1; *p >= '0' && *p <= '9'; p++)
                s.dns_port = s.dns_port * 10 + (uint32_t)(*p - '0');
        }
        ip_parse(value, &s.dns, 0);
    }
    s.flags = l->address ? NETSET_LEASED : 0;
    s.lease = l->seconds;
    if (ritteros_netset(&s) < 0 && !(errno == EBUSY && !l->address))
        printf("dhclient: %s: %s\n", c->name, strerror(errno));   /* (38d: a bridge's port
                                                                     * has none to drop) */
    if (out)
        *out = s;
}

/* Ask for an address (renewal: for the one we have). */
static void configure(struct card *c, bool renewal)
{
    uint32_t xid;
    ritteros_random(&xid, sizeof(xid));
    struct lease offer, lease;
    int r = renewal ? 0 : ask(c, MSG_DISCOVER, MSG_OFFER, xid, 0, &offer);
    if (renewal)
        offer = c->lease;
    if (!r)
        r = ask(c, MSG_REQUEST, MSG_ACK, xid, &offer, &lease);
    if (r) {
        c->next = now + (renewal ? RENEW_RETRY : RETRY);
        if (!renewal && !c->told_no_answer) {
            printf("dhclient: %s: no answer from a DHCP server yet; asking again every %d s\n",
                   c->name, RETRY);
            c->told_no_answer = true;
        }
        return;
    }
    if (!lease.server)
        lease.server = offer.server;
    if (!lease.seconds)
        lease.seconds = 86400;
    bool changed = !renewal || lease.address != c->lease.address;
    c->lease = lease;
    c->told_no_answer = false;
    uint32_t half = lease.seconds / 2 < 60 ? 60 : lease.seconds / 2;
    c->next = now + half;
    c->end = now + lease.seconds;
    struct sys_netset set;
    apply(c, &lease, &set);
    if (changed) {
        char address[16], server[16], gateway[16], dns[24];
        text(lease.address, address, sizeof(address));
        text(lease.server, server, sizeof(server));
        text(lease.gateway, gateway, sizeof(gateway));
        text(set.dns, dns, sizeof(dns));
        if (set.dns_port)
            snprintf(dns + strlen(dns), sizeof(dns) - strlen(dns), ":%u", set.dns_port);
        printf("dhclient: %s: %s/%u from %s, gateway %s, DNS %s, lease %u s\n", c->name, address,
               set.prefix, server, lease.gateway ? gateway : "none", set.dns ? dns : "none",
               lease.seconds);
    }
}

/* --- DHCPv6 (64g) -------------------------------------------------------------- */

static uint8_t message6[1024];

static size_t option(uint8_t *at, uint16_t code, const void *data, size_t length)
{
    at[0] = (uint8_t)(code >> 8), at[1] = (uint8_t)code;
    at[2] = (uint8_t)(length >> 8), at[3] = (uint8_t)length;
    if (length)
        memcpy(at + 4, data, length);
    return 4 + length;
}

/* The card's DUID-LL: type 3, hardware type 1 (Ethernet), its MAC. */
static size_t duid(const struct card *c, uint8_t *out)
{
    uint8_t d[10] = { 0, 3, 0, 1 };
    memcpy(d + 4, c->mac, 6);
    memcpy(out, d, sizeof(d));
    return sizeof(d);
}

/* A message of `type` (its transaction ID xid): our identity, the server's
 * when renewing or requesting, the address asked for, the DNS server asked
 * for. */
static size_t build6(const struct card *c, uint8_t type, uint32_t xid)
{
    uint8_t *m = message6;
    m[0] = type, m[1] = (uint8_t)(xid >> 16), m[2] = (uint8_t)(xid >> 8), m[3] = (uint8_t)xid;
    size_t n = 4;
    uint8_t id[10];
    n += option(m + n, O_CLIENTID, id, duid(c, id));
    if ((type == D6_REQUEST || type == D6_RENEW) && c->six.server_id_length)
        n += option(m + n, O_SERVERID, c->six.server_id, c->six.server_id_length);
    static const uint8_t elapsed[2] = { 0, 0 }, wanted[2] = { 0, O_DNS };
    n += option(m + n, O_ELAPSED, elapsed, 2);
    n += option(m + n, O_ORO, wanted, 2);
    if (type != D6_INFO_REQUEST) {
        uint8_t ia[12 + 4 + 24];                    /* IAID, T1, T2 (0: the server's), an address */
        memset(ia, 0, sizeof(ia));
        memcpy(ia, c->mac + 2, 4);                  /* IAID: the MAC's last four bytes */
        size_t ia_n = 12;
        if (type != D6_SOLICIT) {
            uint8_t a[24];
            memset(a, 0, sizeof(a));
            memcpy(a, c->six.address, 16);
            ia_n += option(ia + 12, O_IAADDR, a, sizeof(a));
        }
        n += option(m + n, O_IA_NA, ia, ia_n);
    }
    return n;
}

/* An answer to xid of `want`: 1 and what it gives into s, 0 not ours, -1
 * a refusal (a status other than success). */
static int parse6(const struct card *c, const uint8_t *m, size_t n, uint8_t want, uint32_t xid,
                  struct six *s)
{
    if (n < 4 || m[0] != want || ((uint32_t)m[1] << 16 | (uint32_t)m[2] << 8 | m[3]) != xid)
        return 0;
    uint8_t id[10];
    size_t id_n = duid(c, id);
    bool ours = false, address = false;
    int status = 0;
    for (size_t at = 4; at + 4 <= n;) {
        uint16_t code = (uint16_t)(m[at] << 8 | m[at + 1]), length = (uint16_t)(m[at + 2] << 8 | m[at + 3]);
        const uint8_t *v = m + at + 4;
        if (length > n - at - 4)
            return 0;
        if (code == O_CLIENTID)
            ours = length == id_n && !memcmp(v, id, id_n);
        else if (code == O_SERVERID && length <= sizeof(s->server_id))
            memcpy(s->server_id, v, length), s->server_id_length = length;
        else if (code == O_DNS && length >= 16)
            memcpy(s->dns, v, 16);
        else if (code == O_STATUS && length >= 2)
            status = v[0] << 8 | v[1];
        else if (code == O_IA_NA && length >= 12) {
            s->t1 = (uint32_t)v[4] << 24 | (uint32_t)v[5] << 16 | (uint32_t)v[6] << 8 | v[7];
            s->t2 = (uint32_t)v[8] << 24 | (uint32_t)v[9] << 16 | (uint32_t)v[10] << 8 | v[11];
            for (size_t k = 12; k + 4 <= length;) {
                uint16_t sub = (uint16_t)(v[k] << 8 | v[k + 1]), sl = (uint16_t)(v[k + 2] << 8 | v[k + 3]);
                if (sl > length - k - 4)
                    break;
                const uint8_t *w = v + k + 4;
                if (sub == O_IAADDR && sl >= 24) {
                    memcpy(s->address, w, 16);
                    s->preferred = (uint32_t)w[16] << 24 | (uint32_t)w[17] << 16 | (uint32_t)w[18] << 8 | w[19];
                    s->valid = (uint32_t)w[20] << 24 | (uint32_t)w[21] << 16 | (uint32_t)w[22] << 8 | w[23];
                    address = true;
                } else if (sub == O_STATUS && sl >= 2) {
                    status = w[0] << 8 | w[1];
                }
                k += 4 + sl;
            }
        }
        at += 4 + length;
    }
    if (!ours || !s->server_id_length)
        return 0;
    if (status)
        return -1;                                  /* (NoAddrsAvail and the like) */
    return want == D6_ADVERTISE || c->six.doing != 1 || address ? 1 : -1;
}

/* Send type to every server's group and wait for want (three times, a
 * second each, the same transaction): 1 with s filled, 0 no answer, -1
 * refused. */
static int ask6(struct card *c, int scope, uint8_t type, uint8_t want, struct six *s)
{
    uint32_t xid;
    ritteros_random(&xid, sizeof(xid));
    xid &= 0xFFFFFF;
    size_t n = build6(c, type, xid);
    struct sockaddr_in6 to;
    memset(&to, 0, sizeof(to));
    to.sin6_family = AF_INET6;
    to.sin6_port = htons(SERVER6_PORT);
    inet_pton(AF_INET6, "ff02::1:2", &to.sin6_addr);
    to.sin6_scope_id = (uint32_t)scope;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (sendto(c->six.fd, message6, n, 0, (struct sockaddr *)&to, sizeof(to)) < 0)
            return 0;
        for (;;) {
            static uint8_t answer[1024];
            struct sockaddr_in6 from;
            socklen_t from_length = sizeof(from);
            ssize_t got = recvfrom(c->six.fd, answer, sizeof(answer), 0, (struct sockaddr *)&from,
                                   &from_length);
            if (got < 0)
                break;                              /* (SO_RCVTIMEO: a second) */
            *s = c->six;
            int r = parse6(c, answer, (size_t)got, want, xid, s);
            if (r) {
                memcpy(s->server, from.sin6_addr.s6_addr, 16);
                return r;
            }
        }
    }
    return 0;
}

static void text6(const uint8_t *a, char *out, size_t size)
{
    inet_ntop(AF_INET6, a, out, (socklen_t)size);
}

/* Give the kernel what DHCPv6 said (drop: its end). */
static void lease6(struct card *c, bool drop)
{
    struct sys_netlease6 l;
    memset(&l, 0, sizeof(l));
    snprintf(l.name, sizeof(l.name), "%s", c->name);
    l.flags = drop ? NETLEASE6_DROP : 0;
    if (c->six.doing == 1 || drop)
        memcpy(l.address, c->six.address, 16);
    memcpy(l.dns, c->six.dns, 16);
    l.valid = c->six.valid, l.preferred = c->six.preferred;
    ritteros_netlease6(&l);
}

static void drop6(struct card *c, const char *why)
{
    if (!c->six.have)
        return;
    lease6(c, true);
    if (why)
        printf("dhclient: %s: %s: no IPv6 address from DHCPv6\n", c->name, why);
    c->six.have = false;
    memset(c->six.address, 0, 16);
    memset(c->six.dns, 0, 16);
    c->six.server_id_length = 0;
}

/* The card's DHCPv6, once a second: what its IPv6 settings and router say. */
static void look6(struct card *c)
{
    struct sys_netinfo6 info;
    memset(&info, 0, sizeof(info));
    snprintf(info.name, sizeof(info.name), "%s", c->name);
    if (ritteros_netinfo6(&info) < 0)
        return;
    int mode = info.mode & 0xFF;
    int doing = mode == NETSET6_DHCP || (mode == NETSET6_AUTO && (info.mode & NETINFO6_MANAGED)) ? 1
              : mode == NETSET6_AUTO && (info.mode & NETINFO6_OTHER) ? 2 : 0;
    bool link_local = info.count > 0 && info.addresses[0].state == 2;  /* (it talks from it) */
    if (doing != c->six.doing || !link_local) {
        drop6(c, doing == c->six.doing ? "no link" : 0);
        c->six.doing = link_local ? doing : 0;
        c->six.next = 0;
        c->six.told_no_answer = false;
    }
    if (!c->six.doing)
        return;
    if (c->six.fd < 0) {
        int fd = socket(AF_INET6, SOCK_DGRAM, 0);
        struct sockaddr_in6 here;
        memset(&here, 0, sizeof(here));
        here.sin6_family = AF_INET6;
        here.sin6_port = htons(CLIENT6_PORT);
        struct timeval second = { 1, 0 };
        if (fd < 0 || ritteros_bindcard(fd, c->name) < 0 ||
            bind(fd, (struct sockaddr *)&here, sizeof(here)) < 0 ||
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &second, sizeof(second)) < 0) {
            printf("dhclient: %s: no IPv6 socket for it: %s\n", c->name, strerror(errno));
            if (fd >= 0)
                close(fd);
            return;
        }
        c->six.fd = fd;
    }
    int scope = info.index + 1;
    char address[INET6_ADDRSTRLEN], server[INET6_ADDRSTRLEN], dns[INET6_ADDRSTRLEN];
    if (c->six.have && c->six.doing == 1 && now >= c->six.end) {
        drop6(c, "the IPv6 lease ended");
        c->six.next = 0;
    }
    if (now < c->six.next)
        return;
    struct six s;
    if (c->six.doing == 2) {                        /* only the DNS server */
        int r = ask6(c, scope, D6_INFO_REQUEST, D6_REPLY, &s);
        if (r > 0 && memcmp(s.dns, (uint8_t[16]){ 0 }, 16)) {
            c->six = s;
            c->six.have = true;
            lease6(c, false);
            text6(s.dns, dns, sizeof(dns));
            printf("dhclient: %s: DNS %s from DHCPv6\n", c->name, dns);
            c->six.next = now + INFO_REFRESH;
        } else {
            c->six.next = now + RETRY;
        }
        return;
    }
    if (c->six.have) {                              /* a renewal: the same server, after T2 any */
        bool rebinding = now >= c->six.rebind;
        int r = ask6(c, scope, rebinding ? D6_REBIND : D6_RENEW, D6_REPLY, &s);
        if (r > 0) {
            c->six = s;
            c->six.have = true;
            lease6(c, false);
            text6(s.address, address, sizeof(address));
            printf("dhclient: %s: IPv6 %s renewed, valid %u s\n", c->name, address, s.valid);
        } else {
            c->six.next = now + RENEW_RETRY < c->six.end ? now + RENEW_RETRY : c->six.end;
            return;
        }
    } else {
        int r = ask6(c, scope, D6_SOLICIT, D6_ADVERTISE, &s);
        if (r > 0) {
            struct six offer = s;
            c->six.server_id_length = offer.server_id_length;
            memcpy(c->six.server_id, offer.server_id, offer.server_id_length);
            memcpy(c->six.address, offer.address, 16);
            r = ask6(c, scope, D6_REQUEST, D6_REPLY, &s);
        }
        if (r <= 0) {
            if (!c->six.told_no_answer)
                printf("dhclient: %s: no answer from a DHCPv6 server yet; asking again every %d s\n",
                       c->name, RETRY);
            c->six.told_no_answer = true;
            c->six.server_id_length = 0;
            memset(c->six.address, 0, 16);
            c->six.next = now + RETRY;
            return;
        }
        c->six = s;
        c->six.have = true;
        lease6(c, false);
        text6(s.address, address, sizeof(address));
        text6(s.server, server, sizeof(server));
        text6(s.dns, dns, sizeof(dns));
        printf("dhclient: %s: IPv6 %s from DHCPv6 (%s), DNS %s, valid %u s\n", c->name, address,
               server, memcmp(s.dns, (uint8_t[16]){ 0 }, 16) ? dns : "none", s.valid);
    }
    /* Renew at T1 (half the preferred lifetime if the server leaves it to
     * us), rebind at T2, end at the valid lifetime. */
    uint32_t t1 = c->six.t1 ? c->six.t1 : c->six.preferred / 2;
    uint32_t t2 = c->six.t2 ? c->six.t2 : c->six.preferred / 5 * 4;
    c->six.next = now + (t1 ? t1 : 1);
    c->six.rebind = now + (t2 > t1 ? t2 : t1 + 1);
    c->six.end = c->six.valid == 0xFFFFFFFFu ? UINT64_MAX : now + c->six.valid;
}

static void drop(struct card *c, const char *why)
{
    if (!c->lease.seconds)
        return;
    memset(&c->lease, 0, sizeof(c->lease));
    apply(c, &c->lease, 0);
    printf("dhclient: %s: %s: no address\n", c->name, why);
}

/* One look at a card. */
static void look(struct card *c)
{
    look6(c);                                       /* (64g) whatever its IPv4 settings */
    char path[64], value[16];
    snprintf(path, sizeof(path), "%s/%s", NETWORK_CONFIG, c->name);
    bool dhcp = config_get(path, "address", value, sizeof(value)) > 0 && strcmp(value, "dhcp") == 0;
    if (!dhcp) {
        c->dhcp = false;                            /* not ours (any more) */
        memset(&c->lease, 0, sizeof(c->lease));
        return;
    }
    struct sys_netinfo info;
    memset(&info, 0, sizeof(info));
    snprintf(info.name, sizeof(info.name), "%s", c->name);
    if (ritteros_netinfo(&info) < 0) {
        /* (38d) No such card (a removed tap or bridge): its address went
         * with it, so the lease is forgotten, and the card's return is its
         * link coming (else a bridge made again kept a lease it no longer
         * had, renewed hours later). */
        memset(&c->lease, 0, sizeof(c->lease));
        c->had_link = false;
        return;
    }
    if (!c->dhcp) {
        c->dhcp = true;
        c->next = 0;
        c->had_link = info.link_speed != 0;
        memcpy(c->mac, info.mac, 6);
    }
    if (c->fd < 0) {
        c->fd = ritteros_socket(SOCKET_UDP);
        if (c->fd < 0 || ritteros_bindcard(c->fd, c->name) < 0 ||
            ritteros_bind(c->fd, CLIENT_PORT) < 0) {
            printf("dhclient: %s: no socket for it: %s\n", c->name, strerror(errno));
            if (c->fd >= 0)
                close(c->fd);
            c->fd = -1;
            return;
        }
    }
    bool link = info.link_speed != 0, link_came = link && !c->had_link;
    c->had_link = link;
    if (!link) {
        drop(c, "no link");
        return;
    }
    if (c->lease.seconds && now >= c->end)
        drop(c, "the lease ended");
    if (c->lease.seconds && info.address != c->lease.address) {
        /* (38d) The card lost the address we gave it (a bridge's port, a
         * tap removed and made again): ask again, as for a new card. */
        memset(&c->lease, 0, sizeof(c->lease));
        c->next = 0;
    }
    if (c->lease.seconds ? now >= c->next : now >= c->next || link_came)
        configure(c, c->lease.seconds != 0);
}

int main(void)
{
    uint64_t counted = 0;
    for (;;) {
        /* The clock (exchanges may take seconds); counted without one. */
        uint64_t clock = ritteros_time() / 1000000000u;
        now = clock ? clock : counted++;
        /* Every card the kernel lists, whatever its kind: a new one gets a
         * free entry (cards are not taken away: an entry stays with its name). */
        struct sys_netinfo info;
        for (int i = 0; ritteros_netcard(i, &info) == 0; i++) {
            int free = -1, k = 0;
            for (; k < MAX_CARDS && strcmp(cards[k].name, info.name) != 0; k++)
                if (free < 0 && !cards[k].name[0])
                    free = k;
            if (k == MAX_CARDS && free >= 0) {
                snprintf(cards[free].name, sizeof(cards[free].name), "%s", info.name);
                cards[free].fd = -1;
                cards[free].six.fd = -1;
                memcpy(cards[free].mac, info.mac, 6);
            }
        }
        for (int i = 0; i < MAX_CARDS; i++)
            if (cards[i].name[0])
                look(&cards[i]);
        sleep(1);
    }
}

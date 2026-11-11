/*
 * dnsd: names (DNS) for this machine and others, an elf. It listens on
 * UDP port 53 on every address and answers everyone who asks, the
 * machine's own programs and other machines alike (the user's choice:
 * what may reach it is the firewall's to decide, roadmap item 12): from
 * its cache, kept for each answer's time to live (at most an hour), or by
 * asking the DNS server of the first card that has one (from DHCP, or
 * dns = ... in the card's file; any kind of card, a link up first) and
 * passing its answer on; without one, the first IPv6 DNS server a card
 * has (64d: a router's, or dns6 = ...). Its socket is an IPv6 one, so it
 * answers askers of both families; A and AAAA answers are cached, and
 * (64k) so are the empty ones, "no such name" and "no records of that
 * type" (RFC 2308: for the zone's SOA minimum, else a minute, at most five
 * minutes): on a network with IPv6 every lookup asks for an AAAA record
 * first, and for a name with none each question went to the server again,
 * one after another, while the others waited. (64q) Questions it has to
 * ask go to the server at once, up to 64 on their way together, and the
 * same question asked again meanwhile waits for the first one's answer.
 * Programs
 * ask it through the library (ritteros_resolve, ritteros_resolve6). net up
 * makes its elf's file when the network is set up.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("dnsd", "names (DNS) for this machine and others, an elf: answers kept by their time to live");

#define PORT            53
#define CACHE           32
#define LONGEST_KEEP    3600            /* seconds */
#define EMPTY_KEEP      60              /* (64k) an empty answer without an SOA */
#define LONGEST_EMPTY   300             /* at most, whatever the SOA says */

static struct {
    char name[DNS_NAME_MAX + 1];
    uint16_t type;                      /* 1 A, 28 AAAA */
    uint8_t address[16];                /* (A: the first 4 bytes) */
    bool empty;                         /* (64k) no address: answered with rcode */
    uint8_t rcode;                      /* 0 no records of the type, 3 no such name */
    uint64_t until;                     /* seconds */
} cache[CACHE];

static uint8_t question[512], answer[512], reply[512];

static uint64_t seconds(void)
{
    static uint64_t counted;
    uint64_t clock = ritteros_time() / 1000000000u;
    return clock ? clock : ++counted;               /* (without a clock: per question) */
}

/* The first question's bytes (name, type, class), whatever QR says; its
 * length from offset 12, or 0 if it runs off. Compression is not used in a
 * question. */
static size_t question_section(const uint8_t *m, size_t n)
{
    if (n < 12)
        return 0;
    size_t at = 12;
    while (at < n && m[at]) {
        if (m[at] > 63 || at + 1 + m[at] > n)
            return 0;
        at += 1 + m[at];
    }
    at += 1 + 4;                                    /* the zero label, type, class */
    return at <= n ? at - 12 : 0;
}

/* Do two DNS messages ask the same first question (case-insensitive)?
 * Guards against an answer that matches the ID but asks something else. */
static bool same_question(const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
    size_t la = question_section(a, an), lb = question_section(b, bn);
    if (!la || la != lb)
        return false;
    for (size_t i = 0; i < la; i++) {
        uint8_t x = a[12 + i], y = b[12 + i];
        if (x >= 'A' && x <= 'Z')
            x += 32;
        if (y >= 'A' && y <= 'Z')
            y += 32;
        if (x != y)
            return false;
    }
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

/* Past a name at m[at] (labels, or a pointer that ends it): the offset
 * after it, or 0 if it runs off. */
static size_t skip_name(const uint8_t *m, size_t n, size_t at)
{
    while (at < n) {
        if ((m[at] & 0xC0) == 0xC0)
            return at + 2 <= n ? at + 2 : 0;
        if (m[at] > 63)
            return 0;
        if (!m[at])
            return at + 1;
        at += 1 + m[at];
    }
    return 0;
}

/* (64k) An empty answer: no such name (rcode 3), or no record of the type
 * asked (rcode 0, no answers). How long to keep it: the SOA record's TTL or
 * its minimum, whichever is smaller (RFC 2308), else EMPTY_KEEP; at most
 * LONGEST_EMPTY. False for anything else. */
static bool empty_answer(const uint8_t *m, size_t n, uint8_t *rcode, uint32_t *keep)
{
    if (n < 12 || !(m[2] & 0x80))
        return false;
    *rcode = m[3] & 0x0F;
    if (!(*rcode == 3 || (*rcode == 0 && get16(m + 6) == 0)) || get16(m + 4) != 1)
        return false;
    *keep = EMPTY_KEEP;
    size_t at = question_section(m, n);
    if (!at)
        return true;
    at += 12;
    for (int k = get16(m + 6) + get16(m + 8); k > 0; k--) {
        if (!(at = skip_name(m, n, at)) || at + 10 > n)
            break;
        uint16_t type = get16(m + at);
        uint32_t ttl = get32(m + at + 4);
        size_t rdlength = get16(m + at + 8), rdata = at + 10;
        if (rdlength > n - rdata)
            break;
        if (type == 6) {                            /* SOA: two names, then 5 words */
            size_t p = skip_name(m, n, rdata);
            if (p)
                p = skip_name(m, n, p);
            if (p && p + 20 <= rdata + rdlength) {
                uint32_t minimum = get32(m + p + 16);
                *keep = ttl < minimum ? ttl : minimum;
            }
            break;
        }
        at = rdata + rdlength;
    }
    if (*keep > LONGEST_EMPTY)
        *keep = LONGEST_EMPTY;
    return true;
}

static bool same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y)
            return false;
    }
    return *a == *b;
}

/* The DNS server to ask: the first card with one in the kernel's list
 * (every kind of card, in the order the boot found them on PCI), a card
 * whose link is up before one whose link is down (an unplugged cable keeps
 * its fixed address and server); IPv4 servers first, then (64d) a card's
 * IPv6 one. As an IPv6 socket's address (IPv4 as ::ffff:a.b.c.d); false
 * if there is none. */
static bool server(struct sockaddr_in6 *to)
{
    memset(to, 0, sizeof(*to));
    to->sin6_family = AF_INET6;
    for (int linked = 1; linked >= 0; linked--) {
        struct sys_netinfo info;
        for (int i = 0; ritteros_netcard(i, &info) == 0; i++)
            if (info.address && info.dns && (info.link_speed != 0 || !linked)) {
                uint32_t net = htonl(info.dns);
                to->sin6_addr.s6_addr[10] = to->sin6_addr.s6_addr[11] = 0xFF;
                memcpy(to->sin6_addr.s6_addr + 12, &net, 4);
                to->sin6_port = htons(info.dns_port ? (uint16_t)info.dns_port : 53);
                return true;
            }
    }
    for (int linked = 1; linked >= 0; linked--) {
        struct sys_netinfo info;
        for (int i = 0; ritteros_netcard(i, &info) == 0; i++) {
            struct sys_netinfo6 six;
            memset(&six, 0, sizeof(six));
            six.index = (uint16_t)i;
            static const uint8_t none[16];
            if (ritteros_netinfo6(&six) == 0 && memcmp(six.dns, none, 16) &&
                (info.link_speed != 0 || !linked)) {
                memcpy(to->sin6_addr.s6_addr, six.dns, 16);
                if (six.dns[0] == 0xFE && (six.dns[1] & 0xC0) == 0x80)
                    to->sin6_scope_id = (uint32_t)i + 1;     /* a link-local one: by this card */
                to->sin6_port = htons(six.dns_port ? (uint16_t)six.dns_port : 53);
                return true;
            }
        }
    }
    return false;
}

/* (64q) Questions on their way to the server, many at once: until 64q
 * dnsd asked the server one question at a time and waited for its answer
 * (up to two tries of two seconds), so a burst of questions for names it
 * had not kept queued up behind each other. Each question asked (a
 * "leader") has a socket of its own (a new random port each time, as
 * before) and an ID of its own; a question that arrives while the same one
 * is on its way waits for that answer (a "follower") instead of being
 * asked again. */
#define PENDING         64
#define TRY_MS          2000            /* per try; two tries */

static struct pending {
    bool used;
    int fd;                             /* a leader's socket; -1 for a follower */
    struct sockaddr_in6 asker;
    socklen_t asker_length;
    uint8_t q[512];                     /* the question as asked (the asker's ID) */
    size_t length;
    struct sockaddr_in6 to;             /* a leader's server */
    uint8_t id[2];                      /* the ID a leader asks with */
    int tries;
    uint64_t deadline;                  /* ms */
} pending[PENDING];

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static int listener = -1;

static void send_question(struct pending *p)
{
    uint8_t out[512];
    memcpy(out, p->q, p->length);
    out[0] = p->id[0], out[1] = p->id[1];
    sendto(p->fd, out, p->length, 0, (struct sockaddr *)&p->to, sizeof(p->to));
    p->tries++;
    p->deadline = now_ms() + TRY_MS;
}

/* Answer every asker of leader's question with message m (its ID made
 * each asker's own; none if n is 0), and free their entries. */
static void answer_all(struct pending *leader, uint8_t *m, size_t n)
{
    uint8_t q[512];
    size_t length = leader->length;
    memcpy(q, leader->q, length);
    close(leader->fd);
    for (int i = 0; i < PENDING; i++) {
        struct pending *p = &pending[i];
        if (!p->used || !same_question(p->q, p->length, q, length) ||
            (p != leader && p->fd >= 0))
            continue;
        if (n) {
            m[0] = p->q[0], m[1] = p->q[1];
            sendto(listener, m, n, 0, (struct sockaddr *)&p->asker, p->asker_length);
        }
        p->used = false;
    }
}

static void fail_all(struct pending *leader, int rcode)
{
    answer_all(leader, reply, dns_failure(leader->q, leader->length, rcode, reply, sizeof(reply)));
}

/* Keep a simple answer (an address, or (64k) an empty one). */
static void keep(const uint8_t *q, size_t length, const uint8_t *m, size_t got)
{
    char name[DNS_NAME_MAX + 1];
    uint16_t type = 0;
    if (!dns_question_type(q, length, name, sizeof(name), &type) || (type != 1 && type != 28))
        return;
    uint8_t address[16] = { 0 }, rcode = 0;
    uint32_t v4, ttl;
    bool kept = false, empty = false;
    if (empty_answer(m, got, &rcode, &ttl)) {
        kept = empty = true;
    } else if (type == 28) {
        kept = dns_answer_aaaa(m, got, address, &ttl) == 0;
    } else if (dns_answer(m, got, &v4, &ttl) == 0) {
        address[0] = (uint8_t)(v4 >> 24), address[1] = (uint8_t)(v4 >> 16);
        address[2] = (uint8_t)(v4 >> 8), address[3] = (uint8_t)v4;
        kept = true;
    }
    if (!kept)
        return;
    uint64_t now = seconds();
    int slot = 0;
    for (int i = 0; i < CACHE; i++)
        if (cache[i].until < cache[slot].until)
            slot = i;
    snprintf(cache[slot].name, sizeof(cache[slot].name), "%s", name);
    cache[slot].type = type;
    memcpy(cache[slot].address, address, 16);
    cache[slot].empty = empty;
    cache[slot].rcode = rcode;
    cache[slot].until = now + (ttl > LONGEST_KEEP ? LONGEST_KEEP : ttl);
}

/* An answer on a leader's socket: only from the server asked, with the ID
 * and the question it was asked. */
static void server_answered(struct pending *p)
{
    struct sockaddr_in6 from;
    socklen_t from_length = sizeof(from);
    ssize_t n = recvfrom(p->fd, answer, sizeof(answer), MSG_DONTWAIT, (struct sockaddr *)&from,
                         &from_length);
    if (n < 12 || memcmp(&from.sin6_addr, &p->to.sin6_addr, 16) || from.sin6_port != p->to.sin6_port)
        return;
    if (!(answer[2] & 0x80) || answer[0] != p->id[0] || answer[1] != p->id[1] ||
        !same_question(answer, (size_t)n, p->q, p->length))
        return;
    keep(p->q, p->length, answer, (size_t)n);
    answer_all(p, answer, (size_t)n);
}

/* A question from an asker: from the cache, else with the same question
 * already on its way, else asked. */
static void asked(const uint8_t *m, size_t length, struct sockaddr_in6 *from, socklen_t from_length)
{
    char name[DNS_NAME_MAX + 1];
    uint16_t type = 0;
    bool simple = dns_question_type(m, length, name, sizeof(name), &type) &&
                  (type == 1 || type == 28);
    uint64_t now = seconds();
    size_t out = 0;
    for (int i = 0; simple && i < CACHE && !out; i++) {
        if (cache[i].until <= now || cache[i].type != type || !same_name(cache[i].name, name))
            continue;
        uint32_t left = (uint32_t)(cache[i].until - now);
        const uint8_t *a = cache[i].address;
        if (cache[i].empty)                         /* (64k) as the server said */
            out = dns_failure(m, length, cache[i].rcode, reply, sizeof(reply));
        else
            out = type == 28 ? dns_reply_aaaa(m, length, a, left, reply, sizeof(reply))
            : dns_reply(m, length, (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 |
                        (uint32_t)a[2] << 8 | a[3], left, reply, sizeof(reply));
    }
    if (out) {
        sendto(listener, reply, out, 0, (struct sockaddr *)from, from_length);
        return;
    }
    struct pending *leader = 0, *free_slot = 0;
    for (int i = 0; i < PENDING; i++) {
        struct pending *p = &pending[i];
        if (!p->used) {
            if (!free_slot)
                free_slot = p;
            continue;
        }
        if (p->length == length && !memcmp(p->q, m, length) && p->asker_length == from_length &&
            !memcmp(&p->asker, from, from_length))
            return;                                 /* asked again while it waits: one answer */
        if (p->fd >= 0 && same_question(p->q, p->length, m, length))
            leader = p;
    }
    struct sockaddr_in6 to;
    int fd = -1;
    if (!free_slot || (!leader && (!server(&to) || (fd = socket(AF_INET6, SOCK_DGRAM, 0)) < 0))) {
        /* Refused: no server known; server failure: no room or no socket. */
        out = dns_failure(m, length, free_slot && !server(&to) ? 5 : 2, reply, sizeof(reply));
        if (out)
            sendto(listener, reply, out, 0, (struct sockaddr *)from, from_length);
        return;
    }
    struct pending *p = free_slot;
    memset(p, 0, sizeof(*p));
    p->used = true;
    memcpy(p->q, m, length);
    p->length = length;
    memcpy(&p->asker, from, from_length);
    p->asker_length = from_length;
    p->fd = fd;
    if (leader)
        return;                                     /* it waits for the leader's answer */
    p->to = to;
    ritteros_random(p->id, sizeof(p->id));
    send_question(p);
}

int main(void)
{
    listener = socket(AF_INET6, SOCK_DGRAM, 0);     /* (64d: IPv6 and IPv4 askers) */
    struct sockaddr_in6 here;
    memset(&here, 0, sizeof(here));
    here.sin6_family = AF_INET6;
    here.sin6_port = htons(PORT);
    if (listener < 0 || bind(listener, (struct sockaddr *)&here, sizeof(here)) < 0) {
        printf("dnsd: port %d: %s\n", PORT, strerror(errno));
        return 1;
    }
    for (;;) {
        struct pollfd fds[1 + PENDING];
        int which[1 + PENDING], count = 1;
        fds[0] = (struct pollfd){ .fd = listener, .events = POLLIN };
        uint64_t now = now_ms(), soonest = 0;
        for (int i = 0; i < PENDING; i++) {
            if (!pending[i].used || pending[i].fd < 0)
                continue;
            fds[count] = (struct pollfd){ .fd = pending[i].fd, .events = POLLIN };
            which[count++] = i;
            if (!soonest || pending[i].deadline < soonest)
                soonest = pending[i].deadline;
        }
        int wait = !soonest ? -1 : soonest <= now ? 0 : (int)(soonest - now);
        if (poll(fds, (nfds_t)count, wait) < 0)
            continue;
        for (int k = 1; k < count; k++)
            if ((fds[k].revents & POLLIN) && pending[which[k]].used && pending[which[k]].fd == fds[k].fd)
                server_answered(&pending[which[k]]);
        now = now_ms();
        for (int i = 0; i < PENDING; i++) {
            struct pending *p = &pending[i];
            if (!p->used || p->fd < 0 || p->deadline > now)
                continue;
            if (p->tries < 2)
                send_question(p);
            else
                fail_all(p, 2);                     /* server failure: it did not answer */
        }
        if (fds[0].revents & POLLIN) {
            struct sockaddr_in6 from;
            socklen_t from_length = sizeof(from);
            ssize_t n = recvfrom(listener, question, sizeof(question), MSG_DONTWAIT,
                                 (struct sockaddr *)&from, &from_length);
            if (n < 12 || (question[2] & 0x80))
                continue;                           /* not even a header, or a response */
            asked(question, (size_t)n, &from, from_length);
        }
    }
}

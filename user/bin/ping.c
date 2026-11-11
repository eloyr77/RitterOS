/*
 * ping [-4|-6] [-c COUNT] HOST: send ICMP echo requests (one a second, 4
 * unless COUNT says otherwise) and show each answer's round trip, then a
 * summary. Ctrl-C stops it early. HOST may be a name (asked of the DNS
 * server through getaddrinfo, 64d: IPv6 first on a network with IPv6,
 * then IPv4, as every program; -4 or -6 for one family; the user's) or
 * an address, IPv4 or (since 64a) IPv6 (fe80::1%eth0: a link-local one
 * with its card after the %), pinged in its own family.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("ping [-4|-6] [-c COUNT] HOST", "send ICMP echo requests and show the round trips");

#define SIZE 56

static void ms(char *out, size_t n, int64_t us)
{
    snprintf(out, n, "%ld.%02ld", (long)(us / 1000), (long)(us % 1000 / 10));
}

/* An IPv6 address, "ADDRESS" or "ADDRESS%CARD", into *to: 0, or a message. */
static const char *ipv6_address(const char *text, struct sys_inaddr *to)
{
    char address[INET6_ADDRSTRLEN];
    const char *card = strchr(text, '%');
    size_t n = card ? (size_t)(card - text) : strlen(text);
    if (n >= sizeof(address))
        return "not an IPv6 address";
    memcpy(address, text, n);
    address[n] = 0;
    memset(to, 0, sizeof(*to));
    to->family = AF_INET6;
    if (inet_pton(AF_INET6, address, to->address) != 1)
        return "not an IPv6 address";
    bool link = (to->address[0] == 0xFE && (to->address[1] & 0xC0) == 0x80) ||
                (to->address[0] == 0xFF && (to->address[1] & 15) <= 2);   /* fe80::/10, ff02:: */
    if (!card)
        return link ? "a link-local address needs its card (ADDRESS%eth0)" : 0;
    struct sys_netinfo info;
    for (int i = 0; ritteros_netcard(i, &info) == 0; i++) {
        if (strcmp(info.name, card + 1) == 0) {
            to->scope = (uint32_t)i + 1;
            return 0;
        }
    }
    return "no such card";
}

/* An address to ping and how it is shown ("10.0.2.2", "fe80::1%eth0"). */
struct target {
    struct sys_inaddr to;
    char shown[INET6_ADDRSTRLEN + 8];
};

static void show(struct target *t)
{
    bool six = t->to.family == AF_INET6;
    inet_ntop(six ? AF_INET6 : AF_INET, six ? t->to.address : t->to.address + 12, t->shown,
              sizeof(t->shown));
    struct sys_netinfo card;
    if (six && t->to.scope && ritteros_netcard((int)t->to.scope - 1, &card) == 0) {
        size_t used = strlen(t->shown);
        snprintf(t->shown + used, sizeof(t->shown) - used, "%%%s", card.name);
    }
}

/* HOST's addresses to try, in order, into t (at most max): an address
 * written out as it is (its family must be the one forced), a name's by
 * getaddrinfo (64d: IPv6 first on a network with IPv6, as every program
 * takes them; -4/-6 ask for one family). Their count, or 0 (a message
 * written). */
static int targets(const char *host, int family, struct target *t, int max)
{
    memset(t, 0, sizeof(*t));
    uint8_t v4[4];
    if (inet_pton(AF_INET, host, v4) == 1) {
        if (family == 6) {
            dprintf(STDERR_FILENO, "ping: %s: not an IPv6 address\n", host);
            return 0;
        }
        t->to.family = AF_INET;
        t->to.address[10] = t->to.address[11] = 0xFF;
        memcpy(t->to.address + 12, v4, 4);
        show(t);
        return 1;
    }
    if (strchr(host, ':')) {
        if (family == 4) {
            dprintf(STDERR_FILENO, "ping: %s: not an IPv4 address\n", host);
            return 0;
        }
        const char *wrong = ipv6_address(host, &t->to);
        if (wrong) {
            dprintf(STDERR_FILENO, "ping: %s: %s\n", host, wrong);
            return 0;
        }
        show(t);
        return 1;
    }
    struct addrinfo hints, *list;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family == 4 ? AF_INET : family == 6 ? AF_INET6 : AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    errno = 0;
    int r = getaddrinfo(host, 0, &hints, &list);
    if (r) {
        if (!errno)
            errno = r == EAI_NONAME ? ENONAME : EINVAL;
        dprintf(STDERR_FILENO, "ping: %s: %s\n", host, strerror(errno));
        return 0;
    }
    int n = 0;
    for (struct addrinfo *a = list; a && n < max; a = a->ai_next) {
        memset(&t[n], 0, sizeof(t[n]));
        if (a->ai_family == AF_INET6) {
            const struct sockaddr_in6 *in = (const struct sockaddr_in6 *)a->ai_addr;
            t[n].to.family = AF_INET6;
            memcpy(t[n].to.address, in->sin6_addr.s6_addr, 16);
            t[n].to.scope = in->sin6_scope_id;
        } else {
            const struct sockaddr_in *in = (const struct sockaddr_in *)a->ai_addr;
            t[n].to.family = AF_INET;
            t[n].to.address[10] = t[n].to.address[11] = 0xFF;
            memcpy(t[n].to.address + 12, &in->sin_addr.s_addr, 4);
        }
        show(&t[n++]);
    }
    freeaddrinfo(list);
    return n;
}

int main(int argc, char **argv)
{
    long count = 4;
    int arg = 1, family = 0;                        /* 4, 6, or 0: as HOST says */
    while (arg < argc - 1 && argv[arg][0] == '-') {
        if (strcmp(argv[arg], "-4") == 0 || strcmp(argv[arg], "-6") == 0) {
            family = argv[arg][1] - '0';
            arg++;
        } else if (strcmp(argv[arg], "-c") == 0 && arg + 2 < argc) {
            count = 0;
            const char *c = argv[arg + 1];
            for (; *c >= '0' && *c <= '9' && count < 1000000; c++)
                count = count * 10 + (*c - '0');
            if (*c || c == argv[arg + 1])
                count = 0;
            arg += 2;
        } else {
            break;
        }
    }
    if (argc != arg + 1 || count < 1) {
        dprintf(STDERR_FILENO, "usage: ping [-4|-6] [-c COUNT] HOST\n");
        return 2;
    }
    struct target t[8];
    int n = targets(argv[arg], family, t, 8);
    if (!n)
        return 2;
    /* The first request to each address in turn until one can be sent (a
     * name's IPv6 address on a machine whose IPv6 does not reach it, say:
     * then its IPv4 one). */
    uint16_t id = (uint16_t)getpid();
    int k = 0;
    int64_t first;
    for (;; k++) {
        first = ritteros_ping6(&t[k].to, id, 1, SIZE, 2000);
        if (first >= 0 || k + 1 == n || (errno != ENETUNREACH && errno != EHOSTUNREACH))
            break;
    }
    const char *shown = t[k].shown;
    if (strcmp(shown, argv[arg]) == 0)
        printf("PING %s: %d data bytes\n", shown, SIZE);
    else
        printf("PING %s (%s): %d data bytes\n", argv[arg], shown, SIZE);
    long received = 0, sent = 0;
    int64_t min = 0, max = 0, sum = 0;
    for (long seq = 1; seq <= count; seq++) {
        int64_t us = first;
        if (seq > 1) {
            sleep(1);
            us = ritteros_ping6(&t[k].to, id, (uint16_t)seq, SIZE, 2000);
        }
        sent++;
        if (us < 0 && errno == EINTR)
            break;
        if (us < 0) {
            printf("seq=%ld: %s\n", seq, strerror(errno));
            continue;
        }
        char text[24];
        ms(text, sizeof(text), us);
        printf("%d bytes from %s: seq=%ld time=%s ms\n", SIZE + 8, shown, seq, text);
        min = received == 0 || us < min ? us : min;
        max = us > max ? us : max;
        sum += us;
        received++;
    }
    printf("--- %s: %ld sent, %ld received, %ld%% lost", argv[arg], sent, received,
           sent ? (sent - received) * 100 / sent : 0);
    if (received) {
        char a[24], b[24], c[24];
        ms(a, sizeof(a), min), ms(b, sizeof(b), sum / received), ms(c, sizeof(c), max);
        printf("; round trip min/avg/max %s/%s/%s ms", a, b, c);
    }
    printf("\n");
    return received ? 0 : 1;
}

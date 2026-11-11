/*
 * An Internet address of either family (64c): what TCP, UDP, the sockets
 * and the firewall hold. 16 bytes as on the wire; IPv4 as ::ffff:a.b.c.d
 * (RFC 4291's IPv4-mapped form), so one comparison serves both, and one
 * TCP and one UDP carry both. `scope` names the card of a link-local
 * IPv6 address (its index in the card list + 1, as sys_inaddr's; 0: none).
 * The helpers that send are in ipv6.c: they choose IPv4 or IPv6 by the
 * address.
 */
#ifndef RITTEROS_INET_H
#define RITTEROS_INET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct inet_addr {
    uint8_t b[16];
    uint32_t scope;
};

/* Which families a socket takes (64d): an IPv4 socket's INET_V4, an IPv6
 * one's both, or INET_V6 alone with IPV6_V6ONLY. */
#define INET_V4 1u
#define INET_V6 2u

/* The family bit of an address. */
static inline unsigned inet_family(const struct inet_addr *a);

static inline struct inet_addr inet_v4(uint32_t a)
{
    struct inet_addr x = { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, (uint8_t)(a >> 24),
                             (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a }, 0 };
    return x;
}

static inline bool inet_is_v4(const struct inet_addr *a)
{
    for (int i = 0; i < 10; i++)
        if (a->b[i])
            return false;
    return a->b[10] == 0xFF && a->b[11] == 0xFF;
}

static inline unsigned inet_family(const struct inet_addr *a)
{
    return inet_is_v4(a) ? INET_V4 : INET_V6;
}

/* The IPv4 address in host order (0 for an IPv6 one). */
static inline uint32_t inet_v4_of(const struct inet_addr *a)
{
    return inet_is_v4(a) ? (uint32_t)a->b[12] << 24 | (uint32_t)a->b[13] << 16 |
                           (uint32_t)a->b[14] << 8 | a->b[15]
                         : 0;
}

/* The same address (the scope is not compared: link-local addresses of
 * two cards are told apart by the card the packet came on). */
static inline bool inet_same(const struct inet_addr *a, const struct inet_addr *b)
{
    for (int i = 0; i < 16; i++)
        if (a->b[i] != b->b[i])
            return false;
    return true;
}

/* No address: :: or 0.0.0.0. */
static inline bool inet_none(const struct inet_addr *a)
{
    for (int i = 0; i < 16; i++)
        if (a->b[i] && !(inet_is_v4(a) && i < 12))
            return false;
    return true;
}

/* The sum of the transport checksum's pseudo-header (IPv4's 12 bytes or
 * IPv6's 40) for a payload of `length` bytes of `protocol`. */
uint32_t inet_pseudo(const struct inet_addr *src, const struct inet_addr *dst, uint8_t protocol,
                     size_t length);

/* The address a packet to dst leaves from (a card's, by dst's route; dst
 * itself for this machine): false without one. */
bool inet_source(const struct inet_addr *dst, struct inet_addr *src);

/* Send a transport packet (its checksum made with src) to dst; 0 or ERR_*. */
int inet_send(const struct inet_addr *src, const struct inet_addr *dst, uint8_t protocol,
              const void *payload, size_t length);

/* The most data a TCP segment to dst may carry: the link's MTU less the
 * headers (1460 for IPv4 on Ethernet, 1440 for IPv6). */
uint32_t inet_mss(const struct inet_addr *dst);

/* The biggest transport payload one packet to dst carries. */
size_t inet_payload_max(const struct inet_addr *dst);

#endif

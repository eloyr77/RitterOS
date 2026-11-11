/*
 * IPv6 over Ethernet (64a; roadmap item 29): each card's link-local
 * address (fe80:: and the card's MAC as an EUI-64), checked for
 * duplicates when its link comes up; neighbour discovery (IPv6's ARP:
 * solicitations to a multicast group made from the address, answers with
 * the MAC); MLD reports of the groups a card listens to; ICMPv6 echo
 * (ping), answered and sent. ::1 is the loopback's (lo, as 127.0.0.1).
 * Addresses are 16 bytes in network order, as on the wire.
 */
#ifndef RITTEROS_IPV6_H
#define RITTEROS_IPV6_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct netdev;

#define IP6_HEADER          40
#define IP6_PAYLOAD_MAX     (1500 - IP6_HEADER)

/* An IPv6 packet that arrived on card d (src_mac: the frame's Ethernet
 * source; 0 for the loopback). */
void ip6_input(struct netdev *d, const uint8_t *src_mac, const uint8_t *packet, size_t length);

/* Every LINK_LOOK (100 ms), from net.c's thread link: start a card whose
 * link came up (its link-local address), drop the addresses of one whose
 * link went, and take the duplicate checks' next steps. */
void ip6_tick(void);

/* One ICMPv6 echo to dst (on card `card` for a link-local or multicast
 * address, else 0): the round trip in microseconds, or -ERR_*. */
int64_t icmp6_ping(const uint8_t dst[16], struct netdev *card, uint16_t id, uint16_t seq,
                   size_t size, uint32_t timeout_ms);

struct sys_netset6;

/* A card's IPv6 settings (SYS_NETSET6): 0 or ERR_INVAL. A change of mode or
 * of the fixed address starts the card again (its addresses checked anew);
 * the same settings again change nothing. */
int ip6_configure(struct netdev *d, const struct sys_netset6 *settings);

/* (64d) What SYS_NETINFO6 tells of card d (info's name and index already
 * set): its addresses, router, DNS server and MTU. */
struct sys_netinfo6;
void ip6_info(struct netdev *d, struct sys_netinfo6 *info);

/* (64g) A DHCPv6 lease of card d (SYS_NETLEASE6): 0 or ERR_*. */
struct sys_netlease6;
int ip6_lease(struct netdev *d, const struct sys_netlease6 *lease);

/* net's report: a card's IPv6 lines, lo's, the routes, and the neighbours
 * known. */
void ip6_print_card(struct netdev *d);
void ip6_print_routes(void);
void ip6_print_loopback(void);
void ip6_print_neighbours(void);

#endif

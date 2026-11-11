/*
 * IPv4 (kernel/ipv4.c): Ethernet frames in and out, ARP (finding a
 * neighbour's MAC address), IPv4 packets (no fragments), routes and forwarding
 * between cards (no NAT), the loopback interface lo, ICMP echo (ping). Addresses
 * are in host byte order.
 */
#ifndef RITTEROS_IPV4_H
#define RITTEROS_IPV4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct netdev;

#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17
#define IP_PAYLOAD_MAX  (1500 - 20)

/* A frame from a card (net_receive): ARP or IPv4. */
void ether_input(struct netdev *d, const uint8_t *frame, size_t length);

/* Send an IPv4 packet with this payload to dst, on the card and to the
 * next hop its route names; to 127.0.0.0/8 or a card's own address
 * through the loopback. 0 or ERR_* (ERR_NETUNREACH: no address or no
 * route; ERR_HOSTUNREACH: no answer to ARP). */
int ip_send(uint32_t dst, uint8_t protocol, const void *payload, size_t length);

/* The same from address src (0: ip_source(dst)). */
int ip_send_from(uint32_t src, uint32_t dst, uint8_t protocol, const void *payload, size_t length);

/* A broadcast (to 255.255.255.255) on card d (DHCP: also without an
 * address). */
int ip_broadcast(struct netdev *d, uint8_t protocol, const void *payload, size_t length);

/* The address packets to dst are sent from: dst itself for this machine,
 * else the address of the card its route takes (0: no route). */
uint32_t ip_source(uint32_t dst);

/* The interface (the first card with an address), or 0. */
struct netdev *ip_interface(void);

/* The loopback interface lo (127.0.0.1/8), never in the list of cards. */
struct netdev *ip_loopback(void);

/* A packet for this machine (IPv4 or IPv6) into the loopback queue: the
 * thread loopback delivers it as a card's thread would. */
void ip_loop_send(const uint8_t *packet, size_t length);

/* Forwarding packets between cards (off at the start). */
void ip_set_forwarding(bool on);
bool ip_forwarding(void);

/* Masquerade (source NAT, 49i): the cards marked private (none at the
 * start). A packet forwarded from a private card out a non-private one has
 * its source rewritten to that card's address. */
void ip_masquerade_reset(void);
int ip_masquerade_add(struct netdev *d);
bool ip_masquerade_on(void);

/* The default route's cards (50g), by name, in order of preference: the
 * first with a link, an address and a gateway carries it; when none can,
 * the first other card with a gateway (as without a preference). */
void ip_default_reset(void);
int ip_default_add(const char *name);   /* 0, or -1 if the list is full */
/* The preferred cards' names (net default), in order, into names: how many. IPv6's
 * default route (64b) takes them too. */
int ip_default_order(char (*names)[8], int max);

/* For net's report: the routes ("route: ..." lines) and the forwarding
 * switch with its counters. */
void ip_print_routes(void);
void ip_print_forwarding(void);

/* Send an echo request (ICMP) and wait for the answer: the round trip in
 * microseconds, or -ERR_* (ERR_TIMEDOUT, ERR_INTR on Ctrl-C). */
int64_t icmp_ping(uint32_t dst, uint16_t id, uint16_t seq, size_t size, uint32_t timeout_ms);

/* The ARP cache, for the report: entry index (false past the end). */
bool arp_entry(int index, uint32_t *ip, uint8_t *mac);

/* The Internet checksum (RFC 1071) of a buffer, added to sum. */
uint32_t ip_checksum_add(uint32_t sum, const void *data, size_t length);
uint16_t ip_checksum_fold(uint32_t sum);

#endif

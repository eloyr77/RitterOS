/*
 * UDP (kernel/udp.c): datagrams to and from ports, for programs' sockets,
 * over IPv4 and (64c) IPv6 alike: addresses are struct inet_addr.
 */
#ifndef RITTEROS_UDP_H
#define RITTEROS_UDP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "inet.h"

struct netdev;

/* Called for a datagram to a bound port (from the card's thread, or the
 * sender's for the loopback). */
typedef void (*udp_handler)(void *context, const struct inet_addr *src, uint16_t src_port,
                            const uint8_t *data, size_t length);

/* Bind port (card: for what arrives on that card only; 0: any card). A
 * port may be bound once per card, or once for all. families: INET_V4,
 * INET_V6 or both (inet.h): the datagrams that come. */
bool udp_bind(uint16_t port, struct netdev *card, unsigned families, udp_handler handler,
              void *context);
void udp_unbind(uint16_t port, struct netdev *card);

/* A free port from 49152 up (not bound yet). */
uint16_t udp_ephemeral_port(void);

/* Send a datagram; 0 or ERR_* (as ip_send). */
int udp_send(uint16_t src_port, const struct inet_addr *dst, uint16_t dst_port, const void *data,
             size_t length);

/* A broadcast (to 255.255.255.255) on card d (programs' DHCP), from its
 * address or 0.0.0.0. */
int udp_broadcast(struct netdev *d, uint16_t src_port, uint16_t dst_port, const void *data,
                  size_t length);

/* (64j) net's udp: line: ports bound, datagrams dropped by full queues. */
void udp_print(void);

/* A UDP packet that IPv4 or IPv6 received on card d. */
void udp_input(struct netdev *d, const struct inet_addr *src, const struct inet_addr *dst,
               const uint8_t *p, size_t length);

#endif

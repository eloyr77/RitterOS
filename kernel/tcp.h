/*
 * TCP (kernel/tcp.c): connections for sockets.
 */
#ifndef RITTEROS_TCP_H
#define RITTEROS_TCP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "inet.h"

struct tcp;

/* families (64c, 64d): INET_V4 for an IPv4 socket's connection or
 * listener, both for an IPv6 socket's, INET_V6 alone with IPV6_V6ONLY:
 * the peers it takes. */
int tcp_bind(struct tcp **t, uint16_t port, unsigned families);
int tcp_connect(struct tcp **t, const struct inet_addr *ip, uint16_t port, unsigned families);
int tcp_listen(struct tcp **t, int backlog, unsigned families);
int tcp_accept(struct tcp *listener, struct tcp **out);
int64_t tcp_send(struct tcp *t, const void *data, size_t length, uint32_t timeout_ms);
int64_t tcp_receive(struct tcp *t, void *data, size_t size, uint32_t timeout_ms);
struct inet_addr tcp_peer(struct tcp *t, uint16_t *port);
uint16_t tcp_local_port(struct tcp *t);
/* (53a) The address the connection is made from, and shutdown's sides. */
struct inet_addr tcp_local_address(struct tcp *t);
/* (53b) A connection that ended without the program's close: its error
 * (ERR_CONNREFUSED, ERR_CONNRESET, ...), else 0 (SO_ERROR). */
int tcp_error(struct tcp *t);
void tcp_shutdown(struct tcp *t, bool read, bool write);
void tcp_close(struct tcp *t);

/* net's report: connections and what happened to segments. */
void tcp_print(void);

/* Tests: lose one segment in one_in (both ways); 0 or 1: none. */
void tcp_set_loss(uint32_t one_in);

/* A TCP segment that IPv4 or IPv6 received. */
void tcp_input(const struct inet_addr *src, const struct inet_addr *dst, const uint8_t *p,
               size_t length);

/* (64c) An ICMPv6 "packet too big" for a segment of ours: the
 * connection's segments get smaller (at least for IPv6's 1280). */
void tcp_path_mtu(const struct inet_addr *local, const struct inet_addr *remote, uint16_t local_port,
                  uint16_t remote_port, uint32_t mtu);

/* (52d) For poll: POLLIN (data, the peer's end, or a connection to accept),
 * POLLOUT (room to send), POLLHUP (closed). */
int tcp_poll(struct tcp *t);

/* (65e) The end of a card's burst of frames (net_batch_end): the ACKs owed
 * for data that came in order go out, one per connection. */
void tcp_batch_end(void);

#endif

/*
 * Sockets (kernel/socket.c): the network for programs. UDP sockets send
 * and receive datagrams; TCP sockets (kernel/tcp.c) are connections. The
 * system calls (process.c) copy data between the program and these.
 */
#ifndef RITTEROS_SOCKET_H
#define RITTEROS_SOCKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "inet.h"
#include "syscalls.h"                       /* SOCKET_UDP, SOCKET_TCP */

struct socket;
struct netdev;

/* One more holder (a child got the descriptor): close frees it after the
 * last. */
void socket_hold(struct socket *s);

/* The socket's type (SOCKET_UDP, SOCKET_TCP, SOCKET_LOCAL). */
int socket_type(struct socket *s);

/* (64j) Datagrams dropped so far because a UDP socket's queue was full. */
uint64_t socket_udp_dropped(void);

/* Pass / take a shared buffer over a local socket (SYS_SHARE SEND/RECV).
 * send takes a held reference; recv returns one (0 when none waits). */
struct shbuf;
int socket_send_buffer(struct socket *s, struct shbuf *b);
struct shbuf *socket_recv_buffer(struct socket *s);

/* type: SOCKET_UDP or SOCKET_TCP, | SOCKET_INET6 for an IPv6 socket
 * (64c), or SOCKET_LOCAL. */
struct socket *socket_new(int type);

/* (52d) POLLIN, POLLOUT, POLLHUP: what a read or write would find (a
 * listener is readable with a connection to accept). */
int socket_poll(struct socket *s);
/* Bind to a port (0: any free one). 0 or ERR_*. */
int socket_bind(struct socket *s, uint16_t port);

/* Local (SOCKET_LOCAL) sockets: a name instead of a port. bind_local names
 * the listener (then socket_listen registers it); connect_local connects to
 * a listener's name. 0 or ERR_*. */
int socket_bind_local(struct socket *s, const char *name, int uid);
int socket_connect_local(struct socket *s, const char *name, int uid);
/* The user at the other end of a connected local socket; -1 otherwise. */
int socket_peer_uid(struct socket *s);

/* A UDP socket of one card only, before bind: it receives what arrives on
 * that card, and its broadcasts leave through it (DHCP). 0 or ERR_*. */
int socket_bind_card(struct socket *s, struct netdev *card);
/* UDP: the default destination; TCP: connect (waits). 0 or ERR_*. */
int socket_connect(struct socket *s, const struct inet_addr *ip, uint16_t port);
/* Send (to 0: to the connected peer); bytes sent or -ERR_*. */
int64_t socket_sendto(struct socket *s, const void *data, size_t length, const struct inet_addr *to,
                      uint16_t port);
/* Receive (timeout_ms 0: wait as long as it takes); bytes or -ERR_*
 * (0 for TCP: the peer closed). The sender's address and port to *ip,
 * *port if not 0. */
int64_t socket_recvfrom(struct socket *s, void *data, size_t size, struct inet_addr *ip,
                        uint16_t *port, uint32_t timeout_ms);
/* TCP: wait for connections; take one. 0 or ERR_* / the new socket. */
int socket_listen(struct socket *s, int backlog);
struct socket *socket_accept(struct socket *s, int *error);
/* The peer (TCP: of the connection; UDP: the connected one). */
struct inet_addr socket_peer(struct socket *s, uint16_t *port);

/* (53a) shutdown (SHUTDOWN_*), a socket's own or peer's address, and its
 * options (SOCKOPT_*); 0 or an ERR_*. */
int socket_shutdown(struct socket *s, int how);
int socket_name(struct socket *s, bool peer, struct sys_sockname *out);
/* (64c) The same as an address of either family (an IPv6 socket's "any"
 * is ::), and whether the socket is an IPv6 one. */
int socket_name6(struct socket *s, bool peer, struct inet_addr *address, uint16_t *port);
bool socket_six(struct socket *s);
int socket_option(struct socket *s, bool set, int option, uint64_t *value);
/* The local port (0: none yet). */
uint16_t socket_port(struct socket *s);
/* A holder lets go; after the last, the socket ends and is freed. */
void socket_close(struct socket *s);

#endif

/* BSD sockets (53a): connections and datagrams by address. AF_INET is
 * IPv4 over TCP (SOCK_STREAM) and UDP (SOCK_DGRAM), AF_INET6 (64d) IPv6
 * and IPv4 (netinet/in.h); AF_UNIX the local stream sockets, named by
 * sun_path in a kernel registry (no file is made). Binding takes a port;
 * the socket answers on every address the machine has. Calls return -1
 * with errno set on errors. */
#ifndef RITTEROS_SYS_SOCKET_H
#define RITTEROS_SYS_SOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "ritteros.h"

typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

/* Room for any address (sockaddr_in, sockaddr_in6, sockaddr_un). */
struct sockaddr_storage {
    sa_family_t ss_family;
    char ss_padding[118];
    uint64_t ss_align;
};

#define AF_UNSPEC       0
#define AF_UNIX         1
#define AF_LOCAL        AF_UNIX
#define AF_INET         2
#define AF_INET6        10
#define PF_UNSPEC       AF_UNSPEC
#define PF_UNIX         AF_UNIX
#define PF_LOCAL        AF_UNIX
#define PF_INET         AF_INET
#define PF_INET6        AF_INET6

#define SOCK_STREAM     1
#define SOCK_DGRAM      2

#define SOMAXCONN       128

/* setsockopt/getsockopt at SOL_SOCKET. SO_RCVTIMEO and SO_SNDTIMEO (a
 * struct timeval) end a wait with EAGAIN; SO_REUSEADDR, SO_KEEPALIVE and
 * SO_BROADCAST are kept and change nothing (a port is only taken while
 * something listens on it; broadcasts are allowed); SO_ERROR reads 0,
 * SO_TYPE the SOCK_* kind, the buffers 65536 (setting them changes
 * nothing). Values are ints unless said. */
#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_BROADCAST    6
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21

/* send/recv flags: MSG_NOSIGNAL is taken (no signal is ever sent for a
 * closed connection: the call fails with EPIPE); MSG_DONTWAIT (53b) makes
 * this call not wait (EAGAIN), as O_NONBLOCK does; MSG_PEEK is EOPNOTSUPP. */
#define MSG_PEEK        0x2
#define MSG_DONTWAIT    0x40
#define MSG_WAITALL     0x100
#define MSG_NOSIGNAL    0x4000

#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

int socket(int family, int type, int protocol);
int bind(int fd, const struct sockaddr *address, socklen_t length);
int connect(int fd, const struct sockaddr *address, socklen_t length);
int listen(int fd, int backlog);
/* A new descriptor for the next connection; its peer's address into
 * address (if not 0; *length says its room and gets the size). */
int accept(int fd, struct sockaddr *address, socklen_t *length);
ssize_t send(int fd, const void *data, size_t size, int flags);
ssize_t recv(int fd, void *data, size_t size, int flags);
ssize_t sendto(int fd, const void *data, size_t size, int flags, const struct sockaddr *to,
               socklen_t length);
ssize_t recvfrom(int fd, void *data, size_t size, int flags, struct sockaddr *from,
                 socklen_t *length);
/* End a connection's receiving side (SHUT_RD: reads give the end), its
 * sending side (SHUT_WR: the peer reads the end; this side reads on), or
 * both; the descriptor stays until close. */
int shutdown(int fd, int how);
/* The socket's own address, and its peer's (ENOTCONN: none). */
int getsockname(int fd, struct sockaddr *address, socklen_t *length);
int getpeername(int fd, struct sockaddr *address, socklen_t *length);
int setsockopt(int fd, int level, int option, const void *value, socklen_t length);
int getsockopt(int fd, int level, int option, void *value, socklen_t *length);

#endif

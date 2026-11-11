/* Internet addresses (53a): IPv4's sockaddr_in, and (64a, 64d) IPv6's
 * in6_addr (16 bytes as on the wire) and sockaddr_in6. Addresses and
 * ports are in network byte order (big-endian) in them: htonl, htons. An
 * AF_INET6 socket talks to IPv4 peers too, as ::ffff:a.b.c.d, unless
 * IPV6_V6ONLY (level IPPROTO_IPV6) is set before it is bound. */
#ifndef RITTEROS_NETINET_IN_H
#define RITTEROS_NETINET_IN_H

#include <stdint.h>
#include <sys/socket.h>

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr {
    in_addr_t s_addr;
};

struct in6_addr {
    uint8_t s6_addr[16];
};

struct sockaddr_in {
    sa_family_t sin_family;     /* AF_INET */
    in_port_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};

struct sockaddr_in6 {
    sa_family_t sin6_family;    /* AF_INET6 */
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;     /* (not used) */
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;     /* a link-local address's card: its place in net's list + 1 */
};

/* :: and ::1 (calls: no data leaves the library). */
const struct in6_addr *__ritteros_in6addr_any(void);
const struct in6_addr *__ritteros_in6addr_loopback(void);
#define in6addr_any         (*__ritteros_in6addr_any())
#define in6addr_loopback    (*__ritteros_in6addr_loopback())
#define IN6ADDR_ANY_INIT        { { 0 } }
#define IN6ADDR_LOOPBACK_INIT   { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } }

#define IN6_IS_ADDR_UNSPECIFIED(a) \
    (((const uint32_t *)(a)->s6_addr)[0] == 0 && ((const uint32_t *)(a)->s6_addr)[1] == 0 && \
     ((const uint32_t *)(a)->s6_addr)[2] == 0 && ((const uint32_t *)(a)->s6_addr)[3] == 0)
#define IN6_IS_ADDR_LOOPBACK(a) \
    (((const uint32_t *)(a)->s6_addr)[0] == 0 && ((const uint32_t *)(a)->s6_addr)[1] == 0 && \
     ((const uint32_t *)(a)->s6_addr)[2] == 0 && ((const uint32_t *)(a)->s6_addr)[3] == htonl(1))
#define IN6_IS_ADDR_V4MAPPED(a) \
    (((const uint32_t *)(a)->s6_addr)[0] == 0 && ((const uint32_t *)(a)->s6_addr)[1] == 0 && \
     ((const uint32_t *)(a)->s6_addr)[2] == htonl(0xFFFF))
#define IN6_IS_ADDR_MULTICAST(a)    ((a)->s6_addr[0] == 0xFF)
#define IN6_IS_ADDR_LINKLOCAL(a)    ((a)->s6_addr[0] == 0xFE && ((a)->s6_addr[1] & 0xC0) == 0x80)
#define IN6_IS_ADDR_SITELOCAL(a)    ((a)->s6_addr[0] == 0xFE && ((a)->s6_addr[1] & 0xC0) == 0xC0)

#define IPPROTO_IP      0
#define IPPROTO_ICMP    1
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17
#define IPPROTO_IPV6    41
#define IPPROTO_ICMPV6  58

#define IPV6_V6ONLY     26      /* (level IPPROTO_IPV6; an int) */

#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK  ((in_addr_t)0x7F000001)     /* (host order: htonl it) */
#define INADDR_BROADCAST ((in_addr_t)0xFFFFFFFF)
#define INADDR_NONE      ((in_addr_t)0xFFFFFFFF)

#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46

/* Host to network byte order and back, 32 and 16 bits. */
uint32_t htonl(uint32_t v);
uint16_t htons(uint16_t v);
uint32_t ntohl(uint32_t v);
uint16_t ntohs(uint16_t v);

#endif

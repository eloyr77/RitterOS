/* Internet addresses as text (53a; IPv6's since 64a). */
#ifndef RITTEROS_ARPA_INET_H
#define RITTEROS_ARPA_INET_H

#include <netinet/in.h>

/* "a.b.c.d" (AF_INET) or an IPv6 address (AF_INET6: "fe80::1", "::ffff:
 * 10.0.2.15"; RFC 4291's forms, no "%card") into *out (network order, 4
 * or 16 bytes): 1, 0 if it is not one, -1 with EAFNOSUPPORT for another
 * family. */
int inet_pton(int family, const char *text, void *out);
/* An address (network order) as "a.b.c.d" or as IPv6's shortest form
 * (RFC 5952) into out (size at least INET_ADDRSTRLEN or INET6_ADDRSTRLEN);
 * out, or 0 with errno set (ENOSPC, EAFNOSUPPORT). */
const char *inet_ntop(int family, const void *address, char *out, socklen_t size);
/* The older forms: inet_aton 1 or 0 (also a.b.c, a.b and a, as BSD's),
 * inet_addr the address or INADDR_NONE, inet_ntoa into a buffer of its
 * own (the next call reuses it). */
int inet_aton(const char *text, struct in_addr *out);
in_addr_t inet_addr(const char *text);
char *inet_ntoa(struct in_addr address);

#endif

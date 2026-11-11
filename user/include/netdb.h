/* Names and services (53d): getaddrinfo and the older calls. Names are
 * looked up by the elf dnsd (DNS: A records, and since 64d AAAA records
 * for getaddrinfo when the machine has an IPv6 address beyond the link,
 * IPv6 then first; gethostbyname stays IPv4), "localhost" is 127.0.0.1,
 * an address written out is taken as it is. Service names come from /system/config/services ("http = 80" lines,
 * the same port for TCP and UDP). There is no reverse lookup: getnameinfo
 * gives addresses as numbers. */
#ifndef RITTEROS_NETDB_H
#define RITTEROS_NETDB_H

#include <netinet/in.h>
#include <sys/socket.h>

#define SERVICES_FILE   "/system/config/services"

struct addrinfo {
    int ai_flags;
    int ai_family;              /* AF_INET, AF_INET6 */
    int ai_socktype;            /* SOCK_STREAM, SOCK_DGRAM */
    int ai_protocol;            /* IPPROTO_TCP, IPPROTO_UDP */
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;         /* AI_CANONNAME: the name as given (no CNAMEs) */
    struct addrinfo *ai_next;
};

/* hints' ai_flags */
#define AI_PASSIVE      0x0001  /* no node: INADDR_ANY (for bind), not 127.0.0.1 */
#define AI_CANONNAME    0x0002
#define AI_NUMERICHOST  0x0004  /* the node must be an address: no lookup */
#define AI_V4MAPPED     0x0008  /* AF_INET6: IPv4 addresses as ::ffff:a.b.c.d when there is no IPv6 one */
#define AI_ALL          0x0010  /* with AI_V4MAPPED: the IPv4 ones too */
#define AI_ADDRCONFIG   0x0020  /* (always so: AAAA only with an IPv6 address beyond the link) */
#define AI_NUMERICSERV  0x0400  /* the service must be a number */

/* getaddrinfo's and getnameinfo's results (gai_strerror's texts) */
#define EAI_BADFLAGS    (-1)
#define EAI_NONAME      (-2)
#define EAI_AGAIN       (-3)
#define EAI_FAIL        (-4)
#define EAI_FAMILY      (-6)
#define EAI_SOCKTYPE    (-7)
#define EAI_SERVICE     (-8)
#define EAI_MEMORY      (-10)
#define EAI_SYSTEM      (-11)
#define EAI_OVERFLOW    (-12)

/* getnameinfo's flags */
#define NI_NUMERICHOST  0x01
#define NI_NUMERICSERV  0x02
#define NI_NOFQDN       0x04
#define NI_NAMEREQD     0x08    /* a name or nothing: always EAI_NONAME (no reverse lookup) */
#define NI_DGRAM        0x10
#define NI_MAXHOST      1025
#define NI_MAXSERV      32

/* The addresses for node (a name, an address, or 0) and service (a name,
 * a number, or 0), as a list to free with freeaddrinfo: one entry per
 * socket type asked (hints' ai_socktype 0: SOCK_STREAM, then SOCK_DGRAM).
 * 0, or an EAI_* (EAI_SYSTEM: errno says). */
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **result);
void freeaddrinfo(struct addrinfo *list);
const char *gai_strerror(int error);
/* An address and port as text: the address as a number, the port by its
 * name from the services file unless NI_NUMERICSERV. 0 or an EAI_*. */
int getnameinfo(const struct sockaddr *address, socklen_t length, char *host, socklen_t host_size,
                char *service, socklen_t service_size, int flags);

/* The older calls: a static entry each, which the next call reuses. */
struct hostent {
    char *h_name;
    char **h_aliases;
    int h_addrtype;             /* AF_INET */
    int h_length;               /* 4 */
    char **h_addr_list;
};
#define h_addr h_addr_list[0]

struct servent {
    char *s_name;
    char **s_aliases;
    int s_port;                 /* network byte order */
    char *s_proto;
};

struct hostent *gethostbyname(const char *name);
struct servent *getservbyname(const char *name, const char *protocol);
struct servent *getservbyport(int port, const char *protocol);

/* gethostbyname's reason for a 0 (a call underneath: libraries export no
 * data). */
int *__h_errno(void);
#define h_errno (*__h_errno())
#define HOST_NOT_FOUND  1
#define TRY_AGAIN       2
#define NO_RECOVERY     3
#define NO_DATA         4

#endif

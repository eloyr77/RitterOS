/*
 * Names and services (53d, netdb.h): getaddrinfo and the older calls over
 * ritteros_resolve (dnsd) and the services file. Since 64d getaddrinfo and
 * getnameinfo take IPv6 too (ritteros_resolve6: AAAA records); the older
 * calls (gethostbyname) stay IPv4.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ritteros.h"

/* A service's port: a number, or a name from the services file. -1 if
 * there is none (or a name with numeric_only). */
static int service_port(const char *service, bool numeric_only)
{
    char *end;
    long v = strtol(service, &end, 10);
    if (*service && !*end)
        return v >= 0 && v <= 65535 ? (int)v : -1;
    if (numeric_only)
        return -1;
    char value[16];
    if (config_get(SERVICES_FILE, service, value, sizeof(value)) != 1)
        return -1;
    v = strtol(value, &end, 10);
    return value[0] && !*end && v >= 0 && v <= 65535 ? (int)v : -1;
}

/* A port's name in the services file, or 0. */
static bool service_name(int port, char *out, size_t size)
{
    FILE *f = fopen(SERVICES_FILE, "r");
    if (!f)
        return false;
    char line[128];
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) {
        char name[64];
        int p;
        if (line[0] != '#' && sscanf(line, " %63[^= \t] = %d", name, &p) == 2 && p == port) {
            snprintf(out, size, "%s", name);
            found = true;
        }
    }
    fclose(f);
    return found;
}

/* An address getaddrinfo found: IPv4 (4 bytes) or IPv6 (16). */
struct found {
    int family;
    uint8_t bytes[16];
    uint32_t scope;
};

/* One entry: the addrinfo, its sockaddr and the name in one block. */
static struct addrinfo *entry(const struct found *f, int port, int socktype, const char *canon)
{
    size_t name = canon ? strlen(canon) + 1 : 0;
    size_t size = f->family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    struct addrinfo *a = calloc(1, sizeof(struct addrinfo) + size + name);
    if (!a)
        return 0;
    if (f->family == AF_INET6) {
        struct sockaddr_in6 *in = (struct sockaddr_in6 *)(a + 1);
        in->sin6_family = AF_INET6;
        in->sin6_port = htons((uint16_t)port);
        memcpy(in->sin6_addr.s6_addr, f->bytes, 16);
        in->sin6_scope_id = f->scope;
        a->ai_addr = (struct sockaddr *)in;
    } else {
        struct sockaddr_in *in = (struct sockaddr_in *)(a + 1);
        in->sin_family = AF_INET;
        in->sin_port = htons((uint16_t)port);
        memcpy(&in->sin_addr.s_addr, f->bytes, 4);
        a->ai_addr = (struct sockaddr *)in;
    }
    a->ai_family = f->family;
    a->ai_socktype = socktype;
    a->ai_protocol = socktype == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
    a->ai_addrlen = (socklen_t)size;
    if (canon) {
        a->ai_canonname = (char *)a->ai_addr + size;
        memcpy(a->ai_canonname, canon, name);
    }
    return a;
}

/* (64d) Has this machine an IPv6 address in use beyond the link (not
 * fe80::)? Then IPv6 comes first (RFC 6724, the user's choice), and AAAA
 * records are asked for at all (as AI_ADDRCONFIG: never on a network
 * without IPv6). */
static bool global_ipv6(void)
{
    struct sys_netinfo6 info;
    for (int i = 0;; i++) {
        memset(&info, 0, sizeof(info));
        info.index = (uint16_t)i;
        if (ritteros_netinfo6(&info) < 0)
            return false;
        for (uint32_t k = 0; k < info.count && k < 4; k++)
            if (info.addresses[k].state == 2 && !(info.addresses[k].address[0] == 0xFE &&
                                                  (info.addresses[k].address[1] & 0xC0) == 0x80))
                return true;
    }
}

/* The place of a card named `name` + 1 (a link-local address's scope), or 0. */
static uint32_t card_scope(const char *name)
{
    struct sys_netinfo info;
    for (int i = 0; ritteros_netcard(i, &info) == 0; i++)
        if (strcmp(info.name, name) == 0)
            return (uint32_t)i + 1;
    return 0;
}

/* An IPv6 address written out, "ADDRESS" or "ADDRESS%CARD". */
static bool literal6(const char *node, struct found *f)
{
    char text[INET6_ADDRSTRLEN];
    const char *card = strchr(node, '%');
    size_t n = card ? (size_t)(card - node) : strlen(node);
    if (n >= sizeof(text))
        return false;
    memcpy(text, node, n);
    text[n] = 0;
    memset(f, 0, sizeof(*f));
    f->family = AF_INET6;
    if (inet_pton(AF_INET6, text, f->bytes) != 1)
        return false;
    return !card || (f->scope = card_scope(card + 1)) != 0;
}

static void four(struct found *f, uint32_t ip)
{
    memset(f, 0, sizeof(*f));
    f->family = AF_INET;
    uint32_t net = htonl(ip);
    memcpy(f->bytes, &net, 4);
}

/* An IPv4 address as ::ffff:a.b.c.d (AI_V4MAPPED). */
static void mapped(struct found *f)
{
    uint8_t v4[4];
    memcpy(v4, f->bytes, 4);
    memset(f->bytes, 0, 16);
    f->bytes[10] = f->bytes[11] = 0xFF;
    memcpy(f->bytes + 12, v4, 4);
    f->family = AF_INET6;
}

static int lookup_error(int e)
{
    return e == ENONAME ? EAI_NONAME : e == ETIMEDOUT ? EAI_AGAIN : EAI_FAIL;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **result)
{
    struct addrinfo none = { 0 };
    const struct addrinfo *h = hints ? hints : &none;
    int known = AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_V4MAPPED | AI_ALL | AI_ADDRCONFIG |
                AI_NUMERICSERV;
    *result = 0;
    if (h->ai_flags & ~known)
        return EAI_BADFLAGS;
    if (!node && !service)
        return EAI_NONAME;
    if ((h->ai_flags & AI_CANONNAME) && !node)
        return EAI_BADFLAGS;
    int family = h->ai_family;
    if (family != AF_UNSPEC && family != AF_INET && family != AF_INET6)
        return EAI_FAMILY;
    if (h->ai_socktype != 0 && h->ai_socktype != SOCK_STREAM && h->ai_socktype != SOCK_DGRAM)
        return EAI_SOCKTYPE;
    if (h->ai_protocol && !(h->ai_protocol == IPPROTO_TCP && h->ai_socktype != SOCK_DGRAM) &&
        !(h->ai_protocol == IPPROTO_UDP && h->ai_socktype != SOCK_STREAM))
        return EAI_SOCKTYPE;
    int port = 0;
    if (service && (port = service_port(service, h->ai_flags & AI_NUMERICSERV)) < 0)
        return h->ai_flags & AI_NUMERICSERV ? EAI_NONAME : EAI_SERVICE;
    bool six_first = global_ipv6();
    bool mapping = family == AF_INET6 && (h->ai_flags & AI_V4MAPPED);
    struct found found[4];
    int count = 0;
    struct found f;
    struct in_addr v4;
    if (!node) {
        /* No node: every address (for bind) or the loopback, in each family
         * asked for, the preferred first. */
        bool passive = h->ai_flags & AI_PASSIVE;
        struct found any6, any4;
        memset(&any6, 0, sizeof(any6));
        any6.family = AF_INET6;
        if (!passive)
            any6.bytes[15] = 1;                     /* ::1 */
        four(&any4, passive ? INADDR_ANY : INADDR_LOOPBACK);
        if (family == AF_INET6 || (family == AF_UNSPEC && six_first))
            found[count++] = any6;
        if (family != AF_INET6)
            found[count++] = any4;
        if (family == AF_UNSPEC && !six_first)
            found[count++] = any6;
    } else if (inet_aton(node, &v4) && strchr(node, ':') == 0) {
        four(&f, ntohl(v4.s_addr));
        if (family == AF_INET6 && !mapping)
            return EAI_NONAME;                      /* (an IPv4 address, IPv6 asked for) */
        if (family == AF_INET6)
            mapped(&f);
        found[count++] = f;
    } else if (literal6(node, &f)) {
        if (family == AF_INET)
            return EAI_NONAME;                      /* (an IPv6 address, IPv4 asked for) */
        found[count++] = f;
    } else if (h->ai_flags & AI_NUMERICHOST) {
        return EAI_NONAME;
    } else {
        /* A name: its AAAA record (when IPv6 can be used, or is what was
         * asked for) and its A record, the preferred family first. */
        bool want6 = family == AF_INET6 || (family == AF_UNSPEC && six_first);
        bool want4 = family != AF_INET6 || mapping;
        int error = ENONAME;
        for (int pass = 0; pass < 2; pass++) {
            bool six = (pass == 0) == (six_first || family == AF_INET6);
            if (six && want6) {
                memset(&f, 0, sizeof(f));
                f.family = AF_INET6;
                if (ritteros_resolve6(node, f.bytes) == 0)
                    found[count++] = f;
                else
                    error = errno;
            } else if (!six && want4) {
                uint32_t ip;
                if (family == AF_INET6 && count && !(h->ai_flags & AI_ALL))
                    continue;                       /* (V4MAPPED: only without AAAA, or with AI_ALL) */
                if (ritteros_resolve(node, &ip) == 0) {
                    four(&f, ip);
                    if (family == AF_INET6)
                        mapped(&f);
                    found[count++] = f;
                } else {
                    error = errno;
                }
            }
        }
        if (!count)
            return lookup_error(error);
    }
    int types[2], kinds = 0;
    if (h->ai_socktype)
        types[kinds++] = h->ai_socktype;
    else if (h->ai_protocol == IPPROTO_UDP)
        types[kinds++] = SOCK_DGRAM;
    else if (h->ai_protocol == IPPROTO_TCP)
        types[kinds++] = SOCK_STREAM;
    else
        types[kinds++] = SOCK_STREAM, types[kinds++] = SOCK_DGRAM;
    struct addrinfo **link = result;
    for (int k = 0; k < count; k++)
        for (int i = 0; i < kinds; i++) {
            struct addrinfo *a = entry(&found[k], port, types[i],
                                       k == 0 && i == 0 && (h->ai_flags & AI_CANONNAME) ? node : 0);
            if (!a) {
                freeaddrinfo(*result);
                *result = 0;
                return EAI_MEMORY;
            }
            a->ai_flags = h->ai_flags;
            *link = a;
            link = &a->ai_next;
        }
    return 0;
}

void freeaddrinfo(struct addrinfo *list)
{
    while (list) {
        struct addrinfo *next = list->ai_next;
        free(list);
        list = next;
    }
}

const char *gai_strerror(int error)
{
    switch (error) {
    case 0: return "Success";
    case EAI_BADFLAGS: return "Invalid value for ai_flags";
    case EAI_NONAME: return "Name or service not known";
    case EAI_AGAIN: return "Temporary failure in name resolution";
    case EAI_FAIL: return "Non-recoverable failure in name resolution";
    case EAI_FAMILY: return "ai_family not supported";
    case EAI_SOCKTYPE: return "ai_socktype not supported";
    case EAI_SERVICE: return "Servname not supported for ai_socktype";
    case EAI_MEMORY: return "Memory allocation failure";
    case EAI_SYSTEM: return "System error";
    case EAI_OVERFLOW: return "Argument buffer overflow";
    default: return "Unknown error";
    }
}

int getnameinfo(const struct sockaddr *address, socklen_t length, char *host, socklen_t host_size,
                char *service, socklen_t service_size, int flags)
{
    if (!address)
        return EAI_FAMILY;
    bool six = address->sa_family == AF_INET6;
    if (six ? length < sizeof(struct sockaddr_in6)
            : length < sizeof(struct sockaddr_in) || address->sa_family != AF_INET)
        return EAI_FAMILY;
    const struct sockaddr_in *in = (const struct sockaddr_in *)address;
    const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)address;
    if (host && host_size) {
        if (flags & NI_NAMEREQD)
            return EAI_NONAME;                  /* (no reverse lookup) */
        if (!(six ? inet_ntop(AF_INET6, &in6->sin6_addr, host, host_size)
                  : inet_ntop(AF_INET, &in->sin_addr, host, host_size)))
            return EAI_OVERFLOW;
        struct sys_netinfo card;
        if (six && in6->sin6_scope_id && ritteros_netcard((int)in6->sin6_scope_id - 1, &card) == 0) {
            size_t used = strlen(host);         /* a link-local address's card: "%eth0" */
            if (used + 1 + strlen(card.name) >= host_size)
                return EAI_OVERFLOW;
            snprintf(host + used, host_size - used, "%%%s", card.name);
        }
    }
    if (service && service_size) {
        int port = ntohs(six ? in6->sin6_port : in->sin_port);
        char name[NI_MAXSERV];
        if ((flags & NI_NUMERICSERV) || !service_name(port, name, sizeof(name)))
            snprintf(name, sizeof(name), "%d", port);
        if (strlen(name) >= service_size)
            return EAI_OVERFLOW;
        memcpy(service, name, strlen(name) + 1);
    }
    return 0;
}

static __thread int host_error;

int *__h_errno(void)
{
    return &host_error;
}

struct hostent *gethostbyname(const char *name)
{
    static char text[256];
    static uint32_t address;
    static char *addresses[2] = { (char *)&address, 0 };
    static char *aliases[1] = { 0 };
    static struct hostent entry_ = { text, aliases, AF_INET, 4, addresses };
    uint32_t ip;
    struct in_addr a;
    if (inet_aton(name, &a)) {
        ip = ntohl(a.s_addr);
    } else if (ritteros_resolve(name, &ip) < 0) {
        host_error = errno == ENONAME ? HOST_NOT_FOUND : TRY_AGAIN;
        return 0;
    }
    snprintf(text, sizeof(text), "%s", name);
    address = htonl(ip);
    return &entry_;
}

static struct servent *service_entry(const char *name, int port, const char *protocol)
{
    static char text[64], proto[8];
    static char *aliases[1] = { 0 };
    static struct servent entry_ = { text, aliases, 0, proto };
    if (protocol && strcmp(protocol, "tcp") && strcmp(protocol, "udp"))
        return 0;
    snprintf(text, sizeof(text), "%s", name);
    snprintf(proto, sizeof(proto), "%s", protocol ? protocol : "tcp");
    entry_.s_port = htons((uint16_t)port);
    return &entry_;
}

struct servent *getservbyname(const char *name, const char *protocol)
{
    int port = service_port(name, false);
    char *end;
    strtol(name, &end, 10);
    if (port < 0 || !*end)                      /* (a name, not a number) */
        return 0;
    return service_entry(name, port, protocol);
}

struct servent *getservbyport(int port, const char *protocol)
{
    char name[64];
    if (!service_name(ntohs((uint16_t)port), name, sizeof(name)))
        return 0;
    return service_entry(name, ntohs((uint16_t)port), protocol);
}

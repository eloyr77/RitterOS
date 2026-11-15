/*
 * net/if.h (43b-6): network interfaces by name and number. A card's number
 * is its place in the kernel's list + 1 (the IPv6 scope sin6_scope_id
 * takes: eth0 is usually 1); lo, which is no card of the kernel's, is
 * IF_LOOPBACK_INDEX, past every card. if_nameindex lists lo first, then
 * the cards (a removed tap or bridge keeps its place and is listed too).
 */
#ifndef RITTEROS_NET_IF_H
#define RITTEROS_NET_IF_H

#ifdef __cplusplus
extern "C" {
#endif

#define IF_NAMESIZE         16
#define IFNAMSIZ            IF_NAMESIZE
#define IF_LOOPBACK_INDEX   65          /* (RitterOS's: the kernel lists at most 64 cards) */

/* getifaddrs's ifa_flags (Linux's values). */
#define IFF_UP          0x1             /* (every card: there is no "down" setting) */
#define IFF_BROADCAST   0x2
#define IFF_LOOPBACK    0x8
#define IFF_RUNNING     0x40            /* it has a link */
#define IFF_MULTICAST   0x1000
#define IFF_LOWER_UP    0x10000

struct if_nameindex {
    unsigned int if_index;
    char *if_name;
};

unsigned int if_nametoindex(const char *name);
char *if_indextoname(unsigned int index, char *name);
struct if_nameindex *if_nameindex(void);
void if_freenameindex(struct if_nameindex *list);

#ifdef __cplusplus
}
#endif

#endif

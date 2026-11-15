/*
 * ifaddrs.h (43b-6): every network interface's addresses, as glibc's
 * getifaddrs gives them: lo (127.0.0.1/8, ::1/128), then each card's IPv4
 * address (netmask, broadcast) and IPv6 addresses (in use), one entry per
 * address; a card without any address one entry with ifa_addr 0. No
 * AF_PACKET entries (there are no packet sockets). freeifaddrs frees the
 * list.
 */
#ifndef RITTEROS_IFADDRS_H
#define RITTEROS_IFADDRS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/socket.h>

struct ifaddrs {
    struct ifaddrs *ifa_next;
    char *ifa_name;
    unsigned int ifa_flags;             /* IFF_* (net/if.h) */
    struct sockaddr *ifa_addr;
    struct sockaddr *ifa_netmask;
    union {
        struct sockaddr *ifu_broadaddr; /* IFF_BROADCAST */
        struct sockaddr *ifu_dstaddr;
    } ifa_ifu;
    void *ifa_data;
};
#define ifa_broadaddr   ifa_ifu.ifu_broadaddr
#define ifa_dstaddr     ifa_ifu.ifu_dstaddr

int getifaddrs(struct ifaddrs **list);
void freeifaddrs(struct ifaddrs *list);

#ifdef __cplusplus
}
#endif

#endif

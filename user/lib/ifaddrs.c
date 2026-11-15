/*
 * (43b-6) Network interfaces for ported programs: net/if.h's names and
 * numbers and getifaddrs, from the kernel's cards (SYS_NETINFO and
 * SYS_NETINFO6 by place: a card's number is its place + 1, the IPv6
 * scope) and lo. Each entry of getifaddrs is one allocation: the struct,
 * its name and its addresses.
 */
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include "ritteros.h"

static int card_at(int place, struct sys_netinfo *info)
{
    memset(info, 0, sizeof(*info));
    info->index = (uint16_t)place;
    return ritteros_netinfo(info);
}

unsigned int if_nametoindex(const char *name)
{
    if (!strcmp(name, "lo"))
        return IF_LOOPBACK_INDEX;
    struct sys_netinfo info;
    for (int i = 0; card_at(i, &info) == 0; i++)
        if (!strncmp(info.name, name, sizeof(info.name)) && strlen(name) < sizeof(info.name))
            return (unsigned)i + 1;
    errno = ENODEV;
    return 0;
}

char *if_indextoname(unsigned int index, char *name)
{
    struct sys_netinfo info;
    if (index == IF_LOOPBACK_INDEX) {
        strcpy(name, "lo");
        return name;
    }
    if (index == 0 || index > 0xFFFF || card_at((int)index - 1, &info) < 0) {
        errno = ENXIO;
        return 0;
    }
    memcpy(name, info.name, sizeof(info.name));
    name[sizeof(info.name)] = 0;
    return name;
}

struct if_nameindex *if_nameindex(void)
{
    struct sys_netinfo info;
    int cards = 0;
    while (card_at(cards, &info) == 0)
        cards++;
    struct if_nameindex *list = calloc((size_t)cards + 2, sizeof(*list) + IF_NAMESIZE);
    if (!list) {
        errno = ENOBUFS;
        return 0;
    }
    char *names = (char *)(list + cards + 2);
    list[0].if_index = IF_LOOPBACK_INDEX;
    list[0].if_name = strcpy(names, "lo");
    for (int i = 0; i < cards && card_at(i, &info) == 0; i++) {
        list[i + 1].if_index = (unsigned)i + 1;
        list[i + 1].if_name = names + (size_t)(i + 1) * IF_NAMESIZE;
        memcpy(list[i + 1].if_name, info.name, sizeof(info.name));
    }
    return list;                                /* (ended by an entry with index 0) */
}

void if_freenameindex(struct if_nameindex *list)
{
    free(list);
}

/* One getifaddrs entry: its name, and its address, mask and broadcast of
 * family (0: none) in the same block. */
struct entry {
    struct ifaddrs ifa;
    char name[IF_NAMESIZE];
    struct sockaddr_storage addr, mask, broadcast;
};

static struct ifaddrs *entry(struct ifaddrs ***tail, const char *name, unsigned flags)
{
    struct entry *e = calloc(1, sizeof(*e));
    if (!e)
        return 0;
    memcpy(e->name, name, strnlen(name, IF_NAMESIZE - 1));
    e->ifa.ifa_name = e->name;
    e->ifa.ifa_flags = flags;
    **tail = &e->ifa;
    *tail = &e->ifa.ifa_next;
    return &e->ifa;
}

static void four(struct ifaddrs *a, uint32_t address, unsigned prefix, bool broadcast)
{
    struct entry *e = (struct entry *)a;
    uint32_t mask = prefix ? ~0U << (32 - prefix) : 0;
    struct sockaddr_in *s = (struct sockaddr_in *)&e->addr, *m = (struct sockaddr_in *)&e->mask;
    s->sin_family = m->sin_family = AF_INET;
    s->sin_addr.s_addr = htonl(address);
    m->sin_addr.s_addr = htonl(mask);
    a->ifa_addr = (struct sockaddr *)s;
    a->ifa_netmask = (struct sockaddr *)m;
    if (broadcast) {
        struct sockaddr_in *b = (struct sockaddr_in *)&e->broadcast;
        b->sin_family = AF_INET;
        b->sin_addr.s_addr = htonl(address | ~mask);
        a->ifa_broadaddr = (struct sockaddr *)b;
    }
}

static void six(struct ifaddrs *a, const uint8_t *address, unsigned prefix, uint32_t scope)
{
    struct entry *e = (struct entry *)a;
    struct sockaddr_in6 *s = (struct sockaddr_in6 *)&e->addr, *m = (struct sockaddr_in6 *)&e->mask;
    s->sin6_family = m->sin6_family = AF_INET6;
    memcpy(s->sin6_addr.s6_addr, address, 16);
    if (address[0] == 0xFE && (address[1] & 0xC0) == 0x80)
        s->sin6_scope_id = scope;               /* (link-local: its card) */
    for (unsigned i = 0; i < prefix && i < 128; i++)
        m->sin6_addr.s6_addr[i / 8] |= (uint8_t)(0x80 >> (i % 8));
    a->ifa_addr = (struct sockaddr *)s;
    a->ifa_netmask = (struct sockaddr *)m;
}

int getifaddrs(struct ifaddrs **list)
{
    struct ifaddrs *head = 0, **tail = &head, *a;
    static const uint8_t loopback6[16] = { [15] = 1 };
    unsigned lo = IFF_UP | IFF_LOOPBACK | IFF_RUNNING | IFF_LOWER_UP;
    if (!(a = entry(&tail, "lo", lo)))
        goto none;
    four(a, 0x7F000001, 8, false);
    if (!(a = entry(&tail, "lo", lo)))
        goto none;
    six(a, loopback6, 128, 0);
    struct sys_netinfo info;
    for (int i = 0; card_at(i, &info) == 0; i++) {
        unsigned flags = IFF_UP | IFF_BROADCAST | IFF_MULTICAST |
                         (info.link_speed ? IFF_RUNNING | IFF_LOWER_UP : 0);
        bool any = false;
        if (info.address) {
            if (!(a = entry(&tail, info.name, flags)))
                goto none;
            four(a, info.address, info.prefix, true);
            any = true;
        }
        struct sys_netinfo6 v6;
        memset(&v6, 0, sizeof(v6));
        v6.index = (uint16_t)i;
        if (ritteros_netinfo6(&v6) == 0) {
            for (uint32_t k = 0; k < v6.count && k < 4; k++) {
                if (v6.addresses[k].state != 2)
                    continue;                   /* (only those in use) */
                if (!(a = entry(&tail, info.name, flags)))
                    goto none;
                six(a, v6.addresses[k].address, v6.addresses[k].prefix, (uint32_t)i + 1);
                any = true;
            }
        }
        if (!any && !entry(&tail, info.name, flags))
            goto none;
    }
    *list = head;
    return 0;
none:
    freeifaddrs(head);
    errno = ENOMEM;
    return -1;
}

void freeifaddrs(struct ifaddrs *list)
{
    while (list) {
        struct ifaddrs *next = list->ifa_next;
        free(list);                             /* (the entry's one block) */
        list = next;
    }
}

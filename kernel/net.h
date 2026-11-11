/*
 * Network devices (kernel/net.c): every Ethernet card a driver found,
 * named eth0, eth1, ... in the order found, with its MAC address, link
 * state and counters. Drivers hand received frames to net_receive (from
 * their own thread, never in interrupt context); the protocols above send
 * through netdev.send. The loopback interface lo (ipv4.c) is a struct
 * netdev too, but not in this list: it is shown first by net_print.
 */
#ifndef RITTEROS_NET_H
#define RITTEROS_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver_net.h"     /* (67b) struct net_card: the drivers' view of the first fields */

#define ETH_FRAME_MAX   1514            /* an Ethernet frame without its checksum */
#define ETH_HEADER      14
#define MAX_NETDEVS     64              /* network cards at once (unplugged ones stay listed;
                                         * 38a: 16 before the virtual cards) */
#define NET_MULTICAST_MAX 16            /* multicast groups a card is asked to take */

struct pci_device;
struct bridge;

/* An IPv6 address of a card (64a, ipv6.c). */
#define IP6_ADDRESSES   4               /* the link-local one first */
#define IP6_TENTATIVE   1               /* being checked for duplicates (not used yet) */
#define IP6_PREFERRED   2               /* in use */
#define IP6_DUPLICATE   3               /* another machine has it: never used */

#define IP6_LINK_LOCAL  1               /* where an address came from */
#define IP6_FROM_ROUTER 2               /* a router's prefix (SLAAC, 64b) */
#define IP6_FIXED       3               /* the card's settings (address6 = ADDRESS/N) */
#define IP6_FROM_DHCP   4               /* (64g) a DHCPv6 lease (SYS_NETLEASE6) */

struct ip6_address {
    uint8_t address[16];
    uint8_t prefix;                     /* the on-link prefix's length */
    uint8_t state;                      /* IP6_*; 0: a free slot */
    uint8_t probes;                     /* duplicate checks sent */
    uint8_t origin;                     /* IP6_LINK_LOCAL, ... */
    uint64_t next;                      /* timer tick of its next step */
    uint64_t valid_until, preferred_until;  /* timer ticks; UINT64_MAX: for ever */
};

/* IPv6's settings of a card (64b, SYS_NETSET6: address6, gateway6, dns6)
 * and what its network's routers said (router advertisements). */
#define IP6_PREFIXES    4

struct ip6_link {
    uint8_t mode;                       /* NETSET6_*: automatic from the start */
    uint8_t fixed_prefix;
    uint8_t fixed[16], gateway[16], dns[16];    /* the settings' (0: none) */
    uint16_t dns_port;
    uint8_t router[16];                 /* the default router named last (link-local) */
    uint64_t router_until;              /* 0: none */
    uint8_t router_dns[16];             /* the DNS server it named (RDNSS) */
    uint64_t router_dns_until;
    uint8_t dhcp_dns[16];               /* (64g) the DNS server DHCPv6 named */
    uint64_t dhcp_dns_until;
    struct {
        uint8_t prefix[16];
        uint8_t length;
        uint64_t until;                 /* 0: a free slot */
    } on_link[IP6_PREFIXES];            /* prefixes the routers say are on this link */
    uint16_t mtu;                       /* the router's (0: the card's, 1500) */
    bool managed, other;                /* its M and O flags (DHCPv6, 64g) */
    uint8_t solicits;                   /* router solicitations sent since the link came */
    uint64_t solicit_next;
};

/* (67b) Its fields up to `gone` are struct net_card's (driver_net.h), in
 * the same order and at the same offsets (net.c checks): a module sees
 * those only, the rest is the stack's own and may change. */
struct netdev {
    char name[8];                       /* "eth0" */
    uint8_t mac[6];
    const char *driver;                 /* "e1000" */
    char model[40];                     /* "Intel 82540EM" */
    char where[16];                     /* its PCI address: "00:03.0" */
    /* Link state and speed in Mbit/s (0: no link). */
    uint32_t (*link_speed)(struct netdev *d);
    /* Send one frame (14-byte header and data, at most ETH_FRAME_MAX
     * bytes); false if it could not be queued (the ring is full). */
    bool (*send)(struct netdev *d, const void *frame, size_t length);
    const struct pci_device *pci;       /* where it sits (net's report), or 0 */
    /* More lines for net's report (the chip's own counters and state), or 0. */
    void (*details)(struct netdev *d);
    /* The multicast groups the protocols want from now on (64a: IPv6's),
     * as their MAC addresses (33:33:...), count of them (0: none; -1,
     * macs 0: every multicast frame, more groups than a list holds). How a
     * card filters is its own: the driver sets the chip's filter (exact
     * entries, a hash, or every multicast frame where it cannot do
     * better), so that at least these come in; more may, the protocols
     * drop what they did not ask for. 0: the card passes every multicast
     * frame anyway (USB NCM's filter takes them all; a Wi-Fi access point
     * sends group frames to every station). Only net_set_multicast calls
     * it, from the thread link (it may wait; never a receive thread). */
    void (*set_multicast)(struct netdev *d, const uint8_t (*macs)[6], int count);
    void *priv;
    struct wlan *wlan;                  /* a wireless card's (wlan.h), else 0 */
    volatile uint64_t rx_frames, rx_bytes, tx_frames, tx_bytes, tx_dropped;
    /* Unplugged (a USB adapter; 50f): it stays listed, "unplugged" in
     * net's report, and its name is free unless the names file keeps it. */
    volatile bool gone;
    /* (38b) Every frame (a bridge's port) or only its own (driver_net.h). */
    void (*set_promiscuous)(struct netdev *d, bool on);
    /* --- the stack's own (not the drivers') --- */
    /* Its IPv4 settings (host byte order; 0: none): set by net up or the
     * elf dhclient (SYS_NETSET). */
    uint32_t address, prefix, gateway, dns;
    uint16_t dns_port;                  /* 0: 53 */
    uint32_t lease_seconds;             /* DHCP's lease (dhclient); 0: a fixed address */
    /* IPv6 (64a, ipv6.c, under its lock): the addresses, made when the
     * link comes up and dropped when it goes. */
    struct ip6_address ip6[IP6_ADDRESSES];
    bool ip6_up;                        /* its link was up at ipv6.c's last look */
    struct ip6_link ip6_link;
    /* The groups last given to set_multicast (net.c). */
    uint8_t multicast[NET_MULTICAST_MAX][6];
    int multicast_count;
    /* (38a) A virtual card (tap.c): named by its maker, never from the
     * names file; removed (gone), it is left out of net's report. */
    bool virtual_card;
    /* (38b) The bridge whose port it is (bridge.c; set with release, read
     * with acquire), or 0. A port's frames go to the bridge, and the stack
     * sees no link on it (net_link). */
    struct bridge *bridge;
    bool promiscuous;                   /* set_promiscuous's last (under filters) */
    /* (38d) net_link's memory of the link: its speed at timer tick
     * link_tick - 1 (0: not read yet), and how often it was asked and the
     * driver read (net's report). */
    uint32_t link_now;
    uint64_t link_tick, link_asked, link_reads;
};

/* An IPv4 address as text: "10.0.2.15". */
void ip_text(uint32_t ip, char *out, size_t size);

/* A new card: it gets its name and is listed. Its name (50f) is the one
 * /system/config/network/names gives its MAC address; a card not there gets
 * the lowest ethN (wlanN for a wireless card) that no listed card and no
 * card here holds. Before the system disk is mounted, cards are named in
 * the order found until net_names_load. */
void net_register(struct netdev *d);

/* (38a) A virtual card (a tap), its name already given: listed as it is
 * (the names file does not name it). False when the list is full. */
bool net_register_virtual(struct netdev *d);

/* (38a) A virtual card removed: gone, its IPv4 settings dropped (IPv6's
 * go with its link); back: made again in the same place. */
void net_virtual_gone(struct netdev *d);
void net_virtual_back(struct netdev *d);

/* (38b) d's link as the stack may use it: its speed in Mbit/s, 0 without a
 * link or while d is a bridge's port (the bridge carries its traffic).
 * (38d) The driver is asked at most once a timer tick (10 ms): the stack
 * asks several times a packet, and a driver's answer is often a read of
 * its chip's register (the J1900's: ~9 a frame sent, ~65,000 a second). */
uint32_t net_link(struct netdev *d);

/* (38b) Ask d's driver for every frame or only its own (set_promiscuous,
 * never at once with set_multicast). A program's thread. */
void net_set_promiscuous(struct netdev *d, bool on);

/* (38b) d becomes a bridge's port: its IPv4 settings go (IPv6's go with
 * the link the stack no longer sees). */
void net_card_quiet(struct netdev *d);

/* (67b) A card for a module (driver_net.h): zeroed, with room for the
 * stack's part; the module registers it with net_register. */
struct net_card *net_card_new(void);

/* (67d) This thread hands a received frame up now (driver_net.h). */
bool net_receiving(void);

/* The system's file system is mounted: read the names file and give the
 * cards found so far their names (a line for each one that changes). */
void net_names_load(void);

/* A card unplugged earlier is back (net_register is not called again):
 * it is named again, as a card just found. */
void net_rename(struct netdev *d);

/* The multicast groups card d is to take (their MACs): the driver's
 * set_multicast is called when they differ from the last ones. More than
 * NET_MULTICAST_MAX: the card is asked for every multicast frame (-1).
 * From the thread link only. */
void net_set_multicast(struct netdev *d, const uint8_t (*macs)[6], int count);

/* A frame the card received (a driver's thread). */
void net_receive(struct netdev *d, const void *frame, size_t length);

/* (65e) Around a burst of frames a card's receive thread delivers (an NTB,
 * a ring's worth): TCP acknowledges data that came in order once per
 * connection, at the end. A card that does not call them is acknowledged
 * segment by segment, as before. */
void net_batch_begin(void);
void net_batch_end(void);
#define NET_BATCH_MAX 32                /* frames, then the owed ACKs go out anyway */

/* The boot's lines for the cards found, with their links as they are now
 * (no waiting), and the thread link, which reports every change of a
 * link from then on ("eth0: link up, 1000 Mbit/s", "eth0: link down"). */
void net_announce(void);

int net_count(void);
struct netdev *net_at(int index);
/* d's place in the list (a link-local address's scope is it + 1), or -1. */
int net_index(const struct netdev *d);

/* The cards as the program net and the rescue console show them
 * (REPORT_NET). */
void net_print(void);

#endif

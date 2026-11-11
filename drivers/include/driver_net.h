/*
 * (67b) The network card class of the driver interface (driver.h): what a
 * card's driver gives the network stack and what it may call. The stack is
 * the same for every card (kernel/net.c): a driver fills in a card's
 * identity and its operations, registers it, and hands every received
 * frame to net_receive; how a chip does it is the driver's own.
 *
 * A driver never makes a struct net_card itself: net_card_new gives one
 * (the stack keeps its own state with it, past these fields), which the
 * driver fills in and registers once its chip works. The kernel's struct
 * netdev begins with exactly these fields (checked when it is compiled).
 */
#ifndef RITTEROS_DRIVER_NET_H
#define RITTEROS_DRIVER_NET_H

#include "driver.h"

#define ETH_FRAME_MAX   1514            /* an Ethernet frame without its checksum */
#define ETH_HEADER      14
#define NET_BATCH_MAX   32              /* frames in a batch, then the owed ACKs go out */

struct wlan;

struct net_card {
    char name[8];                       /* (the stack's: "eth0") */
    uint8_t mac[6];
    const char *driver;                 /* the driver's name: "rtl8168" */
    char model[40];                     /* "Realtek RTL8168H" */
    char where[16];                     /* its PCI address: "00:03.0" */
    /* Link state and speed in Mbit/s (0: no link). */
    uint32_t (*link_speed)(struct net_card *card);
    /* Send one frame (14-byte header and data, at most ETH_FRAME_MAX
     * bytes); false if it could not be queued (the ring is full). */
    bool (*send)(struct net_card *card, const void *frame, size_t length);
    const struct pci_device *pci;       /* where it sits (net's report), or 0 */
    /* More lines for net's report (the chip's own counters and state), or 0. */
    void (*details)(struct net_card *card);
    /* The multicast groups the protocols want from now on (their MAC
     * addresses, count of them; count -1, macs 0: every multicast frame).
     * The driver sets the chip's filter so that at least these come in;
     * 0: the card passes every multicast frame anyway. Called from the
     * stack's thread (it may wait; never a receive thread). */
    void (*set_multicast)(struct net_card *card, const uint8_t (*macs)[6], int count);
    void *priv;                         /* the driver's own */
    struct wlan *wlan;                  /* (the Wi-Fi layer's; 0 for a wired card) */
    volatile uint64_t rx_frames, rx_bytes, tx_frames, tx_bytes, tx_dropped;
    volatile bool gone;                 /* unplugged (the card stays listed) */
    /* (6.1, 38b) Take every frame, whatever its destination (on: the card
     * is a bridge's port), or again only its own, broadcasts and the
     * multicast groups set_multicast asked for (off). 0: the card cannot
     * (a bridge refuses it). Called from a program's thread (it may wait),
     * never at the same time as set_multicast. */
    void (*set_promiscuous)(struct net_card *card, bool on);
};

#ifndef RITTEROS_KERNEL

/* A card for the driver to fill in (all zero), 0 without memory. */
struct net_card *net_card_new(void);

/* The card works: the stack names it (eth0, ...), watches its link and
 * sends through it. */
void net_register(struct net_card *card);

/* A received frame (from the driver's own thread, never an interrupt
 * handler). A receive loop marks its burst with net_batch_begin/_end, so
 * the stack acknowledges once per connection at its end. */
void net_receive(struct net_card *card, const void *frame, size_t length);
void net_batch_begin(void);
void net_batch_end(void);

/* (1.2) Is this thread handing a received frame up (net_receive) right now?
 * Then it must not wait for room to send: the stack may answer from inside
 * net_receive, and waiting there would stop the receiving. */
bool net_receiving(void);

/* (1.3) A card the driver has again (a USB adapter plugged in again: the
 * same MAC): its name from the names file, as a new card's. */
void net_rename(struct net_card *card);

#endif /* RITTEROS_KERNEL */
#endif

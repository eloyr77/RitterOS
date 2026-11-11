/*
 * (67d) The Wi-Fi card class of the driver interface (driver.h): a Wi-Fi
 * card is a network card (driver_net.h) whose frames are 802.11's. The
 * kernel's Wi-Fi layer turns Ethernet frames into 802.11 data frames and
 * back (protecting them with CCMP under the keys the elf wifid sets) and
 * hands management frames to wifid, which scans, joins and runs the
 * handshakes. A driver only moves 802.11 frames and tunes channels; what a
 * card's firmware can do itself (scanning, joining, keys, Block Ack) it
 * offers through the optional operations.
 *
 * A driver keeps a struct wifi_card with its operations and channels (the
 * kernel's struct wlan has exactly this layout, checked when it is
 * compiled), fills in its struct net_card (net_card_new) as for a wired
 * card but leaves send and link_speed (the layer's), and registers both
 * with wlan_register: the card is named wlan0, wlan1, ...
 */
#ifndef RITTEROS_DRIVER_WIFI_H
#define RITTEROS_DRIVER_WIFI_H

#include "driver_net.h"

#define WLAN_FRAME_MAX      2400        /* an 802.11 frame's bytes, at most */
#define WLAN_KEY_PAIRWISE   1           /* CCMP-128, for frames to and from the access point */
#define WLAN_KEY_GROUP      2           /* CCMP-128, for group frames from it (index 1-3) */

/* What the access point's association response said about 802.11n (its HT
 * Capabilities element) and WMM (QoS), for the driver's join: the card's
 * firmware picks the rates from it. */
struct wlan_ht {
    bool on;                            /* the access point does HT (and we asked) */
    uint16_t cap;                       /* its HT capability information */
    uint8_t ampdu;                      /* its A-MPDU parameters */
    uint8_t mcs[10];                    /* the MCS it can receive (bit n: MCS n) */
    bool wmm;                           /* it does WMM: data goes as QoS data (TID 0) */
    bool vht;                           /* 802.11ac (5 GHz): its VHT Capabilities */
    uint32_t vht_cap;
    uint16_t vht_rx_mcs, vht_tx_mcs;    /* its MCS maps (2 bits per stream) */
    int width;                          /* the channel's width it runs: 20, 40 or 80 MHz */
    int center;                         /* (40, 80) the centre channel */
};

struct wlan_state;

struct wifi_card {
    /* Send one 802.11 frame as it is (no FCS); false if it was not queued. */
    bool (*transmit)(struct net_card *card, const void *frame, size_t length);
    /* Listen and send on a channel (1-14: 2.4 GHz; 36-177: 5 GHz). */
    bool (*set_channel)(struct net_card *card, int channel);
    /* Optional: a full scan in the card's firmware; the beacons it hears go
     * up through wlan_receive, and wlan_scan_done ends it. ssid (length
     * bytes, or 0): also ask for that network by name (an active scan).
     * 0: wifid scans channel by channel. */
    bool (*scan)(struct net_card *card, const uint8_t *ssid, size_t length);
    /* Optional: the two-letter country whose rules the card's firmware
     * applies to its channels and power. */
    bool (*set_country)(struct net_card *card, const char *cc);
    /* Optional: wifid has associated with bssid on channel and got aid; the
     * card's firmware takes over what it can (watching the beacons);
     * powersave: it may doze between beacons. */
    bool (*join)(struct net_card *card, const uint8_t *bssid, uint16_t aid, int channel,
                 const struct wlan_ht *ht, bool powersave);
    /* Optional: take a CCMP-128 key (WLAN_KEY_PAIRWISE, or WLAN_KEY_GROUP
     * under index); true if the card encrypts and decrypts with it from now
     * on: the layer then hands frames over in plain text (the Protected bit
     * set) and takes the card's decrypted ones through
     * wlan_receive_decrypted. False: the layer does it. */
    bool (*set_key)(struct net_card *card, int kind, int index, const uint8_t key[16]);
    /* Optional: a Block Ack agreement for tid is in place (on) or gone,
     * sending or receiving; ssn its starting sequence number, window the
     * frames a burst may hold. The card's firmware does the bursting. */
    bool (*block_ack)(struct net_card *card, int tid, bool sending, bool on, uint16_t ssn,
                      uint16_t window);
    /* Optional: the link is gone (wifid left the access point). */
    void (*leave)(struct net_card *card);
    uint8_t channels[64];               /* the channels the card has, */
    int nchannels;                      /* how many */
    struct wlan_state *state;           /* (the layer's own) */
};

#ifndef RITTEROS_KERNEL

/* The card (its net_card filled in) and its Wi-Fi side: named wlanN and
 * listed with the other cards (its send and link_speed are the layer's). */
DRIVER_WRAPPED void wlan_register(struct net_card *card, struct wifi_card *wifi);

/* A frame the card received (the driver's thread): an 802.11 frame without
 * its FCS, the channel it came on and its signal (dBm, 0 if unknown). */
void wlan_receive(struct net_card *card, const void *frame, size_t length, int channel, int signal);

/* A frame the card decrypted with a key it took (set_key): the header and
 * the plain body, its CCMP header and MIC removed; pn its packet number,
 * key_index the key it used; bad: the card found its MIC wrong. */
void wlan_receive_decrypted(struct net_card *card, const void *frame, size_t length, int channel,
                            int signal, uint64_t pn, int key_index, bool bad);

/* The card's firmware lost the access point: the link goes down and
 * wifid joins again; why is for the console line (the driver's thread). */
void wlan_link_lost(struct net_card *card, const char *why);

/* The card's own scan (scan) has ended. */
void wlan_scan_done(struct net_card *card);

#endif /* RITTEROS_KERNEL */
#endif

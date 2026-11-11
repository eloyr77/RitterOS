/*
 * Wireless cards (step 24c): a Wi-Fi card is a network card (struct netdev,
 * named wlan0, wlan1, ...) whose link is up once the elf wifid has joined
 * an access point. TCP/IP sends and receives Ethernet frames as on any card;
 * this layer turns them into 802.11 data frames and back, protecting them
 * with CCMP (AES-CCM, common/aes.c) under the keys wifid sets. Management
 * frames and WPA's EAPOL frames go to wifid instead (SYS_WLAN), which scans,
 * joins and runs the handshakes. A driver only moves 802.11 frames and tunes
 * channels.
 */
#ifndef RITTEROS_WLAN_H
#define RITTEROS_WLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "net.h"
#include "driver_wifi.h"    /* (67d) struct wlan_ht; struct wifi_card: the drivers' view of struct wlan */

struct wlan_state;

/* (67d) struct wifi_card's layout (driver_wifi.h), field for field (wlan.c
 * checks): a module's card is its own struct wifi_card. */
struct wlan {
    /* Send one 802.11 frame as it is (no FCS); false if it was not queued. */
    bool (*transmit)(struct netdev *d, const void *frame, size_t length);
    /* Listen and send on a channel (1-14: 2.4 GHz; 36-177: 5 GHz). */
    bool (*set_channel)(struct netdev *d, int channel);
    /* Optional: start a full scan in the card's firmware (the MT7921 scans
     * itself). The beacons it hears are handed up with wlan_receive, like any
     * management frame. ssid (length bytes, or 0): also ask for that network
     * by name, an active scan (hidden networks only answer that). NULL for a
     * card wifid scans by hand (the test card). */
    bool (*scan)(struct netdev *d, const uint8_t *ssid, size_t length);
    /* Optional: the two-letter country whose rules the card's firmware should
     * apply to the channels and power it uses (the MT7921 chooses them from
     * its own tables). NULL for a card with no such setting. */
    bool (*set_country)(struct netdev *d, const char *cc);
    /* Optional: wifid has associated with bssid on channel and got aid; the
     * card's firmware takes over what it can from here (the MT7921 watches
     * the access point's beacons itself); powersave: it may doze between
     * beacons. leave: the link is gone. NULL for a
     * card with nothing to tell (the test card). */
    bool (*join)(struct netdev *d, const uint8_t *bssid, uint16_t aid, int channel,
                 const struct wlan_ht *ht, bool powersave);
    /* Optional: take a CCMP-128 key into the card (WLAN_KEY_PAIRWISE, or
     * WLAN_KEY_GROUP under index); true if the card encrypts and decrypts
     * with it from now on. The layer then never uses that key itself (two
     * users of one key would reuse packet numbers): it hands frames over in
     * plain text with the Protected bit set and takes the card's decrypted
     * ones through wlan_receive_decrypted. False: the layer does it. */
    bool (*set_key)(struct netdev *d, int kind, int index, const uint8_t key[16]);
    /* Optional: a Block Ack agreement with the access point for tid is in
     * place (on) or gone: sending (we send its frames in bursts, A-MPDU) or
     * receiving; ssn its starting sequence number, window how many frames a
     * burst may hold. The card's firmware does the bursting. */
    bool (*block_ack)(struct netdev *d, int tid, bool sending, bool on, uint16_t ssn,
                      uint16_t window);
    void (*leave)(struct netdev *d);
    uint8_t channels[64];               /* the channels the card has, */
    int nchannels;                      /* how many */
    struct wlan_state *state;           /* (the layer's own) */
};

/* A new wireless card: d->wlan set by the driver; it is named wlanN and
 * listed with the others (its send and link_speed are the layer's). */
void wlan_register(struct netdev *d);

/* A frame the card received (the driver's thread): an 802.11 frame without
 * its FCS, the channel it came on and its signal (dBm, 0 if unknown). */
void wlan_receive(struct netdev *d, const void *frame, size_t length, int channel, int signal);

/* A frame the card decrypted with a key it took (set_key): the 802.11
 * header and the plain body, its CCMP header and MIC already removed; pn its
 * packet number (checked here against replays), key_index the key it used;
 * bad: the card found its MIC wrong (dropped and counted). */
void wlan_receive_decrypted(struct netdev *d, const void *frame, size_t length, int channel,
                            int signal, uint64_t pn, int key_index, bool bad);

/* The card's firmware lost the access point (its beacons stopped): the link
 * goes down, the keys and wifid's waiting frames are dropped, and wifid joins
 * again. why is for the console line. Callable from the driver's thread; it
 * makes no driver calls (the firmware's side is reset at the next join). */
void wlan_link_lost(struct netdev *d, const char *why);

/* The driver's own scan (wlan.scan) has ended: wifid gets a
 * WLAN_FRAME_SCAN_DONE after the beacons it heard. */
void wlan_scan_done(struct netdev *d);

/* SYS_WLAN for the card d (root's): op, a kernel buffer of length bytes
 * (filled for WLAN_INFO and WLAN_RECEIVE), value. */
int64_t wlan_call(struct netdev *d, int op, void *buffer, size_t length, uint64_t value);

/* net's report: a line about the link (joined, channel). */
void wlan_print(struct netdev *d);

#endif

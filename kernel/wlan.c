/*
 * Wireless cards (step 24c, wlan.h): 802.11 data frames to and from the
 * Ethernet frames TCP/IP knows, CCMP (IEEE 802.11-2020 12.5.3) with the keys
 * the elf wifid sets, and wifid's side: management frames and EAPOL frames
 * queued for it, its frames sent, the channel, the link, the keys (SYS_WLAN).
 *
 * A station's data frames go to the access point (To DS: address 1 the
 * BSSID, 2 ours, 3 the destination) and come from it (From DS: 1 the
 * destination, 2 the BSSID, 3 the source), the Ethernet type behind an
 * LLC/SNAP header (AA AA 03 00 00 00). Frames from the access point are
 * taken only once joined; protected ones are opened with the pairwise key
 * (to us) or the group key their header names (to a group), and refused
 * when their packet number is not above the last one seen (replays).
 */
#include "wlan.h"
#include "aes.h"
#include "heap.h"
#include "kprintf.h"
#include "sched.h"
#include "string.h"
#include "sync.h"
#include "syscalls.h"
#include "timer.h"

/* (67d) A module's Wi-Fi card (driver_wifi.h): struct wifi_card is struct
 * wlan, field for field. */
#define SAME(field) _Static_assert(offsetof(struct wlan, field) == offsetof(struct wifi_card, field) && \
                                   sizeof(((struct wlan *)0)->field) == sizeof(((struct wifi_card *)0)->field), \
                                   "struct wifi_card's " #field " is not struct wlan's")
SAME(transmit); SAME(set_channel); SAME(scan); SAME(set_country); SAME(join); SAME(set_key);
SAME(block_ack); SAME(leave); SAME(channels); SAME(nchannels); SAME(state);
_Static_assert(sizeof(struct wlan) == sizeof(struct wifi_card), "struct wifi_card is not struct wlan");
#undef SAME

#define QUEUE 128                       /* frames waiting for wifid (a firmware
                                         * scan hands up many beacons at once) */
#define HEADER 24                       /* a non-QoS 802.11 header (three addresses) */
#define CCMP_HEADER 8
#define CCMP_MIC 8
#define LLC 8                           /* AA AA 03 00 00 00 + the Ethernet type */
#define TIDS 17                         /* replay counters: 16 TIDs and management */

struct key {
    bool set;
    bool in_card;                       /* the card encrypts/decrypts with it (set_key) */
    struct aes aes;
    uint64_t rx_pn[TIDS];               /* the last packet number received, per TID */
};

/* A Block Ack agreement for receiving (one TID): frames of a burst can
 * arrive out of order (parts of it sent again); they wait here and leave in
 * sequence order (IEEE 802.11-2020 10.25.6). A hole older than REORDER_MS is
 * given up on. Frames are kept as they came (still to be checked). */
#define REORDER 64                      /* the most we agree to (its window) */
#define REORDER_MS 100
struct held {
    uint8_t *frame;                     /* (kmalloc'd copy; 0: empty) */
    size_t length;
    int64_t card_pn;
    int key_index;
    uint16_t sn;
    uint64_t since;                     /* timer ticks */
};
struct reorder {
    bool on;
    uint16_t head;                      /* the sequence number expected next */
    uint16_t window;
    int stored;
    struct held slot[REORDER];
};

struct wlan_state {
    struct mutex lock;
    bool joined, mfp, secured;          /* secured: the link waits for the keys */
    bool driver_joined;                 /* the driver's join was called (leave due) */
    struct wlan_ht ht;                  /* from the access point's association response */
    int ba_send;                        /* sending in bursts (TID 0): 0 not asked, 1 asked,
                                         * 2 agreed, 3 refused (not asked again) */
    uint8_t ba_token;
    struct reorder rx_ba[8];            /* receiving in bursts, per TID */
    struct mutex rx_lock;               /* the receive path (the driver's thread and the
                                         * work thread both deliver) */
    /* Work for the thread "wlanN work": what answers the access point or
     * talks to the card must not run on the driver's receive thread (only
     * that thread hands the card's replies over). */
    struct wait_queue work_wq;
    volatile bool work_send_ba;         /* our ADDBA request was accepted: tell the card */
    int work_send_tid, work_send_window;
    volatile bool work_recv_ba;         /* the AP asked to send to us in bursts: answer */
    int work_recv_tid, work_recv_window, work_recv_ssn, work_recv_token, work_recv_timeout;
    volatile bool work_recv_off;        /* the AP ended its bursts */
    int work_off_tid;
    int signal16;                       /* our AP's signal, dBm x 16, averaged (0: none) */
    uint64_t scan_open_until;           /* a scan while joined: other senders' beacons and
                                         * probe responses are let through until then */
    volatile int csa_channel;           /* the AP announced a move there (0: none), */
    uint64_t csa_at;                    /* to happen at this tick */
    uint8_t bssid[6];
    uint16_t aid;
    int channel;
    uint16_t sequence;                  /* our frames' sequence numbers */
    struct key pairwise, group[4];
    struct {                            /* BIP's key (the IGTK, WPA3): group management */
        bool set;                       /* frames carry its MIC in an MMIE */
        int index;                      /* (4 or 5) */
        struct aes aes;
        uint64_t ipn;                   /* the last packet number taken */
    } igtk;
    uint64_t tx_pn;                     /* our packet numbers (pairwise key) */
    struct sys_wlan_frame *queue[QUEUE];
    int head, tail;                     /* (under the lock) */
    struct wait_queue arrived;
    uint8_t out[WLAN_FRAME_MAX];        /* the frame being built (under the lock) */
    uint8_t in[WLAN_FRAME_MAX];         /* the frame being opened (the receive thread) */
    uint16_t last_sequence[16];         /* duplicate detection: the last data frame's, per
                                         * TID, recorded only once it proved genuine */
    uint16_t has_last;                  /* (a bit per TID) */
    uint32_t rx_management, rx_eapol, rx_data, rx_replayed, rx_bad_mic, rx_dropped;
    uint32_t tx_data, tx_management;
    uint32_t rx_by_card, rx_by_layer;       /* protected frames opened by the card / here */
    uint32_t rx_bip_bad;                    /* group management frames failing BIP */
    uint32_t rx_others;                     /* management frames of other senders while
                                             * joined: dropped on arrival */
};

static const uint8_t SNAP[6] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00 };

/* Up once joined, and with WPA once the pairwise key is set (until then
 * only EAPOL may pass). */
static bool link_up(const struct wlan_state *s)
{
    return s->joined && (!s->secured || s->pairwise.set);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8);
}

/* --- CCMP --- */

/* The nonce and the associated data of a frame with header h (hl bytes). */
static size_t ccmp_aad(const uint8_t *h, size_t hl, bool management, uint8_t aad[32],
                       uint8_t nonce[13], uint64_t pn)
{
    aad[0] = management ? h[0] : (uint8_t)(h[0] & 0x8F);   /* data: subtype bits 4-6 */
    aad[1] = (uint8_t)((h[1] & ~0x38) | 0x40);             /* retry, power, more data off; protected */
    memcpy(aad + 2, h + 4, 18);                             /* addresses 1-3 */
    aad[20] = h[22] & 0x0F;                                 /* the fragment number only */
    aad[21] = 0;
    size_t n = 22;
    uint8_t priority = 0;
    if (hl >= 26) {                                         /* QoS data: the TID */
        aad[1] &= 0x7F;                                     /* (order off) */
        aad[22] = h[24] & 0x0F;
        aad[23] = 0;
        priority = h[24] & 0x0F;
        n = 24;
    }
    nonce[0] = (uint8_t)(priority | (management ? 0x10 : 0));
    memcpy(nonce + 1, h + 10, 6);                           /* the transmitter */
    for (int i = 0; i < 6; i++)
        nonce[7 + i] = (uint8_t)(pn >> (40 - 8 * i));
    return n;
}

/* Protect the frame in s->out (header hl bytes, body n) with the pairwise
 * key: the CCMP header after the header, the MIC after the body; the new
 * length. */
static size_t ccmp_seal(struct wlan_state *s, size_t hl, size_t n, bool management)
{
    uint8_t *f = s->out, aad[32], nonce[13];
    uint64_t pn = ++s->tx_pn;
    memmove(f + hl + CCMP_HEADER, f + hl, n);
    f[1] |= 0x40;                                           /* protected */
    uint8_t *c = f + hl;
    c[0] = (uint8_t)pn, c[1] = (uint8_t)(pn >> 8), c[2] = 0, c[3] = 0x20; /* key 0, ExtIV */
    c[4] = (uint8_t)(pn >> 16), c[5] = (uint8_t)(pn >> 24);
    c[6] = (uint8_t)(pn >> 32), c[7] = (uint8_t)(pn >> 40);
    size_t an = ccmp_aad(f, hl, management, aad, nonce, pn);
    uint8_t *body = f + hl + CCMP_HEADER;
    aes_ccm_seal(&s->pairwise.aes, nonce, aad, an, body, body, n, body + n, CCMP_MIC);
    return hl + CCMP_HEADER + n + CCMP_MIC;
}

/* Open a protected frame f (header hl, total n) into s->in: header then
 * body; the body's length, or -1 (no key, a replay, a wrong MIC). */
static long ccmp_open(struct wlan_state *s, const uint8_t *f, size_t hl, size_t n,
                      bool management, int tid)
{
    if (n < hl + CCMP_HEADER + CCMP_MIC)
        return -1;
    const uint8_t *c = f + hl;
    if (!(c[3] & 0x20))
        return -1;                                          /* (no ExtIV: not CCMP) */
    int index = c[3] >> 6;
    bool group = f[4] & 1;                                  /* address 1 a group */
    struct key *k = group ? &s->group[index] : &s->pairwise;
    if (!k->set)
        return -1;
    uint64_t pn = (uint64_t)c[0] | (uint64_t)c[1] << 8 | (uint64_t)c[4] << 16 |
                  (uint64_t)c[5] << 24 | (uint64_t)c[6] << 32 | (uint64_t)c[7] << 40;
    if (pn <= k->rx_pn[tid]) {
        s->rx_replayed++;
        return -1;
    }
    uint8_t aad[32], nonce[13];
    size_t an = ccmp_aad(f, hl, management, aad, nonce, pn);
    size_t body = n - hl - CCMP_HEADER - CCMP_MIC;
    memcpy(s->in, f, hl);
    if (!aes_ccm_open(&k->aes, nonce, aad, an, c + CCMP_HEADER, s->in + hl, body,
                      c + CCMP_HEADER + body, CCMP_MIC)) {
        s->rx_bad_mic++;
        return -1;
    }
    k->rx_pn[tid] = pn;                                     /* only once it checked */
    s->in[1] &= (uint8_t)~0x40;
    return (long)body;
}

/* --- wifid's queue --- */

static void enqueue(struct wlan_state *s, int kind, const uint8_t *bytes, size_t n, int channel,
                    int signal, const uint8_t source[6], bool opened)
{
    struct sys_wlan_frame *e = kmalloc(sizeof(*e) + n);
    if (!e)
        return;
    memset(e, 0, sizeof(*e));
    e->kind = (uint16_t)kind;
    e->length = (uint16_t)n;
    e->channel = (uint8_t)channel;
    e->signal = (int8_t)signal;
    memcpy(e->source, source, 6);
    e->protected_frame = opened;
    memcpy(e + 1, bytes, n);
    mutex_lock(&s->lock);
    int next = (s->head + 1) % QUEUE;
    if (next == s->tail) {                                  /* full: this one goes */
        s->rx_dropped++;
        mutex_unlock(&s->lock);
        kfree(e);
        return;
    }
    s->queue[s->head] = e;
    s->head = next;
    mutex_unlock(&s->lock);
    wait_queue_wake_all(&s->arrived);
}

static void forget_keys(struct wlan_state *s);

/* Drop every frame waiting for wifid (under the lock): after a lost link they
 * are stale, and a full queue would also drop the next scan's end. */
static void flush_queue(struct wlan_state *s)
{
    while (s->tail != s->head) {
        kfree(s->queue[s->tail]);
        s->tail = (s->tail + 1) % QUEUE;
    }
}

void wlan_link_lost(struct netdev *d, const char *why)
{
    struct wlan_state *s = d->wlan->state;
    if (!s)
        return;                                 /* (not registered yet: no link to lose) */
    mutex_lock(&s->lock);
    bool was = s->joined;
    s->joined = false;
    forget_keys(s);
    flush_queue(s);
    mutex_unlock(&s->lock);
    if (was)
        kprintf("%s: Wi-Fi link lost: %s\n", d->name, why);
}

void wlan_scan_done(struct netdev *d)
{
    static const uint8_t none[6];
    if (d->wlan->state) {
        d->wlan->state->scan_open_until = 0;                /* (the sweep is over) */
        enqueue(d->wlan->state, WLAN_FRAME_SCAN_DONE, none, 0, 0, 0, none, false);
    }
}

/* --- receiving --- */

/* Has a frame the card decrypted come in order? Its packet number must be
 * above the last one taken for that key and TID (the card does not check). */
static bool fresh(struct wlan_state *s, struct key *k, int tid, uint64_t pn)
{
    if (pn <= k->rx_pn[tid]) {
        s->rx_replayed++;
        return false;
    }
    k->rx_pn[tid] = pn;
    return true;
}

/* card_pn: the frame was decrypted by the card (its packet number; -1: not).
 * In order: after the reorder buffer, if there is one. */
static void deliver_data(struct netdev *d, struct wlan_state *s, const uint8_t *f, size_t n,
                         int64_t card_pn, int key_index)
{
    uint16_t fc = get16(f);
    int subtype = (fc >> 4) & 15;
    if (subtype & 4)
        return;                                             /* no data (null frames) */
    if ((fc & 0x0300) != 0x0200 || memcmp(f + 10, s->bssid, 6))
        return;                                             /* not from our access point */
    if (!(f[4] & 1) && memcmp(f + 4, d->mac, 6))
        return;                                             /* to another station */
    size_t hl = HEADER;
    int tid = 0;
    if (subtype & 8) {                                      /* QoS data */
        hl = 26 + ((fc & 0x8000) ? 4 : 0);                  /* (HT control with order) */
        tid = f[24] & 15;
    }
    if (n < hl)
        return;
    uint16_t sequence = get16(f + 22);
    if ((fc & 0x0800) && (s->has_last & (1u << tid)) && sequence == s->last_sequence[tid])
        return;                                             /* a retry of one we have */
    const uint8_t *body;
    size_t bn;
    if (card_pn >= 0) {                                     /* the card opened it */
        struct key *k = (f[4] & 1) ? &s->group[key_index & 3] : &s->pairwise;
        if (!k->set || !fresh(s, k, tid, (uint64_t)card_pn))
            return;
        s->rx_by_card++;
        body = f + hl, bn = n - hl;
    } else if (fc & 0x4000) {
        long k = ccmp_open(s, f, hl, n, false, tid);
        if (k < 0)
            return;
        s->rx_by_layer++;
        body = s->in + hl, bn = (size_t)k;
    } else {
        if (s->pairwise.set && !(f[4] & 1))
            return;                                         /* keys set: plain unicast refused */
        body = f + hl, bn = n - hl;
    }
    if (bn < LLC || memcmp(body, SNAP, 6))
        return;
    uint16_t type = (uint16_t)(body[6] << 8 | body[7]);
    if (!(fc & 0x4000) && card_pn < 0 && (f[4] & 1) && type != 0x888E &&
        s->group[1].set + s->group[2].set + s->group[3].set)
        return;                                             /* keys set: plain group refused */
    s->last_sequence[tid] = sequence;                       /* (only a frame that passed: a
                                                             * forged one must not make the
                                                             * genuine one look like a retry) */
    s->has_last |= (uint16_t)(1u << tid);
    if (type == 0x888E) {                                   /* EAPOL: wifid's */
        if (f[4] & 1)
            return;                                         /* key messages are to one station:
                                                             * a group one is forged (49k) */
        s->rx_eapol++;
        enqueue(s, WLAN_FRAME_EAPOL, body + LLC, bn - LLC, s->channel, 0, f + 16, false);
        return;
    }
    if (!link_up(s))
        return;                                             /* (only EAPOL before the keys) */
    size_t payload = bn - LLC;
    if (ETH_HEADER + payload > ETH_FRAME_MAX)
        return;
    uint8_t eth[ETH_FRAME_MAX];
    memcpy(eth, f + 4, 6);                                  /* destination: address 1 */
    memcpy(eth + 6, f + 16, 6);                             /* source: address 3 */
    eth[12] = body[6], eth[13] = body[7];
    memcpy(eth + ETH_HEADER, body + LLC, payload);
    s->rx_data++;
    net_receive(d, eth, ETH_HEADER + payload);
}

/* An association response from our access point to us: note its HT
 * Capabilities element (id 45) for the driver's join. Its fixed fields are
 * capability, status and AID (6 bytes); the elements follow. */
static void note_ht(struct netdev *d, struct wlan_state *s, const uint8_t *f, size_t n)
{
    int subtype = (get16(f) >> 4) & 15;
    if ((subtype != 1 && subtype != 3) || !s->joined || n < HEADER + 6 ||
        memcmp(f + 4, d->mac, 6) || memcmp(f + 10, s->bssid, 6))
        return;
    memset(&s->ht, 0, sizeof(s->ht));
    for (size_t i = HEADER + 6; i + 2 <= n && i + 2 + f[i + 1] <= n; i += 2 + f[i + 1]) {
        if (f[i] == 45 && f[i + 1] >= 26) {
            s->ht.on = true;
            s->ht.cap = get16(f + i + 2);
            s->ht.ampdu = f[i + 4];
            memcpy(s->ht.mcs, f + i + 5, 10);
        }
        if (f[i] == 0xDD && f[i + 1] >= 7 && f[i + 2] == 0x00 && f[i + 3] == 0x50 &&
            f[i + 4] == 0xF2 && f[i + 5] == 2)      /* WMM (Microsoft OUI, type 2) */
            s->ht.wmm = true;
        if (f[i] == 191 && f[i + 1] >= 12) {        /* VHT Capabilities */
            s->ht.vht = true;
            s->ht.vht_cap = (uint32_t)get16(f + i + 2) | (uint32_t)get16(f + i + 4) << 16;
            s->ht.vht_rx_mcs = get16(f + i + 6);
            s->ht.vht_tx_mcs = get16(f + i + 10);
        }
        if (f[i] == 61 && f[i + 1] >= 22) {         /* HT Operation: 40 MHz, which side */
            int offset = f[i + 3] & 3, any = f[i + 3] & 4, primary = f[i + 2];
            if (any && (offset == 1 || offset == 3) && s->ht.width < 40) {
                s->ht.width = 40;
                s->ht.center = offset == 1 ? primary + 2 : primary - 2;
            }
        }
        if (f[i] == 192 && f[i + 1] >= 5 && f[i + 2] >= 1 && f[i + 3]) {  /* VHT Operation: */
            s->ht.width = 80;                       /* 80 MHz (160 and 80+80 are taken */
            s->ht.center = f[i + 3];                /* as their first 80) */
        }
    }
    if (!s->ht.width || s->channel <= 14)
        s->ht.width = 20;                           /* (wifid offers 40/80 at 5 GHz only) */
    if (!s->ht.vht && s->ht.width > 40)
        s->ht.width = 40;
}

/* Block Ack action frames from our access point to us (category 3): its
 * answer to our ADDBA request (the agreement goes to the card's firmware,
 * which then sends in bursts) and DELBA (the agreement ends). They are the
 * layer's, not wifid's: true when f was one. */
static bool block_ack_frame(struct netdev *d, struct wlan_state *s, const uint8_t *f, size_t n)
{
    int subtype = (get16(f) >> 4) & 15;
    if (subtype != 13 || n < HEADER + 2 || f[HEADER] != 3 || !s->joined ||
        memcmp(f + 4, d->mac, 6) || memcmp(f + 10, s->bssid, 6))
        return false;
    const uint8_t *a = f + HEADER;
    if (a[1] == 1 && n >= HEADER + 9 && a[2] == s->ba_token && s->ba_send == 1) {
        uint16_t status = get16(a + 3), params = get16(a + 5);
        if (status == 0) {                                  /* accepted: the work thread */
            s->work_send_tid = (params >> 2) & 15;          /* tells the card */
            s->work_send_window = params >> 6;
            s->work_send_ba = true;
            wait_queue_wake_all(&s->work_wq);
        } else
            s->ba_send = 3;                                 /* refused: not asked again */
    } else if (a[1] == 0 && n >= HEADER + 9) {              /* ADDBA request: they would */
        uint16_t params = get16(a + 3);                     /* send to us in bursts */
        s->work_recv_token = a[2];
        s->work_recv_tid = (params >> 2) & 15;
        s->work_recv_window = params >> 6;
        s->work_recv_timeout = get16(a + 5);
        s->work_recv_ssn = get16(a + 7) >> 4;
        s->work_recv_ba = true;
        wait_queue_wake_all(&s->work_wq);
    } else if (a[1] == 2 && n >= HEADER + 6) {              /* DELBA */
        uint16_t params = get16(a + 2);
        int tid = params >> 12;
        if (params & (1 << 11)) {                           /* theirs: they stop sending */
            s->work_off_tid = tid;                          /* to us in bursts */
            s->work_recv_off = true;
            wait_queue_wake_all(&s->work_wq);
        } else if (s->ba_send == 2) {
            s->ba_send = 3;                                 /* ours: the card is told on */
            s->work_send_tid = tid;                         /* the work thread */
            s->work_send_window = -1;
            s->work_send_ba = true;
            wait_queue_wake_all(&s->work_wq);
        }
    }
    return true;
}

/* Ask the access point for a Block Ack agreement for TID 0 (an ADDBA
 * request; IEEE 802.11-2020 9.6.4.2), so the card may send in bursts. The
 * card fills in the starting sequence number (it numbers QoS data itself).
 * Under the lock, using s->out. */
static void ask_block_ack(struct netdev *d, struct wlan_state *s)
{
    uint8_t *f = s->out;
    put16(f, 0x00D0);                                       /* management, action */
    put16(f + 2, 0);
    memcpy(f + 4, s->bssid, 6);
    memcpy(f + 10, d->mac, 6);
    memcpy(f + 16, s->bssid, 6);
    put16(f + 22, (uint16_t)(s->sequence++ << 4));
    uint8_t *a = f + HEADER;
    a[0] = 3, a[1] = 0;                                     /* Block Ack, ADDBA request */
    a[2] = ++s->ba_token;
    put16(a + 3, (uint16_t)((1 << 1) | (0 << 2) | (64 << 6)));  /* immediate, TID 0, 64 */
    put16(a + 5, 0);                                        /* no timeout */
    put16(a + 7, 0);                                        /* starting sequence (the card's) */
    size_t total = HEADER + 9;
    if (s->mfp && s->pairwise.in_card)
        f[1] |= 0x40;                                       /* robust: the card seals it */
    else if (s->mfp)
        total = ccmp_seal(s, HEADER, 9, true);
    s->ba_send = 1;
    s->tx_management++;
    d->wlan->transmit(d, f, total);
}

/* BIP (IEEE 802.11-2020 12.5.4): a group-addressed management frame under
 * WPA3 ends with an MMIE (element 76: key ID, a 6-byte packet number, an
 * 8-byte MIC). True if it is there, under our IGTK, newer than the last, and
 * its MIC is AES-CMAC over the masked header and the body (MIC zeroed). The
 * receive lock is held (s->in is the scratch). */
static bool bip_ok(struct wlan_state *s, const uint8_t *f, size_t n)
{
    if (!s->igtk.set || n < HEADER + 18 || n - HEADER + 20 > WLAN_FRAME_MAX)
        return false;
    const uint8_t *mmie = f + n - 18;
    if (mmie[0] != 76 || mmie[1] != 16 || get16(mmie + 2) != s->igtk.index)
        return false;
    uint64_t ipn = 0;
    for (int i = 5; i >= 0; i--)
        ipn = ipn << 8 | mmie[4 + i];
    if (ipn <= s->igtk.ipn)
        return false;                                       /* a replay */
    uint8_t *m = s->in;                                     /* AAD, then the body */
    m[0] = f[0];
    m[1] = (uint8_t)(f[1] & ~0x38);                         /* retry, power, more data off */
    memcpy(m + 2, f + 4, 18);                               /* addresses 1-3 */
    memcpy(m + 20, f + HEADER, n - HEADER);
    memset(m + 20 + (n - HEADER) - 8, 0, 8);                /* the MIC zeroed */
    uint8_t tag[16];
    aes_cmac(&s->igtk.aes, m, 20 + (n - HEADER), tag);
    if (memcmp(tag, mmie + 10, 8))
        return false;
    s->igtk.ipn = ipn;
    return true;
}

/* A beacon from our access point announcing a channel switch (IEEE
 * 802.11-2020 11.8.8: Channel Switch Announcement, element 37: mode, new
 * channel, count; or Extended, element 60: mode, operating class, new
 * channel, count): the work thread moves with it when count beacon
 * intervals have passed, so the link survives the move. */
static void channel_switch(struct wlan_state *s, const uint8_t *f, size_t n)
{
    if (((get16(f) >> 4) & 15) != 8 || !s->joined || n < HEADER + 12 ||
        memcmp(f + 10, s->bssid, 6))
        return;
    uint16_t interval = get16(f + HEADER + 8);              /* TUs (1.024 ms) */
    for (size_t i = HEADER + 12; i + 2 <= n && i + 2 + f[i + 1] <= n; i += 2 + f[i + 1]) {
        int channel = 0, count = 0;
        if (f[i] == 37 && f[i + 1] >= 3)
            channel = f[i + 3], count = f[i + 4];
        else if (f[i] == 60 && f[i + 1] >= 4)
            channel = f[i + 4], count = f[i + 5];
        if (!channel || channel == s->channel || channel == s->csa_channel)
            continue;
        uint64_t ms = (uint64_t)count * (interval ? interval : 100) * 1024 / 1000;
        s->csa_at = timer_ticks() + ms * TIMER_HZ / 1000;
        s->csa_channel = channel;
        wait_queue_wake_all(&s->work_wq);
    }
}

/* A deauthentication or disassociation from our access point to us ends the
 * link (the access point restarted, renewed its keys without us, or sent us
 * away); its reason code goes on the console. */
static bool sent_away(struct netdev *d, struct wlan_state *s, const uint8_t *f, size_t n)
{
    int subtype = (get16(f) >> 4) & 15;
    if ((subtype != 10 && subtype != 12) || n < HEADER + 2 || !s->joined ||
        (memcmp(f + 4, d->mac, 6) && !(f[4] & 1)) || memcmp(f + 10, s->bssid, 6))
        return false;                                       /* (to everyone: BIP checked) */
    char why[64];
    ksnprintf(why, sizeof(why), "%s by the access point, reason %u",
              subtype == 12 ? "deauthenticated" : "disassociated", (uint32_t)get16(f + HEADER));
    wlan_link_lost(d, why);
    return true;
}

/* A frame from our access point with its signal: the running average (1/8
 * of each new one), for WLAN_SIGNAL and roaming. */
static void note_signal(struct wlan_state *s, const uint8_t *f, int signal)
{
    if (!signal || !s->joined || memcmp(f + 10, s->bssid, 6))
        return;
    s->signal16 = s->signal16 ? s->signal16 + (signal * 16 - s->signal16) / 8 : signal * 16;
}

/* Is sequence number a before b (12 bits, half the space each way)? */
static bool sn_before(uint16_t a, uint16_t b)
{
    return ((a - b) & 0xFFF) > 0x800;
}

static void release_slot(struct netdev *d, struct wlan_state *s, struct reorder *r, int i)
{
    struct held *h = &r->slot[i];
    if (!h->frame)
        return;
    uint8_t *f = h->frame;
    h->frame = 0;
    r->stored--;
    deliver_data(d, s, f, h->length, h->card_pn, h->key_index);
    kfree(f);
}

/* Deliver what is waiting in sequence from the head on. */
static void release_ready(struct netdev *d, struct wlan_state *s, struct reorder *r)
{
    for (int n = 0; n < REORDER; n++) {
        int i = r->head % REORDER;
        if (!r->slot[i].frame || r->slot[i].sn != r->head)
            break;
        release_slot(d, s, r, i);
        r->head = (r->head + 1) & 0xFFF;
    }
}

/* Move the head to target, delivering what waits before it (holes skipped). */
static void release_until(struct netdev *d, struct wlan_state *s, struct reorder *r,
                          uint16_t target)
{
    for (int n = 0; n < 4096 && r->head != target; n++) {
        int i = r->head % REORDER;
        if (r->slot[i].frame && r->slot[i].sn == r->head)
            release_slot(d, s, r, i);
        r->head = (r->head + 1) & 0xFFF;
        if (!r->stored) {                                   /* (nothing more to deliver) */
            r->head = target;
            break;
        }
    }
    release_ready(d, s, r);
}

static void reorder_off(struct reorder *r)
{
    for (int i = 0; i < REORDER; i++)
        if (r->slot[i].frame) {
            kfree(r->slot[i].frame);
            r->slot[i].frame = 0;
        }
    r->stored = 0;
    r->on = false;
}

/* Would deliver_data take this unicast QoS frame as genuine (its packet
 * number apart, which only counts in order)? A forged frame far ahead
 * would otherwise move the reorder window past the real ones (49k). The
 * card checked what it opened; the layer checks the MIC of what it opens
 * (again when it delivers: the packet number is checked then). */
static bool genuine(struct wlan_state *s, const uint8_t *f, size_t n, int64_t card_pn)
{
    if (card_pn >= 0)
        return s->pairwise.set;
    if (!(get16(f) & 0x4000))
        return !s->pairwise.set;                            /* plain: only before the keys */
    size_t hl = 26 + ((get16(f) & 0x8000) ? 4 : 0);
    if (n < hl + CCMP_HEADER + CCMP_MIC || !s->pairwise.set)
        return false;
    const uint8_t *c = f + hl;
    if (!(c[3] & 0x20))
        return false;
    uint64_t pn = (uint64_t)c[0] | (uint64_t)c[1] << 8 | (uint64_t)c[4] << 16 |
                  (uint64_t)c[5] << 24 | (uint64_t)c[6] << 32 | (uint64_t)c[7] << 40;
    uint8_t aad[32], nonce[13];
    size_t an = ccmp_aad(f, hl, false, aad, nonce, pn);
    size_t body = n - hl - CCMP_HEADER - CCMP_MIC;
    return aes_ccm_open(&s->pairwise.aes, nonce, aad, an, c + CCMP_HEADER, s->in + hl, body,
                        c + CCMP_HEADER + body, CCMP_MIC);
}

/* A data frame from the access point: through its TID's reorder buffer when
 * it sends that TID in bursts, else straight on. The receive lock is held. */
static void receive_data(struct netdev *d, struct wlan_state *s, const uint8_t *f, size_t n,
                         int64_t card_pn, int key_index)
{
    uint16_t fc = get16(f);
    int subtype = (fc >> 4) & 15;
    if (!(subtype & 8) || (subtype & 4) || n < 26 || (f[4] & 1) || memcmp(f + 4, d->mac, 6) ||
        !s->rx_ba[f[24] & 7].on || (f[24] & 8)) {
        deliver_data(d, s, f, n, card_pn, key_index);       /* not part of an agreement */
        return;
    }
    if (!genuine(s, f, n, card_pn))
        return;                                             /* (49k: never moves the window) */
    struct reorder *r = &s->rx_ba[f[24] & 7];
    uint16_t sn = get16(f + 22) >> 4;
    if (sn_before(sn, r->head))
        return;                                             /* old: delivered or given up */
    uint16_t end = (uint16_t)((r->head + r->window) & 0xFFF);
    if (!sn_before(sn, end))                                /* beyond the window: move it */
        release_until(d, s, r, (uint16_t)((sn - r->window + 1) & 0xFFF));
    if (sn == r->head) {
        deliver_data(d, s, f, n, card_pn, key_index);
        r->head = (r->head + 1) & 0xFFF;
        release_ready(d, s, r);
        return;
    }
    struct held *h = &r->slot[sn % REORDER];
    if (h->frame)
        return;                                             /* we have it already */
    h->frame = kmalloc(n);
    if (!h->frame)
        return;
    memcpy(h->frame, f, n);
    h->length = n, h->card_pn = card_pn, h->key_index = key_index;
    h->sn = sn, h->since = timer_ticks();
    r->stored++;
}

/* Holes given up on: what waited longer than REORDER_MS moves the head past
 * the gap before it. (The work thread, with the receive lock.) */
static void reorder_timeouts(struct netdev *d, struct wlan_state *s)
{
    for (int t = 0; t < 8; t++) {
        struct reorder *r = &s->rx_ba[t];
        if (!r->on || !r->stored)
            continue;
        for (int n = 0; n < REORDER && r->stored; n++) {
            int i = (r->head + n) % REORDER;
            struct held *h = &r->slot[i];
            if (!h->frame)
                continue;
            if ((timer_ticks() - h->since) * 1000 / TIMER_HZ < REORDER_MS)
                break;                                      /* the oldest waits on */
            release_until(d, s, r, h->sn);                  /* skip the hole before it */
        }
    }
}

void wlan_receive(struct netdev *d, const void *frame, size_t length, int channel, int signal)
{
    struct wlan_state *s = d->wlan->state;
    if (!s)
        return;                                 /* (not registered yet: nothing to give it to) */
    const uint8_t *f = frame;
    if (length < HEADER || length > WLAN_FRAME_MAX)
        return;
    uint16_t fc = get16(f);
    int type = (fc >> 2) & 3;
    note_signal(s, f, signal);
    int sub0 = (get16(f) >> 4) & 15;
    bool scanning = (sub0 == 8 || sub0 == 5) && timer_ticks() < s->scan_open_until;
    if (type == 0 && s->joined && !scanning && memcmp(f + 10, s->bssid, 6) &&
        memcmp(f + 4, s->bssid, 6)) {
        /* While joined only our access point's management frames matter
         * (wifi scan does not run while joined). The card's firmware passes
         * other devices' broadcast action frames up whatever its filter says
         * (about 20 a second on the laptop's network): dropped here, first
         * thing, with nothing copied or queued. */
        s->rx_others++;
        return;
    }
    if (type == 0) {                                        /* management: wifid's */
        if (fc & 0x4000) {                                  /* protected (MFP): to us only */
            if (!s->joined || memcmp(f + 4, d->mac, 6) || memcmp(f + 10, s->bssid, 6))
                return;
            long k = ccmp_open(s, f, HEADER, length, true, TIDS - 1);
            if (k < 0)
                return;
            s->rx_management++;
            if (sent_away(d, s, s->in, HEADER + (size_t)k) ||
                block_ack_frame(d, s, s->in, HEADER + (size_t)k))
                return;
            enqueue(s, WLAN_FRAME_MANAGEMENT, s->in, HEADER + (size_t)k, channel, signal,
                    f + 10, true);
            return;
        }
        s->rx_management++;
        note_ht(d, s, f, length);
        channel_switch(s, f, length);
        int sub = (get16(f) >> 4) & 15;
        if (s->mfp && s->joined && (f[4] & 1) && (sub == 10 || sub == 12 || sub == 13) &&
            !memcmp(f + 10, s->bssid, 6)) {
            /* A group management frame under WPA3: only with a good BIP MIC
             * (a forged "everybody leave" must not end the link). */
            mutex_lock(&s->rx_lock);
            bool ok = bip_ok(s, f, length);
            mutex_unlock(&s->rx_lock);
            if (!ok) {
                s->rx_bip_bad++;
                return;
            }
            if (sent_away(d, s, f, length))
                return;
            enqueue(s, WLAN_FRAME_MANAGEMENT, f, length, channel, signal, f + 10, true);
            return;
        }
        if (!s->mfp && sent_away(d, s, f, length))         /* (with MFP only a protected */
            return;                                         /* one counts: no forged ones) */
        if (!s->mfp && block_ack_frame(d, s, f, length))
            return;
        enqueue(s, WLAN_FRAME_MANAGEMENT, f, length, channel, signal, f + 10, false);
        return;
    }
    if (type == 2 && s->joined) {
        mutex_lock(&s->rx_lock);
        receive_data(d, s, f, length, -1, 0);
        mutex_unlock(&s->rx_lock);
    }
}

void wlan_receive_decrypted(struct netdev *d, const void *frame, size_t length, int channel,
                            int signal, uint64_t pn, int key_index, bool bad)
{
    struct wlan_state *s = d->wlan->state;
    if (!s)
        return;                                 /* (not registered yet: nothing to give it to) */
    const uint8_t *f = frame;
    if (length < HEADER || length > WLAN_FRAME_MAX || !s->joined)
        return;
    if (bad) {
        s->rx_bad_mic++;
        return;
    }
    if (memcmp(f + 10, s->bssid, 6))
        return;                                             /* not from our access point */
    note_signal(s, f, signal);
    int type = (get16(f) >> 2) & 3;
    if (type == 2) {
        mutex_lock(&s->rx_lock);
        receive_data(d, s, f, length, (int64_t)(pn & 0xFFFFFFFFFFFFull), key_index);
        mutex_unlock(&s->rx_lock);
        return;
    }
    if (type == 0 && !memcmp(f + 4, d->mac, 6) && fresh(s, &s->pairwise, TIDS - 1, pn)) {
        s->rx_management++;                                 /* protected (MFP), opened */
        if (sent_away(d, s, f, length) || block_ack_frame(d, s, f, length))
            return;
        enqueue(s, WLAN_FRAME_MANAGEMENT, f, length, channel, signal, f + 10, true);
    }
}

/* --- sending --- */

/* The header of a frame to the access point in s->out; its length. */
static size_t data_header(struct netdev *d, struct wlan_state *s, const uint8_t *destination)
{
    uint8_t *f = s->out;
    put16(f, s->ht.wmm ? 0x0188 : 0x0108);                  /* (QoS) data, To DS */
    put16(f + 2, 0);
    memcpy(f + 4, s->bssid, 6);
    memcpy(f + 10, d->mac, 6);
    memcpy(f + 16, destination, 6);
    put16(f + 22, (uint16_t)(s->sequence++ << 4));
    if (!s->ht.wmm)
        return HEADER;
    put16(f + 24, 0);                                       /* QoS control: TID 0 (best */
    return HEADER + 2;                                      /* effort), normal ack */
}

/* Send what s->out holds (header hl, body n), protected if a pairwise key
 * is set. Called with the lock held. */
static bool send_data(struct netdev *d, struct wlan_state *s, size_t hl, size_t n)
{
    size_t total = hl + n;
    if (s->pairwise.set && s->pairwise.in_card)
        s->out[1] |= 0x40;                                  /* the card seals it */
    else if (s->pairwise.set)
        total = ccmp_seal(s, hl, n, false);
    s->tx_data++;
    return d->wlan->transmit(d, s->out, total);
}

static bool wlan_send(struct netdev *d, const void *frame, size_t length)
{
    struct wlan_state *s = d->wlan->state;
    const uint8_t *eth = frame;
    if (length < ETH_HEADER || length > ETH_FRAME_MAX)
        return false;
    mutex_lock(&s->lock);
    if (!link_up(s)) {
        mutex_unlock(&s->lock);
        return false;
    }
    size_t hl = data_header(d, s, eth);
    memcpy(s->out + hl, SNAP, 6);
    s->out[hl + 6] = eth[12], s->out[hl + 7] = eth[13];
    memcpy(s->out + hl + LLC, eth + ETH_HEADER, length - ETH_HEADER);
    bool ok = send_data(d, s, hl, LLC + length - ETH_HEADER);
    if (ok && s->ba_send == 0 && s->ht.on && s->ht.wmm && d->wlan->block_ack)
        ask_block_ack(d, s);                                /* (once per join) */
    mutex_unlock(&s->lock);
    if (ok)                                                 /* (counted like a driver's) */
        d->tx_frames++, d->tx_bytes += length;
    else
        d->tx_dropped++;
    return ok;
}

/* The fastest the link may go (the short guard interval): 802.11g's 54
 * Mbit/s; 802.11n 72/144 at 20 MHz, 150/300 at 40 (one or two streams);
 * 802.11ac (MCS 9) 200/400 at 40 MHz, 433/867 at 80. */
static uint32_t wlan_link_speed(struct netdev *d)
{
    struct wlan_state *s = d->wlan->state;
    if (!link_up(s))
        return 0;
    if (!s->ht.on)
        return 54;
    bool two = s->ht.mcs[1];
    if (s->ht.vht && s->ht.width == 80)
        return two ? 867 : 433;
    if (s->ht.vht && s->ht.width == 40)
        return two ? 400 : 200;
    if (s->ht.width == 40)
        return two ? 300 : 150;
    return two ? 144 : 72;
}

/* Answer the access point's ADDBA request (accepted; our window at most
 * REORDER) and prepare the reorder buffer; the card is told too. */
static void accept_block_ack(struct netdev *d, struct wlan_state *s)
{
    int tid = s->work_recv_tid & 7;
    int window = s->work_recv_window;
    if (window <= 0 || window > REORDER)
        window = REORDER;
    struct wlan *w = d->wlan;
    if (!w->block_ack || !w->block_ack(d, tid, false, true, (uint16_t)s->work_recv_ssn,
                                       (uint16_t)window))
        return;                                             /* (no answer: no bursts) */
    mutex_lock(&s->rx_lock);
    struct reorder *r = &s->rx_ba[tid];
    reorder_off(r);
    r->head = (uint16_t)s->work_recv_ssn;
    r->window = (uint16_t)window;
    r->on = true;
    mutex_unlock(&s->rx_lock);
    mutex_lock(&s->lock);
    uint8_t *f = s->out;
    put16(f, 0x00D0);                                       /* management, action */
    put16(f + 2, 0);
    memcpy(f + 4, s->bssid, 6);
    memcpy(f + 10, d->mac, 6);
    memcpy(f + 16, s->bssid, 6);
    put16(f + 22, (uint16_t)(s->sequence++ << 4));
    uint8_t *a = f + HEADER;
    a[0] = 3, a[1] = 1;                                     /* Block Ack, ADDBA response */
    a[2] = (uint8_t)s->work_recv_token;
    put16(a + 3, 0);                                        /* success */
    put16(a + 5, (uint16_t)((1 << 1) | (tid << 2) | (window << 6)));  /* immediate */
    put16(a + 7, (uint16_t)s->work_recv_timeout);
    size_t total = HEADER + 9;
    if (s->mfp && s->pairwise.in_card)
        f[1] |= 0x40;                                       /* robust: the card seals it */
    else if (s->mfp)
        total = ccmp_seal(s, HEADER, 9, true);
    s->tx_management++;
    w->transmit(d, f, total);
    mutex_unlock(&s->lock);
    kprintf("%s: receiving in bursts from the access point (TID %d, up to %d frames)\n",
            d->name, tid, window);
}

/* The thread "wlanN work": the agreements' answers and the card's part, and
 * the reorder buffers' timeouts (every 20 ms while frames wait). */
static void work(void *context)
{
    struct netdev *d = context;
    struct wlan_state *s = d->wlan->state;
    for (;;) {
        wait_event_until(&s->work_wq, s->work_send_ba || s->work_recv_ba || s->work_recv_off,
                         timer_ticks() + (20 * TIMER_HZ + 999) / 1000);
        struct wlan *w = d->wlan;
        if (s->csa_channel && timer_ticks() >= s->csa_at) {
            int channel = s->csa_channel;
            bool known = false;
            for (int i = 0; i < w->nchannels; i++)
                known |= w->channels[i] == channel;
            if (s->joined && known && w->set_channel(d, channel)) {
                mutex_lock(&s->lock);
                s->channel = channel;
                mutex_unlock(&s->lock);
                kprintf("%s: the access point moved to channel %d; following it\n", d->name,
                        channel);
            }
            s->csa_channel = 0;
        }
        if (s->work_send_ba) {
            s->work_send_ba = false;
            bool on = s->work_send_window >= 0;
            if (w->block_ack && w->block_ack(d, s->work_send_tid, true, on, 0,
                                             (uint16_t)(on ? s->work_send_window : 0)) && on) {
                s->ba_send = 2;
                kprintf("%s: sending in bursts to the access point (TID %d, up to %d frames)\n",
                        d->name, s->work_send_tid, s->work_send_window);
            } else if (on)
                s->ba_send = 3;
            else
                kprintf("%s: the access point ended our bursts (TID %d)\n", d->name,
                        s->work_send_tid);
        }
        if (s->work_recv_ba) {
            s->work_recv_ba = false;
            accept_block_ack(d, s);
        }
        if (s->work_recv_off) {
            s->work_recv_off = false;
            int tid = s->work_off_tid & 7;
            mutex_lock(&s->rx_lock);
            reorder_off(&s->rx_ba[tid]);
            mutex_unlock(&s->rx_lock);
            if (w->block_ack)
                w->block_ack(d, tid, false, false, 0, 0);
        }
        mutex_lock(&s->rx_lock);
        reorder_timeouts(d, s);
        mutex_unlock(&s->rx_lock);
    }
}

void wlan_register(struct netdev *d)
{
    struct wlan_state *s = kmalloc(sizeof(*s));
    if (!s)
        return;
    memset(s, 0, sizeof(*s));
    s->lock = (struct mutex)MUTEX_INIT("wlan");
    s->rx_lock = (struct mutex)MUTEX_INIT("wlan rx");
    d->wlan->state = s;
    d->send = wlan_send;
    d->link_speed = wlan_link_speed;
    net_register(d);
    char name[16];
    ksnprintf(name, sizeof(name), "%s work", d->name);
    thread_create(name, work, d);
}

/* --- SYS_WLAN --- */

static void forget_keys(struct wlan_state *s)
{
    memset(&s->pairwise, 0, sizeof(s->pairwise));
    memset(s->group, 0, sizeof(s->group));
    memset(&s->igtk, 0, sizeof(s->igtk));
    s->tx_pn = 0;
}

static int64_t receive_for_wifid(struct wlan_state *s, void *buffer, size_t length, uint64_t ms)
{
    uint64_t deadline = timer_ticks() + ms * TIMER_HZ / 1000 + 1;
    struct thread *self = thread_current();
    wait_event_until(&s->arrived, s->head != s->tail || self->stop_requested, deadline);
    mutex_lock(&s->lock);
    if (s->head == s->tail) {
        mutex_unlock(&s->lock);
        return self->stop_requested ? -ERR_INTR : -ERR_TIMEDOUT;
    }
    struct sys_wlan_frame *e = s->queue[s->tail];
    size_t n = sizeof(*e) + e->length;
    if (n > length) {                                       /* too big for the buffer: dropped */
        s->tail = (s->tail + 1) % QUEUE;
        mutex_unlock(&s->lock);
        kfree(e);
        return -ERR_2BIG;
    }
    s->tail = (s->tail + 1) % QUEUE;
    mutex_unlock(&s->lock);
    memcpy(buffer, e, n);
    kfree(e);
    return (int64_t)n;
}

int64_t wlan_call(struct netdev *d, int op, void *buffer, size_t length, uint64_t value)
{
    struct wlan *w = d->wlan;
    struct wlan_state *s = w->state;
    switch (op) {
    case WLAN_INFO: {
        if (length < sizeof(struct sys_wlan_info))
            return -ERR_INVAL;
        struct sys_wlan_info *info = buffer;
        memset(info, 0, sizeof(*info));
        mutex_lock(&s->lock);
        memcpy(info->mac, d->mac, 6);
        info->joined = s->joined;
        info->channel = (uint8_t)s->channel;
        memcpy(info->bssid, s->bssid, 6);
        info->aid = s->aid;
        info->channels = (uint8_t)w->nchannels;
        memcpy(info->channel_list, w->channels, (size_t)w->nchannels);
        info->rx_management = s->rx_management, info->rx_eapol = s->rx_eapol;
        info->rx_data = s->rx_data, info->rx_replayed = s->rx_replayed;
        info->rx_bad_mic = s->rx_bad_mic, info->rx_dropped = s->rx_dropped;
        info->tx_data = s->tx_data, info->tx_management = s->tx_management;
        mutex_unlock(&s->lock);
        return 0;
    }
    case WLAN_CHANNEL: {
        bool known = false;
        for (int i = 0; i < w->nchannels; i++)
            known |= w->channels[i] == value;
        if (!known)
            return -ERR_INVAL;
        if (!s->joined && s->driver_joined && w->leave) {   /* a lost link's join is over: */
            w->leave(d);                                    /* the card tunes for a new */
            s->driver_joined = false;                       /* one (and stays on it) */
        }
        if (s->joined)                                      /* a scan by hand while joined */
            s->scan_open_until = timer_ticks() + 2 * TIMER_HZ;
        if (!w->set_channel(d, (int)value))
            return -ERR_IO;
        mutex_lock(&s->lock);
        s->channel = (int)value;
        mutex_unlock(&s->lock);
        return 0;
    }
    case WLAN_SEND: {
        if (length < HEADER || length > WLAN_FRAME_MAX - CCMP_HEADER - CCMP_MIC)
            return -ERR_INVAL;
        if (((get16(buffer) >> 2) & 3) != 0)
            return -ERR_INVAL;                              /* management frames only */
        mutex_lock(&s->lock);
        memcpy(s->out, buffer, length);
        put16(s->out + 22, (uint16_t)(s->sequence++ << 4));
        size_t total = length;
        if (value && s->mfp && s->pairwise.set && s->pairwise.in_card)
            s->out[1] |= 0x40;                              /* protected: the card seals it */
        else if (value && s->mfp && s->pairwise.set)       /* protected (robust, unicast) */
            total = ccmp_seal(s, HEADER, length - HEADER, true);
        s->tx_management++;
        bool ok = w->transmit(d, s->out, total);
        mutex_unlock(&s->lock);
        return ok ? 0 : -ERR_AGAIN;
    }
    case WLAN_RECEIVE:
        return receive_for_wifid(s, buffer, length, value);
    case WLAN_SCAN:
        if (!w->scan)
            return -ERR_NOTSUP;                             /* wifid scans by hand instead */
        if (length > 32)
            return -ERR_INVAL;
        s->scan_open_until = timer_ticks() + 12 * TIMER_HZ;  /* (a sweep takes ~5 s) */
        return w->scan(d, length ? buffer : 0, length) ? 0 : -ERR_IO;
    case WLAN_SIGNAL:
        return s->joined && s->signal16 ? -s->signal16 / 16 : 0;   /* (-dBm: errors are
                                                                       * the negative ones) */
    case WLAN_COUNTRY: {
        if (!w->set_country)
            return -ERR_NOTSUP;
        char cc[3] = { 0 };
        if (length < 2)
            return -ERR_INVAL;
        memcpy(cc, buffer, 2);
        return w->set_country(d, cc) ? 0 : -ERR_IO;
    }
    case WLAN_JOIN: {
        if (length < sizeof(struct sys_wlan_join))
            return -ERR_INVAL;
        const struct sys_wlan_join *j = buffer;
        if (!j->aid && s->driver_joined && w->leave) {    /* a new join: the firmware */
            w->leave(d);                                    /* forgets the old one first */
            s->driver_joined = false;
        }
        mutex_lock(&s->lock);
        bool again = s->joined && !memcmp(s->bssid, j->bssid, 6);
        memcpy(s->bssid, j->bssid, 6);
        s->aid = j->aid;
        s->mfp = j->flags & WLAN_JOIN_MFP;
        s->secured = j->flags & WLAN_JOIN_SECURED;
        if (!again) {                                       /* (again: now associated) */
            forget_keys(s);
            s->has_last = 0;
            memset(&s->ht, 0, sizeof(s->ht));
            s->ba_send = 0;
            s->signal16 = 0;
        }
        s->joined = true;
        mutex_unlock(&s->lock);
        if (!again) {                                       /* (the receive lock comes */
            mutex_lock(&s->rx_lock);                        /* before the layer's, never */
            for (int t = 0; t < 8; t++)                     /* inside it) */
                reorder_off(&s->rx_ba[t]);
            mutex_unlock(&s->rx_lock);
        }
        if (j->aid && w->join) {
            if (!w->join(d, j->bssid, j->aid, j->channel, &s->ht,
                         j->flags & WLAN_JOIN_POWERSAVE))
                return -ERR_IO;
            s->driver_joined = true;
        }
        return 0;
    }
    case WLAN_LEAVE:
        mutex_lock(&s->lock);
        s->joined = false;
        forget_keys(s);
        mutex_unlock(&s->lock);
        if (w->leave)
            w->leave(d);
        s->driver_joined = false;
        return 0;
    case WLAN_KEY: {
        if (length < sizeof(struct sys_wlan_key))
            return -ERR_INVAL;
        const struct sys_wlan_key *k = buffer;
        if (k->length != 16)
            return -ERR_NOTSUP;                             /* CCMP-128 only */
        struct key *to;
        if (k->kind == WLAN_KEY_PAIRWISE)
            to = &s->pairwise;
        else if (k->kind == WLAN_KEY_GROUP && k->index >= 1 && k->index <= 3)
            to = &s->group[k->index];
        else if (k->kind == WLAN_KEY_IGTK && (k->index == 4 || k->index == 5)) {
            uint64_t ipn = 0;                               /* BIP's key: the layer checks */
            for (int i = 5; i >= 0; i--)                    /* group management frames */
                ipn = ipn << 8 | k->rsc[i];
            mutex_lock(&s->rx_lock);
            aes_init(&s->igtk.aes, k->key, 16);
            s->igtk.index = k->index;
            s->igtk.ipn = ipn ? ipn - 1 : 0;
            s->igtk.set = true;
            mutex_unlock(&s->rx_lock);
            return 0;
        }
        else
            return -ERR_INVAL;
        uint64_t rsc = 0;
        for (int i = 0; i < 6; i++)
            rsc |= (uint64_t)k->rsc[i] << (8 * i);
        /* The card's first: if it takes the key, the layer never uses it
         * (only the card counts its packet numbers). */
        bool card = w->set_key && w->set_key(d, k->kind, k->index, k->key);
        mutex_lock(&s->lock);
        aes_init(&to->aes, k->key, 16);
        for (int i = 0; i < TIDS; i++)                      /* the next one must be >= rsc */
            to->rx_pn[i] = rsc ? rsc - 1 : 0;
        to->set = true;
        to->in_card = card;
        if (to == &s->pairwise)
            s->tx_pn = 0;
        mutex_unlock(&s->lock);
        return 0;
    }
    case WLAN_EAPOL: {
        if (length < 6 + 4 || length > 6 + 1500)
            return -ERR_INVAL;
        const uint8_t *b = buffer;
        mutex_lock(&s->lock);
        if (!s->joined || memcmp(b, s->bssid, 6)) {
            mutex_unlock(&s->lock);
            return -ERR_NOTCONN;
        }
        size_t hl = data_header(d, s, b);
        memcpy(s->out + hl, SNAP, 6);
        s->out[hl + 6] = 0x88, s->out[hl + 7] = 0x8E;
        memcpy(s->out + hl + LLC, b + 6, length - 6);
        bool ok = send_data(d, s, hl, LLC + length - 6);
        mutex_unlock(&s->lock);
        return ok ? 0 : -ERR_AGAIN;
    }
    }
    return -ERR_INVAL;
}

void wlan_print(struct netdev *d)
{
    struct wlan_state *s = d->wlan->state;
    if (s->joined)
        kprintf("       Wi-Fi: joined %02x:%02x:%02x:%02x:%02x:%02x on channel %d%s\n",
                s->bssid[0], s->bssid[1], s->bssid[2], s->bssid[3], s->bssid[4], s->bssid[5],
                s->channel, !s->pairwise.set ? "" : s->pairwise.in_card ?
                ", protected (CCMP, in the card)" : ", protected (CCMP)");
    else
        kprintf("       Wi-Fi: not joined (channel %d)\n", s->channel);
    kprintf("       Wi-Fi frames: data %u in, %u out; management %u in, %u out; EAPOL %u in;"
            " refused %u (MIC), %u (replay), %u (queue full)\n",
            s->rx_data, s->tx_data, s->rx_management, s->tx_management, s->rx_eapol,
            s->rx_bad_mic, s->rx_replayed, s->rx_dropped);
    kprintf("       Wi-Fi: protected frames opened by the card %u, by the layer %u;"
            " %u management frames of other senders dropped while joined;"
            " %u group management frames failed BIP\n",
            s->rx_by_card, s->rx_by_layer, s->rx_others, s->rx_bip_bad);
}

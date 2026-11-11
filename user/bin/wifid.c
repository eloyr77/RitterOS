/*
 * wifid: the elf that owns the wireless link (step 24e). It reads the saved
 * networks in /system/config/wifi/networks/, scans wlan0 for one that is in
 * range, and joins it: authentication (open, or SAE for WPA3), association,
 * and -- for WPA2/WPA3 -- the 4-way handshake, after which it installs the
 * keys and the kernel's link comes up. The IP side (a fixed address or
 * dhclient) is the ordinary network stack's, unchanged. wifid then watches
 * the link and rejoins if it drops. Driven entirely through SYS_WLAN
 * (ritteros_wlan); the WPA cryptography is crypto.h's (sha1.c, wpacrypto.c,
 * sae.c).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "crypto.h"
#include "ritteros.h"
#include "settings_file.h"

PROGRAM_HELP("wifid", "join and hold the wireless link (the elf; see the wifi command)");

#define NETWORKS_DIR "/system/config/wifi/networks"

struct network {
    char ssid[33];
    char passphrase[64];
    char security[8];           /* open, wpa2, wpa3 */
    bool automatic;
};

static char warned[33];         /* the network last told "needs a passphrase" (said once:
                                 * wifid looks every few seconds, the files are read again) */

static struct network saved[16];
static int nsaved;

static char card[8] = "wlan0";
static uint8_t our_mac[6];
static uint8_t frame[sizeof(struct sys_wlan_frame) + WLAN_FRAME_MAX];
static size_t frame_length;                     /* (key_handshakes' message 1, for four_way) */

/* --- reading the saved networks --- */

static void load_networks(void)
{
    nsaved = 0;
    DIR *dir = opendir(NETWORKS_DIR);
    if (!dir)
        return;
    struct dirent *e;
    while (nsaved < 16 && (e = readdir(dir))) {
        if (e->d_name[0] == '.')
            continue;
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", NETWORKS_DIR, e->d_name);
        struct network *n = &saved[nsaved];
        memset(n, 0, sizeof(*n));
        char auto_value[8] = "yes";
        if (config_get(path, "ssid", n->ssid, sizeof(n->ssid)) != 1)
            continue;
        config_get(path, "passphrase", n->passphrase, sizeof(n->passphrase));
        if (config_get(path, "security", n->security, sizeof(n->security)) != 1)
            snprintf(n->security, sizeof(n->security), "open");
        config_get(path, "automatic", auto_value, sizeof(auto_value));
        n->automatic = strcmp(auto_value, "no") != 0;
        nsaved++;
    }
    closedir(dir);
}

/* --- 802.11 helpers --- */

static uint16_t get16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/* Receive a management or EAPOL frame (kind) for us within ms; its length in
 * frame (after the sys_wlan_frame header), or 0. */
static size_t receive(int kind, int subtype, uint32_t ms)
{
    uint64_t end = ritteros_time() + (uint64_t)ms * 1000000;
    while (ritteros_time() < end) {
        int64_t r = ritteros_wlan(WLAN_RECEIVE, card, frame, sizeof(frame), 100);
        if (r < (int64_t)sizeof(struct sys_wlan_frame))
            continue;
        struct sys_wlan_frame *h = (struct sys_wlan_frame *)frame;
        const uint8_t *f = frame + sizeof(*h);
        if (h->kind != kind)
            continue;
        if (kind == WLAN_FRAME_MANAGEMENT && subtype >= 0 && ((f[0] >> 4) & 0xf) != subtype)
            continue;
        return h->length;
    }
    return 0;
}

static const uint8_t *frame_body(void)
{
    return frame + sizeof(struct sys_wlan_frame);
}

/* The access point's answer to us: a management frame of subtype from bssid
 * to our address with at least `body` bytes after its header, within ms;
 * others are passed over (49l: before the join, the kernel lets every
 * sender's management frames through, and a short frame would leave the
 * previous one's bytes where its fields should be). Its length, or 0. */
static size_t reply(int subtype, const uint8_t *bssid, size_t body, uint32_t ms)
{
    uint64_t end = ritteros_time() + (uint64_t)ms * 1000000;
    for (;;) {
        uint64_t now = ritteros_time();
        if (now >= end)
            return 0;
        size_t got = receive(WLAN_FRAME_MANAGEMENT, subtype, (uint32_t)((end - now) / 1000000) + 1);
        const uint8_t *f = frame_body();
        if (got >= 24 + body && !memcmp(f + 4, our_mac, 6) && !memcmp(f + 10, bssid, 6) &&
            !memcmp(f + 16, bssid, 6))
            return got;
    }
}

/* A management frame's header (type 0, subtype, to the access point). */
static size_t management(uint8_t *out, int subtype, const uint8_t *bssid)
{
    out[0] = (uint8_t)(subtype << 4);
    out[1] = 0;
    out[2] = out[3] = 0;                        /* duration */
    memcpy(out + 4, bssid, 6);                  /* address 1: the AP */
    memcpy(out + 10, our_mac, 6);               /* address 2: us */
    memcpy(out + 16, bssid, 6);                 /* address 3: the BSSID */
    out[22] = out[23] = 0;                      /* the kernel fills the sequence */
    return 24;
}

static size_t rsn_ie(uint8_t *out, int akm)
{
    static const uint8_t ccmp[4] = { 0x00, 0x0f, 0xac, 0x04 };
    size_t n = 0;
    out[n++] = 48;
    out[n++] = 20;
    out[n++] = 1, out[n++] = 0;                 /* version */
    memcpy(out + n, ccmp, 4), n += 4;           /* group cipher */
    out[n++] = 1, out[n++] = 0;                 /* one pairwise */
    memcpy(out + n, ccmp, 4), n += 4;
    out[n++] = 1, out[n++] = 0;                 /* one AKM */
    out[n++] = 0x00, out[n++] = 0x0f, out[n++] = 0xac, out[n++] = (uint8_t)akm;
    uint16_t caps = akm == 8 ? 0x00c0 : 0x0000; /* WPA3: MFP required */
    out[n++] = (uint8_t)caps, out[n++] = (uint8_t)(caps >> 8);
    return n;
}

/* --- scanning --- */

struct found {
    uint8_t bssid[6];
    int channel;
    int signal;                 /* dBm, 0 if the card does not say */
    char security[8];
};

/* Is a better than b for joining? 5 GHz when it is heard well enough (-75
 * dBm or better; less crowded, faster), else the stronger signal. */
#define GOOD_5GHZ (-75)
static bool better(const struct found *a, const struct found *b)
{
    bool a5 = a->channel > 14, b5 = b->channel > 14;
    if (a5 != b5) {
        const struct found *f = a5 ? a : b;
        bool good = !f->signal || f->signal >= GOOD_5GHZ;
        return a5 ? good : !good;
    }
    return a->signal > b->signal;
}

/* The security a beacon advertises. Every length is the frame's to say:
 * an element is read only as far as it and the frame both go (49l). */
static void beacon_security(const uint8_t *ies, size_t n, char *out)
{
    snprintf(out, 8, "open");
    for (size_t i = 0; i + 2 <= n && i + 2 + ies[i + 1] <= n; i += 2 + ies[i + 1]) {
        size_t len = ies[i + 1];
        if (ies[i] == 48 && len >= 12) {
            const uint8_t *r = ies + i + 2;     /* version(2) group(4) count(2) pairwise... */
            size_t at = 8 + 4 * (size_t)get16le(r + 6);
            if (at + 2 > len)
                return;
            size_t akms = get16le(r + at);
            if (!akms || at + 2 + 4 * akms > len)
                return;
            const uint8_t *akm = r + at + 2;    /* (the first one) */
            if (akm[2] == 0xac)
                snprintf(out, 8, akm[3] == 8 ? "wpa3" : "wpa2");
            return;
        }
    }
}

/* The frame just received (in `frame`): if it is a beacon for ssid, fill out
 * and return true. channel_hint is where it was heard when wifid tuned there
 * itself; 0 takes the beacon's own channel (its DS parameter set; a 2.4 GHz
 * beacon is often heard on the neighbouring channel too), else the card's. */
static bool beacon_matches(const char *ssid, int channel_hint, struct found *out)
{
    struct sys_wlan_frame *h = (struct sys_wlan_frame *)frame;
    const uint8_t *f = frame_body();
    int sub = (f[0] >> 4) & 0xf;
    if (h->kind != WLAN_FRAME_MANAGEMENT || (sub != 8 && sub != 5))
        return false;                                  /* beacons and probe responses */
    if (h->length < 24 + 12)
        return false;
    const uint8_t *ies = f + 24 + 12;
    size_t ie_n = h->length - 24 - 12;
    char name[33] = "";
    int ds = 0;                                        /* the beacon's own channel */
    for (size_t j = 0; j + 2 <= ie_n && j + 2 + ies[j + 1] <= ie_n; j += 2 + ies[j + 1]) {
        if (ies[j] == 0 && ies[j + 1] <= 32) {
            memcpy(name, ies + j + 2, ies[j + 1]);
            name[ies[j + 1]] = 0;
        } else if (ies[j] == 3 && ies[j + 1] == 1) {
            ds = ies[j + 2];
        }
    }
    if (strcmp(name, ssid))
        return false;
    memcpy(out->bssid, f + 16, 6);
    out->channel = channel_hint ? channel_hint : ds ? ds : h->channel;
    out->signal = h->signal;
    beacon_security(ies, ie_n, out->security);
    return true;
}

/* Scan for the network named ssid; true and its details if found. A card
 * whose driver scans in its firmware (the MT7921) does the sweep itself
 * (WLAN_SCAN) and hands the beacons up through WLAN_RECEIVE; for the rest
 * wifid tunes each channel and listens. */
/* A probe request asking for ssid (to everyone; the card sends it on its
 * channel): a hidden network answers with its name. */
static void probe_for(const char *ssid)
{
    static const uint8_t everyone[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    uint8_t out[80];
    size_t n = management(out, 4, everyone), len = strlen(ssid);
    out[n++] = 0, out[n++] = (uint8_t)len;      /* SSID */
    memcpy(out + n, ssid, len);
    n += len;
    static const uint8_t rates[] = { 1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
    memcpy(out + n, rates, sizeof(rates));
    n += sizeof(rates);
    ritteros_wlan(WLAN_SEND, card, out, n, 0);
}

/* by_name: ask for the network by name (active: probe requests), for one the
 * passive scan did not find -- hidden networks answer only that. Never on
 * radar channels (52-144), where a station must not speak first. */
static bool scan_by_hand_at;                    /* the last scan tuned channel by channel */

static bool scan_for(const char *ssid, struct found *out, bool by_name)
{
    scan_by_hand_at = false;
    struct sys_wlan_info info;
    if (ritteros_wlan(WLAN_INFO, card, &info, sizeof(info), 0) < 0)
        return false;

    for (int i = 0; i < 256 && ritteros_wlan(WLAN_RECEIVE, card, frame, sizeof(frame), 0) >= 0; i++)
        ;                                              /* frames (an old scan's end) from before */
    if (ritteros_wlan(WLAN_SCAN, card, by_name ? (void *)ssid : 0,
                      by_name ? strlen(ssid) : 0, 0) >= 0) {
        /* The whole sweep, both bands: the network may be on each (often
         * the same name), and the best one is chosen (better). */
        bool have = false;
        struct found f;
        uint64_t end = ritteros_time() + 10000000000ULL;   /* the sweep ends sooner */
        while (ritteros_time() < end) {
            int64_t r = ritteros_wlan(WLAN_RECEIVE, card, frame, sizeof(frame), 200);
            if (r < (int64_t)sizeof(struct sys_wlan_frame))
                continue;
            if (((struct sys_wlan_frame *)frame)->kind == WLAN_FRAME_SCAN_DONE)
                break;
            if (beacon_matches(ssid, 0, &f) && (!have || better(&f, out))) {
                *out = f;
                have = true;
            }
        }
        return have;
    }

    scan_by_hand_at = true;
    bool have = false;                          /* every channel: the best is chosen */
    struct found f;                             /* (better; several access points of */
    for (int i = 0; i < info.channels; i++) {   /* one network: roaming) */
        int ch = info.channel_list[i];
        if (ritteros_wlan(WLAN_CHANNEL, card, 0, 0, (uint32_t)ch) < 0)
            continue;
        if (by_name && (ch < 52 || ch > 144))
            probe_for(ssid);
        uint64_t end = ritteros_time() + 250000000ULL;    /* 250 ms a channel */
        while (ritteros_time() < end) {
            int64_t r = ritteros_wlan(WLAN_RECEIVE, card, frame, sizeof(frame), 100);
            if (r < (int64_t)sizeof(struct sys_wlan_frame))
                continue;
            if (beacon_matches(ssid, info.channel_list[i], &f) && (!have || better(&f, out))) {
                *out = f;
                have = true;
            }
        }
    }
    return have;
}

/* --- authentication --- */

/* Open-system authentication: send seq 1, expect a success response. */
static bool auth_open(const uint8_t *bssid)
{
    uint8_t out[32];
    size_t n = management(out, 11, bssid);
    out[n++] = 0, out[n++] = 0;                 /* algorithm 0 (open) */
    out[n++] = 1, out[n++] = 0;                 /* sequence 1 */
    out[n++] = 0, out[n++] = 0;                 /* status */
    if (ritteros_wlan(WLAN_SEND, card, out, n, 0) < 0)
        return false;
    if (!reply(11, bssid, 6, 1500))
        return false;
    const uint8_t *b = frame_body() + 24;
    return get16le(b + 4) == 0;                 /* status 0 */
}

/* SAE authentication (WPA3): commit, then confirm; leaves the PMK in s. */
static bool auth_sae(const uint8_t *bssid, struct network *net, struct sae *s)
{
    uint8_t pt[64];
    sae_pt(pt, net->ssid, strlen(net->ssid), net->passphrase, strlen(net->passphrase), "", 0);
    if (!sae_pwe_from_pt(s, pt, our_mac, bssid) || !sae_commit(s, 0, 0))
        return false;
    uint8_t out[160];
    size_t n = management(out, 11, bssid);
    out[n++] = 3, out[n++] = 0;                 /* algorithm 3 (SAE) */
    out[n++] = 1, out[n++] = 0;                 /* sequence 1 (commit) */
    out[n++] = 0, out[n++] = 0;                 /* status */
    out[n++] = 19, out[n++] = 0;                /* group 19 */
    memcpy(out + n, s->scalar, 32), n += 32;
    memcpy(out + n, s->element, 64), n += 64;
    if (ritteros_wlan(WLAN_SEND, card, out, n, 0) < 0)
        return false;
    if (!reply(11, bssid, 8 + 32 + 64, 1500))
        return false;
    const uint8_t *b = frame_body() + 24;       /* alg(2) seq(2) status(2) group(2) scalar elem */
    if (get16le(b) != 3 || get16le(b + 2) != 1 || get16le(b + 4) != 0 || get16le(b + 6) != 19)
        return false;
    if (!sae_process(s, b + 8, b + 40))
        return false;
    n = management(out, 11, bssid);
    out[n++] = 3, out[n++] = 0;
    out[n++] = 2, out[n++] = 0;                 /* sequence 2 (confirm) */
    out[n++] = 0, out[n++] = 0;
    out[n++] = 0, out[n++] = 0;                 /* send-confirm */
    sae_confirm(s, 0, out + n);
    n += 32;
    if (ritteros_wlan(WLAN_SEND, card, out, n, 0) < 0)
        return false;
    if (!reply(11, bssid, 8 + 32, 1500))
        return false;
    b = frame_body() + 24;                      /* alg seq status send-confirm(2) confirm(32) */
    if (get16le(b) != 3 || get16le(b + 2) != 2 || get16le(b + 4) != 0)
        return false;
    return sae_check_confirm(s, get16le(b + 6), b + 8);
}

/* Association: send a request with our RSN IE, expect a success response.
 * Returns the AID the access point gave us, 0 if it refused. */
static uint16_t associate(const uint8_t *bssid, struct network *net, int channel)
{
    uint8_t out[256];
    size_t n = management(out, 0, bssid);
    out[n++] = 0x01, out[n++] = 0x00;           /* capability: ESS */
    out[n++] = 0x0a, out[n++] = 0x00;           /* listen interval */
    out[n++] = 0, out[n++] = (uint8_t)strlen(net->ssid);   /* SSID element */
    memcpy(out + n, net->ssid, strlen(net->ssid));
    n += strlen(net->ssid);
    static const uint8_t rates[] = {            /* 1, 2, 5.5, 11 (basic), 6, 9, 12, 18 Mbit/s */
        1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24,
        50, 4, 0x30, 0x48, 0x60, 0x6c,          /* extended: 24, 36, 48, 54 Mbit/s */
    };
    static const uint8_t rates5[] = {           /* 5 GHz: OFDM only, 6, 12, 24 basic */
        1, 8, 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c,
    };
    if (channel > 14) {
        memcpy(out + n, rates5, sizeof(rates5));
        n += sizeof(rates5);
    } else {
        memcpy(out + n, rates, sizeof(rates));
        n += sizeof(rates);
    }
    if (strcmp(net->security, "open"))
        n += rsn_ie(out + n, strcmp(net->security, "wpa3") == 0 ? 8 : 2);
    /* 802.11n: HT Capabilities (20 MHz, short guard interval, spatial
     * multiplexing power save off; MCS 0-15, two streams), then WMM, which
     * access points want before they use HT. No A-MPDU promises beyond the
     * smallest (aggregated frames would need reordering). The access point's
     * own HT element in its response goes to the card's firmware. */
    static const uint8_t ht[] = {
        45, 26, 0x2C, 0x00, 0x00,               /* capability info, A-MPDU parameters */
        0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0,     /* the MCS we receive: 0-15 */
        0, 0, 0x01, 0, 0, 0,                    /* highest rate; TX MCS set as RX */
        0, 0, 0, 0, 0, 0, 0,                    /* extended, beamforming, ASEL */
    };
    memcpy(out + n, ht, sizeof(ht));
    if (channel > 14)
        out[n + 2] |= 0x42;                     /* 5 GHz: 40 MHz, and its short guard */
    n += sizeof(ht);                            /* interval */
    if (channel > 14) {
        /* 802.11ac (5 GHz only): VHT Capabilities -- the short guard interval
         * at 80 MHz (no 160), two streams of MCS 0-9 each way. The access
         * point's answer sets the width; the card's firmware the rates. */
        static const uint8_t vht[] = {
            191, 12, 0x20, 0x00, 0x00, 0x00,    /* capability info */
            0xFA, 0xFF, 0x00, 0x00,             /* RX MCS map, highest rate */
            0xFA, 0xFF, 0x00, 0x00,             /* TX MCS map, highest rate */
        };
        memcpy(out + n, vht, sizeof(vht));
        n += sizeof(vht);
    }
    static const uint8_t wmm[] = {              /* WMM information (vendor: last) */
        0xDD, 7, 0x00, 0x50, 0xF2, 0x02, 0x00, 0x01, 0x00,
    };
    memcpy(out + n, wmm, sizeof(wmm));
    n += sizeof(wmm);
    if (ritteros_wlan(WLAN_SEND, card, out, n, 0) < 0)
        return 0;
    if (!reply(1, bssid, 6, 1500))
        return 0;
    const uint8_t *b = frame_body() + 24;       /* capability(2) status(2) AID(2) */
    if (get16le(b + 2) != 0)
        return 0;
    uint16_t aid = get16le(b + 4) & 0x3FFF;     /* (the top two bits are always set) */
    return aid ? aid : 1;
}

/* --- the 4-way handshake --- */

#define EAPOL_MAX 256

/* The MIC of an EAPOL frame (its MIC field already zeroed) under the KCK. */
static void eapol_mic(const uint8_t *kck, const uint8_t *eapol, size_t n, bool sha256,
                      uint8_t out[16])
{
    if (sha256) {
        struct aes a;
        aes_init(&a, kck, 16);
        aes_cmac(&a, eapol, n, out);
    } else {
        uint8_t full[SHA1_SIZE];
        hmac_sha1(kck, 16, eapol, n, full);
        memcpy(out, full, 16);
    }
}

/* Build an EAPOL-Key frame; its length. */
static size_t build_eapol(uint8_t *out, uint16_t key_info, uint64_t replay,
                          const uint8_t nonce[32], const uint8_t *key_data, size_t kd_n,
                          const uint8_t *kck, bool sha256)
{
    size_t n = 0;
    out[n++] = 2, out[n++] = 3;                 /* 802.1X version 2, type Key */
    size_t len_at = n;
    n += 2;                                     /* length, filled below */
    out[n++] = 2;                               /* descriptor type RSN */
    out[n++] = (uint8_t)(key_info >> 8), out[n++] = (uint8_t)key_info;
    out[n++] = 0, out[n++] = 16;                /* key length */
    for (int i = 0; i < 8; i++)
        out[n++] = (uint8_t)(replay >> (56 - 8 * i));
    memcpy(out + n, nonce ? nonce : (uint8_t[32]){0}, 32), n += 32;
    memset(out + n, 0, 16), n += 16;            /* EAPOL IV */
    memset(out + n, 0, 8), n += 8;              /* RSC */
    memset(out + n, 0, 8), n += 8;              /* reserved */
    size_t mic_at = n;
    memset(out + n, 0, 16), n += 16;            /* MIC (zeroed for the computation) */
    out[n++] = (uint8_t)(kd_n >> 8), out[n++] = (uint8_t)kd_n;
    if (kd_n)
        memcpy(out + n, key_data, kd_n), n += kd_n;
    out[len_at] = (uint8_t)((n - 4) >> 8), out[len_at + 1] = (uint8_t)(n - 4);
    if (kck && (key_info & (1 << 8)))
        eapol_mic(kck, out, n, sha256, out + mic_at);
    return n;
}

/* The handshake's keys, kept after the join: the access point renews the
 * group key now and then with a short handshake of its own, signed and
 * wrapped with these. */
static struct {
    bool on;
    uint8_t bssid[6], kck[16], kek[16];
    uint8_t pmk[32];                            /* for a handshake the AP restarts */
    bool sha256;
    uint64_t replay;                            /* the last replay counter taken */
} session;

/* Find the GTK KDE (type 00-0f-ac-01) in unwrapped key data (end bytes).
 * Other elements may come first (real access points put their RSN element
 * there), so step over anything else; 0xdd 0x00 starts the padding. */
static bool find_gtk(const uint8_t *kde, size_t end, uint8_t gtk[16], int *index)
{
    for (size_t i = 0; i + 2 <= end;) {
        uint8_t id = kde[i], len = kde[i + 1];
        if ((id == 0xdd && len == 0) || i + 2 + len > end)
            break;
        if (id == 0xdd && len >= 22 && kde[i + 2] == 0x00 && kde[i + 3] == 0x0f &&
            kde[i + 4] == 0xac && kde[i + 5] == 0x01) {
            *index = kde[i + 6] & 3;
            memcpy(gtk, kde + i + 8, 16);
            return true;
        }
        i += 2 + len;
    }
    return false;
}

/* The IGTK KDE (type 00-0f-ac-09: key ID, IPN, key; WPA3's BIP key for
 * group management frames) in unwrapped key data, as find_gtk; installed in
 * the kernel, which checks those frames. False if there is none. */
static bool install_igtk(const uint8_t *kde, size_t end)
{
    for (size_t i = 0; i + 2 <= end;) {
        uint8_t id = kde[i], len = kde[i + 1];
        if ((id == 0xdd && len == 0) || i + 2 + len > end)
            break;
        if (id == 0xdd && len >= 28 && kde[i + 2] == 0x00 && kde[i + 3] == 0x0f &&
            kde[i + 4] == 0xac && kde[i + 5] == 0x09) {
            struct sys_wlan_key key;
            memset(&key, 0, sizeof(key));
            key.kind = WLAN_KEY_IGTK;
            key.index = (uint8_t)(kde[i + 6] | kde[i + 7] << 8);
            key.length = 16;
            memcpy(key.rsc, kde + i + 8, 6);            /* the IPN to start from */
            memcpy(key.key, kde + i + 14, 16);
            return ritteros_wlan(WLAN_KEY, card, &key, sizeof(key), 0) >= 0;
        }
        i += 2 + len;
    }
    return false;
}

/* Say which step of the handshake stopped (wifid's log); false. */
/* Is m (got bytes) a whole EAPOL-Key frame? Its length from its header in
 * *total (what a MIC covers); its key data inside it. */
static bool key_frame(const uint8_t *m, size_t got, size_t *total)
{
    if (got < 99 || m[1] != 3)                  /* (3: EAPOL-Key) */
        return false;
    size_t t = 4 + (size_t)(m[2] << 8 | m[3]);
    if (t < 99 || t > got || t > EAPOL_MAX || 99 + (size_t)(m[97] << 8 | m[98]) > t)
        return false;
    *total = t;
    return true;
}

static uint64_t replay_counter(const uint8_t *m)
{
    uint64_t r = 0;
    for (int i = 0; i < 8; i++)
        r = r << 8 | m[9 + i];
    return r;
}

/* Does m's MIC check with the KCK? */
static bool mic_ok(const uint8_t *kck, const uint8_t *m, size_t total, bool sha256)
{
    uint8_t copy[EAPOL_MAX], mic[16];
    memcpy(copy, m, total);
    memset(copy + 81, 0, 16);
    eapol_mic(kck, copy, total, sha256, mic);
    return !memcmp(mic, m + 81, 16);
}

static bool handshake_fail(const char *why)
{
    printf("wifid: handshake: %s\n", why);
    return false;
}

/* The 4-way handshake (IEEE 802.11-2020 12.7.6). have_msg1: message 1 is
 * already in the frame buffer (the access point restarted the handshake
 * while joined); otherwise it is awaited. Gives the pairwise key, the group
 * key with its index and receive counter. */
static uint8_t pending_kde[256];                /* message 3's key data, for the IGTK */
static size_t pending_kde_n;

static bool four_way(const uint8_t *bssid, const uint8_t *pmk, bool sha256,
                     uint8_t tk[16], uint8_t gtk[16], int *gtk_index, uint8_t gtk_rsc[8],
                     bool have_msg1)
{
    /* msg1: ANonce. */
    size_t got = have_msg1 ? frame_length : receive(WLAN_FRAME_EAPOL, -1, 2000);
    if (!got)
        return handshake_fail("no message 1");
    const uint8_t *m1 = frame_body();
    size_t total;
    if (!key_frame(m1, got, &total))
        return handshake_fail("message 1 malformed");
    uint8_t anonce[32], snonce[32];
    memcpy(anonce, m1 + 17, 32);
    uint64_t replay = replay_counter(m1);
    if (ritteros_random(snonce, 32) != 32)
        return false;

    /* PTK = PRF(PMK, "Pairwise key expansion", min||max macs, min||max nonces). */
    uint8_t data[76], ptk[48];
    const uint8_t *a = our_mac, *b = bssid;
    const uint8_t *lo = memcmp(a, b, 6) < 0 ? a : b, *hi = lo == a ? b : a;
    memcpy(data, lo, 6);
    memcpy(data + 6, hi, 6);
    const uint8_t *nlo = memcmp(anonce, snonce, 32) < 0 ? anonce : snonce;
    const uint8_t *nhi = nlo == anonce ? snonce : anonce;
    memcpy(data + 12, nlo, 32);
    memcpy(data + 44, nhi, 32);
    if (sha256)
        ieee80211_kdf_sha256(pmk, 32, "Pairwise key expansion", data, 76, ptk, 48);
    else
        ieee80211_prf(pmk, 32, "Pairwise key expansion", data, 76, ptk, 48);
    const uint8_t *kck = ptk, *kek = ptk + 16;
    memcpy(tk, ptk + 32, 16);

    /* msg2: SNonce, our RSN IE, MIC. */
    uint8_t eapol[EAPOL_MAX], rsn[24];
    size_t rsn_n = rsn_ie(rsn, sha256 ? 8 : 2);
    uint16_t ki2 = (1 << 3) | (1 << 8) | (sha256 ? 0 : 2);   /* pairwise, MIC, version */
    size_t n = build_eapol(eapol, ki2, replay, snonce, rsn, rsn_n, kck, sha256);
    uint8_t out[6 + EAPOL_MAX];
    memcpy(out, bssid, 6);
    memcpy(out + 6, eapol, n);
    if (ritteros_wlan(WLAN_EAPOL, card, out, 6 + n, 0) < 0)
        return handshake_fail("message 2 not sent");

    /* msg3: ANonce again, a wrapped GTK, MIC. Nothing in it is used before
     * its MIC checks with our KCK (49k): an unprotected frame on the air can
     * be anyone's. Frames that fail are ignored while the wait lasts, so a
     * forged one cannot end the handshake either. */
    const uint16_t ki3 = (1 << 3) | (1 << 6) | (1 << 7) | (1 << 8) | (1 << 9) | (1 << 12);
    uint64_t end = ritteros_time() + 2000ull * 1000000;     /* (pairwise, install, ACK, MIC,
                                                             * secure, encrypted key data) */
    const uint8_t *m3 = 0;
    while (!m3) {
        uint64_t now = ritteros_time();
        if (now >= end)
            return handshake_fail("no message 3");
        got = receive(WLAN_FRAME_EAPOL, -1, (uint32_t)((end - now) / 1000000) + 1);
        const uint8_t *m = frame_body();
        if (!got || !key_frame(m, got, &total))
            continue;
        uint16_t ki = (uint16_t)(m[5] << 8 | m[6]);
        if ((ki & ki3) != ki3 || replay_counter(m) < replay || memcmp(m + 17, anonce, 32) ||
            !mic_ok(kck, m, total, sha256)) {
            printf("wifid: handshake: a message that is not message 3 ignored\n");
            continue;
        }
        m3 = m;
    }
    replay = replay_counter(m3);
    uint16_t kd_len = (uint16_t)(m3[97] << 8 | m3[98]);
    struct aes kekaes;
    aes_init(&kekaes, kek, 16);
    uint8_t kde[256];
    if (kd_len < 24 || kd_len > sizeof(kde) + 8 || !aes_key_unwrap(&kekaes, m3 + 99, kd_len, kde))
        return handshake_fail("message 3 key data not readable");
    if (!find_gtk(kde, (size_t)(kd_len - 8), gtk, gtk_index))
        return handshake_fail("no group key in message 3");
    memcpy(gtk_rsc, m3 + 65, 8);
    memcpy(pending_kde, kde, (size_t)(kd_len - 8));     /* (its IGTK, after the keys) */
    pending_kde_n = (size_t)(kd_len - 8);

    /* msg4: MIC, no key data. */
    uint16_t ki4 = (1 << 3) | (1 << 8) | (1 << 9) | (sha256 ? 0 : 2);   /* pairwise, MIC, secure */
    n = build_eapol(eapol, ki4, replay, 0, 0, 0, kck, sha256);
    memcpy(out, bssid, 6);
    memcpy(out + 6, eapol, n);
    if (ritteros_wlan(WLAN_EAPOL, card, out, 6 + n, 0) < 0)
        return handshake_fail("message 4 not sent");
    memcpy(session.bssid, bssid, 6);
    memcpy(session.kck, kck, 16);
    memcpy(session.kek, kek, 16);
    session.sha256 = sha256;
    session.replay = replay;
    session.on = true;
    return true;
}

/* Install the handshake's keys: the pairwise key, then the group key under
 * its index from its receive counter. */
static bool install_keys(const uint8_t tk[16], const uint8_t gtk[16], int gtk_index,
                         const uint8_t gtk_rsc[8])
{
    struct sys_wlan_key key;
    memset(&key, 0, sizeof(key));
    key.kind = WLAN_KEY_PAIRWISE;
    key.length = 16;
    memcpy(key.key, tk, 16);
    if (ritteros_wlan(WLAN_KEY, card, &key, sizeof(key), 0) < 0)
        return false;
    memset(&key, 0, sizeof(key));
    key.kind = WLAN_KEY_GROUP;
    key.index = (uint8_t)gtk_index;
    key.length = 16;
    memcpy(key.rsc, gtk_rsc, 8);
    memcpy(key.key, gtk, 16);
    if (ritteros_wlan(WLAN_KEY, card, &key, sizeof(key), 0) < 0)
        return false;
    install_igtk(pending_kde, pending_kde_n);    /* (WPA3 only: none in WPA2's) */
    return true;
}

/* While joined: wait up to ms for a key handshake message from the access
 * point and answer it. A group key handshake (IEEE 802.11-2020 12.7.7) brings
 * a new key for its broadcasts: checked (its MIC, a newer replay counter),
 * unwrapped, installed, answered with message 2; an access point that gets no
 * answer sends the station away. A new 4-way handshake (the access point
 * renews the pairwise key) runs in place from the kept PMK: messages 2 and 4
 * still travel under the old key, then both new keys go in, and the link
 * stays up. */
static void key_handshakes(uint32_t ms)
{
    size_t got = receive(WLAN_FRAME_EAPOL, -1, ms);
    size_t whole;
    if (!got || !session.on || !key_frame(frame_body(), got, &whole))
        return;
    frame_length = got;
    const uint8_t *m = frame_body();
    uint16_t ki = (uint16_t)(m[5] << 8 | m[6]);
    if (ki & (1 << 3)) {                        /* pairwise: the access point starts over */
        uint64_t r1 = 0;
        for (int i = 0; i < 8; i++)
            r1 = r1 << 8 | m[9 + i];
        if (!(ki & (1 << 7)) || (ki & (1 << 8)) || r1 <= session.replay)
            return;                             /* only a new message 1 (ACK, no MIC) */
        uint8_t tk[16], gtk[16], rsc[8];
        int index = 1;
        if (four_way(session.bssid, session.pmk, session.sha256, tk, gtk, &index, rsc, true) &&
            install_keys(tk, gtk, index, rsc)) {
            printf("wifid: pairwise key renewed\n");
            return;
        }
        /* The old keys stay (49k): a forged message 1 must not end the link.
         * An access point that really renewed them and got no answer sends
         * us away, and that rejoins. */
        printf("wifid: the renewed key handshake failed; the keys stay\n");
        return;
    }
    if (!(ki & (1 << 7)) || !(ki & (1 << 8)))  /* a request needs ACK and MIC */
        return;
    uint64_t replay = 0;
    for (int i = 0; i < 8; i++)
        replay = replay << 8 | m[9 + i];
    size_t total = 4 + (size_t)(m[2] << 8 | m[3]);
    if (replay <= session.replay || total > got || total > EAPOL_MAX || total < 99)
        return;                                 /* old (a replay) or malformed */
    uint8_t copy[EAPOL_MAX], mic[16];
    memcpy(copy, m, total);
    memset(copy + 81, 0, 16);
    eapol_mic(session.kck, copy, total, session.sha256, mic);
    if (memcmp(mic, m + 81, 16)) {
        printf("wifid: group key message with a wrong MIC ignored\n");
        return;
    }
    uint16_t kd_len = (uint16_t)(m[97] << 8 | m[98]);
    struct aes kekaes;
    aes_init(&kekaes, session.kek, 16);
    uint8_t kde[256], gtk[16];
    int index;
    if (99 + (size_t)kd_len > total || kd_len < 24 || kd_len > sizeof(kde) + 8 ||
        !aes_key_unwrap(&kekaes, m + 99, kd_len, kde) ||
        !find_gtk(kde, (size_t)(kd_len - 8), gtk, &index)) {
        printf("wifid: group key message without a readable key ignored\n");
        return;
    }
    struct sys_wlan_key key;
    memset(&key, 0, sizeof(key));
    key.kind = WLAN_KEY_GROUP;
    key.index = (uint8_t)index;
    key.length = 16;
    memcpy(key.rsc, m + 65, 8);                 /* its receive counter to start from */
    memcpy(key.key, gtk, 16);
    if (ritteros_wlan(WLAN_KEY, card, &key, sizeof(key), 0) < 0)
        return;
    install_igtk(kde, (size_t)(kd_len - 8));    /* (renewed with it under WPA3) */
    session.replay = replay;
    uint8_t eapol[EAPOL_MAX], out[6 + EAPOL_MAX];
    uint16_t ki2 = (1 << 8) | (1 << 9) | (session.sha256 ? 0 : 2);  /* group, MIC, secure */
    size_t n = build_eapol(eapol, ki2, replay, 0, 0, 0, session.kck, session.sha256);
    memcpy(out, session.bssid, 6);
    memcpy(out + 6, eapol, n);
    ritteros_wlan(WLAN_EAPOL, card, out, 6 + n, 0);
    printf("wifid: group key renewed (index %d)\n", index);
}

/* --- joining --- */

/* Join net: found, the access point to join (a roam), or 0 to scan for it. */
static bool join_at(struct network *net, const struct found *found)
{
    struct found f;
    if (found)
        f = *found;
    else if (!scan_for(net->ssid, &f, false) && !scan_for(net->ssid, &f, true))
        return false;                           /* (the second asks by name: hidden) */
    if (strcmp(net->security, "open") == 0 && strcmp(f.security, "open")) {
        if (strcmp(warned, net->ssid))
            printf("wifid: %s needs a passphrase (%s): wifi join %s PASSPHRASE%s\n",
                   net->ssid, f.security, net->ssid, strcmp(f.security, "wpa3") ? "" : " wpa3");
        snprintf(warned, sizeof(warned), "%s", net->ssid);
        return false;
    }
    printf("wifid: %s found on channel %d (%s GHz); joining\n", net->ssid, f.channel,
           f.channel > 14 ? "5" : "2.4");
    if (ritteros_wlan(WLAN_CHANNEL, card, 0, 0, (uint32_t)f.channel) < 0)
        return false;
    bool wpa3 = strcmp(net->security, "wpa3") == 0;
    bool wpa2 = strcmp(net->security, "wpa2") == 0;
    static struct sae sae;
    uint8_t pmk[32];
    if (wpa3) {
        if (!auth_sae(f.bssid, net, &sae))
            return false;
        memcpy(pmk, sae.pmk, 32);
    } else if (!auth_open(f.bssid)) {
        printf("wifid: %s: authentication failed\n", net->ssid);
        return false;
    }
    printf("wifid: %s: authenticated\n", net->ssid);
    /* Join (so the kernel accepts the access point's frames) before
     * associating: the first handshake message follows the association
     * response at once and would be dropped if we were not yet ready. */
    struct sys_wlan_join j;
    memset(&j, 0, sizeof(j));
    memcpy(j.bssid, f.bssid, 6);
    j.channel = (uint8_t)f.channel;
    j.aid = 0;                                  /* (not associated yet) */
    if (wpa2 || wpa3)
        j.flags = WLAN_JOIN_SECURED | (wpa3 ? WLAN_JOIN_MFP : 0);
    char ps[8] = "off";                         /* powersave = on|off (radio), off unless */
    config_get("/system/config/wifi/radio", "powersave", ps, sizeof(ps));   /* said */
    if (!strcmp(ps, "on"))
        j.flags |= WLAN_JOIN_POWERSAVE;
    if (ritteros_wlan(WLAN_JOIN, card, &j, sizeof(j), 0) < 0)
        return false;
    j.aid = associate(f.bssid, net, f.channel);
    if (!j.aid) {
        printf("wifid: %s: association failed\n", net->ssid);
        ritteros_wlan(WLAN_LEAVE, card, 0, 0, 0);
        return false;
    }
    /* Again with the AID: the card's firmware takes over what it can. */
    if (ritteros_wlan(WLAN_JOIN, card, &j, sizeof(j), 0) < 0) {
        printf("wifid: %s: the card did not take the association\n", net->ssid);
        ritteros_wlan(WLAN_LEAVE, card, 0, 0, 0);
        return false;
    }
    printf("wifid: %s: associated (AID %u)\n", net->ssid, j.aid);
    if (wpa2 || wpa3) {
        if (wpa2)
            pbkdf2_sha1(net->passphrase, strlen(net->passphrase), (const uint8_t *)net->ssid,
                        strlen(net->ssid), 4096, pmk, 32);
        uint8_t tk[16], gtk[16], gtk_rsc[8];
        int gtk_index = 1;
        if (!four_way(f.bssid, pmk, wpa3, tk, gtk, &gtk_index, gtk_rsc, false)) {
            printf("wifid: %s: handshake failed\n", net->ssid);
            ritteros_wlan(WLAN_LEAVE, card, 0, 0, 0);
            return false;
        }
        printf("wifid: %s: handshake done\n", net->ssid);
        if (!install_keys(tk, gtk, gtk_index, gtk_rsc))
            return false;
        memcpy(session.pmk, pmk, 32);
    }
    printf("wifid: joined %s (%02x:%02x:%02x:%02x:%02x:%02x) on channel %d, %s\n", net->ssid,
           f.bssid[0], f.bssid[1], f.bssid[2], f.bssid[3], f.bssid[4], f.bssid[5], f.channel,
           net->security);
    return true;
}

static bool join(struct network *net)
{
    return join_at(net, 0);
}

/* Roaming (while joined, every ROAM_EVERY): when our access point's signal
 * is weaker than ROAM_WEAK, look for the network elsewhere; an access point
 * of it ROAM_BETTER dB stronger is joined instead (after a polite goodbye to
 * the old one). True if it roamed (or tried: the loop then sees the result). */
#define ROAM_EVERY 10                           /* seconds */
#define ROAM_WEAK (-75)                         /* dBm */
#define ROAM_BETTER 10                          /* dB */
static bool roam(struct network *net, const struct sys_wlan_info *info)
{
    int64_t weak = ritteros_wlan(WLAN_SIGNAL, card, 0, 0, 0);
    if (weak <= 0 || -weak >= ROAM_WEAK)
        return false;                           /* unknown, or good enough */
    struct found f;
    bool seen = scan_for(net->ssid, &f, false);
    if (scan_by_hand_at)                        /* (a firmware scan comes back by itself) */
        ritteros_wlan(WLAN_CHANNEL, card, 0, 0, info->channel);   /* back home */
    if (!seen || !memcmp(f.bssid, info->bssid, 6) || !f.signal ||
        f.signal < -weak + ROAM_BETTER || strcmp(f.security, net->security))
        return false;
    printf("wifid: %s: roaming to %02x:%02x:%02x:%02x:%02x:%02x on channel %d (%d dBm;"
           " the old one %d dBm)\n", net->ssid, f.bssid[0], f.bssid[1], f.bssid[2],
           f.bssid[3], f.bssid[4], f.bssid[5], f.channel, f.signal, (int)-weak);
    uint8_t bye[32];
    size_t n = management(bye, 12, info->bssid);
    bye[n++] = 3, bye[n++] = 0;                 /* deauthentication: leaving */
    ritteros_wlan(WLAN_SEND, card, bye, n, 0);
    session.on = false;
    ritteros_wlan(WLAN_LEAVE, card, 0, 0, 0);
    join_at(net, &f);
    return true;
}

int main(void)
{
    struct sys_wlan_info info;
    if (ritteros_wlan(WLAN_INFO, "wlan0", &info, sizeof(info), 0) < 0) {
        dprintf(STDERR_FILENO, "wifid: no wireless card\n");
        return 1;
    }
    memcpy(our_mac, info.mac, 6);

    /* Tell the card the country, so its firmware chooses the channels and
     * power it may use. The setting is /system/config/wifi/radio (country =
     * US); without one, "00" is the world-wide safe default. A card with no
     * such setting (the test card) answers ERR_NOTSUP, which is fine. */
    settings_make(SETTINGS_WIFI_RADIO, SETTINGS_WIFI_RADIO_TEXT);   /* (37e-1b) when missing */
    char country[4] = "00";
    config_get("/system/config/wifi/radio", "country", country, sizeof(country));
    ritteros_wlan(WLAN_COUNTRY, card, country, 2, 0);

    char joined[33] = "";                       /* the network we joined, if any */
    unsigned ticks = 0;                         /* rounds while joined (2 s each) */
    for (;;) {
        if (ritteros_wlan(WLAN_INFO, card, &info, sizeof(info), 0) == 0 && !info.joined) {
            if (joined[0]) {                    /* the kernel says why on the console */
                printf("wifid: %s: link lost; joining again\n", joined);
                joined[0] = 0;
                session.on = false;
            }
            load_networks();
            for (int i = 0; i < nsaved; i++)
                if (saved[i].automatic && join(&saved[i])) {
                    snprintf(joined, sizeof(joined), "%s", saved[i].ssid);
                    break;
                }
        }
        if (joined[0]) {
            key_handshakes(2000);               /* (reading also empties the queue) */
            if (++ticks % (ROAM_EVERY / 2) == 0)
                for (int i = 0; i < nsaved; i++)
                    if (!strcmp(saved[i].ssid, joined) &&
                        ritteros_wlan(WLAN_INFO, card, &info, sizeof(info), 0) == 0 &&
                        info.joined)
                        roam(&saved[i], &info);
        } else
            usleep(2000000);
    }
    return 0;
}

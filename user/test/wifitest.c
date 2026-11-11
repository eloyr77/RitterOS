/*
 * Test: wifitest CARD scans for access points (step 24d) through SYS_WLAN:
 * it sets the card's channel, collects beacons for a second, and prints each
 * network once -- its BSSID, SSID, security (open/wpa2/wpa3 from the RSN
 * information element's key-management suite) and channel -- sorted, so the
 * test can compare. It also sends a probe request and counts the responses.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

static uint8_t frame[sizeof(struct sys_wlan_frame) + WLAN_FRAME_MAX];

struct seen {
    uint8_t bssid[6];
    char ssid[33];
    const char *security;
    int channel;
};

static struct seen networks[16];
static int count;

/* The security from a beacon's or probe response's tagged elements. */
static const char *security_of(const uint8_t *ies, size_t n)
{
    size_t i = 0;
    while (i + 2 <= n) {
        uint8_t id = ies[i], len = ies[i + 1];
        if (i + 2 + len > n)
            break;
        if (id == 48 && len >= 12) {                        /* RSN: version(2) group(4) */
            const uint8_t *r = ies + i + 2, *end = r + len;
            uint16_t pairwise = (uint16_t)(r[6] | r[7] << 8); /* the pairwise-cipher count */
            const uint8_t *akm = r + 8 + 4 * pairwise + 2;    /* past the AKM count */
            if (akm + 4 <= end && akm[0] == 0 && akm[1] == 0x0f && akm[2] == 0xac)
                return akm[3] == 8 ? "wpa3" : "wpa2";         /* 8 SAE (WPA3), 2 PSK (WPA2) */
            return "wpa2";
        }
        i += 2 + len;
    }
    return "open";
}

static void ssid_of(const uint8_t *ies, size_t n, char *out)
{
    size_t i = 0;
    out[0] = 0;
    while (i + 2 <= n) {
        uint8_t id = ies[i], len = ies[i + 1];
        if (i + 2 + len > n)
            break;
        if (id == 0 && len <= 32) {
            memcpy(out, ies + i + 2, len);
            out[len] = 0;
            return;
        }
        i += 2 + len;
    }
}

static void record(const uint8_t *f, size_t n)
{
    if (n < 24 + 12)
        return;
    const uint8_t *bssid = f + 16;
    for (int i = 0; i < count; i++)
        if (!memcmp(networks[i].bssid, bssid, 6))
            return;
    if (count == 16)
        return;
    const uint8_t *ies = f + 24 + 12;           /* after timestamp, interval, capability */
    size_t ie_n = n - 24 - 12;
    struct seen *s = &networks[count++];
    memcpy(s->bssid, bssid, 6);
    ssid_of(ies, ie_n, s->ssid);
    s->security = security_of(ies, ie_n);
    s->channel = 0;
    for (size_t i = 0; i + 2 <= ie_n; i += 2 + ies[i + 1])
        if (ies[i] == 3 && ies[i + 1] >= 1)
            s->channel = ies[i + 2];
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    const char *card = argv[1];
    if (ritteros_wlan(WLAN_CHANNEL, card, 0, 0, 6) < 0) {
        printf("wifitest: set channel: %s\n", strerror(errno));
        return 1;
    }
    /* A probe request (broadcast), then collect beacons and responses. */
    uint8_t probe[24 + 2] = {0};
    probe[0] = 0x40;                            /* management, probe request */
    memset(probe + 4, 0xff, 6);                 /* to broadcast */
    memcpy(probe + 10, "\x02\x00\x00\x00\x00\x09", 6);
    memset(probe + 16, 0xff, 6);
    probe[24] = 0, probe[25] = 0;               /* an empty SSID element */
    ritteros_wlan(WLAN_SEND, card, probe, sizeof(probe), 0);

    uint64_t end = ritteros_time() + 1000000000ULL;
    int responses = 0;
    while (ritteros_time() < end) {
        int64_t r = ritteros_wlan(WLAN_RECEIVE, card, frame, sizeof(frame), 200);
        if (r < (int64_t)sizeof(struct sys_wlan_frame))
            continue;
        struct sys_wlan_frame *h = (struct sys_wlan_frame *)frame;
        if (h->kind != WLAN_FRAME_MANAGEMENT)
            continue;
        const uint8_t *f = frame + sizeof(*h);
        int subtype = (f[0] >> 4) & 0xf;
        if (subtype == 8 || subtype == 5) {     /* beacon or probe response */
            record(f, h->length);
            if (subtype == 5)
                responses++;
        }
    }
    /* Sort by BSSID so the output is the same every run. */
    for (int i = 0; i < count; i++)
        for (int j = i + 1; j < count; j++)
            if (memcmp(networks[j].bssid, networks[i].bssid, 6) < 0) {
                struct seen t = networks[i];
                networks[i] = networks[j];
                networks[j] = t;
            }
    for (int i = 0; i < count; i++) {
        struct seen *s = &networks[i];
        printf("wifitest: %02x:%02x:%02x:%02x:%02x:%02x %s %s channel %d\n", s->bssid[0],
               s->bssid[1], s->bssid[2], s->bssid[3], s->bssid[4], s->bssid[5], s->ssid,
               s->security, s->channel);
    }
    printf("wifitest: %d networks, %s to probe\n", count, responses ? "answered" : "no answer");
    return 0;
}

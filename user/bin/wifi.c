/*
 * wifi: manage the wireless link (step 24e). The elf wifid does the joining;
 * this command saves and lists networks and shows the link's state.
 *
 *   wifi scan                       list the networks in range
 *   wifi join SSID [PASSPHRASE [SEC]]  save a network and start wifid
 *                                   (no passphrase: open, or as saved if
 *                                   it is; SEC: wpa2 or wpa3)
 *   wifi forget SSID                remove a saved network
 *   wifi status                     the link and its address
 *
 * Saved networks live in /system/config/wifi/networks/ (root only), one
 * "key = value" file each (ssid, passphrase, security, automatic).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"
#include "../../common/settings_defaults.h"

PROGRAM_HELP("wifi scan | join SSID [PASSPHRASE [open|wpa2|wpa3]] | forget SSID | country CC | status",
             "the wireless link (saves networks; wifid joins them)");

static char card[8] = "wlan0";
static uint8_t frame[sizeof(struct sys_wlan_frame) + WLAN_FRAME_MAX];

static uint16_t get16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static int no_card(void)
{
    struct sys_wlan_info info;
    if (ritteros_wlan(WLAN_INFO, card, &info, sizeof(info), 0) < 0) {
        dprintf(STDERR_FILENO, "wifi: no wireless card\n");
        return 1;
    }
    return 0;
}

static const char *security_of(const uint8_t *ies, size_t n)
{
    for (size_t i = 0; i + 2 <= n; i += 2 + ies[i + 1])
        if (ies[i] == 48 && ies[i + 1] >= 12) {
            const uint8_t *r = ies + i + 2;
            uint16_t pairwise = get16le(r + 6);
            const uint8_t *akm = r + 8 + 4 * pairwise + 2;
            if (akm + 4 <= ies + i + 2 + ies[i + 1] && akm[2] == 0xac)
                return akm[3] == 8 ? "wpa3" : "wpa2";
            return "wpa2";
        }
    return "open";
}

/* The networks a scan heard: the strongest copy of each name. */
#define SEEN_MAX 64
static struct seen {
    char name[33];
    const char *security;
    int channel, signal;                /* signal: dBm, 0 if the card does not say */
} seen[SEEN_MAX];
static int nseen;

/* Note the network the beacon in `frame` names. channel_hint is where it was
 * heard when wifi tuned there itself; 0 takes the beacon's own channel (its DS
 * parameter set), else the channel the card reports. */
static void note_beacon(int channel_hint)
{
    struct sys_wlan_frame *h = (struct sys_wlan_frame *)frame;
    const uint8_t *f = frame + sizeof(*h);
    if (h->kind != WLAN_FRAME_MANAGEMENT || ((f[0] >> 4) & 0xf) != 8 || h->length < 24 + 12)
        return;
    const uint8_t *ies = f + 24 + 12;
    size_t ie_n = h->length - 24 - 12;
    char name[33] = "";
    int channel = channel_hint;
    for (size_t j = 0; j + 2 <= ie_n && j + 2 + ies[j + 1] <= ie_n; j += 2 + ies[j + 1]) {
        if (ies[j] == 0 && ies[j + 1] <= 32) {
            memcpy(name, ies + j + 2, ies[j + 1]);
            name[ies[j + 1]] = 0;
        } else if (ies[j] == 3 && ies[j + 1] == 1 && !channel_hint) {
            channel = ies[j + 2];
        }
    }
    if (!name[0])
        return;                         /* hidden networks have no name to show */
    if (!channel)
        channel = h->channel;
    struct seen *e = 0;
    for (int k = 0; k < nseen; k++)
        if (!strcmp(seen[k].name, name))
            e = &seen[k];
    if (e && (!h->signal || (e->signal && e->signal >= h->signal)))
        return;                         /* the copy we have is as strong */
    if (!e) {
        if (nseen == SEEN_MAX)
            return;
        e = &seen[nseen++];
    }
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->security = security_of(ies, ie_n);
    e->channel = channel;
    e->signal = h->signal;
}

/* A card whose driver scans in its firmware (the MT7921) sweeps the channels
 * itself (WLAN_SCAN) and says when it is done; for the rest wifi tunes each
 * channel and listens. The list is sorted, strongest first. */
static int scan(void)
{
    struct sys_wlan_info info;
    if (ritteros_wlan(WLAN_INFO, card, &info, sizeof(info), 0) < 0)
        return no_card();
    if (info.joined) {
        printf("wifi: joined; not scanning (wifi status shows the link)\n");
        return 0;
    }
    for (int i = 0; i < 256 && ritteros_wlan(WLAN_RECEIVE, card, frame, sizeof(frame), 0) >= 0; i++)
        ;                               /* frames from before (an old scan's end too) */
    if (ritteros_wlan(WLAN_SCAN, card, 0, 0, 0) >= 0) {
        uint64_t end = ritteros_time() + 10000000000ULL;
        while (ritteros_time() < end) {
            int64_t r = ritteros_wlan(WLAN_RECEIVE, card, frame, sizeof(frame), 200);
            if (r < (int64_t)sizeof(struct sys_wlan_frame))
                continue;
            if (((struct sys_wlan_frame *)frame)->kind == WLAN_FRAME_SCAN_DONE)
                break;
            note_beacon(0);
        }
    } else {
        for (int i = 0; i < info.channels; i++) {
            if (ritteros_wlan(WLAN_CHANNEL, card, 0, 0, info.channel_list[i]) < 0)
                continue;
            uint64_t end = ritteros_time() + 200000000ULL;
            while (ritteros_time() < end) {
                if (ritteros_wlan(WLAN_RECEIVE, card, frame, sizeof(frame), 100) >=
                    (int64_t)sizeof(struct sys_wlan_frame))
                    note_beacon(info.channel_list[i]);
            }
        }
    }
    for (int i = 1; i < nseen; i++)     /* strongest first; unknown signals last */
        for (int k = i; k > 0; k--) {
            int a = seen[k - 1].signal, b = seen[k].signal;
            if (!(b && (!a || b > a)))
                break;
            struct seen t = seen[k - 1];
            seen[k - 1] = seen[k];
            seen[k] = t;
        }
    for (int i = 0; i < nseen; i++) {
        if (seen[i].signal)
            printf("wifi: %-32s %-4s channel %-3d %d dBm\n", seen[i].name, seen[i].security,
                   seen[i].channel, seen[i].signal);
        else
            printf("wifi: %-32s %s channel %d\n", seen[i].name, seen[i].security,
                   seen[i].channel);
    }
    printf("wifi: %d network(s)\n", nseen);
    return 0;
}

/* Set the country the card's firmware applies (its channels and power). */
static int country(int argc, char **argv)
{
    if (argc < 3 || strlen(argv[2]) != 2) {
        dprintf(STDERR_FILENO, "usage: wifi country CC (a two-letter code, e.g. US)\n");
        return 2;
    }
    char cc[3] = { 0 };
    for (int i = 0; i < 2; i++) {
        char ch = argv[2][i];
        cc[i] = ch >= 'a' && ch <= 'z' ? (char)(ch - 'a' + 'A') : ch;
    }
    struct stat st;
    if (stat(WIFI_DIR, &st) < 0 && mkdir(WIFI_DIR, 0755) < 0) {
        dprintf(STDERR_FILENO, "wifi: %s: %s\n", WIFI_DIR, strerror(errno));
        return 1;
    }
    /* The radio's file keeps its other lines (powersave = on, comments):
     * only the country line is replaced, or added. (37e-1b) A missing file
     * starts as its default text. */
    static char old[4096], content[4096 + 32];
    ssize_t got = 0;
    int fd = open(WIFI_RADIO, O_RDONLY);
    if (fd >= 0) {
        got = read(fd, old, sizeof(old) - 1);
        close(fd);
    } else {
        got = snprintf(old, sizeof(old), "%s", SETTINGS_WIFI_RADIO_TEXT);
    }
    old[got > 0 ? got : 0] = 0;
    size_t n = 0;
    bool replaced = false;
    for (char *line = old; *line;) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) + 1 : strlen(line);
        const char *k = line + strspn(line, " \t");
        bool is_country = !strncmp(k, "country", 7) && strchr(" \t=", k[7]) && k[7];
        if (is_country && !replaced)
            n += (size_t)snprintf(content + n, sizeof(content) - n, "country = %s\n", cc);
        else if (!is_country) {
            memcpy(content + n, line, len);
            n += len;
            if (!end)
                content[n++] = '\n';
        }
        replaced |= is_country;
        line += len;
    }
    if (!replaced)
        n += (size_t)snprintf(content + n, sizeof(content) - n, "country = %s\n", cc);
    fd = open(WIFI_RADIO, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, content, n) != (ssize_t)n || close(fd) < 0) {
        dprintf(STDERR_FILENO, "wifi: %s: %s\n", WIFI_RADIO, strerror(errno));
        return 1;
    }
    /* Apply it now if the card is up; wifid also sets it at startup. */
    if (ritteros_wlan(WLAN_COUNTRY, "wlan0", cc, 2, 0) == 0)
        printf("wifi: country %s (applied)\n", cc);
    else
        printf("wifi: country %s (saved; takes effect when wifid restarts)\n", cc);
    return 0;
}

static int join(int argc, char **argv)
{
    if (argc < 3) {
        dprintf(STDERR_FILENO, "usage: wifi join SSID [PASSPHRASE [open|wpa2|wpa3]]\n");
        return 2;
    }
    const char *ssid = argv[2];
    const char *passphrase = argc > 3 ? argv[3] : "";
    const char *security = argc > 4 ? argv[4] : (passphrase[0] ? "wpa2" : "open");
    if (strcmp(security, "open") && strcmp(security, "wpa2") && strcmp(security, "wpa3")) {
        dprintf(STDERR_FILENO, "wifi: security must be open, wpa2 or wpa3\n");
        return 2;
    }
    struct stat st;
    if ((stat(WIFI_DIR, &st) < 0 && mkdir(WIFI_DIR, 0755) < 0) ||
        (stat(WIFI_NETWORKS, &st) < 0 && mkdir(WIFI_NETWORKS, 0700) < 0)) {
        dprintf(STDERR_FILENO, "wifi: %s: %s\n", WIFI_NETWORKS, strerror(errno));
        return 1;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", WIFI_NETWORKS, ssid);
    /* (36f-6) A saved network named without a passphrase keeps its
     * settings: before, it was saved again as open and its passphrase
     * lost (forget it first to make it open). */
    char saved_security[8] = "";
    bool keep = argc == 3 && stat(path, &st) == 0 &&
                config_get(path, "security", saved_security, sizeof(saved_security)) > 0;
    char content[256];
    int n = snprintf(content, sizeof(content),
                     "ssid = %s\nsecurity = %s\nautomatic = yes\n", ssid, security);
    if (passphrase[0])
        n += snprintf(content + n, sizeof(content) - n, "passphrase = %s\n", passphrase);
    int fd = keep ? -1 : open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!keep && (fd < 0 || write(fd, content, (size_t)n) != n || close(fd) < 0)) {
        dprintf(STDERR_FILENO, "wifi: %s: %s\n", path, strerror(errno));
        return 1;
    }
    /* Make wifid's elf file and start elvesd (which runs it). */
    char elf[128];
    snprintf(elf, sizeof(elf), "%s/wifid", ELVES_DIR);
    if (stat(elf, &st) < 0) {
        if (stat(ELVES_DIR, &st) < 0)
            mkdir(ELVES_DIR, 0755);
        int e = open(elf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (e >= 0) {
            const char *text = "# the wireless link's elf (wifi made this)\n"
                               "command = wifid\nstart = on\n";
            if (write(e, text, strlen(text)) < 0) { /* reported below if it matters */ }
            close(e);
        }
    }
    const char *slash = strrchr(argv[0], '/');
    char bin[160];
    if (slash)
        snprintf(bin, sizeof(bin), "%.*s", (int)(slash - argv[0]), argv[0]);
    else
        snprintf(bin, sizeof(bin), "/system/%s/bin",
#if defined(__x86_64__)
                 "x86_64");
#else
                 "aarch64");
#endif
    elvesd_start(bin);
    if (keep)
        printf("wifi: %s is saved (%s); wifid joins it with its saved settings\n", ssid,
               saved_security);
    else
        printf("wifi: saved %s (%s); wifid will join it\n", ssid, security);
    return 0;
}

static int forget(int argc, char **argv)
{
    if (argc < 3) {
        dprintf(STDERR_FILENO, "usage: wifi forget SSID\n");
        return 2;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", WIFI_NETWORKS, argv[2]);
    if (unlink(path) < 0) {
        dprintf(STDERR_FILENO, "wifi: %s: %s\n", argv[2], strerror(errno));
        return 1;
    }
    printf("wifi: forgot %s\n", argv[2]);
    return 0;
}

static int status(void)
{
    struct sys_wlan_info info;
    if (ritteros_wlan(WLAN_INFO, card, &info, sizeof(info), 0) < 0)
        return no_card();
    if (!info.joined) {
        printf("wifi: not joined\n");
        return 0;
    }
    int64_t weak = ritteros_wlan(WLAN_SIGNAL, card, 0, 0, 0);    /* -dBm, 0: not known */
    char signal[24] = "";
    if (weak > 0)
        snprintf(signal, sizeof(signal), ", %d dBm", (int)-weak);
    printf("wifi: joined %02x:%02x:%02x:%02x:%02x:%02x on channel %d%s\n", info.bssid[0],
           info.bssid[1], info.bssid[2], info.bssid[3], info.bssid[4], info.bssid[5],
           info.channel, signal);
    struct sys_netinfo net;
    snprintf(net.name, sizeof(net.name), "%s", card);
    if (ritteros_netinfo(&net) == 0 && net.address) {
        char a[16];
        ip_format(net.address, a, sizeof(a));
        printf("wifi: address %s\n", a);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: wifi scan | join SSID ... | forget SSID | country CC | status\n");
        return 2;
    }
    if (!strcmp(argv[1], "scan"))
        return scan();
    if (!strcmp(argv[1], "join"))
        return join(argc, argv);
    if (!strcmp(argv[1], "forget"))
        return forget(argc, argv);
    if (!strcmp(argv[1], "status"))
        return status();
    if (!strcmp(argv[1], "country"))
        return country(argc, argv);
    dprintf(STDERR_FILENO, "wifi: unknown command %s\n", argv[1]);
    return 2;
}

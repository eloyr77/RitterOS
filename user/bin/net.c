/*
 * net [up | forward [on|off] | masquerade [ethN ... | off]]: the loopback
 * lo and the network cards (eth0, eth1, ...): the MAC address, the link and
 * its speed, the card and where it sits, the IPv4 settings, what was
 * received and sent; the routes, forwarding, masquerade, the neighbours
 * known by ARP, TCP. The kernel's report, as its rescue console shows it.
 *
 * net up: apply the settings in /system/config/network/ to the cards (root
 * only; login does it at the start). One file per card, named after it
 * (eth0, eth1, ...); a card found without one gets it, set to DHCP:
 *   address = dhcp           ask the network (DHCP) for all of them, or
 *   address = 10.0.2.15/24   the address and the network's prefix length
 *   gateway = 10.0.2.2       where packets for other networks go (the
 *                            default route: the first card with one)
 *   dns = 10.0.2.3           who answers names (with DHCP: instead of the
 *                            network's; ADDRESS:PORT for another port)
 * and /system/config/routing:
 *   forward = on             pass packets on between the cards (else off)
 *   masquerade = eth2        the private card(s), space-separated: source
 *                            NAT when their traffic is forwarded out a card
 *                            that is not listed (IPv4 only)
 *   default = wlan0 eth1     (50g) the cards whose gateway carries the
 *                            default route, in order: the first with a
 *                            link, an address and a gateway; when none
 *                            can, the first other card with a gateway
 * A disk from before keeps them all in one file /system/config/network
 * (eth0's keys plain, the others' as eth1.KEY): net up moves them.
 *
 * net forward [on|off]: show or switch forwarding (switching: root only).
 * net masquerade [ethN ... | off]: show or set the private cards (root).
 * net default [CARD... | auto]: show or set the default route's cards (root;
 * at once, as masquerade: the file's line is what net up applies).
 * net names: the cards RitterOS has found (/system/config/network/names,
 * 50f): name, MAC, here or not, what it is. net forget NAME (root): a card
 * that is not plugged in leaves the list and its settings file goes: the
 * name is free for the next new card.
 *
 * (38a) net tap add [user NAME] (root): a new virtual card tapN (the lowest
 * number free), whose /dev/tapN the user (else root) opens to be its wire;
 * net tap remove TAP takes it away (not while a program has it open). A
 * tap is a card as any other: net up makes its settings file (DHCP) when
 * it has none (38b-3; 38a made none).
 *
 * (38b) net bridge add PORT... (root): a new bridge brN with these cards as
 * its ports (wired cards and taps; brN takes the first one's MAC address);
 * net bridge add brN PORT... adds ports; net bridge remove brN [PORT...]
 * takes ports out, or the whole bridge. The bridge is a card: its settings
 * file (address = dhcp when made by net up) gives it the address; a port's
 * own settings are not used while it is one.
 *
 * (38c) /system/config/network/virtual keeps them for the next start
 * (tapN = user NAME, brN = PORT...): net up makes what it says before the
 * cards' own settings (taps, then bridges), and the commands above change
 * only their own lines. net up makes it with its default text when missing.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <pwd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "netnames.h"
#include "ritteros.h"
#include "settings_file.h"

PROGRAM_HELP("net [up | names | forget NAME | forward [on|off] | masquerade [CARD... | off] | default [CARD... | auto] | tap add [user NAME] | tap remove TAP | bridge add [BRIDGE] PORT... | bridge remove BRIDGE [PORT...]]",
             "the network cards; up: apply /system/config/network/ (root); names, forwarding, NAT, the default route, virtual cards, bridges");

#define MAX_CARDS 4                         /* (old settings files: eth0-eth3) */
#define ROUTING_CONFIG CONFIG_DIR "/routing"
#define DEFAULT_SETTINGS NETNAMES_SETTINGS     /* (shared with netnamesd, 50h) */

static char text[65536];                    /* (38a: room for 64 cards) */
static bool uses_dhcp;                      /* a card is set to dhcp (up) */
static bool any_card;                       /* a card was set up (up) */
static char bin_dir[160];                   /* where net and elvesd live */

static int fail(const char *what, const char *why)
{
    dprintf(STDERR_FILENO, "net: %s: %s\n", what, why);
    return 1;
}

static void card_file(const char *card, char *path, size_t size)
{
    snprintf(path, size, "%s/%s", NETWORK_CONFIG, card);
}

/* Write a whole file (made or replaced); false with errno set. */
static bool write_file(const char *path, const char *content)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    size_t length = strlen(content);
    bool ok = write(fd, content, length) == (ssize_t)length;
    return close(fd) == 0 && ok;
}

/* Is there a card of this name? */
static bool card_exists(const char *card)
{
    struct sys_netinfo info;
    memset(&info, 0, sizeof(info));
    snprintf(info.name, sizeof(info.name), "%s", card);
    return ritteros_netinfo(&info) == 0;
}

/* A disk from before: one file /system/config/network for every card
 * (eth0's keys plain or as eth0.KEY, the others' as eth1.KEY) and forward.
 * Its settings go into a file per card and /system/config/routing. */
static int move_old_settings(void)
{
    const char *old = CONFIG_DIR "/network.old";
    if (rename(NETWORK_CONFIG, old) < 0 || mkdir(NETWORK_CONFIG, 0755) < 0)
        return fail(NETWORK_CONFIG, strerror(errno));
    static const char *keys[] = { "address", "gateway", "dns" };
    for (int i = 0; i < MAX_CARDS; i++) {
        char card[8], content[512] = "", value[64], name[32];
        snprintf(card, sizeof(card), "eth%d", i);
        for (int k = 0; k < 3; k++) {
            snprintf(name, sizeof(name), "%s.%s", card, keys[k]);
            int r = config_get(old, name, value, sizeof(value));
            if (r == 0 && i == 0)
                r = config_get(old, keys[k], value, sizeof(value));
            if (r > 0)
                snprintf(content + strlen(content), sizeof(content) - strlen(content),
                         "%s = %s\n", keys[k], value);
        }
        char path[64];
        card_file(card, path, sizeof(path));
        if (content[0] && !write_file(path, content))
            return fail(path, strerror(errno));
    }
    char value[16], line[32];
    if (config_get(old, "forward", value, sizeof(value)) > 0) {
        snprintf(line, sizeof(line), "forward = %s\n", value);
        if (!write_file(ROUTING_CONFIG, line))
            return fail(ROUTING_CONFIG, strerror(errno));
    }
    unlink(old);
    printf("net: %s: moved into one file per card in %s/ (forward: %s)\n", NETWORK_CONFIG,
           NETWORK_CONFIG, ROUTING_CONFIG);
    return 0;
}

/* The folder of settings, and a file for every card found that has none
 * (DHCP). 0 or 1 (failed). */
static int settings_ready(void)
{
    struct stat st;
    if (stat(NETWORK_CONFIG, &st) == 0 && S_ISREG(st.st_mode) && move_old_settings())
        return 1;
    if (stat(NETWORK_CONFIG, &st) < 0 && mkdir(NETWORK_CONFIG, 0755) < 0)
        return fail(NETWORK_CONFIG, strerror(errno));
    struct sys_netinfo info;
    for (int i = 0; ritteros_netcard(i, &info) == 0; i++) {   /* every card, any kind */
        char path[64];
        card_file(info.name, path, sizeof(path));
        if (stat(path, &st) == 0 || !card_exists(info.name))
            continue;                               /* (38d: a removed tap is not one) */
        if (!write_file(path, DEFAULT_SETTINGS))
            return fail(path, strerror(errno));
        printf("net: %s: %s made (address = dhcp)\n", info.name, path);
    }
    return 0;
}

/* Apply the settings of one card: 0, 1 (failed), 2 (no file), 3 (no such
 * card). */
/* "ADDRESS" or "[ADDRESS]:PORT" (IPv6) into address and *port. */
static bool ipv6_server(const char *text, uint8_t *address, uint32_t *port)
{
    char a[INET6_ADDRSTRLEN];
    const char *end = text + strlen(text);
    *port = 0;
    if (text[0] == '[') {
        const char *close = strchr(text, ']');
        if (!close || close[1] != ':' || !close[2])
            return false;
        for (const char *c = close + 2; *c; c++) {
            if (*c < '0' || *c > '9' || *port > 65535)
                return false;
            *port = *port * 10 + (uint32_t)(*c - '0');
        }
        if (*port == 0 || *port > 65535)
            return false;
        text++;
        end = close;
    }
    if ((size_t)(end - text) >= sizeof(a))
        return false;
    memcpy(a, text, (size_t)(end - text));
    a[end - text] = 0;
    return inet_pton(AF_INET6, a, address) == 1;
}

/* A card's IPv6 settings (64b) from its file: address6 = auto (also
 * without the line), off, or ADDRESS/N with gateway6; dns6 for a server
 * of one's own. A line only when the file says something about IPv6. */
static int up_card6(const char *card, const char *path)
{
    struct sys_netset6 s;
    memset(&s, 0, sizeof(s));
    snprintf(s.name, sizeof(s.name), "%s", card);
    char value[64];
    int said = config_get(path, "address6", value, sizeof(value));
    if (said < 0)
        return fail(path, strerror(errno));
    s.mode = NETSET6_AUTO;
    if (said > 0 && strcmp(value, "off") == 0) {
        s.mode = NETSET6_OFF;
    } else if (said > 0 && strcmp(value, "dhcp") == 0) {
        s.mode = NETSET6_DHCP;                      /* (64g) the elf dhclient asks */
        uses_dhcp = true;
    } else if (said > 0 && strcmp(value, "auto") != 0) {
        char *slash = strchr(value, '/');
        unsigned prefix = 0;
        if (slash) {
            *slash = 0;
            for (const char *c = slash + 1; *c >= '0' && *c <= '9' && prefix <= 128; c++)
                prefix = prefix * 10 + (unsigned)(*c - '0');
        }
        if (!slash || prefix < 1 || prefix > 128 || inet_pton(AF_INET6, value, s.address) != 1) {
            if (slash)
                *slash = '/';
            return fail(value, "not auto, dhcp, off or an IPv6 address with its prefix (like 2001:db8::5/64)");
        }
        s.mode = NETSET6_FIXED;
        s.prefix = prefix;
    }
    char gateway[64] = "", dns[64] = "";
    if (config_get(path, "gateway6", gateway, sizeof(gateway)) > 0 && s.mode == NETSET6_FIXED &&
        inet_pton(AF_INET6, gateway, s.gateway) != 1)
        return fail(gateway, "not an IPv6 address (like 2001:db8::1)");
    if (config_get(path, "dns6", dns, sizeof(dns)) > 0 && !ipv6_server(dns, s.dns, &s.dns_port))
        return fail(dns, "not an IPv6 address (like 2001:db8::53 or [2001:db8::53]:5300)");
    if (ritteros_netset6(&s) < 0) {
        bool none = errno == ENOENT;
        fail(card, none ? "no such network card" : strerror(errno));
        return none ? 3 : 1;
    }
    if (s.mode == NETSET6_OFF)
        printf("net: %s: IPv6 off\n", card);
    else if (s.mode == NETSET6_DHCP)
        printf("net: %s: IPv6 by DHCPv6: the elf dhclient asks for its address\n", card);
    else if (s.mode == NETSET6_FIXED)
        printf("net: %s: IPv6 %s/%u, gateway %s, DNS %s\n", card, value, s.prefix,
               gateway[0] ? gateway : "none", dns[0] ? dns : "the router's");
    else if (said > 0 || dns[0])
        printf("net: %s: IPv6 from the network's router%s%s\n", card, dns[0] ? ", DNS " : "", dns);
    return 0;
}

static bool port_of(const char *card, char out[8]);

static int up_card(const char *card)
{
    char bridge[8];
    if (port_of(card, bridge)) {                    /* (38b) the bridge has the address */
        printf("net: %s: a port of %s: its own settings are not used\n", card, bridge);
        return 0;
    }
    struct sys_netset s;
    memset(&s, 0, sizeof(s));
    snprintf(s.name, sizeof(s.name), "%s", card);
    char path[64], value[64];
    card_file(card, path, sizeof(path));
    int r = config_get(path, "address", value, sizeof(value));
    if (r < 0 && errno == ENOENT)
        return card_exists(card) ? 2 : 3;
    if (r < 0)
        return fail(path, strerror(errno));
    if (r == 0)
        return fail(path, "no address = ... in it");
    bool dhcp = strcmp(value, "dhcp") == 0;
    if (!dhcp && (!ip_parse(value, &s.address, &s.prefix) || s.prefix == 0))
        return fail(value, "not dhcp or an address with its prefix (like 10.0.2.15/24)");
    int six = up_card6(card, path);                 /* (64b) */
    if (six)
        return six;
    r = config_get(path, "gateway", value, sizeof(value));
    if (r > 0 && !dhcp && !ip_parse(value, &s.gateway, 0))
        return fail(value, "not an address (like 10.0.2.2)");
    r = config_get(path, "dns", value, sizeof(value));
    if (r > 0) {
        char *colon = strchr(value, ':');
        if (colon) {
            *colon = 0;
            for (const char *c = colon + 1; *c >= '0' && *c <= '9' && s.dns_port <= 65535; c++)
                s.dns_port = s.dns_port * 10 + (uint32_t)(*c - '0');
        }
        if (!ip_parse(value, &s.dns, 0) || (colon && (s.dns_port == 0 || s.dns_port > 65535)))
            return fail(value, "not an address (like 10.0.2.3 or 10.0.2.2:5300)");
    }
    if (dhcp) {
        /* The elf dhclient asks for it (and keeps asking); a fixed address
         * from before goes. */
        struct sys_netinfo info;
        memset(&info, 0, sizeof(info));
        snprintf(info.name, sizeof(info.name), "%s", card);
        if (ritteros_netinfo(&info) < 0)
            return errno == ENOENT ? 3 : fail(card, strerror(errno));
        if (info.address && !info.lease) {
            memset(&s.address, 0, sizeof(s) - sizeof(s.name));
            if (ritteros_netset(&s) < 0)
                return fail(card, strerror(errno));
        }
        printf("net: %s: DHCP: the elf dhclient asks for its address\n", card);
        uses_dhcp = true;
        return 0;
    }
    if (ritteros_netset(&s) < 0) {
        if (errno == EBUSY) {
            printf("net: %s: a bridge's port: its own settings are not used\n", card);
            return 0;
        }
        bool none = errno == ENOENT;
        fail(card, none ? "no such network card" : strerror(errno));
        return none ? 3 : 1;
    }
    char address[16], gateway[16], dns[24];
    ip_format(s.address, address, sizeof(address));
    ip_format(s.gateway, gateway, sizeof(gateway));
    ip_format(s.dns, dns, sizeof(dns));
    if (s.dns_port)
        snprintf(dns + strlen(dns), sizeof(dns) - strlen(dns), ":%u", s.dns_port);
    printf("net: %s: %s/%u, gateway %s, DNS %s\n", card, address, s.prefix,
           s.gateway ? gateway : "none", s.dns ? dns : "none");
    return 0;
}

/* Switch forwarding: 0 or 1. */
static int set_forwarding(const char *value)
{
    bool on = strcmp(value, "on") == 0;
    if (!on && strcmp(value, "off") != 0)
        return fail(value, "not on or off");
    if (ritteros_forward(on ? FORWARD_ON : FORWARD_OFF) < 0)
        return fail("forward", strerror(errno));
    printf("net: forwarding between cards: %s\n", on ? "on" : "off");
    return 0;
}

/* Mark the private cards for source NAT (49i): a space-separated list of
 * card names, or "off"/empty for none. */
static int set_masquerade(const char *value)
{
    if (ritteros_masquerade(MASQ_RESET, 0) < 0)
        return fail("masquerade", strerror(errno));
    char buf[64], shown[64] = "";
    snprintf(buf, sizeof(buf), "%s", value);
    int n = 0, failed = 0;
    char *c = buf;
    while (*c) {
        while (*c == ' ' || *c == '\t')
            *c++ = 0;
        if (!*c)
            break;
        char *name = c;
        while (*c && *c != ' ' && *c != '\t')
            c++;
        if (*c)
            *c++ = 0;
        if (strcmp(name, "off") == 0 || strcmp(name, "none") == 0)
            continue;
        if (ritteros_masquerade(MASQ_ADD, name) < 0)
            failed |= fail(name, strerror(errno));
        else
            n += (snprintf(shown + strlen(shown), sizeof(shown) - strlen(shown), "%s%s",
                           n ? ", " : "", name), 1);
    }
    printf("net: masquerade (source NAT), private: %s\n", n ? shown : "none");
    return failed;
}

/* (50g) The default route's cards from value ("wlan0 eth1", or "auto": none
 * preferred); 0 or 1. */
static int set_default(const char *value)
{
    if (ritteros_default_route(DEFAULT_ROUTE_RESET, 0) < 0)
        return fail("default", strerror(errno));
    char buf[160], shown[160] = "";
    snprintf(buf, sizeof(buf), "%s", value);
    int n = 0, failed = 0;
    for (char *name = strtok(buf, " \t,"); name; name = strtok(0, " \t,")) {
        if (strcmp(name, "auto") == 0)
            continue;
        if (ritteros_default_route(DEFAULT_ROUTE_ADD, name) < 0)
            failed |= fail(name, errno == EINVAL ? "not a card's name (ethN, wlanN, tapN or brN)"
                                                  : strerror(errno));
        else
            n += (snprintf(shown + strlen(shown), sizeof(shown) - strlen(shown), "%s%s",
                           n ? ", then " : "", name), 1);
    }
    if (n)
        printf("net: the default route: %s, then any card with a gateway\n", shown);
    else
        printf("net: the default route: the first card with a gateway (auto)\n");
    return failed;
}

/* (50g) net default: the order and the route now, from the kernel's report. */
static int show_default(void)
{
    int64_t n = ritteros_report(REPORT_NET, text, sizeof(text) - 1);
    if (n < 0)
        return fail("default", strerror(errno));
    text[n] = 0;
    char order[160] = "", now[96] = "";
    for (char *line = strtok(text, "\n"); line; line = strtok(0, "\n")) {
        if (!strncmp(line, "default route: ", 15))
            snprintf(order, sizeof(order), "%s", line + 15);
        else if (!strncmp(line, "route: default ", 15))
            snprintf(now, sizeof(now), "%s", line + 15);
    }
    printf("net: the default route: %s; now %s\n", order[0] ? order : "the first card with a gateway (auto)",
           now[0] ? now : "none (no card with a gateway has a link)");
    return 0;
}

/* An elf of the network (dhclient, dnsd, sshd, guardd): its file (made if missing, on).
 * 0 or 1. */
static int elf_ready(const char *name, const char *what, const char *does)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", ELVES_DIR, name);
    struct stat st;
    if (stat(path, &st) < 0) {
        if (stat(ELVES_DIR, &st) < 0 && mkdir(ELVES_DIR, 0755) < 0)
            return fail(ELVES_DIR, strerror(errno));
        char content[160];
        snprintf(content, sizeof(content), "# %s, an elf (net up made this file)\n"
                 "command = %s\nstart = on\n", what, name);
        if (!write_file(path, content))
            return fail(path, strerror(errno));
        printf("net: %s made: the elf %s %s\n", path, name, does);
    }
    return 0;
}

/* --- (38c) /system/config/network/virtual: taps and bridges at every start --- */

static int bridge_number(const char *name);

/* A tap's number from "tapN", or -1. */
static int tap_number(const char *name)
{
    const char *c = name + 3;
    if (strncmp(name, "tap", 3) || !*c || (c[0] == '0' && c[1]) || strlen(c) > 2)
        return -1;
    int n = 0;
    for (; *c; c++) {
        if (*c < '0' || *c > '9')
            return -1;
        n = n * 10 + (*c - '0');
    }
    return n;
}

/* The line of card `name` in the kernel's report that starts with `prefix`
 * (its block: the card's line and the indented ones under it), the rest of
 * it into out; false when there is none. */
static bool card_report_line(const char *name, const char *prefix, char *out, size_t size)
{
    int64_t n = ritteros_report(REPORT_NET, text, sizeof(text) - 1);
    if (n < 0)
        return false;
    text[n] = 0;
    size_t len = strlen(name), plen = strlen(prefix);
    bool in = false;
    for (char *line = text; *line;) {
        char *end = strchr(line, '\n');
        size_t l = end ? (size_t)(end - line) : strlen(line);
        if (line[0] != ' ')
            in = l > len && !strncmp(line, name, len) && line[len] == ' ';
        else if (in && l >= plen && !strncmp(line, prefix, plen)) {
            snprintf(out, size, "%.*s", (int)(l - plen), line + plen);
            return true;
        }
        line += l + (end ? 1 : 0);
    }
    return false;
}

/* The bridge whose port card is ("br0"), or false. */
static bool port_of(const char *card, char out[8])
{
    char rest[64];
    if (!card_report_line(card, "       a port of ", rest, sizeof(rest)))
        return false;
    snprintf(out, 8, "%.*s", (int)strcspn(rest, ":"), rest);
    return true;
}

/* A bridge's ports as the kernel has them now ("eth0 tap0"; "" for none). */
static void bridge_ports(const char *bridge, char *out, size_t size)
{
    char rest[200];
    out[0] = 0;
    if (card_report_line(bridge, "       bridge: ports ", rest, sizeof(rest))) {
        rest[strcspn(rest, ";")] = 0;
        if (strcmp(rest, "none"))
            snprintf(out, size, "%s", rest);
    }
}

/* KEY = VALUE in place of KEY's line of the file (added at its end when it
 * has none), or KEY's line taken out (value 0): the other lines and the
 * comments stay (the user's rule: a program changes only its line). A
 * missing file starts as its default text. 0 or 1. */
static int virtual_set(const char *key, const char *value)
{
    static char old[8192], content[8192 + 256];
    ssize_t got = 0;
    int fd = open(SETTINGS_VIRTUAL, O_RDONLY);
    if (fd >= 0) {
        got = read(fd, old, sizeof(old) - 1);
        close(fd);
    } else {
        got = snprintf(old, sizeof(old), "%s", SETTINGS_VIRTUAL_TEXT);
    }
    old[got > 0 ? got : 0] = 0;
    char wanted[256] = "";
    if (value)
        snprintf(wanted, sizeof(wanted), "%s = %s\n", key, value);
    size_t n = 0, klen = strlen(key);
    bool replaced = false;
    for (char *line = old; *line;) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) + 1 : strlen(line);
        const char *k = line + strspn(line, " \t");
        bool is_key = !strncmp(k, key, klen) && k[klen] && strchr(" \t=", k[klen]);
        if (is_key && !replaced) {
            n += (size_t)snprintf(content + n, sizeof(content) - n, "%s", wanted);
        } else if (!is_key && n + len + 1 < sizeof(content)) {
            memcpy(content + n, line, len);
            n += len;
            if (!end)
                content[n++] = '\n';
        }
        replaced |= is_key;
        line += len;
    }
    if (!replaced)
        n += (size_t)snprintf(content + n, sizeof(content) - n, "%s", wanted);
    fd = open(SETTINGS_VIRTUAL, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    bool ok = fd >= 0 && write(fd, content, n) == (ssize_t)n;
    if (fd >= 0 && close(fd) < 0)
        ok = false;
    return ok ? 0 : fail(SETTINGS_VIRTUAL, strerror(errno));
}

/* (38c) The bridge's line as its ports are now (out when it has none). */
static int virtual_bridge_line(const char *bridge)
{
    char ports[200];
    bridge_ports(bridge, ports, sizeof(ports));
    return virtual_set(bridge, ports[0] ? ports : 0);
}

/* (38c) Make what the file says that is not there yet: the taps, then the
 * bridges with their ports (a card not there now is said and passed over).
 * The file is made with its default text when it is missing. 0 or 1. */
static int virtual_up(void)
{
    settings_make(SETTINGS_VIRTUAL, SETTINGS_VIRTUAL_TEXT);
    FILE *f = fopen(SETTINGS_VIRTUAL, "r");
    if (!f)
        return errno == ENOENT ? 0 : fail(SETTINGS_VIRTUAL, strerror(errno));
    static char lines[96][160];
    int count = 0, failed = 0;
    while (count < 96 && fgets(lines[count], sizeof(lines[count]), f))
        count++;
    fclose(f);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < count; i++) {
            char key[16], value[144];
            if (sscanf(lines[i], " %15[^ \t=#] = %143[^\n]", key, value) != 2)
                continue;
            int tap = tap_number(key), bridge = bridge_number(key);
            if (pass == 0 && tap >= 0 && !card_exists(key)) {
                char word[16], user[40];
                struct passwd *pw = 0;
                if (sscanf(value, "%15s %39s", word, user) != 2 || strcmp(word, "user") ||
                    !(pw = getpwnam(user))) {
                    failed |= fail(key, "not user NAME (a user there)");
                    continue;
                }
                if (pw->pw_uid >= 1u << 24 ||
                    ritteros_tap(TAP_ADD, pw->pw_uid | (unsigned)(tap + 1) << 24) < 0) {
                    failed |= fail(key, strerror(errno));
                    continue;
                }
                printf("net: %s: a virtual card; /dev/%s is %s's\n", key, key, user);
            } else if (pass == 1 && bridge >= 0) {
                char ports[200];
                bridge_ports(key, ports, sizeof(ports));
                for (char *port = strtok(value, " \t,"); port; port = strtok(0, " \t,")) {
                    char padded[210];
                    snprintf(padded, sizeof(padded), " %s ", ports);
                    char word[16];
                    snprintf(word, sizeof(word), " %s ", port);
                    if (strstr(padded, word))
                        continue;                       /* its port already */
                    if (!card_exists(port)) {
                        failed |= fail(port, "no such network card (not made a port)");
                        continue;
                    }
                    bool made = card_exists(key);
                    if (ritteros_bridge(BRIDGE_ADD, made ? bridge : -2 - bridge, port) < 0) {
                        failed |= fail(port, errno == EBUSY ? "a bridge's port already"
                                             : strerror(errno));
                        continue;
                    }
                    if (!made)
                        printf("net: %s: a bridge (its MAC address %s's)\n", key, port);
                    printf("net: %s: %s is its port\n", key, port);
                    bridge_ports(key, ports, sizeof(ports));
                }
            }
        }
    return failed;
}

static int up(void)
{
    struct sys_netinfo info;
    int failed = settings_ready();
    failed |= virtual_up();                         /* (38c) taps and bridges first */
    if (ritteros_netcard(0, &info) < 0)
        return 1;                                   /* no card at all (login made netnamesd's file) */
    failed |= settings_ready();                     /* (a new tap's or bridge's file) */
    /* Every card the kernel lists, of any kind, is configured the same (a
     * wireless card is a netdev like any other once wifid has joined; the
     * link may come up later, so dhclient keeps trying). */
    for (int i = 0; ritteros_netcard(i, &info) == 0; i++) {
        int r = up_card(info.name);
        if (r == 1)
            failed = 1;
        if (r == 0)
            any_card = true;
    }
    if (uses_dhcp)
        failed |= elf_ready("dhclient", "The DHCP client", "asks for the addresses");
    if (any_card)
        failed |= elf_ready("dnsd", "Names for the programs", "answers their questions");
    if (any_card)                                   /* (on from the start: the user's choice) */
        failed |= elf_ready("sshd", "The SSH server", "lets others log in (port 22)");
    failed |= elf_ready("netnamesd", "The network cards' names",   /* (50f) */
                        "records new cards in " NETNAMES_FILE);
    if (any_card) {
        /* The firewall: its elf loads the rules; switched off, nothing is
         * checked (the kernel drops what comes in until rules are loaded). */
        failed |= elf_ready("guardd", "The firewall", "loads the rules of /system/config/guard");
        char path[64], value[16];
        snprintf(path, sizeof(path), "%s/guardd", ELVES_DIR);
        static struct sys_guard g;
        if (config_get(path, "start", value, sizeof(value)) > 0 && strcmp(value, "off") == 0 &&
            ritteros_guard(GUARD_OFF, &g) == 0)
            printf("net: the firewall checks nothing (the elf guardd is off)\n");
    }
    /* elvesd after all the files: its first look starts them together. */
    if (any_card && elvesd_start(bin_dir) < 0)
        failed |= fail("elvesd", strerror(errno));
    settings_make(SETTINGS_ROUTING, SETTINGS_ROUTING_TEXT);  /* (37e-1b) when missing */
    char value[64];
    int r = config_get(ROUTING_CONFIG, "forward", value, sizeof(value));
    if (r > 0)
        failed |= set_forwarding(value);
    else if (ritteros_forward(FORWARD_ASK) == 1 && ritteros_forward(FORWARD_OFF) == 0)
        printf("net: forwarding between cards: off\n");
    if (config_get(ROUTING_CONFIG, "masquerade", value, sizeof(value)) > 0)
        failed |= set_masquerade(value);
    else
        ritteros_masquerade(MASQ_RESET, 0);             /* none configured: off */
    if (config_get(ROUTING_CONFIG, "default", value, sizeof(value)) > 0)
        failed |= set_default(value);
    else
        ritteros_default_route(DEFAULT_ROUTE_RESET, 0); /* none configured: auto */
    return failed;
}

/* (50f) Is the card with this MAC plugged in now (its line in the kernel's
 * report, not "unplugged")? *about gets what the report says it is. */
static bool card_here(const uint8_t mac[6], char *about, size_t size)
{
    int64_t n = ritteros_report(REPORT_NET, text, sizeof(text) - 1);
    for (const char *line = text; n > 0 && line < text + n;) {
        const char *end = memchr(line, '\n', (size_t)(text + n - line));
        if (!end)
            end = text + n;
        char name[8];
        uint8_t m[6];
        bool unplugged;
        if (netnames_card_line(line, (size_t)(end - line), name, m, &unplugged, about, size) &&
            !memcmp(m, mac, 6))
            return !unplugged;
        line = end + 1;
    }
    return false;
}

/* net names: every card in the names file, here or not. */
static int names(void)
{
    static struct netname known[NETNAMES_MAX];
    int count = netnames_read(known, NETNAMES_MAX);
    if (count == 0) {
        printf("net: no cards in %s yet\n", NETNAMES_FILE);
        return 0;
    }
    for (int i = 0; i < count; i++) {
        char mac[18], about[96];
        netnames_mac_text(known[i].mac, mac);
        bool here = card_here(known[i].mac, about, sizeof(about));
        printf("%-6s %s  %-8s %s\n", known[i].name, mac, here ? "here" : "not here",
               known[i].about[0] ? known[i].about : about);
    }
    return 0;
}

/* net forget NAME: the card's lines out of the names file (the comment
 * above and the entry), and its settings file removed. */
static int forget(const char *name)
{
    static struct netname known[NETNAMES_MAX];
    int count = netnames_read(known, NETNAMES_MAX), at = -1;
    for (int i = 0; i < count && at < 0; i++)
        if (!strcmp(known[i].name, name))
            at = i;
    if (at < 0)
        return fail(name, "not in " NETNAMES_FILE);
    char mac[18], about[96];
    netnames_mac_text(known[at].mac, mac);
    if (card_here(known[at].mac, about, sizeof(about))) {
        dprintf(STDERR_FILENO, "net: %s is plugged in (%s): unplug it first\n", name, about);
        return 1;
    }
    /* Rewrite the file without the entry's lines: "# NAME: ..." and "NAME = ...". */
    FILE *f = fopen(NETNAMES_FILE, "r");
    if (!f)
        return fail(NETNAMES_FILE, strerror(errno));
    static char kept[16384];
    size_t used = 0;
    char line[200], prefix[16];
    snprintf(prefix, sizeof(prefix), "# %s:", name);
    while (fgets(line, sizeof(line), f)) {
        char first[16];
        bool entry = sscanf(line, " %15[^ =]", first) == 1 && !strcmp(first, name) &&
                     strchr(line, '=');
        if (entry || !strncmp(line, prefix, strlen(prefix)))
            continue;
        size_t len = strlen(line);
        if (used + len < sizeof(kept)) {
            memcpy(kept + used, line, len);
            used += len;
        }
    }
    fclose(f);
    int fd = open(NETNAMES_FILE, O_WRONLY | O_TRUNC);
    if (fd < 0 || write(fd, kept, used) != (ssize_t)used) {
        int e = errno;
        if (fd >= 0)
            close(fd);
        return fail(NETNAMES_FILE, strerror(e));
    }
    close(fd);
    char path[64];
    card_file(name, path, sizeof(path));
    bool settings = unlink(path) == 0;
    printf("net: %s (%s) forgotten: the name is free%s%s\n", name, mac,
           settings ? "; removed " : "", settings ? path : "");
    return 0;
}

/* (38a) net tap add [user NAME]: a virtual card. */
static int tap_add(int argc, char **argv)
{
    unsigned uid = 0;
    const char *user = "root";
    if (argc == 5 && !strcmp(argv[3], "user")) {
        struct passwd *pw = getpwnam(argv[4]);
        if (!pw)
            return fail(argv[4], "no such user");
        uid = pw->pw_uid;
        user = argv[4];
    } else if (argc != 3) {
        dprintf(STDERR_FILENO, "usage: net tap add [user NAME]\n");
        return 2;
    }
    int n = ritteros_tap(TAP_ADD, uid);
    if (n < 0)
        return fail("tap", errno == ENOSPC ? "no room for another virtual card" : strerror(errno));
    printf("net: tap%d: a virtual card; /dev/tap%d is %s's\n", n, n, user);
    char key[8], value[48];
    snprintf(key, sizeof(key), "tap%d", n);
    snprintf(value, sizeof(value), "user %s", user);
    return virtual_set(key, value);                 /* (38c) made again at every start */
}

/* (38a) net tap remove TAP. */
static int tap_remove(const char *name)
{
    int n = tap_number(name);
    if (n < 0)
        return fail(name, "not a virtual card (tap0, tap1, ...)");
    if (ritteros_tap(TAP_REMOVE, (unsigned)n) < 0)
        return fail(name, errno == ENOENT ? "no such virtual card"
                          : errno == EBUSY ? "a program has it open" : strerror(errno));
    printf("net: %s removed\n", name);
    return virtual_set(name, 0);                    /* (38c) and not made at the next start */
}

/* (38b) A bridge's number from "brN", or -1. */
static int bridge_number(const char *name)
{
    const char *c = name + 2;
    if (strncmp(name, "br", 2) || !*c || (c[0] == '0' && c[1]) || strlen(c) > 1)
        return -1;
    return *c >= '0' && *c <= '9' ? *c - '0' : -1;
}

/* (38b) Why a card could not join or leave. */
static int port_failed(const char *card, int bridge, bool adding)
{
    char why[96];
    if (errno == ENOENT && !card_exists(card)) {
        snprintf(why, sizeof(why), "no such network card");
    } else if (errno == ENOENT && adding) {
        char name[8];
        snprintf(name, sizeof(name), "br%d", bridge);
        return fail(name, "no such bridge");
    } else if (errno == ENOENT) {
        snprintf(why, sizeof(why), "not a port of br%d, or no such bridge", bridge);
    } else if (errno == EBUSY)
        snprintf(why, sizeof(why), "a bridge's port already");
    else if (errno == ENOTSUP && !strncmp(card, "wlan", 4))
        snprintf(why, sizeof(why), "a Wi-Fi card cannot be a port (it carries only its own MAC address)");
    else if (errno == ENOTSUP)
        snprintf(why, sizeof(why), "its driver cannot take other machines' frames");
    else if (errno == EINVAL)
        snprintf(why, sizeof(why), "a bridge or the loopback cannot be a port");
    else if (errno == ENOSPC)
        snprintf(why, sizeof(why), "no room (br0 to br7, 16 ports each)");
    else
        snprintf(why, sizeof(why), "%s", strerror(errno));
    return fail(card, why);
}

/* (38b) net bridge add [brN] PORT... */
static int bridge_add(int argc, char **argv)
{
    int at = 3, number = argc > 3 ? bridge_number(argv[3]) : -1;
    if (number >= 0)
        at = 4;
    else if (argc > 3 && !strncmp(argv[3], "br", 2) && card_exists(argv[3]) == false)
        return fail(argv[3], "no such bridge");
    if (at >= argc) {
        dprintf(STDERR_FILENO, "usage: net bridge add [BRIDGE] PORT...\n");
        return 2;
    }
    int failed = 0;
    for (int i = at; i < argc; i++) {
        int r = ritteros_bridge(BRIDGE_ADD, number, argv[i]);
        if (r < 0) {
            failed |= port_failed(argv[i], number, true);
            continue;
        }
        if (number < 0)
            printf("net: br%d: a bridge (its MAC address %s's)\n", r, argv[i]);
        number = r;
        printf("net: br%d: %s is its port\n", r, argv[i]);
    }
    if (number >= 0) {                              /* (38c) made again at every start */
        char name[8];
        snprintf(name, sizeof(name), "br%d", number);
        failed |= virtual_bridge_line(name);
    }
    return failed;
}

/* (38b) net bridge remove brN [PORT...] */
static int bridge_remove(int argc, char **argv)
{
    int number = bridge_number(argv[3]);
    if (number < 0)
        return fail(argv[3], "not a bridge (br0, br1, ...)");
    if (argc == 4) {
        if (ritteros_bridge(BRIDGE_REMOVE, number, 0) < 0)
            return fail(argv[3], errno == ENOENT ? "no such bridge" : strerror(errno));
        printf("net: %s removed\n", argv[3]);
        return virtual_set(argv[3], 0);             /* (38c) its line goes */
    }
    int failed = 0;
    for (int i = 4; i < argc; i++) {
        if (ritteros_bridge(BRIDGE_REMOVE, number, argv[i]) < 0)
            failed |= port_failed(argv[i], number, false);
        else
            printf("net: %s: %s is no longer its port\n", argv[3], argv[i]);
    }
    return failed | virtual_bridge_line(argv[3]);
}

int main(int argc, char **argv)
{
    /* elvesd lives next to net: /system/<arch>/bin. */
    const char *slash = strrchr(argv[0], '/');
    if (slash)
        snprintf(bin_dir, sizeof(bin_dir), "%.*s", (int)(slash - argv[0]), argv[0]);
    else
        snprintf(bin_dir, sizeof(bin_dir), "/system/%s/bin",
#if defined(__x86_64__)
                 "x86_64"
#else
                 "aarch64"
#endif
        );
    if (argc == 2 && strcmp(argv[1], "up") == 0)
        return up();
    if (argc == 3 && strcmp(argv[1], "forward") == 0)
        return set_forwarding(argv[2]);
    if (argc == 2 && strcmp(argv[1], "forward") == 0) {
        int on = ritteros_forward(FORWARD_ASK);
        if (on < 0)
            return fail("forward", strerror(errno));
        printf("net: forwarding between cards: %s\n", on ? "on" : "off");
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "masquerade") == 0) {
        char list[64] = "";                             /* join the card names (or "off") */
        for (int i = 2; i < argc; i++)
            snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s", i > 2 ? " " : "",
                     argv[i]);
        return set_masquerade(list);
    }
    if (argc == 2 && strcmp(argv[1], "names") == 0)
        return names();
    if (argc == 2 && strcmp(argv[1], "default") == 0)
        return show_default();
    if (argc >= 3 && strcmp(argv[1], "default") == 0) {
        char list[160] = "";
        for (int i = 2; i < argc; i++)
            snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s", i > 2 ? " " : "",
                     argv[i]);
        return set_default(list);
    }
    if (argc == 3 && strcmp(argv[1], "forget") == 0)
        return forget(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "tap") == 0 && strcmp(argv[2], "add") == 0)
        return tap_add(argc, argv);
    if (argc == 4 && strcmp(argv[1], "tap") == 0 && strcmp(argv[2], "remove") == 0)
        return tap_remove(argv[3]);
    if (argc >= 4 && strcmp(argv[1], "bridge") == 0 && strcmp(argv[2], "add") == 0)
        return bridge_add(argc, argv);
    if (argc >= 4 && strcmp(argv[1], "bridge") == 0 && strcmp(argv[2], "remove") == 0)
        return bridge_remove(argc, argv);
    if (argc != 1 && !(argc == 2 && strcmp(argv[1], "masquerade") == 0)) {
        dprintf(STDERR_FILENO, "usage: net [up | names | forget NAME | forward [on|off] | "
                               "masquerade [ethN ... | off] | default [CARD... | auto] | "
                               "tap add [user NAME] | tap remove TAP | "
                               "bridge add [BRIDGE] PORT... | bridge remove BRIDGE [PORT...]]\n");
        return 2;
    }
    int64_t n = ritteros_report(REPORT_NET, text, sizeof(text));
    if (n < 0)
        return 1;
    if (argc == 2) {                                    /* net masquerade: just its line */
        char *line = text;
        while (line < text + n) {
            char *end = line;
            while (end < text + n && *end != '\n')
                end++;
            if ((size_t)(end - line) >= 11 && strncmp(line, "masquerade:", 11) == 0) {
                write(STDOUT_FILENO, line, (size_t)(end - line + 1));
                return 0;
            }
            line = end + 1;
        }
        return 0;
    }
    write(STDOUT_FILENO, text, n < (int64_t)sizeof(text) ? (size_t)n : sizeof(text) - 1);
    return 0;
}

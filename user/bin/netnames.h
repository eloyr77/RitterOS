/*
 * The network cards' names (50f), shared by net (net names, net forget)
 * and the elf netnamesd: /system/config/network/names holds one line per
 * card RitterOS has found, "NAME = MAC", under a comment "# NAME: what it
 * is, found DATE". The kernel names each card from it (by its MAC) at the
 * start and whenever a card is found; netnamesd adds new cards; people edit
 * it (a name changed counts from the next start). Not a library: each
 * program includes it.
 */
#ifndef RITTEROS_NETNAMES_H
#define RITTEROS_NETNAMES_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ritteros.h"

#define NETNAMES_FILE   NETWORK_CONFIG "/names"
#define NETNAMES_MAX    64
/* A new card's settings file (net up's and netnamesd's, 50h): DHCP. */
#define NETNAMES_SETTINGS "# This card's settings (net up applies them; see the manual):\n" \
                          "#   address = dhcp, or ADDRESS/PREFIX with gateway = ... and dns = ...\n" \
                          "address = dhcp\n"
#define NETNAMES_HEADER "# The network cards RitterOS has found, by MAC address: each keeps its\n" \
                        "# name (and its settings file beside this one). The elf netnamesd adds\n" \
                        "# new cards; `net forget NAME` frees a name (or delete the card's lines);\n" \
                        "# a name changed here counts from the next start.\n"

struct netname {
    char name[8];
    uint8_t mac[6];
    char about[96];                     /* the comment above it (without "# NAME: ") */
};

static inline int netnames_hex(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* "00:11:22:33:44:55" at s into mac; false if it is not one. */
static inline bool netnames_mac(const char *s, uint8_t mac[6])
{
    for (int b = 0; b < 6; b++) {
        int hi = netnames_hex(s[3 * b]), lo = hi < 0 ? -1 : netnames_hex(s[3 * b + 1]);
        if (hi < 0 || lo < 0 || (b < 5 && s[3 * b + 2] != ':'))
            return false;
        mac[b] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

static inline void netnames_mac_text(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
}

/* The file's entries (as the kernel reads them: NAME = MAC lines); the
 * number found, 0 without a file. */
static inline int netnames_read(struct netname *out, int max)
{
    FILE *f = fopen(NETNAMES_FILE, "r");
    if (!f)
        return 0;
    char line[160], about[96] = "";
    int n = 0;
    while (n < max && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '#') {
            const char *colon = strchr(line, ':');
            snprintf(about, sizeof(about), "%s", colon && colon[1] == ' ' ? colon + 2 : line + 1);
            continue;
        }
        char name[16], mac[32];
        if (sscanf(line, " %15[^ =] = %31s", name, mac) == 2 && strlen(name) < 8 &&
            strlen(mac) == 17 && netnames_mac(mac, out[n].mac)) {
            snprintf(out[n].name, sizeof(out[n].name), "%s", name);
            snprintf(out[n].about, sizeof(out[n].about), "%s", about);
            n++;
        }
        about[0] = 0;
    }
    fclose(f);
    return n;
}

/* A card's line in the kernel's network report (REPORT_NET): "eth0   MAC
 * LINK, MODEL (DRIVER at WHERE)". It gives the name, the MAC, whether it
 * is unplugged and what it is (MODEL ...); false for other lines. */
static inline bool netnames_card_line(const char *line, size_t len, char name[8], uint8_t mac[6],
                                      bool *unplugged, char *about, size_t about_size)
{
    size_t k = 0;
    while (k < len && k < 7 && line[k] != ' ')
        k++;
    if (k == 0 || k >= len || line[k] != ' ' || (strncmp(line, "eth", 3) && strncmp(line, "wlan", 4)))
        return false;
    memcpy(name, line, k);
    name[k] = 0;
    size_t i = k;
    while (i < len && line[i] == ' ')
        i++;
    if (len - i < 17 || !netnames_mac(line + i, mac))
        return false;
    i += 17;
    while (i < len && line[i] == ' ')
        i++;
    /* the link: "link up, N Mbit/s, ", "no link, " or "unplugged, " */
    const char *rest = line + i;
    size_t left = len - i;
    *unplugged = left >= 9 && !strncmp(rest, "unplugged", 9);
    int commas = left >= 7 && !strncmp(rest, "link up", 7) ? 2 : 1;
    size_t j = 0;
    for (int c = 0; c < commas && j < left; j++)
        if (rest[j] == ',')
            c++;
    while (j < left && rest[j] == ' ')
        j++;
    size_t take = left - j < about_size - 1 ? left - j : about_size - 1;
    memcpy(about, rest + j, take);
    about[take] = 0;
    return true;
}

#endif

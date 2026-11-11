/* Test: the firewall's kernel half (step 45a), before guardd and guard.
 *   guardload FILE     read FILE as /system/config/guard and load it
 *   guardload off      no checks
 *   guardload status   the loaded table, its counters, the last drops */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "ritteros.h"

static struct sys_guard g;

static const char *protocol(uint8_t p)
{
    return p == 1 ? "icmp" : p == 6 ? "tcp" : p == 17 ? "udp" : "ip";
}

int main(int argc, char **argv)
{
    char why[200], text[160], from[16], to[16];
    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        if (ritteros_guard(GUARD_OFF, &g) < 0) {
            printf("guardload: off: %s\n", strerror(errno));
            return 1;
        }
        printf("guardload: off\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        if (ritteros_guard(GUARD_STATUS, &g) < 0)
            return 1;
        printf("state %s, in = %s, out = %s, %u rule(s)\n",
               g.state == GUARD_STATE_ON ? "on" : g.state == GUARD_STATE_BOOT ? "boot" : "off",
               g.in_default == GUARD_DROP ? "drop" : "allow",
               g.out_default == GUARD_DROP ? "drop" : "allow", g.count);
        for (int i = 0; i < g.count; i++) {
            guard_format(&g.rules[i], text, sizeof(text));
            printf("%d: %s: %lu packet(s)\n", i + 1, text, (unsigned long)g.rules[i].packets);
        }
        printf("defaults: in %lu allowed, %lu dropped; out %lu allowed, %lu dropped; "
               "connections %lu\n", (unsigned long)g.in_allowed, (unsigned long)g.in_dropped,
               (unsigned long)g.out_allowed, (unsigned long)g.out_dropped,
               (unsigned long)g.connection_packets);
        for (uint32_t i = 0; i < g.drop_count; i++) {
            struct sys_guard_drop *d = &g.drops[i];
            ip_format(d->from, from, sizeof(from));
            ip_format(d->to, to, sizeof(to));
            char why_text[16] = "default";
            if (d->rule)
                snprintf(why_text, sizeof(why_text), "rule %u", d->rule);
            char card[12];
            if (d->card_kind == GUARD_KIND_TAP || d->card_kind == GUARD_KIND_BRIDGE)
                snprintf(card, sizeof(card), d->card_kind == GUARD_KIND_TAP ? "tap%u" : "br%u",
                         d->card);
            else
                snprintf(card, sizeof(card), d->card & GUARD_WLAN_CARD ? "wlan%u" : "eth%u",
                         d->card & ~GUARD_WLAN_CARD);
            printf("dropped: %s %s %u from %s to %s on %s (%s)\n",
                   d->direction == GUARD_IN ? "in" : "out", protocol(d->protocol), d->port, from,
                   to, card, why_text);
        }
        return 0;
    }
    if (argc != 2) {
        printf("usage: guardload FILE | off | status\n");
        return 2;
    }
    static struct sys_guard6 g6;                    /* (64e) IPv6's table too */
    if (guard_read6(argv[1], &g, &g6, why, sizeof(why)) < 0) {
        printf("guardload: %s: %s\n", argv[1], why);
        return 1;
    }
    if (ritteros_guard(GUARD_LOAD, &g) < 0 || ritteros_guard6(GUARD_SET6, &g6) < 0) {
        printf("guardload: %s\n", strerror(errno));
        return 1;
    }
    printf("guardload: %u rule(s), in = %s, out = %s\n", g.count,
           g.in_default == GUARD_DROP ? "drop" : "allow", g.out_default == GUARD_DROP ? "drop" : "allow");
    for (int i = 0; i < g.count; i++) {
        guard_format(&g.rules[i], text, sizeof(text));
        printf("  %d: %s\n", i + 1, text);
    }
    return 0;
}

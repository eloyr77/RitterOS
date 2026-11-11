/*
 * guard: the firewall's rules, shown and changed (roadmap item 12).
 *   guard                    the rules as the kernel uses them, numbered, with
 *                            how many packets each decided, the defaults, and
 *                            the last packets dropped
 *   guard add RULE           RULE at the end of /system/config/guard
 *   guard insert N RULE      RULE before rule N
 *   guard remove N           rule N away
 *   guard in drop|allow      what comes in when no rule matches
 *   guard out drop|allow     what goes out when none does
 * Changes are root's; they go into the file (a rule is checked first),
 * and the elf guardd loads it within 2 seconds.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("guard [add RULE | insert N RULE | remove N | in|out|forward drop|allow]",
             "the firewall's rules: show them, change them");

#define LINES   200

static struct sys_guard g;
static char file[16384];
static char *lines[LINES];
static int line_count;

static const char *protocol(uint8_t p)
{
    return p == 1 ? "icmp" : p == 6 ? "tcp" : p == 17 ? "udp" : "ip";
}

/* "2026-10-18 12:01:07" (UTC) from nanoseconds since 1970 (Howard
 * Hinnant's algorithm for the civil date). */
static void time_text(uint64_t ns, char *out, size_t size)
{
    if (!ns) {
        snprintf(out, size, "(no clock)         ");
        return;
    }
    int64_t secs = (int64_t)(ns / 1000000000ULL);
    int64_t days = secs / 86400, rem = secs % 86400;
    int64_t z = days + 719468, era = z / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    int64_t y = yoe + era * 400 + (m <= 2);
    snprintf(out, size, "%04d-%02d-%02d %02d:%02d:%02d", (int)y, (int)m, (int)d,
             (int)(rem / 3600), (int)(rem / 60 % 60), (int)(rem % 60));
}

static struct sys_guard6 g6;               /* (64e) IPv6's table */

/* One dropped packet's line: its family's addresses, its rule's number in
 * the file. */
static void drop_line(uint64_t time, const char *from, const char *to, uint16_t port_number,
                      uint8_t direction, uint8_t proto, uint8_t card_byte, uint16_t kind,
                      unsigned rule)
{
    char when[24], port[8] = "", why[24], card[12];
    time_text(time, when, sizeof(when));
    if (port_number)
        snprintf(port, sizeof(port), " %u", port_number);
    const char *dir = direction == GUARD_IN ? "in" : direction == GUARD_OUT ? "out" : "forward";
    if (rule)
        snprintf(why, sizeof(why), "rule %u", rule);
    else
        snprintf(why, sizeof(why), "%s = drop", dir);
    if (kind == GUARD_KIND_TAP || kind == GUARD_KIND_BRIDGE)       /* (38b) */
        snprintf(card, sizeof(card), kind == GUARD_KIND_TAP ? "tap%u" : "br%u", card_byte);
    else
        snprintf(card, sizeof(card), card_byte & GUARD_WLAN_CARD ? "wlan%u" : "eth%u",
                 card_byte & ~GUARD_WLAN_CARD);
    printf("  %s  %s %s%s from %s to %s on %s (%s)\n", when, dir, protocol(proto), port, from, to,
           card, why);
}

static int show(void)
{
    if (ritteros_guard(GUARD_STATUS, &g) < 0) {
        printf("guard: %s\n", strerror(errno));
        return 1;
    }
    if (ritteros_guard6(GUARD_GET6, &g6) < 0)
        memset(&g6, 0, sizeof(g6));
    const char *in = g.in_default == GUARD_DROP ? "drop" : "allow";
    const char *out = g.out_default == GUARD_DROP ? "drop" : "allow";
    const char *fwd = g.forward_default == GUARD_DROP ? "drop" : "allow";
    if (g.state == GUARD_STATE_OFF)
        printf("firewall: off: nothing is checked (the elf guardd is off)\n");
    else if (g.state == GUARD_STATE_BOOT)
        printf("firewall: no rules loaded yet: nothing comes in\n");
    else
        printf("firewall: on; in = %s, out = %s, forward = %s\n", in, out, fwd);
    if (g.state == GUARD_STATE_BOOT)
        return 0;
    /* The file's rules in its order: IPv4's table holds those without an
     * address and those with an IPv4 one, in order; IPv6's those without
     * one (both) and those with an IPv6 one, each with its number. So an
     * IPv4 rule's number is the next one that is not IPv6's alone. */
    int total = g.count;
    for (int i = 0; i < g6.count; i++)
        total += !g6.rules[i].both;
    int number4[GUARD_RULES], k = 0;
    for (int n = 1; n <= total && k < g.count; n++) {
        bool six_only = false;
        for (int i = 0; i < g6.count; i++)
            six_only |= g6.rules[i].number == n && !g6.rules[i].both;
        if (!six_only)
            number4[k++] = n;
    }
    char text[160];
    for (int n = 1; n <= total; n++) {
        const struct sys_guard6_rule *six = 0;
        for (int i = 0; i < g6.count; i++)
            if (g6.rules[i].number == n)
                six = &g6.rules[i];
        uint64_t packets = six ? six->packets : 0;
        int four = -1;
        for (int i = 0; i < k; i++)
            if (number4[i] == n)
                four = i;
        if (four >= 0) {
            guard_format(&g.rules[four], text, sizeof(text));
            packets += g.rules[four].packets;
        } else if (six) {
            guard_format6(six, text, sizeof(text));
        } else {
            continue;
        }
        printf("%3d  %-50s %8lu packet(s)\n", n, text, (unsigned long)packets);
    }
    printf("     %-50s %8lu packet(s)", g.in_default == GUARD_DROP ? "in = drop (no rule matched)"
                                                                   : "in = allow (no rule matched)",
           (unsigned long)(g.in_default == GUARD_DROP ? g.in_dropped + g6.in_dropped
                                                      : g.in_allowed + g6.in_allowed));
    printf("\n     %-50s %8lu packet(s)\n", g.out_default == GUARD_DROP ? "out = drop (no rule matched)"
                                                                          : "out = allow (no rule matched)",
           (unsigned long)(g.out_default == GUARD_DROP ? g.out_dropped + g6.out_dropped
                                                       : g.out_allowed + g6.out_allowed));
    printf("     %-50s %8lu packet(s)\n",
           g.forward_default == GUARD_DROP ? "forward = drop (no rule matched)"
                                           : "forward = allow (no rule matched)",
           (unsigned long)(g.forward_default == GUARD_DROP ? g.fwd_dropped : g.fwd_allowed));
    printf("connections: %lu packet(s) of connections already let through (not checked again)\n",
           (unsigned long)g.connection_packets);
    if (!g.drop_count && !g6.drop_count) {
        printf("dropped: none yet\n");
        return 0;
    }
    /* Both families' drops, the newest first. */
    printf("last dropped:\n");
    uint32_t a4 = 0, a6 = 0;
    for (int shown = 0; shown < GUARD_DROPS && (a4 < g.drop_count || a6 < g6.drop_count); shown++) {
        bool take4 = a6 >= g6.drop_count ||
                     (a4 < g.drop_count && g.drops[a4].time >= g6.drops[a6].time);
        if (take4) {
            struct sys_guard_drop *d = &g.drops[a4++];
            char from[16], to[16];
            ip_format(d->from, from, sizeof(from));
            ip_format(d->to, to, sizeof(to));
            unsigned rule = d->rule && d->rule <= k ? (unsigned)number4[d->rule - 1] : d->rule;
            drop_line(d->time, from, to, d->port, d->direction, d->protocol, d->card, d->card_kind,
                      rule);
        } else {
            struct sys_guard6_drop *d = &g6.drops[a6++];
            char from[INET6_ADDRSTRLEN], to[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, d->from, from, sizeof(from));
            inet_ntop(AF_INET6, d->to, to, sizeof(to));
            drop_line(d->time, from, to, d->port, d->direction, d->protocol, d->card, d->card_kind,
                      d->rule);
        }
    }
    return 0;
}

/* The file, cut into lines (in place). */
static bool read_file(void)
{
    int fd = open(GUARD_FILE, O_RDONLY);
    ssize_t length = 0, r;
    if (fd >= 0) {
        while (length < (ssize_t)sizeof(file) - 1 &&
               (r = read(fd, file + length, sizeof(file) - 1 - (size_t)length)) > 0)
            length += r;
        close(fd);
    } else if (errno != ENOENT) {
        printf("guard: %s: %s\n", GUARD_FILE, strerror(errno));
        return false;
    }
    file[length] = 0;
    line_count = 0;
    for (char *c = file; *c && line_count < LINES;) {
        lines[line_count++] = c;
        char *end = strchr(c, '\n');
        if (!end)
            break;
        *end = 0;
        c = end + 1;
    }
    return true;
}

static bool write_file(void)
{
    int fd = open(GUARD_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    bool ok = fd >= 0;
    for (int i = 0; ok && i < line_count; i++)
        ok = write(fd, lines[i], strlen(lines[i])) >= 0 && write(fd, "\n", 1) == 1;
    if (fd >= 0)
        close(fd);
    if (!ok)
        printf("guard: %s: %s\n", GUARD_FILE, strerror(errno));
    return ok;
}

/* What a line is: 1 a rule, 2 the default of key ("in"/"out"/"forward"), 0 other. */
static int kind(const char *line, const char *key)
{
    static struct sys_guard probe;
    char why[160];
    memset(&probe, 0, sizeof(probe));
    if (guard_parse(line, &probe, why, sizeof(why)) < 0)
        return 0;
    if (probe.count)
        return 1;
    uint8_t d = !key ? 0 : strcmp(key, "in") == 0 ? probe.in_default
              : strcmp(key, "out") == 0 ? probe.out_default
              : strcmp(key, "forward") == 0 ? probe.forward_default : 0;
    if (d)
        return 2;
    return 0;
}

/* The line index of rule n (1-based), or -1. */
static int rule_line(int n)
{
    for (int i = 0, seen = 0; i < line_count; i++)
        if (kind(lines[i], 0) == 1 && ++seen == n)
            return i;
    return -1;
}

static int rule_count(void)
{
    int n = 0;
    for (int i = 0; i < line_count; i++)
        n += kind(lines[i], 0) == 1;
    return n;
}

static void insert_line(int at, char *text)
{
    memmove(&lines[at + 1], &lines[at], sizeof(lines[0]) * (size_t)(line_count - at));
    lines[at] = text;
    line_count++;
}

static int number(const char *s)
{
    int n = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || n > 1000)
            return -1;
        n = n * 10 + (*s - '0');
    }
    return n;
}

int main(int argc, char **argv)
{
    if (argc == 1)
        return show();
    if (getuid() != 0) {
        printf("guard: only root may change the firewall's rules\n");
        return 1;
    }
    static char rule[256];
    const char *what = argv[1];
    int first = strcmp(what, "insert") == 0 ? 3 : 2, n = 0;
    if (strcmp(what, "add") == 0 || strcmp(what, "insert") == 0) {
        if (argc <= first) {
            printf("usage: guard add RULE, guard insert N RULE\n");
            return 2;
        }
        for (int i = first; i < argc; i++)
            snprintf(rule + strlen(rule), sizeof(rule) - strlen(rule), "%s%s", i > first ? " " : "",
                     argv[i]);
        static struct sys_guard probe;
        char why[160];
        if (guard_parse(rule, &probe, why, sizeof(why)) < 0) {
            printf("guard: %s\n", why);
            return 1;
        }
        if (probe.count != 1) {
            printf("guard: '%s': not a rule (it begins with allow, drop or limit)\n", rule);
            return 1;
        }
    }
    if (!read_file())
        return 1;
    int rules = rule_count();
    if (strcmp(what, "add") == 0 && argc > 2) {
        insert_line(line_count, rule);
        n = rules + 1;
    } else if (strcmp(what, "insert") == 0 && argc > 3) {
        n = number(argv[2]);
        if (n < 1 || n > rules + 1) {
            printf("guard: %s: no such rule (there are %d)\n", argv[2], rules);
            return 1;
        }
        int at = n <= rules ? rule_line(n) : line_count;
        insert_line(at, rule);
    } else if (strcmp(what, "remove") == 0 && argc == 3) {
        n = number(argv[2]);
        int at = n >= 1 ? rule_line(n) : -1;
        if (at < 0) {
            printf("guard: %s: no such rule (there are %d)\n", argv[2], rules);
            return 1;
        }
        memmove(&lines[at], &lines[at + 1], sizeof(lines[0]) * (size_t)(line_count - at - 1));
        line_count--;
    } else if ((strcmp(what, "in") == 0 || strcmp(what, "out") == 0 ||
                strcmp(what, "forward") == 0) && argc == 3 &&
               (strcmp(argv[2], "drop") == 0 || strcmp(argv[2], "allow") == 0)) {
        snprintf(rule, sizeof(rule), "%s = %s", what, argv[2]);
        int at = -1;
        for (int i = 0; i < line_count && at < 0; i++)
            if (kind(lines[i], what) == 2)
                at = i;
        if (at >= 0) {
            lines[at] = rule;
        } else {
            int before = rule_line(1);
            insert_line(before >= 0 ? before : line_count, rule);
        }
    } else {
        printf("usage: guard [add RULE | insert N RULE | remove N | "
               "in|out|forward drop|allow]\n");
        return 2;
    }
    if (!write_file())
        return 1;
    if (strcmp(what, "remove") == 0)
        printf("guard: rule %d removed; guardd loads the rules within 2 s\n", n);
    else if (n)
        printf("guard: rule %d: %s; guardd loads it within 2 s\n", n, rule);
    else
        printf("guard: %s; guardd loads it within 2 s\n", rule);
    return 0;
}

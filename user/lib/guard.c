/*
 * The firewall's rules as text (/system/config/guard), for guardd and
 * guard: read into the table the kernel takes (struct sys_guard), and
 * written back in words. The format is in ritteros.h.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

#define WORDS 16

static const char *protocol_name(uint8_t p)
{
    return p == 1 ? "icmp" : p == 6 ? "tcp" : p == 17 ? "udp" : 0;
}

/* A decimal number of 1 to max, the whole word (or up to stop). */
static bool number(const char *s, char stop, uint32_t max, uint32_t *out, const char **end)
{
    uint32_t n = 0;
    const char *c = s;
    for (; *c >= '0' && *c <= '9'; c++) {
        n = n * 10 + (uint32_t)(*c - '0');
        if (n > max)
            return false;
    }
    if (c == s || n == 0 || (*c && *c != stop))
        return false;
    *out = n;
    if (end)
        *end = c;
    return true;
}

static int fail(char *why, size_t size, const char *format, const char *word)
{
    snprintf(why, size, format, word);
    return -1;
}

/* "ethN" or "wlanN" (N up to two digits) into a card number (wlanN with
 * GUARD_WLAN_CARD, 50k), (38b) "tapN" and "brN" with their kind; false if
 * it is not one. */
static bool card_number_of(const char *v, uint8_t *out, uint8_t *kind)
{
    bool wireless = !strncmp(v, "wlan", 4), tap = !strncmp(v, "tap", 3), bridge = !strncmp(v, "br", 2);
    const char *digits = v + (wireless ? 4 : tap || !bridge ? 3 : 2);
    if ((!wireless && !tap && !bridge && strncmp(v, "eth", 3) != 0) || !*digits ||
        strlen(digits) > 2)
        return false;
    uint32_t card = 0;
    for (const char *c = digits; *c; c++) {
        if (*c < '0' || *c > '9')
            return false;
        card = card * 10 + (uint32_t)(*c - '0');
    }
    *out = (uint8_t)(wireless ? GUARD_WLAN_CARD | card : card);
    *kind = tap ? GUARD_KIND_TAP : bridge ? GUARD_KIND_BRIDGE : GUARD_KIND_ETH;
    return true;
}

/* A card number and kind as its name: "eth2", "wlan0", "tap1", "br0". */
static void card_name(uint8_t card, uint8_t kind, char *out, size_t size)
{
    if (kind == GUARD_KIND_TAP || kind == GUARD_KIND_BRIDGE)
        snprintf(out, size, kind == GUARD_KIND_TAP ? "tap%u" : "br%u", card);
    else
        snprintf(out, size, card & GUARD_WLAN_CARD ? "wlan%u" : "eth%u", card & ~GUARD_WLAN_CARD);
}

/* What a rule line says (64e: either family). Its addresses: IPv4 ones
 * in from4/to4, IPv6 ones in r6's; family 4 or 6 for the family they name,
 * 0 for none. */
struct parsed {
    struct sys_guard6_rule r6;          /* everything but IPv4's addresses */
    uint32_t from4, to4, from4_prefix, to4_prefix;
    int family;
};

/* "ADDRESS[/N]" of either family into p (from or to); false if it is
 * not one, or not the family the rule's other address is. */
static bool rule_address(const char *v, struct parsed *p, bool to)
{
    if (strchr(v, ':')) {                           /* IPv6 (64e) */
        char text[64];
        snprintf(text, sizeof(text), "%s", v);
        char *slash = strchr(text, '/');
        uint32_t prefix = 128;
        if (slash) {
            *slash = 0;
            if (!number(slash + 1, 0, 128, &prefix, 0))
                return false;
        }
        uint8_t *out = to ? p->r6.to : p->r6.from;
        if (inet_pton(AF_INET6, text, out) != 1 || p->family == 4)
            return false;
        *(to ? &p->r6.to_prefix : &p->r6.from_prefix) = (uint8_t)prefix;
        p->family = 6;
        return true;
    }
    uint32_t *ip = to ? &p->to4 : &p->from4, *prefix = to ? &p->to4_prefix : &p->from4_prefix;
    if (!ip_parse(v, ip, prefix) || p->family == 6)
        return false;
    if (!strchr(v, '/'))
        *prefix = 32;
    p->family = 4;
    return true;
}

/* One line: 0 for nothing to add (empty, a comment, a default, put into
 * g), 1 for a rule (in *p), -1 with the reason in why. */
static int parse_line(const char *line, struct sys_guard *g, struct parsed *p, char *why,
                      size_t size)
{
    char copy[256], *w[WORDS];
    snprintf(copy, sizeof(copy), "%s", line);
    char *hash = strchr(copy, '#');
    if (hash)
        *hash = 0;
    int n = 0;
    for (char *c = copy; *c && n < WORDS;) {
        while (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n')
            *c++ = 0;
        if (!*c)
            break;
        w[n++] = c;
        while (*c && *c != ' ' && *c != '\t' && *c != '\r' && *c != '\n')
            c++;
    }
    if (n == 0)
        return 0;                                   /* empty, or a comment */

    /* A default: "in = drop" (also "in=drop"): the words joined. */
    char joined[64] = "";
    for (int k = 0; k < n; k++)
        snprintf(joined + strlen(joined), sizeof(joined) - strlen(joined), "%s", w[k]);
    char *eq = strchr(joined, '=');
    if (eq) {
        *eq = 0;
        const char *key = joined, *value = eq + 1;
        uint8_t action = strcmp(value, "drop") == 0 ? GUARD_DROP
                       : strcmp(value, "allow") == 0 ? GUARD_ALLOW : 0;
        if (!action)
            return fail(why, size, "'%s': not drop or allow", value);
        if (strcmp(key, "in") == 0)
            g->in_default = action;
        else if (strcmp(key, "out") == 0)
            g->out_default = action;
        else if (strcmp(key, "forward") == 0)
            g->forward_default = action;
        else
            return fail(why, size, "'%s': not in, out or forward", key);
        return 0;
    }

    memset(p, 0, sizeof(*p));
    struct sys_guard6_rule *r = &p->r6;
    r->card = r->out_card = GUARD_ANY_CARD;
    int i = 0;
    if (strcmp(w[i], "allow") == 0) {
        r->action = GUARD_ALLOW;
    } else if (strcmp(w[i], "drop") == 0) {
        r->action = GUARD_DROP;
    } else if (strcmp(w[i], "limit") == 0) {
        /* Its rate is the last word: limit in tcp 22 10/minute. */
        r->action = GUARD_LIMIT;
        const char *rate = w[n - 1];
        if (n < 3 || !strchr(rate, '/') || !number(rate, '/', 65535, &r->limit, 0) ||
            strcmp(strchr(rate, '/'), "/minute") != 0)
            return fail(why, size, "'%s': limit needs N/minute at the end (like limit in tcp 22 10/minute)",
                        n < 3 ? "limit" : rate);
        n--;
    } else {
        return fail(why, size, "'%s': not allow, drop or limit", w[i]);
    }
    if (++i >= n || (strcmp(w[i], "in") != 0 && strcmp(w[i], "out") != 0 &&
                     strcmp(w[i], "forward") != 0))
        return fail(why, size, "'%s': in, out or forward is missing", i < n ? w[i] : w[0]);
    r->direction = strcmp(w[i], "in") == 0 ? GUARD_IN
                : strcmp(w[i], "out") == 0 ? GUARD_OUT : GUARD_FORWARD;
    i++;
    if (i < n && (strcmp(w[i], "tcp") == 0 || strcmp(w[i], "udp") == 0 || strcmp(w[i], "icmp") == 0)) {
        r->protocol = w[i][0] == 't' ? 6 : w[i][0] == 'u' ? 17 : 1;
        i++;
    }
    if (i < n && w[i][0] >= '0' && w[i][0] <= '9') {
        uint32_t low, high;
        const char *end;
        if (!number(w[i], '-', 65535, &low, &end) ||
            (*end && !number(end + 1, 0, 65535, &high, 0)))
            return fail(why, size, "'%s': not a port (1-65535) or ports (like 6000-6010)", w[i]);
        if (!*end)
            high = low;
        if (high < low)
            return fail(why, size, "'%s': the ports go down", w[i]);
        if (r->protocol != 6 && r->protocol != 17)
            return fail(why, size, "'%s': a port needs tcp or udp", w[i]);
        r->port_low = (uint16_t)low;
        r->port_high = (uint16_t)high;
        i++;
    }
    bool from = false, to = false, oncard = false, outcard = false;
    for (; i < n; i += 2) {
        if (i + 1 >= n)
            return fail(why, size, "'%s': what follows is missing", w[i]);
        const char *v = w[i + 1];
        if ((strcmp(w[i], "from") == 0 && !from) || (strcmp(w[i], "to") == 0 && !to)) {
            bool is_to = w[i][0] == 't';
            int before = p->family;
            if (!rule_address(v, p, is_to))
                return fail(why, size, before && (before == 6) != (strchr(v, ':') != 0)
                            ? "'%s': not the family of the rule's other address"
                            : "'%s': not an address (like 192.168.0.0/24 or 2001:db8::/32)", v);
            *(is_to ? &to : &from) = true;
        } else if (strcmp(w[i], "on") == 0 && !oncard) {
            if (!card_number_of(v, &r->card, &r->kinds[0]))   /* the card for an in/out rule */
                return fail(why, size, "'%s': not a card (like eth0, wlan0, tap0, br0)", v);
            oncard = true;
        } else if (strcmp(w[i], "in") == 0 && !oncard && r->direction == GUARD_FORWARD) {
            if (!card_number_of(v, &r->card, &r->kinds[0]))   /* forward: the incoming card */
                return fail(why, size, "'%s': not a card (like eth0, wlan0, tap0, br0)", v);
            oncard = true;
        } else if (strcmp(w[i], "out") == 0 && !outcard && r->direction == GUARD_FORWARD) {
            if (!card_number_of(v, &r->out_card, &r->kinds[1]))   /* forward: the outgoing card */
                return fail(why, size, "'%s': not a card (like eth0, wlan0, tap0, br0)", v);
            outcard = true;
        } else {
            return fail(why, size, "'%s': not understood here", w[i]);
        }
    }
    return 1;
}

/* A parsed rule for the IPv4 table. */
static struct sys_guard_rule rule4(const struct parsed *p)
{
    struct sys_guard_rule r;
    memset(&r, 0, sizeof(r));
    r.action = p->r6.action, r.direction = p->r6.direction, r.protocol = p->r6.protocol;
    r.card = p->r6.card, r.out_card = p->r6.out_card;
    r.kinds[0] = p->r6.kinds[0], r.kinds[1] = p->r6.kinds[1];
    r.port_low = p->r6.port_low, r.port_high = p->r6.port_high;
    r.from = p->from4, r.from_prefix = p->from4_prefix;
    r.to = p->to4, r.to_prefix = p->to4_prefix;
    r.limit = p->r6.limit;
    return r;
}

/* One line into the tables: g (IPv4's: rules without an address or with
 * an IPv4 one) and g6 if not 0 (IPv6's: without one or with an IPv6 one),
 * number the rule's place in the file. */
static int add_line(const char *line, struct sys_guard *g, struct sys_guard6 *g6, int *number,
                    char *why, size_t size)
{
    struct parsed p;
    int r = parse_line(line, g, &p, why, size);
    if (r <= 0)
        return r;
    ++*number;
    if (p.family != 6) {
        if (g->count >= GUARD_RULES)
            return fail(why, size, "%s", "more than 64 rules");
        g->rules[g->count++] = rule4(&p);
    }
    if (p.family != 4 && g6) {
        if (g6->count >= GUARD_RULES)
            return fail(why, size, "%s", "more than 64 rules");
        p.r6.number = (uint8_t)*number;
        p.r6.both = p.family == 0;
        g6->rules[g6->count++] = p.r6;
    }
    return 0;
}

int guard_parse(const char *line, struct sys_guard *g, char *why, size_t size)
{
    int number = 0;
    return add_line(line, g, 0, &number, why, size) < 0 ? -1 : 0;     /* (an IPv6 rule: checked) */
}

/* The file into g and (if not 0) g6. */
static int read_rules(const char *path, struct sys_guard *g, struct sys_guard6 *g6, char *why,
                      size_t size)
{
    memset(g, 0, sizeof(*g));
    if (g6)
        memset(g6, 0, sizeof(*g6));
    g->in_default = GUARD_DROP;
    g->out_default = GUARD_ALLOW;
    g->forward_default = GUARD_DROP;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return fail(why, size, "%s", strerror(errno));
    static char text[16384];
    ssize_t length = 0, r;
    while (length < (ssize_t)sizeof(text) - 1 &&
           (r = read(fd, text + length, sizeof(text) - 1 - (size_t)length)) > 0)
        length += r;
    close(fd);
    text[length] = 0;
    int number = 1, rules = 0;
    for (char *line = text; *line; number++) {
        char *end = strchr(line, '\n');
        if (end)
            *end = 0;
        char reason[160];
        if (add_line(line, g, g6, &rules, reason, sizeof(reason)) < 0) {
            snprintf(why, size, "line %d: %s", number, reason);
            errno = EINVAL;
            return -1;
        }
        if (!end)
            break;
        line = end + 1;
    }
    return 0;
}

int guard_read(const char *path, struct sys_guard *g, char *why, size_t size)
{
    return read_rules(path, g, 0, why, size);
}

int guard_read6(const char *path, struct sys_guard *g, struct sys_guard6 *g6, char *why, size_t size)
{
    return read_rules(path, g, g6, why, size);
}

static void address(char *out, size_t size, const char *word, uint32_t ip, uint32_t prefix)
{
    char text[16];
    ip_format(ip, text, sizeof(text));
    if (prefix == 32)
        snprintf(out, size, " %s %s", word, text);
    else
        snprintf(out, size, " %s %s/%u", word, text, prefix);
}

void guard_format(const struct sys_guard_rule *r, char *out, size_t size)
{
    char part[48];
    size_t used = 0;
    used += (size_t)snprintf(out, size, "%s", r->action == GUARD_LIMIT ? "limit"
                                              : r->action == GUARD_DROP ? "drop" : "allow");
    snprintf(out + used, size - used, " %s", r->direction == GUARD_IN ? "in"
                                           : r->direction == GUARD_OUT ? "out" : "forward");
    used = strlen(out);
    if (protocol_name(r->protocol)) {
        snprintf(out + used, size - used, " %s", protocol_name(r->protocol));
        used = strlen(out);
    }
    if (r->port_low) {
        if (r->port_low == r->port_high)
            snprintf(part, sizeof(part), " %u", r->port_low);
        else
            snprintf(part, sizeof(part), " %u-%u", r->port_low, r->port_high);
        snprintf(out + used, size - used, "%s", part);
        used = strlen(out);
    }
    if (r->from_prefix) {
        address(part, sizeof(part), "from", r->from, r->from_prefix);
        snprintf(out + used, size - used, "%s", part);
        used = strlen(out);
    }
    if (r->to_prefix) {
        address(part, sizeof(part), "to", r->to, r->to_prefix);
        snprintf(out + used, size - used, "%s", part);
        used = strlen(out);
    }
    if (r->card != GUARD_ANY_CARD) {
        card_name(r->card, r->kinds[0], part, sizeof(part));
        snprintf(out + used, size - used, r->direction == GUARD_FORWARD ? " in %s" : " on %s", part);
        used = strlen(out);
    }
    if (r->direction == GUARD_FORWARD && r->out_card != GUARD_ANY_CARD) {
        card_name(r->out_card, r->kinds[1], part, sizeof(part));
        snprintf(out + used, size - used, " out %s", part);
        used = strlen(out);
    }
    if (r->action == GUARD_LIMIT)
        snprintf(out + used, size - used, " %u/minute", r->limit);
}

void guard_format6(const struct sys_guard6_rule *r, char *out, size_t size)
{
    /* The words of the same rule without its addresses, then IPv6's. */
    struct sys_guard_rule plain;
    memset(&plain, 0, sizeof(plain));
    plain.action = r->action, plain.direction = r->direction, plain.protocol = r->protocol;
    plain.port_low = r->port_low, plain.port_high = r->port_high, plain.limit = r->limit;
    plain.card = plain.out_card = GUARD_ANY_CARD;
    guard_format(&plain, out, size);
    char *rate = r->action == GUARD_LIMIT ? strrchr(out, ' ') : 0;
    char tail[24] = "";
    if (rate) {                                     /* (N/minute stays last) */
        snprintf(tail, sizeof(tail), "%s", rate);
        *rate = 0;
    }
    for (int k = 0; k < 2; k++) {
        uint8_t prefix = k ? r->to_prefix : r->from_prefix;
        if (!prefix)
            continue;
        char text[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, k ? r->to : r->from, text, sizeof(text));
        size_t used = strlen(out);
        snprintf(out + used, size - used, prefix == 128 ? " %s %s" : " %s %s/%u", k ? "to" : "from",
                 text, prefix);
    }
    char part[16];
    if (r->card != GUARD_ANY_CARD) {
        card_name(r->card, r->kinds[0], part, sizeof(part));
        size_t used = strlen(out);
        snprintf(out + used, size - used, r->direction == GUARD_FORWARD ? " in %s" : " on %s", part);
    }
    if (r->direction == GUARD_FORWARD && r->out_card != GUARD_ANY_CARD) {
        card_name(r->out_card, r->kinds[1], part, sizeof(part));
        size_t used = strlen(out);
        snprintf(out + used, size - used, " out %s", part);
    }
    size_t used = strlen(out);
    snprintf(out + used, size - used, "%s", tail);
}

int ritteros_guard6(int op, struct sys_guard6 *g)
{
    int64_t r = syscall6(SYS_GUARD, op, (int64_t)g, 0, 0, 0, 0);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return (int)r;
}

int ritteros_guard(int op, struct sys_guard *g)
{
    int64_t r = syscall6(SYS_GUARD, op, (int64_t)g, 0, 0, 0, 0);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return (int)r;
}

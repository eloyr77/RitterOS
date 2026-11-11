/*
 * IPv6 addresses as text (see ip6text.h).
 */
#include "ip6text.h"

static int hex_digit(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Text being written: it stops counting room at the end, `full` says so. */
struct out {
    char *p;
    size_t at, size;
    bool full;
};

static void put(struct out *o, char c)
{
    if (o->at + 1 >= o->size)
        o->full = true;
    else
        o->p[o->at++] = c;
}

static void put_decimal(struct out *o, unsigned v)
{
    if (v >= 100)
        put(o, (char)('0' + v / 100));
    if (v >= 10)
        put(o, (char)('0' + v / 10 % 10));
    put(o, (char)('0' + v % 10));
}

size_t ip6_format(const uint8_t a[16], char *out, size_t size)
{
    unsigned group[8];
    for (int i = 0; i < 8; i++)
        group[i] = (unsigned)a[2 * i] << 8 | a[2 * i + 1];
    int best = -1, best_len = 0;
    for (int i = 0; i < 8;) {
        int j = i;
        while (j < 8 && group[j] == 0)
            j++;
        if (j - i > best_len)
            best = i, best_len = j - i;
        i = j > i ? j : i + 1;
    }
    if (best_len < 2)
        best = -1;                                  /* a single zero group stays "0" */
    struct out o = { out, 0, size, size == 0 };
    for (int i = 0; i < 8; i++) {
        if (best >= 0 && i >= best && i < best + best_len) {
            if (i == best)
                put(&o, ':');
            continue;
        }
        if (i)
            put(&o, ':');
        /* An IPv4 address inside (BSD's rule): ::a.b.c.d and ::ffff:a.b.c.d. */
        if (i == 6 && best == 0 && (best_len == 6 || (best_len == 5 && group[5] == 0xFFFF))) {
            for (int k = 12; k < 16; k++) {
                put_decimal(&o, a[k]);
                if (k < 15)
                    put(&o, '.');
            }
            break;
        }
        bool started = false;
        for (int shift = 12; shift >= 0; shift -= 4) {
            unsigned d = group[i] >> shift & 15;
            if (d || started || shift == 0) {
                put(&o, "0123456789abcdef"[d]);
                started = true;
            }
        }
    }
    if (best >= 0 && best + best_len == 8)
        put(&o, ':');                               /* a run at the end: "1::" */
    if (o.full)
        return 0;
    out[o.at] = 0;
    return o.at;
}

/* a.b.c.d (four decimal numbers 0-255 without leading zeros) as the whole text. */
static bool dotted(const char *s, size_t n, uint8_t out[4])
{
    size_t i = 0;
    for (int part = 0; part < 4; part++) {
        unsigned v = 0;
        size_t start = i;
        while (i < n && s[i] >= '0' && s[i] <= '9' && i - start < 3)
            v = v * 10 + (unsigned)(s[i++] - '0');
        if (i == start || v > 255 || (i - start > 1 && s[start] == '0'))
            return false;
        out[part] = (uint8_t)v;
        if (part < 3 && (i >= n || s[i++] != '.'))
            return false;
    }
    return i == n;
}

bool ip6_parse(const char *s, size_t n, uint8_t out[16])
{
    unsigned group[8];
    int count = 0, gap = -1;                        /* gap: where "::" stands */
    size_t i = 0;
    if (n >= 2 && s[0] == ':' && s[1] == ':') {
        gap = 0;
        i = 2;
    } else if (n && s[0] == ':') {
        return false;
    }
    while (i < n) {
        size_t start = i;
        unsigned v = 0;
        while (i < n && hex_digit(s[i]) >= 0 && i - start < 5)
            v = v * 16 + (unsigned)hex_digit(s[i++]);
        if (i < n && s[i] == '.') {                 /* the last 32 bits as a.b.c.d */
            uint8_t v4[4];
            if (count > 6 || !dotted(s + start, n - start, v4))
                return false;
            group[count++] = (unsigned)v4[0] << 8 | v4[1];
            group[count++] = (unsigned)v4[2] << 8 | v4[3];
            break;
        }
        if (i == start || i - start > 4 || count == 8)
            return false;
        group[count++] = v;
        if (i == n)
            break;
        if (s[i++] != ':' || i == n)
            return false;                           /* (a single ':' at the end) */
        if (s[i] == ':') {
            if (gap >= 0)
                return false;                       /* "::" only once */
            gap = count;
            i++;
        }
    }
    if (gap < 0 ? count != 8 : count > 7)
        return false;
    for (int k = 0; k < 16; k++)
        out[k] = 0;
    int tail = gap < 0 ? 0 : count - gap;           /* groups after the "::" */
    for (int k = 0; k < count; k++) {
        int at = gap < 0 || k < gap ? k : 8 - tail + (k - gap);
        out[2 * at] = (uint8_t)(group[k] >> 8);
        out[2 * at + 1] = (uint8_t)group[k];
    }
    return true;
}

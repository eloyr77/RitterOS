/*
 * DEFLATE (RFC 1951) and the zlib wrapper (RFC 1950): decompression for
 * PNG's image data now, and HTTP's gzip later. Our own code from the RFCs.
 * Every read of the input is bounds-checked: compressed data comes from other
 * machines, and a damaged or hostile stream must end in an error, never in
 * a read or write outside the buffers.
 *
 * Huffman codes are canonical (RFC 1951 3.2.2): decoded through a table of
 * the first FAST bits, with the rare longer codes found by walking the code
 * lengths, which needs no further table.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "imagedec.h"

#define MAXBITS 15                              /* the longest code DEFLATE allows */
#define FAST 10                                 /* bits looked up in one step */

struct bits {
    const uint8_t *d;
    size_t n, at;                               /* the input and the next byte */
    uint64_t buf;                               /* bits not used yet, lowest first */
    int count;
    int pad;                                    /* zero bytes put in past the end */
    bool bad;                                   /* bits used past the end */
};

/* Make sure k (<= 32) bits are in the buffer; past the end, zero bytes.
 * Those may be looked at (a code is decoded by a look at FAST bits, more
 * than the last code of a stream may have) but not used: only using them
 * makes the data bad. (Before 27b any look past the end did; zlib's and
 * gzip's checksums after the data hid it, a bare DEFLATE stream failed.) */
static void need(struct bits *b, int k)
{
    while (b->count < k) {
        uint64_t byte = 0;
        if (b->at < b->n)
            byte = b->d[b->at++];
        else
            b->pad++;
        b->buf |= byte << b->count;
        b->count += 8;
    }
}

/* The real bits left in the buffer (the padding is above them). */
static int real_bits(const struct bits *b)
{
    return b->count - 8 * b->pad;
}

static void used_bits(struct bits *b, int k)
{
    b->buf >>= k;
    b->count -= k;
    if (real_bits(b) < 0)
        b->bad = true;
}

static unsigned take(struct bits *b, int k)
{
    if (k == 0)
        return 0;
    need(b, k);
    unsigned v = (unsigned)(b->buf & ((1u << k) - 1));
    used_bits(b, k);
    return v;
}

/* A canonical Huffman code: how many codes of each length, the symbols in
 * code order, and the table of the first FAST bits (each entry: symbol << 4 |
 * length, or 0 when the code is longer than FAST). */
struct huffman {
    uint16_t count[MAXBITS + 1];
    uint16_t symbol[288];
    uint16_t fast[1 << FAST];
};

/* Build a code from n lengths; false if they over-subscribe the code space
 * (an incomplete code is allowed: RFC 1951 lets a single distance code
 * stand alone). */
static bool build(struct huffman *h, const uint8_t *length, int n)
{
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++)
        h->count[length[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return false;                       /* more codes than there is room for */
    }
    uint16_t offset[MAXBITS + 2];
    offset[1] = 0;
    for (int len = 1; len <= MAXBITS; len++)
        offset[len + 1] = (uint16_t)(offset[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (length[i])
            h->symbol[offset[length[i]]++] = (uint16_t)i;
    /* The fast table: walk the codes in order; codes are sent most significant
     * bit first, while the bit reader takes the lowest bit first, so each
     * code's table index is its bits reversed. */
    memset(h->fast, 0, sizeof(h->fast));
    int code = 0, index = 0;
    for (int len = 1; len <= FAST; len++) {
        for (int k = 0; k < h->count[len]; k++, code++, index++) {
            int rev = 0;
            for (int i = 0; i < len; i++)
                rev |= ((code >> i) & 1) << (len - 1 - i);
            for (int fill = rev; fill < (1 << FAST); fill += 1 << len)
                h->fast[fill] = (uint16_t)(h->symbol[index] << 4 | len);
        }
        code <<= 1;
    }
    return true;
}

/* The next symbol of code h, or -1 (no such code: damaged data). */
static int decode(struct bits *b, const struct huffman *h)
{
    need(b, FAST);
    uint16_t e = h->fast[b->buf & ((1u << FAST) - 1)];
    if (e) {
        used_bits(b, e & 15);
        return e >> 4;
    }
    /* Longer than FAST: one bit at a time against the counts (canonical
     * codes of one length are consecutive numbers). */
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code |= (int)take(b, 1);
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t distance_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t distance_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12,
    13, 13 };

struct output {
    uint8_t *p;
    size_t cap, at;
};

/* One block's symbols until the end-of-block code; false on damage. */
static bool codes(struct bits *b, struct output *o, const struct huffman *lit,
                  const struct huffman *dist)
{
    for (;;) {
        int s = decode(b, lit);
        if (s < 0 || b->bad)
            return false;
        if (s < 256) {                          /* a literal byte */
            if (o->at >= o->cap)
                return false;
            o->p[o->at++] = (uint8_t)s;
            continue;
        }
        if (s == 256)
            return true;                        /* end of block */
        s -= 257;
        if (s >= 29)
            return false;
        size_t len = length_base[s] + take(b, length_extra[s]);
        int ds = decode(b, dist);
        if (ds < 0 || ds >= 30)
            return false;
        size_t back = distance_base[ds] + take(b, distance_extra[ds]);
        if (b->bad || back > o->at || len > o->cap - o->at)
            return false;                       /* before the start, or past the end */
        const uint8_t *from = o->p + o->at - back;
        uint8_t *to = o->p + o->at;
        for (size_t i = 0; i < len; i++)        /* (may overlap: byte by byte) */
            to[i] = from[i];
        o->at += len;
    }
}

static bool fixed_codes(struct huffman *lit, struct huffman *dist)
{
    uint8_t length[288];
    int i = 0;
    for (; i < 144; i++)
        length[i] = 8;
    for (; i < 256; i++)
        length[i] = 9;
    for (; i < 280; i++)
        length[i] = 7;
    for (; i < 288; i++)
        length[i] = 8;
    if (!build(lit, length, 288))
        return false;
    for (i = 0; i < 30; i++)
        length[i] = 5;
    return build(dist, length, 30);
}

/* A dynamic block's code lengths (RFC 1951 3.2.7). */
static bool dynamic_codes(struct bits *b, struct huffman *lit, struct huffman *dist)
{
    static const uint8_t order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int nlen = (int)take(b, 5) + 257, ndist = (int)take(b, 5) + 1, ncode = (int)take(b, 4) + 4;
    if (nlen > 286 || ndist > 30)
        return false;
    uint8_t length[320];
    memset(length, 0, sizeof(length));
    for (int i = 0; i < ncode; i++)
        length[order[i]] = (uint8_t)take(b, 3);
    struct huffman lengths;
    if (!build(&lengths, length, 19))
        return false;
    for (int i = 0; i < nlen + ndist;) {
        int s = decode(b, &lengths);
        if (s < 0 || b->bad)
            return false;
        if (s < 16) {
            length[i++] = (uint8_t)s;
            continue;
        }
        int repeat, value = 0;
        if (s == 16) {                          /* the previous length 3-6 times */
            if (i == 0)
                return false;
            value = length[i - 1];
            repeat = 3 + (int)take(b, 2);
        } else if (s == 17)
            repeat = 3 + (int)take(b, 3);       /* zeros, 3-10 */
        else
            repeat = 11 + (int)take(b, 7);      /* zeros, 11-138 */
        if (i + repeat > nlen + ndist)
            return false;
        while (repeat--)
            length[i++] = (uint8_t)value;
    }
    if (length[256] == 0)
        return false;                           /* no end-of-block code */
    return build(lit, length, nlen) && build(dist, length + nlen, ndist);
}

long inflate_raw(const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *used)
{
    struct bits b = { in, n, 0, 0, 0, 0, false };
    struct output o = { out, cap, 0 };
    struct huffman lit, dist;                   /* each call its own: programs have threads */
    struct huffman *l = &lit, *dd = &dist;
    for (bool last = false; !last;) {
        last = take(&b, 1);
        unsigned type = take(&b, 2);
        if (type == 0) {                        /* stored: aligned length, its complement */
            used_bits(&b, b.count & 7);
            unsigned len = take(&b, 16), nlen = take(&b, 16);
            if (b.bad || (len ^ 0xFFFF) != nlen)
                return -1;
            /* (the bit buffer holds no whole bytes now: it was just emptied
             * to the byte boundary and refilled only as needed) */
            while (len && real_bits(&b) >= 8) { /* bytes already in the buffer first */
                if (o.at >= o.cap)
                    return -1;
                o.p[o.at++] = (uint8_t)take(&b, 8);
                len--;
            }
            if (len > b.n - b.at || len > o.cap - o.at)
                return -1;
            memcpy(o.p + o.at, b.d + b.at, len);
            b.at += len, o.at += len;
        } else if (type == 1) {
            if (!fixed_codes(l, dd) || !codes(&b, &o, l, dd))
                return -1;
        } else if (type == 2) {
            if (!dynamic_codes(&b, l, dd) || !codes(&b, &o, l, dd))
                return -1;
        } else
            return -1;
        if (b.bad)
            return -1;
    }
    if (used)
        *used = b.at - (size_t)(real_bits(&b) / 8);     /* (whole bytes read ahead go back) */
    return (long)o.at;
}

long inflate_zlib(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    if (n < 6 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 || (in[1] & 0x20))
        return -1;                              /* not DEFLATE, a bad check, a dictionary */
    size_t used;
    long got = inflate_raw(in + 2, n - 2, out, cap, &used);
    if (got < 0 || 2 + used + 4 > n)
        return -1;
    uint32_t a = 1, b = 0;                      /* Adler-32 of what came out */
    for (long i = 0; i < got; i++) {
        a = (a + out[i]) % 65521;
        b = (b + a) % 65521;
    }
    const uint8_t *s = in + 2 + used;
    uint32_t want = (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | s[3];
    return (b << 16 | a) == want ? got : -1;
}

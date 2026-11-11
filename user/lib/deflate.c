/*
 * DEFLATE (RFC 1951) compression in a zlib stream (RFC 1950), for libimage's
 * PNG writer (41h-2). One final block with the fixed Huffman codes: each
 * position looks for its longest earlier match (up to 258 bytes, 32 KiB
 * back) through hash chains of three-byte prefixes, a few dozen links deep;
 * a match of 3 or more is a length and a distance, anything else a literal.
 * Dynamic codes and lazy matching would compress further; screenshots and
 * drawings, mostly runs and repeats, need neither.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "imagedec.h"

#define WINDOW_SIZE (1 << 15)           /* how far back a match may start */
#define HASH_BITS 15
#define CHAIN_MAX 48                    /* links looked at per position */
#define MATCH_MIN 3
#define MATCH_MAX 258

struct bits {
    uint8_t *out;
    size_t n, cap;
    uint32_t acc;                       /* bits not yet written, the first lowest */
    int count;
    int failed;
};

static void put_byte(struct bits *b, uint8_t v)
{
    if (b->n == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        uint8_t *p = realloc(b->out, cap);
        if (!p) {
            b->failed = 1;
            return;
        }
        b->out = p, b->cap = cap;
    }
    b->out[b->n++] = v;
}

/* n bits of v, the lowest first (the order DEFLATE packs everything but
 * Huffman codes in). */
static void put_bits(struct bits *b, uint32_t v, int n)
{
    b->acc |= v << b->count;
    b->count += n;
    while (b->count >= 8) {
        put_byte(b, (uint8_t)b->acc);
        b->acc >>= 8;
        b->count -= 8;
    }
}

/* A Huffman code of n bits, its highest bit first (RFC 1951 3.1.1). */
static void put_code(struct bits *b, uint32_t code, int n)
{
    uint32_t r = 0;
    for (int i = 0; i < n; i++)
        r |= (code >> i & 1) << (n - 1 - i);
    put_bits(b, r, n);
}

/* The fixed literal/length code (RFC 1951 3.2.6). */
static void put_symbol(struct bits *b, int sym)
{
    if (sym < 144)
        put_code(b, 0x30 + (uint32_t)sym, 8);
    else if (sym < 256)
        put_code(b, 0x190 + (uint32_t)(sym - 144), 9);
    else if (sym < 280)
        put_code(b, (uint32_t)(sym - 256), 7);
    else
        put_code(b, 0xC0 + (uint32_t)(sym - 280), 8);
}

static const uint16_t length_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                          35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t length_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                          3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                        8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static void put_match(struct bits *b, int len, int dist)
{
    int l = 28;
    while (length_base[l] > len)
        l--;
    put_symbol(b, 257 + l);
    put_bits(b, (uint32_t)(len - length_base[l]), length_extra[l]);
    int d = 29;
    while (dist_base[d] > dist)
        d--;
    put_code(b, (uint32_t)d, 5);                /* the fixed distance code: 5 bits */
    put_bits(b, (uint32_t)(dist - dist_base[d]), dist_extra[d]);
}

static uint32_t hash3(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]) * 2654435761u >> (32 - HASH_BITS);
}

static uint32_t adler32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, s = 0;
    while (n > 0) {
        size_t k = n < 5552 ? n : 5552;         /* (the most before the sums overflow) */
        n -= k;
        while (k--) {
            a += *p++;
            s += a;
        }
        a %= 65521;
        s %= 65521;
    }
    return s << 16 | a;
}

long deflate_zlib(const uint8_t *in, size_t n, uint8_t **out)
{
    struct bits b = { 0 };
    int32_t *head = malloc(sizeof(int32_t) << HASH_BITS);
    int32_t *prev = malloc(sizeof(int32_t) * WINDOW_SIZE);
    if (!head || !prev) {
        free(head);
        free(prev);
        return -1;
    }
    memset(head, 0xFF, sizeof(int32_t) << HASH_BITS);     /* -1: no earlier position */
    put_byte(&b, 0x78);                         /* zlib: DEFLATE, 32 KiB window */
    put_byte(&b, 0x01);                         /* (fastest level; 0x7801 % 31 == 0) */
    put_bits(&b, 1, 1);                         /* the final block */
    put_bits(&b, 1, 2);                         /* fixed Huffman codes */
    size_t i = 0;
    while (i < n) {
        int best = 0, best_dist = 0;
        if (i + MATCH_MIN <= n) {
            uint32_t h = hash3(in + i);
            size_t limit = n - i < MATCH_MAX ? n - i : MATCH_MAX;
            int32_t c = head[h];
            for (int k = 0; c >= 0 && k < CHAIN_MAX && i - (size_t)c <= WINDOW_SIZE; k++) {
                const uint8_t *p = in + c, *q = in + i;
                if (p[best] == q[best]) {       /* (could it be longer than the best?) */
                    size_t len = 0;
                    while (len < limit && p[len] == q[len])
                        len++;
                    if ((int)len > best) {
                        best = (int)len;
                        best_dist = (int)(i - (size_t)c);
                        if (len == limit)
                            break;
                    }
                }
                c = prev[(size_t)c & (WINDOW_SIZE - 1)];
            }
        }
        size_t step = best >= MATCH_MIN ? (size_t)best : 1;
        if (best >= MATCH_MIN)
            put_match(&b, best, best_dist);
        else
            put_symbol(&b, in[i]);
        for (size_t k = 0; k < step; k++, i++)  /* every position covered goes into the chains */
            if (i + MATCH_MIN <= n) {
                uint32_t h = hash3(in + i);
                prev[i & (WINDOW_SIZE - 1)] = head[h];
                head[h] = (int32_t)i;
            }
    }
    put_symbol(&b, 256);                        /* the block's end */
    if (b.count > 0)
        put_bits(&b, 0, 8 - b.count);           /* to a whole byte */
    uint32_t a = adler32(in, n);
    for (int s = 24; s >= 0; s -= 8)
        put_byte(&b, (uint8_t)(a >> s));
    free(head);
    free(prev);
    if (b.failed) {
        free(b.out);
        return -1;
    }
    *out = b.out;
    return (long)b.n;
}

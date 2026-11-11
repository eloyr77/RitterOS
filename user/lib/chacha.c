/*
 * ChaCha20 and Poly1305 (RFC 8439). SSH's cipher chacha20-poly1305 (an
 * OpenSSH extension) uses them: ChaCha20 encrypts, Poly1305 makes a tag
 * that shows a packet was not changed.
 *
 * ChaCha20: a 4x4 matrix of 32-bit words (four constants, the 256-bit
 * key, a block counter and a nonce) mixed by 20 rounds of additions,
 * XORs and rotations; added to its input, it is 64 bytes of key stream.
 *
 * Poly1305: the message in 16-byte pieces, each read as a number (plus
 * 2^128), added to an accumulator that is multiplied by r modulo
 * 2^130 - 5; the tag is the accumulator plus s, modulo 2^128 (r and s:
 * the key's two halves). Numbers are kept in five 26-bit limbs, so that
 * products fit 64 bits.
 */
#include <string.h>
#include "crypto.h"

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

static uint32_t rotl(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

#define QUARTER(a, b, c, d)                                                        \
    do {                                                                           \
        x[a] += x[b], x[d] = rotl(x[d] ^ x[a], 16);                                \
        x[c] += x[d], x[b] = rotl(x[b] ^ x[c], 12);                                \
        x[a] += x[b], x[d] = rotl(x[d] ^ x[a], 8);                                 \
        x[c] += x[d], x[b] = rotl(x[b] ^ x[c], 7);                                 \
    } while (0)

static void block_function(const uint32_t state[16], uint8_t out[64])
{
    uint32_t x[16];
    memcpy(x, state, sizeof(x));
    for (int i = 0; i < 10; i++) {                  /* 20 rounds: columns, then diagonals */
        QUARTER(0, 4, 8, 12);
        QUARTER(1, 5, 9, 13);
        QUARTER(2, 6, 10, 14);
        QUARTER(3, 7, 11, 15);
        QUARTER(0, 5, 10, 15);
        QUARTER(1, 6, 11, 12);
        QUARTER(2, 7, 8, 13);
        QUARTER(3, 4, 9, 14);
    }
    for (int i = 0; i < 16; i++)
        put_le32(out + 4 * i, x[i] + state[i]);
}

void chacha20(const uint8_t key[32], const uint8_t block[16], const void *in, void *out,
              size_t n)
{
    uint32_t state[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574 };  /* "expand 32-byte k" */
    for (int i = 0; i < 8; i++)
        state[4 + i] = le32(key + 4 * i);
    for (int i = 0; i < 4; i++)
        state[12 + i] = le32(block + 4 * i);
    const uint8_t *src = in;
    uint8_t *dst = out, stream[64];
    while (n) {
        block_function(state, stream);
        size_t take = n < 64 ? n : 64;
        for (size_t i = 0; i < take; i++)
            dst[i] = src[i] ^ stream[i];
        src += take, dst += take, n -= take;
        if (++state[12] == 0)
            state[13]++;                            /* a 64-bit counter */
    }
    memset(stream, 0, sizeof(stream));
}

void poly1305(const uint8_t key[32], const void *message, size_t n, uint8_t tag[16])
{
    const uint32_t m26 = 0x3ffffff;
    /* r, "clamped": some bits cleared, as the RFC says. */
    uint32_t r0 = le32(key) & 0x3ffffff, r1 = (le32(key + 3) >> 2) & 0x3ffff03;
    uint32_t r2 = (le32(key + 6) >> 4) & 0x3ffc0ff, r3 = (le32(key + 9) >> 6) & 0x3f03fff;
    uint32_t r4 = (le32(key + 12) >> 8) & 0x00fffff;
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;  /* 2^130 = 5 (mod p) */
    uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;
    const uint8_t *p = message;
    while (n) {
        uint8_t piece[16];
        uint32_t high = 1 << 24;                    /* the 2^128 above a whole piece */
        if (n >= 16) {
            memcpy(piece, p, 16);
            p += 16, n -= 16;
        } else {
            memset(piece, 0, sizeof(piece));
            memcpy(piece, p, n);
            piece[n] = 1;                           /* the last one: its 1 inside */
            high = 0;
            n = 0;
        }
        h0 += le32(piece) & m26;
        h1 += (le32(piece + 3) >> 2) & m26;
        h2 += (le32(piece + 6) >> 4) & m26;
        h3 += (le32(piece + 9) >> 6) & m26;
        h4 += (le32(piece + 12) >> 8) | high;
        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 +
                      (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 +
                      (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 +
                      (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 +
                      (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 +
                      (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
        uint64_t c = d0 >> 26;
        h0 = (uint32_t)d0 & m26;
        d1 += c, c = d1 >> 26, h1 = (uint32_t)d1 & m26;
        d2 += c, c = d2 >> 26, h2 = (uint32_t)d2 & m26;
        d3 += c, c = d3 >> 26, h3 = (uint32_t)d3 & m26;
        d4 += c, c = d4 >> 26, h4 = (uint32_t)d4 & m26;
        h0 += (uint32_t)c * 5;
        h1 += h0 >> 26, h0 &= m26;
    }
    /* Fully reduce modulo 2^130 - 5: h, or h - p if that is not negative. */
    uint32_t c = h1 >> 26;
    h1 &= m26, h2 += c, c = h2 >> 26;
    h2 &= m26, h3 += c, c = h3 >> 26;
    h3 &= m26, h4 += c, c = h4 >> 26;
    h4 &= m26, h0 += c * 5, c = h0 >> 26;
    h0 &= m26, h1 += c;
    uint32_t g0 = h0 + 5;
    c = g0 >> 26, g0 &= m26;
    uint32_t g1 = h1 + c;
    c = g1 >> 26, g1 &= m26;
    uint32_t g2 = h2 + c;
    c = g2 >> 26, g2 &= m26;
    uint32_t g3 = h3 + c;
    c = g3 >> 26, g3 &= m26;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t use_g = (g4 >> 31) - 1;                /* all ones if h >= p */
    h0 = (h0 & ~use_g) | (g0 & use_g);
    h1 = (h1 & ~use_g) | (g1 & use_g);
    h2 = (h2 & ~use_g) | (g2 & use_g);
    h3 = (h3 & ~use_g) | (g3 & use_g);
    h4 = (h4 & ~use_g) | (g4 & use_g);
    /* As 128 bits, plus s. */
    uint32_t w0 = h0 | h1 << 26, w1 = h1 >> 6 | h2 << 20, w2 = h2 >> 12 | h3 << 14;
    uint32_t w3 = h3 >> 18 | h4 << 8;
    uint64_t f = (uint64_t)w0 + le32(key + 16);
    put_le32(tag, (uint32_t)f);
    f = (uint64_t)w1 + le32(key + 20) + (f >> 32);
    put_le32(tag + 4, (uint32_t)f);
    f = (uint64_t)w2 + le32(key + 24) + (f >> 32);
    put_le32(tag + 8, (uint32_t)f);
    f = (uint64_t)w3 + le32(key + 28) + (f >> 32);
    put_le32(tag + 12, (uint32_t)f);
}

bool crypto_equal(const void *a, const void *b, size_t n)
{
    const volatile uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++)
        d |= x[i] ^ y[i];
    return d == 0;
}

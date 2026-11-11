/*
 * SHA-1 (FIPS 180-4), HMAC-SHA1 (RFC 2104) and PBKDF2-HMAC-SHA1 (RFC 8018),
 * for Wi-Fi (step 24a): WPA2 turns a passphrase into its key with PBKDF2
 * (4,096 rounds, the network's name as the salt) and checks its handshake
 * with HMAC-SHA1. SHA-1's collisions do not touch either use (HMAC needs no
 * collision resistance). Our own code; no branches on the data.
 */
#include <string.h>
#include "crypto.h"

static uint32_t rol(uint32_t x, int n)
{
    return x << n | x >> (32 - n);
}

static void compress(struct sha1 *s, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)
            f = (b & c) | (~b & d), k = 0x5A827999;
        else if (i < 40)
            f = b ^ c ^ d, k = 0x6ED9EBA1;
        else if (i < 60)
            f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
        else
            f = b ^ c ^ d, k = 0xCA62C1D6;
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e;
}

void sha1_init(struct sha1 *s)
{
    static const uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    memcpy(s->h, h, sizeof(h));
    s->length = 0;
    s->used = 0;
}

void sha1_update(struct sha1 *s, const void *data, size_t n)
{
    const uint8_t *p = data;
    s->length += n;
    while (n) {
        size_t k = 64 - s->used < n ? 64 - s->used : n;
        memcpy(s->block + s->used, p, k);
        s->used += k, p += k, n -= k;
        if (s->used == 64) {
            compress(s, s->block);
            s->used = 0;
        }
    }
}

void sha1_final(struct sha1 *s, uint8_t out[SHA1_SIZE])
{
    uint64_t bits = s->length * 8;
    uint8_t pad = 0x80, zero = 0, len[8];
    sha1_update(s, &pad, 1);
    while (s->used != 56)
        sha1_update(s, &zero, 1);
    for (int i = 0; i < 8; i++)
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(s, len, 8);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++)
            out[4 * i + j] = (uint8_t)(s->h[i] >> (24 - 8 * j));
    memset(s, 0, sizeof(*s));
}

void sha1(const void *data, size_t n, uint8_t out[SHA1_SIZE])
{
    struct sha1 s;
    sha1_init(&s);
    sha1_update(&s, data, n);
    sha1_final(&s, out);
}

/* HMAC's two hashes, started with the key's pads (kept for PBKDF2's rounds). */
struct hmac1 {
    struct sha1 inner, outer;
};

static void hmac1_key(struct hmac1 *m, const void *key, size_t n)
{
    uint8_t k[64] = { 0 }, pad[64];
    if (n > 64)
        sha1(key, n, k);
    else
        memcpy(k, key, n);
    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x36;
    sha1_init(&m->inner);
    sha1_update(&m->inner, pad, 64);
    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x5c;
    sha1_init(&m->outer);
    sha1_update(&m->outer, pad, 64);
    memset(k, 0, sizeof(k));
}

static void hmac1(const struct hmac1 *m, const void *data, size_t n, uint8_t out[SHA1_SIZE])
{
    struct sha1 s = m->inner;
    uint8_t inner[SHA1_SIZE];
    sha1_update(&s, data, n);
    sha1_final(&s, inner);
    s = m->outer;
    sha1_update(&s, inner, sizeof(inner));
    sha1_final(&s, out);
}

void hmac_sha1(const void *key, size_t key_length, const void *data, size_t n,
               uint8_t out[SHA1_SIZE])
{
    struct hmac1 m;
    hmac1_key(&m, key, key_length);
    hmac1(&m, data, n, out);
    memset(&m, 0, sizeof(m));
}

void pbkdf2_sha1(const void *password, size_t password_length, const uint8_t *salt,
                 size_t salt_length, uint32_t iterations, uint8_t *out, size_t out_length)
{
    struct hmac1 m;
    hmac1_key(&m, password, password_length);
    for (uint32_t block = 1; out_length; block++) {
        uint8_t first[PBKDF2_SHA1_SALT_MAX + 4], u[SHA1_SIZE], t[SHA1_SIZE];
        size_t n = salt_length < PBKDF2_SHA1_SALT_MAX ? salt_length : PBKDF2_SHA1_SALT_MAX;
        memcpy(first, salt, n);
        first[n] = (uint8_t)(block >> 24), first[n + 1] = (uint8_t)(block >> 16);
        first[n + 2] = (uint8_t)(block >> 8), first[n + 3] = (uint8_t)block;
        hmac1(&m, first, n + 4, u);
        memcpy(t, u, sizeof(t));
        for (uint32_t i = 1; i < iterations; i++) {
            hmac1(&m, u, sizeof(u), u);
            for (int k = 0; k < SHA1_SIZE; k++)
                t[k] ^= u[k];
        }
        size_t take = out_length < SHA1_SIZE ? out_length : SHA1_SIZE;
        memcpy(out, t, take);
        out += take, out_length -= take;
        memset(t, 0, sizeof(t));
        memset(u, 0, sizeof(u));
    }
    memset(&m, 0, sizeof(m));
}

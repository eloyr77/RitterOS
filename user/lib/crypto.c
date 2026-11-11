/*
 * SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104) and PBKDF2-HMAC-SHA256
 * (RFC 8018), for storing passwords as salted, slow hashes. Our own code;
 * the test compares it with Python's hashlib.
 */
#include <stdint.h>
#include <string.h>
#include "ritteros.h"

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2,
};

static uint32_t rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static void compress(uint32_t h[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16 |
               (uint32_t)block[4 * i + 2] << 8 | block[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] +
                      w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
}

void sha256_init(struct sha256 *s)
{
    static const uint32_t start[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, start, sizeof(start));
    s->length = 0;
    s->used = 0;
}

void sha256_update(struct sha256 *s, const void *data, size_t n)
{
    const uint8_t *p = data;
    s->length += n;
    while (n) {
        size_t take = 64 - s->used < n ? 64 - s->used : n;
        memcpy(s->block + s->used, p, take);
        s->used += take, p += take, n -= take;
        if (s->used == 64) {
            compress(s->h, s->block);
            s->used = 0;
        }
    }
}

void sha256_final(struct sha256 *s, uint8_t out[SHA256_SIZE])
{
    uint64_t bits = s->length * 8;
    uint8_t pad = 0x80, zero = 0, length[8];
    sha256_update(s, &pad, 1);
    while (s->used != 56)
        sha256_update(s, &zero, 1);
    for (int i = 0; i < 8; i++)
        length[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(s, length, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

void sha256(const void *data, size_t n, uint8_t out[SHA256_SIZE])
{
    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, data, n);
    sha256_final(&s, out);
}

/* HMAC with the key's inner and outer states prepared once (PBKDF2 uses
 * the same key thousands of times). */
struct hmac {
    struct sha256 inner, outer;
};

static void hmac_key(struct hmac *m, const uint8_t *key, size_t n)
{
    uint8_t k[64] = {0}, pad[64];
    if (n > 64)
        sha256(key, n, k);
    else
        memcpy(k, key, n);
    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x36;
    sha256_init(&m->inner);
    sha256_update(&m->inner, pad, 64);
    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x5c;
    sha256_init(&m->outer);
    sha256_update(&m->outer, pad, 64);
}

static void hmac(const struct hmac *m, const void *data, size_t n, uint8_t out[SHA256_SIZE])
{
    struct sha256 s = m->inner;
    uint8_t inner[SHA256_SIZE];
    sha256_update(&s, data, n);
    sha256_final(&s, inner);
    s = m->outer;
    sha256_update(&s, inner, sizeof(inner));
    sha256_final(&s, out);
}

void hmac_sha256(const void *key, size_t key_length, const void *data, size_t n,
                 uint8_t out[SHA256_SIZE])
{
    struct hmac m;
    hmac_key(&m, key, key_length);
    hmac(&m, data, n, out);
}

void pbkdf2_sha256(const void *password, size_t password_length, const uint8_t *salt,
                   size_t salt_length, uint32_t iterations, uint8_t *out, size_t out_length)
{
    struct hmac m;
    hmac_key(&m, password, password_length);
    for (uint32_t block = 1; out_length; block++) {
        uint8_t first[PBKDF2_SALT_MAX + 4], u[SHA256_SIZE], t[SHA256_SIZE];
        size_t n = salt_length < PBKDF2_SALT_MAX ? salt_length : PBKDF2_SALT_MAX;
        memcpy(first, salt, n);
        first[n] = (uint8_t)(block >> 24), first[n + 1] = (uint8_t)(block >> 16);
        first[n + 2] = (uint8_t)(block >> 8), first[n + 3] = (uint8_t)block;
        hmac(&m, first, n + 4, u);
        memcpy(t, u, sizeof(t));
        for (uint32_t i = 1; i < iterations; i++) {
            hmac(&m, u, sizeof(u), u);
            for (int k = 0; k < SHA256_SIZE; k++)
                t[k] ^= u[k];
        }
        size_t take = out_length < SHA256_SIZE ? out_length : SHA256_SIZE;
        memcpy(out, t, take);
        out += take, out_length -= take;
    }
}

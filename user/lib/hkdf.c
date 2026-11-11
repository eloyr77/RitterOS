/*
 * SHA-384, HMAC-SHA384 and HKDF (RFC 5869) with SHA-256 or SHA-384, for TLS
 * 1.3's key schedule (step 23a). SHA-384 is SHA-512 with other initial
 * values, cut to 48 bytes (FIPS 180-4).
 */
#include <string.h>
#include "crypto.h"
#include "ritteros.h"

void sha384_init(struct sha512 *s)
{
    static const uint64_t initial[8] = {
        0xcbbb9d5dc1059ed8, 0x629a292a367cd507, 0x9159015a3070dd17, 0x152fecd8f70e5939,
        0x67332667ffc00b31, 0x8eb44a8768581511, 0xdb0c2e0d64f98fa7, 0x47b5481dbefa4fa4,
    };
    sha512_init(s);
    memcpy(s->h, initial, sizeof(initial));
}

void sha384_final(struct sha512 *s, uint8_t out[SHA384_SIZE])
{
    uint8_t full[SHA512_SIZE];
    sha512_final(s, full);
    memcpy(out, full, SHA384_SIZE);
}

void sha384(const void *data, size_t n, uint8_t out[SHA384_SIZE])
{
    struct sha512 s;
    sha384_init(&s);
    sha512_update(&s, data, n);
    sha384_final(&s, out);
}

/* HMAC (RFC 2104) with SHA-384: 128-byte blocks. */
void hmac_sha384(const void *key, size_t key_length, const void *data, size_t n,
                 uint8_t out[SHA384_SIZE])
{
    uint8_t k[128] = { 0 }, pad[128], inner[SHA384_SIZE];
    if (key_length > sizeof(k))
        sha384(key, key_length, k);
    else
        memcpy(k, key, key_length);
    struct sha512 s;
    for (int i = 0; i < 128; i++)
        pad[i] = k[i] ^ 0x36;
    sha384_init(&s);
    sha512_update(&s, pad, sizeof(pad));
    sha512_update(&s, data, n);
    sha384_final(&s, inner);
    for (int i = 0; i < 128; i++)
        pad[i] = k[i] ^ 0x5C;
    sha384_init(&s);
    sha512_update(&s, pad, sizeof(pad));
    sha512_update(&s, inner, sizeof(inner));
    sha384_final(&s, out);
    memset(k, 0, sizeof(k));
}

/* HMAC with the hash of `size` bytes (32: SHA-256, 48: SHA-384). */
static void hmac_of(size_t size, const void *key, size_t key_length, const void *data, size_t n,
                    uint8_t *out)
{
    if (size == SHA384_SIZE)
        hmac_sha384(key, key_length, data, n, out);
    else
        hmac_sha256(key, key_length, data, n, out);
}

void hkdf_extract(size_t size, const void *salt, size_t salt_length, const void *ikm,
                  size_t ikm_length, uint8_t *prk)
{
    uint8_t zeros[SHA384_SIZE] = { 0 };
    if (!salt_length)                           /* (no salt: a hash length of zeros) */
        salt = zeros, salt_length = size;
    hmac_of(size, salt, salt_length, ikm, ikm_length, prk);
}

int hkdf_expand(size_t size, const uint8_t *prk, const void *info, size_t info_length,
                uint8_t *out, size_t length)
{
    uint8_t t[SHA384_SIZE], block[SHA384_SIZE + HKDF_INFO_MAX + 1];
    if (length > 255 * size || info_length > HKDF_INFO_MAX)
        return -1;
    size_t t_length = 0;
    for (uint8_t i = 1; length; i++) {          /* T(i) = HMAC(PRK, T(i-1) | info | i) */
        memcpy(block, t, t_length);
        memcpy(block + t_length, info, info_length);
        block[t_length + info_length] = i;
        hmac_of(size, prk, size, block, t_length + info_length + 1, t);
        t_length = size;
        size_t k = length < size ? length : size;
        memcpy(out, t, k);
        out += k;
        length -= k;
    }
    memset(t, 0, sizeof(t));
    return 0;
}

/*
 * Wi-Fi's cryptography beyond SHA-1 (step 24a): IEEE 802.11's key
 * derivations (the PRF for WPA2, KDF-SHA256 for WPA3), AES key wrap (RFC
 * 3394: the group key comes wrapped in the handshake), AES-CMAC (RFC 4493:
 * WPA3's handshake check, protected management frames); AES-CCM is in
 * common/aes.c (the kernel protects frames with it). Our own code, on its cipher;
 * nothing branches on secret data (the CMAC subkeys' carry is a mask).
 */
#include <string.h>
#include "crypto.h"
#include "ritteros.h"

void ieee80211_prf(const void *key, size_t key_length, const char *label, const void *data,
                   size_t n, uint8_t *out, size_t out_length)
{
    uint8_t in[64 + 1 + IEEE80211_DATA_MAX + 1], h[SHA1_SIZE];
    size_t l = strlen(label);
    if (l > 64)
        l = 64;
    if (n > IEEE80211_DATA_MAX)
        n = IEEE80211_DATA_MAX;
    memcpy(in, label, l);
    in[l] = 0;                                  /* A || Y=0 || B || i */
    memcpy(in + l + 1, data, n);
    for (uint8_t i = 0; out_length; i++) {
        in[l + 1 + n] = i;
        hmac_sha1(key, key_length, in, l + 2 + n, h);
        size_t take = out_length < SHA1_SIZE ? out_length : SHA1_SIZE;
        memcpy(out, h, take);
        out += take, out_length -= take;
    }
    memset(h, 0, sizeof(h));
}

void ieee80211_kdf_sha256(const void *key, size_t key_length, const char *label,
                          const void *context, size_t n, uint8_t *out, size_t out_length)
{
    uint8_t in[2 + 64 + IEEE80211_DATA_MAX + 2], h[SHA256_SIZE];
    size_t l = strlen(label);
    if (l > 64)
        l = 64;
    if (n > IEEE80211_DATA_MAX)
        n = IEEE80211_DATA_MAX;
    if (out_length > IEEE80211_KDF_MAX)
        out_length = IEEE80211_KDF_MAX;
    uint16_t bits = (uint16_t)(out_length * 8);
    memcpy(in + 2, label, l);                   /* i || label || context || length */
    memcpy(in + 2 + l, context, n);
    in[2 + l + n] = (uint8_t)bits, in[3 + l + n] = (uint8_t)(bits >> 8);
    for (uint16_t i = 1; out_length; i++) {
        in[0] = (uint8_t)i, in[1] = (uint8_t)(i >> 8);
        hmac_sha256(key, key_length, in, 4 + l + n, h);
        size_t take = out_length < SHA256_SIZE ? out_length : SHA256_SIZE;
        memcpy(out, h, take);
        out += take, out_length -= take;
    }
    memset(h, 0, sizeof(h));
}

/* --- AES key wrap --- */

static const uint8_t WRAP_IV[8] = { 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6 };

void aes_key_wrap(const struct aes *kek, const uint8_t *in, size_t n, uint8_t *out)
{
    size_t blocks = n / 8;
    uint8_t a[8], b[16];
    memcpy(a, WRAP_IV, 8);
    memmove(out + 8, in, n);
    for (int j = 0; j <= 5; j++)
        for (size_t i = 1; i <= blocks; i++) {
            memcpy(b, a, 8);
            memcpy(b + 8, out + 8 * i, 8);
            aes_encrypt(kek, b, b);
            uint64_t t = (uint64_t)(blocks * (size_t)j + i);
            for (int k = 0; k < 8; k++)
                a[k] = b[k] ^ (uint8_t)(t >> (56 - 8 * k));
            memcpy(out + 8 * i, b + 8, 8);
        }
    memcpy(out, a, 8);
    memset(b, 0, sizeof(b));
}

bool aes_key_unwrap(const struct aes *kek, const uint8_t *in, size_t n, uint8_t *out)
{
    if (n < 24 || n % 8 || n - 8 > AES_WRAP_MAX) {
        memset(out, 0, n >= 8 ? n - 8 : 0);
        return false;
    }
    size_t blocks = n / 8 - 1;
    uint8_t a[8], b[16], r[AES_WRAP_MAX];
    memcpy(a, in, 8);
    memcpy(r, in + 8, n - 8);
    for (int j = 5; j >= 0; j--)
        for (size_t i = blocks; i >= 1; i--) {
            uint64_t t = (uint64_t)(blocks * (size_t)j + i);
            for (int k = 0; k < 8; k++)
                b[k] = a[k] ^ (uint8_t)(t >> (56 - 8 * k));
            memcpy(b + 8, r + 8 * (i - 1), 8);
            aes_decrypt(kek, b, b);
            memcpy(a, b, 8);
            memcpy(r + 8 * (i - 1), b + 8, 8);
        }
    bool ok = crypto_equal(a, WRAP_IV, 8);
    if (ok)
        memcpy(out, r, n - 8);
    else
        memset(out, 0, n - 8);
    memset(r, 0, sizeof(r));
    memset(b, 0, sizeof(b));
    return ok;
}

/* AES-CMAC (aes_cmac) lives in common/aes.c: the kernel checks BIP with it too. */

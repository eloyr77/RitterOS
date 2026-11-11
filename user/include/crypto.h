/*
 * Cryptography for SSH (user/lib/sha512.c, chacha.c, c25519.c), TLS
 * (aes.c, hkdf.c, verify.c; item 23) and Wi-Fi (sha1.c, wpacrypto.c; item
 * 24), our own code from the RFCs, IEEE 802.11 and FIPS
 * standards, checked against their test vectors (scripts/crypto_ref.py,
 * user/test/tlscrypto.c). SHA-256, HMAC-SHA256 and PBKDF2 are in ritteros.h.
 * Secret-dependent work takes the same time for every secret (no branches or
 * table lookups on secret data: AES is bitsliced); checking signatures works
 * on public data only and need not.
 */
#ifndef RITTEROS_CRYPTO_H
#define RITTEROS_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* SHA-512 (FIPS 180-4). */
#define SHA512_SIZE 64
struct sha512 {
    uint64_t h[8];
    uint64_t length;                    /* bytes so far */
    uint8_t block[128];
    size_t used;
};
void sha512_init(struct sha512 *s);
void sha512_update(struct sha512 *s, const void *data, size_t n);
void sha512_final(struct sha512 *s, uint8_t out[SHA512_SIZE]);
void sha512(const void *data, size_t n, uint8_t out[SHA512_SIZE]);

/* ChaCha20 (RFC 8439): data XOR the key stream, from the 16 bytes that are
 * the state's last four words: a 64-bit block counter (little-endian;
 * RFC 8439's 32-bit counter and 96-bit nonce fit the same bytes) and the
 * nonce. in and out may be the same. */
void chacha20(const uint8_t key[32], const uint8_t block[16], const void *in, void *out,
              size_t n);

/* Poly1305 (RFC 8439): a 16-byte tag of a message under a one-time key. */
void poly1305(const uint8_t key[32], const void *message, size_t n, uint8_t tag[16]);

/* Equal without a branch on the bytes (for tags and hashes). */
bool crypto_equal(const void *a, const void *b, size_t n);

/* X25519 (RFC 7748): out = scalar * point (32 bytes each, little-endian);
 * the public key of a secret is x25519_base(secret). */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);

/* Ed25519 (RFC 8032): a 32-byte secret seed, its 32-byte public key,
 * 64-byte signatures. */
void ed25519_public(uint8_t public_key[32], const uint8_t seed[32]);
void ed25519_sign(uint8_t signature[64], const uint8_t seed[32], const uint8_t public_key[32],
                  const void *message, size_t n);
bool ed25519_verify(const uint8_t signature[64], const uint8_t public_key[32],
                    const void *message, size_t n);

/* AES (FIPS 197) and GCM (NIST SP 800-38D), for TLS (23a). A key of 16
 * (AES-128) or 32 bytes (AES-256); aes_encrypt4 does four blocks at once.
 * In common/aes.c with CCM (shared with the kernel; common/aes.h declares
 * the same: keep them in step). */
struct aes {
    uint64_t key[15][8];                /* each round key, bitsliced */
    int rounds;
};
void aes_init(struct aes *a, const uint8_t *key, size_t length);
void aes_encrypt(const struct aes *a, const uint8_t in[16], uint8_t out[16]);
void aes_encrypt4(const struct aes *a, const uint8_t in[64], uint8_t out[64]);
/* The inverse cipher (24a: WPA's key unwrap). */
void aes_decrypt(const struct aes *a, const uint8_t in[16], uint8_t out[16]);
void aes_decrypt4(const struct aes *a, const uint8_t in[64], uint8_t out[64]);
/* GCM with a 12-byte IV and a 16-byte tag; open checks the tag first and
 * decrypts nothing if it is wrong (false). in and out may be the same. */
void aes_gcm_seal(const struct aes *a, const uint8_t iv[12], const void *aad, size_t aad_n,
                  const void *in, void *out, size_t n, uint8_t tag[16]);
bool aes_gcm_open(const struct aes *a, const uint8_t iv[12], const void *aad, size_t aad_n,
                  const void *in, void *out, size_t n, const uint8_t tag[16]);

/* SHA-384 (FIPS 180-4: SHA-512 with other initial values, 48 bytes), its
 * HMAC, and HKDF (RFC 5869) with SHA-256 or SHA-384 (size 32 or 48): the
 * key schedule of TLS 1.3 (23a). */
#define SHA384_SIZE 48
#define HKDF_INFO_MAX 512
void sha384_init(struct sha512 *s);
void sha384_final(struct sha512 *s, uint8_t out[SHA384_SIZE]);
void sha384(const void *data, size_t n, uint8_t out[SHA384_SIZE]);
void hmac_sha384(const void *key, size_t key_length, const void *data, size_t n,
                 uint8_t out[SHA384_SIZE]);
void hkdf_extract(size_t size, const void *salt, size_t salt_length, const void *ikm,
                  size_t ikm_length, uint8_t *prk);
int hkdf_expand(size_t size, const uint8_t *prk, const void *info, size_t info_length,
                uint8_t *out, size_t length);

/* Checking signatures (23b), public data only: RSA PKCS #1 v1.5 and PSS
 * (salt as long as the hash; RFC 8017) with a SHA-256, -384 or -512 digest,
 * the modulus and exponent as big-endian bytes; ECDSA on P-256 or P-384
 * (bits) with an uncompressed public point (04 x y) and r, s as big-endian
 * bytes. True for a good signature. */
bool rsa_verify_pkcs1(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                      const uint8_t *digest, size_t dlen, const uint8_t *sig, size_t slen);
bool rsa_verify_pss(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                    const uint8_t *digest, size_t dlen, const uint8_t *sig, size_t slen);
bool ecdsa_verify(int bits, const uint8_t *pub, size_t pub_len, const uint8_t *digest,
                  size_t dlen, const uint8_t *r, size_t rlen, const uint8_t *s, size_t slen);

/* --- Wi-Fi (item 24) --- */

/* SHA-1 (FIPS 180-4), HMAC-SHA1 (RFC 2104), PBKDF2-HMAC-SHA1 (RFC 8018;
 * salt up to PBKDF2_SHA1_SALT_MAX bytes): WPA2's passphrase to key, with
 * the network's name as the salt and 4,096 rounds. */
#define SHA1_SIZE 20
#define PBKDF2_SHA1_SALT_MAX 64
struct sha1 {
    uint32_t h[5];
    uint64_t length;                    /* bytes so far */
    uint8_t block[64];
    size_t used;
};
void sha1_init(struct sha1 *s);
void sha1_update(struct sha1 *s, const void *data, size_t n);
void sha1_final(struct sha1 *s, uint8_t out[SHA1_SIZE]);
void sha1(const void *data, size_t n, uint8_t out[SHA1_SIZE]);
void hmac_sha1(const void *key, size_t key_length, const void *data, size_t n,
               uint8_t out[SHA1_SIZE]);
void pbkdf2_sha1(const void *password, size_t password_length, const uint8_t *salt,
                 size_t salt_length, uint32_t iterations, uint8_t *out, size_t out_length);

/* IEEE 802.11's key derivations: the PRF (12.7.1.2: HMAC-SHA1 over the
 * label, a zero byte, the data and a counter; WPA2's keys) and KDF-SHA256
 * (12.7.1.7.2: HMAC-SHA256 over a counter, the label, the context and the
 * length in bits, both 16-bit little-endian; WPA3's). out_length bytes, at
 * most IEEE80211_KDF_MAX; data/context at most IEEE80211_DATA_MAX bytes. */
#define IEEE80211_KDF_MAX 128
#define IEEE80211_DATA_MAX 256
void ieee80211_prf(const void *key, size_t key_length, const char *label, const void *data,
                   size_t n, uint8_t *out, size_t out_length);
void ieee80211_kdf_sha256(const void *key, size_t key_length, const char *label,
                          const void *context, size_t n, uint8_t *out, size_t out_length);

/* AES key wrap (RFC 3394): n bytes of key data (a multiple of 8, at least
 * 16, at most AES_WRAP_MAX) into n + 8 bytes; unwrap takes the n + 8 and
 * gives n back, or false if the check value is wrong (then out is zeroed). */
#define AES_WRAP_MAX 256
void aes_key_wrap(const struct aes *kek, const uint8_t *in, size_t n, uint8_t *out);
bool aes_key_unwrap(const struct aes *kek, const uint8_t *in, size_t n, uint8_t *out);

/* AES-CMAC (RFC 4493): a 16-byte tag. WPA3's handshake check and the
 * protection of management frames (BIP) use it. */
void aes_cmac(const struct aes *a, const void *data, size_t n, uint8_t tag[16]);

/* AES-CCM (RFC 3610) with a 13-byte nonce (L = 2: messages up to 65,535
 * bytes) and a tag of tag_length bytes (4 to 16, even): CCMP, Wi-Fi's frame
 * protection, uses 8. open gives out only if the tag is right; if not
 * (false), out is zeroed. in and out may be the same. */
void aes_ccm_seal(const struct aes *a, const uint8_t nonce[13], const void *aad, size_t aad_n,
                  const void *in, void *out, size_t n, uint8_t *tag, size_t tag_length);
bool aes_ccm_open(const struct aes *a, const uint8_t nonce[13], const void *aad, size_t aad_n,
                  const void *in, void *out, size_t n, const uint8_t *tag, size_t tag_length);

/* SAE, WPA3-Personal's key exchange (802.11-2020 12.4, sae.c, 24b), group
 * 19 (P-256), constant-time where the password is involved. The password
 * element (PWE) by hunting and pecking (both stations' MAC addresses and the
 * password, SAE_HUNT_ROUNDS rounds always run) or by hash to element (sae_pt
 * once per network: its name, the password and an optional identifier;
 * then sae_pwe_from_pt per peer). sae_commit makes our scalar and element
 * (random rand and mask, or the given ones: tests); sae_process takes the
 * peer's (refused: a scalar outside (1, r), an element off the curve, our
 * own commit reflected) and makes KCK, PMK and PMKID. Points are x || y,
 * numbers big-endian. */
#define SAE_PASSWORD_MAX 256
#define SAE_HUNT_ROUNDS 40
struct sae {
    uint8_t pwe[64];
    uint8_t rand[32], scalar[32], element[64];
    uint8_t peer_scalar[32], peer_element[64];
    uint8_t kck[32], pmk[32], pmkid[16];
};
bool sae_pwe_hunt(struct sae *s, const uint8_t mac_a[6], const uint8_t mac_b[6],
                  const void *password, size_t n);
void sae_pt(uint8_t pt[64], const void *ssid, size_t ssid_n, const void *password, size_t n,
            const void *id, size_t id_n);
bool sae_pwe_from_pt(struct sae *s, const uint8_t pt[64], const uint8_t mac_a[6],
                     const uint8_t mac_b[6]);
bool sae_commit(struct sae *s, const uint8_t *rand, const uint8_t *mask);
bool sae_process(struct sae *s, const uint8_t peer_scalar[32], const uint8_t peer_element[64]);
/* Our confirm (send-confirm, our commit, then the peer's), and the check of
 * the peer's (its counter, its commit, then ours). */
void sae_confirm(const struct sae *s, uint16_t send_confirm, uint8_t out[32]);
bool sae_check_confirm(const struct sae *s, uint16_t peer_send_confirm, const uint8_t confirm[32]);
/* For tests (RFC 9380's vectors): SSWU of u (a number below p) and a sum. */
void p256_sswu(const uint8_t u[32], uint8_t point[64]);
bool p256_add(const uint8_t a[64], const uint8_t b[64], uint8_t sum[64]);

#endif

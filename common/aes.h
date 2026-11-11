/*
 * AES (FIPS 197) with GCM and CCM, common/aes.c: shared by the programs'
 * library (crypto.h declares the same, keep the two in step) and the kernel
 * (Wi-Fi's frame protection, 24c). The work takes the same time for every
 * key and message (bitsliced AES).
 */
#ifndef RITTEROS_COMMON_AES_H
#define RITTEROS_COMMON_AES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct aes {
    uint64_t key[15][8];                /* each round key, bitsliced */
    int rounds;
};
void aes_init(struct aes *a, const uint8_t *key, size_t length);
void aes_encrypt(const struct aes *a, const uint8_t in[16], uint8_t out[16]);
void aes_encrypt4(const struct aes *a, const uint8_t in[64], uint8_t out[64]);
void aes_decrypt(const struct aes *a, const uint8_t in[16], uint8_t out[16]);
void aes_decrypt4(const struct aes *a, const uint8_t in[64], uint8_t out[64]);
void aes_gcm_seal(const struct aes *a, const uint8_t iv[12], const void *aad, size_t aad_n,
                  const void *in, void *out, size_t n, uint8_t tag[16]);
bool aes_gcm_open(const struct aes *a, const uint8_t iv[12], const void *aad, size_t aad_n,
                  const void *in, void *out, size_t n, const uint8_t tag[16]);
/* AES-CMAC (RFC 4493): the 16-byte tag of n bytes. */
void aes_cmac(const struct aes *a, const void *data, size_t n, uint8_t tag[16]);
void aes_ccm_seal(const struct aes *a, const uint8_t nonce[13], const void *aad, size_t aad_n,
                  const void *in, void *out, size_t n, uint8_t *tag, size_t tag_length);
bool aes_ccm_open(const struct aes *a, const uint8_t nonce[13], const void *aad, size_t aad_n,
                  const void *in, void *out, size_t n, const uint8_t *tag, size_t tag_length);

#endif

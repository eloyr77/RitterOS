/*
 * XTS-AES (see xts.h). Block j of a data unit is encrypted as
 * C = E(K1, P xor T) xor T with T = E(K2, unit) * alpha^j in GF(2^128)
 * (little-endian, the polynomial x^128 + x^7 + x^2 + x + 1); four blocks
 * go through AES at once (aes_encrypt4, the bitsliced code's width).
 */
#include "xts.h"

void xts_init(struct xts *x, const uint8_t key[64])
{
    aes_init(&x->data, key, 32);
    aes_init(&x->tweak, key + 32, 32);
}

/* T * alpha: a shift left by one bit across the 16 bytes, folding the
 * carry back in as 0x87. */
static void next_tweak(uint8_t t[16])
{
    unsigned carry = t[15] >> 7;
    for (int i = 15; i > 0; i--)
        t[i] = (uint8_t)(t[i] << 1 | t[i - 1] >> 7);
    t[0] = (uint8_t)(t[0] << 1 ^ (carry ? 0x87 : 0));
}

static void crypt(const struct xts *x, uint64_t unit, const uint8_t *in, uint8_t *out, size_t n,
                  int decrypt)
{
    uint8_t t[16] = {0}, tweaks[64], buf[64];
    for (int i = 0; i < 8; i++)
        t[i] = (uint8_t)(unit >> (8 * i));
    aes_encrypt(&x->tweak, t, t);
    for (size_t at = 0; at < n;) {
        size_t blocks = n - at >= 64 ? 4 : 1;
        for (size_t b = 0; b < blocks; b++) {
            for (int i = 0; i < 16; i++) {
                tweaks[16 * b + i] = t[i];
                buf[16 * b + i] = in[at + 16 * b + i] ^ t[i];
            }
            next_tweak(t);
        }
        if (blocks == 4) {
            if (decrypt)
                aes_decrypt4(&x->data, buf, buf);
            else
                aes_encrypt4(&x->data, buf, buf);
        } else if (decrypt) {
            aes_decrypt(&x->data, buf, buf);
        } else {
            aes_encrypt(&x->data, buf, buf);
        }
        for (size_t i = 0; i < 16 * blocks; i++)
            out[at + i] = buf[i] ^ tweaks[i];
        at += 16 * blocks;
    }
}

void xts_encrypt(const struct xts *x, uint64_t unit, const void *in, void *out, size_t n)
{
    crypt(x, unit, in, out, n, 0);
}

void xts_decrypt(const struct xts *x, uint64_t unit, const void *in, void *out, size_t n)
{
    crypt(x, unit, in, out, n, 1);
}

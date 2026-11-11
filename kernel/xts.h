/*
 * XTS-AES (IEEE 1619, NIST SP 800-38E), kernel/xts.c: how the swap file's
 * pages are encrypted (35a), as disk encryption does. Each page is one data
 * unit whose tweak is its place in the file (the slot), so equal pages at
 * two places look unrelated on the disk. Two AES-256 keys: one for the data,
 * one for the tweak. No ciphertext stealing: lengths are multiples of 16.
 * Needs only common/aes.c (swap_shell_test builds it on the Mac and compares
 * it with LibreSSL's aes-256-xts).
 */
#ifndef RITTEROS_XTS_H
#define RITTEROS_XTS_H

#include <stddef.h>
#include <stdint.h>
#include "aes.h"

struct xts {
    struct aes data, tweak;
};

/* key: 64 bytes, the data key then the tweak key. */
void xts_init(struct xts *x, const uint8_t key[64]);

/* n bytes (a multiple of 16, at least 16) of data unit `unit`, from in to
 * out (they may be the same buffer). */
void xts_encrypt(const struct xts *x, uint64_t unit, const void *in, void *out, size_t n);
void xts_decrypt(const struct xts *x, uint64_t unit, const void *in, void *out, size_t n);

#endif

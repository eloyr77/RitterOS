/*
 * AES (FIPS 197), GCM (NIST SP 800-38D) for TLS (step 23a) and CCM (RFC
 * 3610) for Wi-Fi's frames (24a), our own code, shared by the programs'
 * library and the kernel (24c: it protects Wi-Fi frames itself; common/,
 * like crc32.c, but not in the bootloader).
 * Like the rest of crypto.h, the work takes the same time for every key and
 * every message: no table lookups or branches on secret data. AES is
 * "bitsliced": four blocks at once, held as eight 64-bit planes -- plane k
 * has bit k of every byte (bit 16*block + byte) -- so each step is a fixed
 * sequence of word operations. The S-box is computed, not looked up: the
 * inverse in GF(2^8) as x^254 (four multiplications, the squarings), then
 * FIPS 197's affine map. GHASH multiplies in GF(2^128) bit by bit with masks.
 * The inverse cipher (24a: WPA's key unwrap) is the same in reverse: the
 * affine map undone before the inverse, rows shifted the other way, and
 * InvMixColumns as a small step before MixColumns (FIPS 197's inverse
 * matrix is MixColumns' times 04x^2 + 05).
 */
#include "aes.h"
#include "string.h"

typedef uint64_t planes[8];

/* Equal without a branch on the bytes (tags). */
static bool same_bytes(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++)
        d |= x[i] ^ y[i];
    return d == 0;
}

/* Each 16-bit group's mask, repeated for the four blocks. */
#define REP(x) ((uint64_t)(x) * 0x0001000100010001ULL)

/* --- GF(2^8) on planes: x^8 = x^4 + x^3 + x + 1 --- */

static void gf_mul(const uint64_t *a, const uint64_t *b, uint64_t *out)
{
    uint64_t p[15] = { 0 };
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            p[i + j] ^= a[i] & b[j];
    for (int k = 14; k >= 8; k--) {             /* x^k = x^(k-8) * (x^4 + x^3 + x + 1) */
        p[k - 4] ^= p[k];
        p[k - 5] ^= p[k];
        p[k - 7] ^= p[k];
        p[k - 8] ^= p[k];
    }
    memcpy(out, p, 8 * sizeof(uint64_t));
}

static void sub_bytes(uint64_t *x)
{
    uint64_t x2[8], x3[8], x6[8], x12[8], x15[8], t[8], inv[8];
    gf_mul(x, x, x2);
    gf_mul(x2, x, x3);
    gf_mul(x3, x3, x6);
    gf_mul(x6, x6, x12);
    gf_mul(x12, x3, x15);
    gf_mul(x15, x15, t);                        /* x^30 */
    gf_mul(t, t, t);                            /* x^60 */
    gf_mul(t, t, t);                            /* x^120 */
    gf_mul(t, t, t);                            /* x^240 */
    gf_mul(t, x12, t);                          /* x^252 */
    gf_mul(t, x2, inv);                         /* x^254: the inverse (0 for 0) */
    for (int i = 0; i < 8; i++)                 /* the affine map, then 0x63 */
        x[i] = inv[i] ^ inv[(i + 4) & 7] ^ inv[(i + 5) & 7] ^ inv[(i + 6) & 7] ^
               inv[(i + 7) & 7] ^ ((0x63 >> i) & 1 ? ~0ULL : 0);
}

/* The inverse S-box: the affine map undone (and 0x05), then the inverse. */
static void inv_sub_bytes(uint64_t *x)
{
    uint64_t y[8];
    for (int i = 0; i < 8; i++)
        y[i] = x[(i + 2) & 7] ^ x[(i + 5) & 7] ^ x[(i + 7) & 7] ^ ((0x05 >> i) & 1 ? ~0ULL : 0);
    uint64_t x2[8], x3[8], x6[8], x12[8], x15[8], t[8];
    gf_mul(y, y, x2);
    gf_mul(x2, y, x3);
    gf_mul(x3, x3, x6);
    gf_mul(x6, x6, x12);
    gf_mul(x12, x3, x15);
    gf_mul(x15, x15, t);                        /* x^30 */
    gf_mul(t, t, t);
    gf_mul(t, t, t);
    gf_mul(t, t, t);                            /* x^240 */
    gf_mul(t, x12, t);                          /* x^252 */
    gf_mul(t, x2, x);                           /* x^254 */
}

/* Byte i of a block is row i % 4 of column i / 4 (FIPS 197's order). */
static uint64_t shift_rows1(uint64_t p)
{
    return (p & REP(0x1111)) |                                          /* row 0 stays */
           ((p >> 4) & REP(0x0222)) | ((p << 12) & REP(0x2000)) |       /* row 1: one column */
           ((p >> 8) & REP(0x0044)) | ((p << 8) & REP(0x4400)) |        /* row 2: two */
           ((p << 4) & REP(0x8880)) | ((p >> 12) & REP(0x0008));        /* row 3: three */
}

/* The other way: row r moves r columns to the right. */
static uint64_t inv_shift_rows1(uint64_t p)
{
    return (p & REP(0x1111)) |
           ((p << 4) & REP(0x2220)) | ((p >> 12) & REP(0x0002)) |
           ((p >> 8) & REP(0x0044)) | ((p << 8) & REP(0x4400)) |
           ((p >> 4) & REP(0x0888)) | ((p << 12) & REP(0x8000));
}

/* The row below in the same column (row 3 takes row 0). */
static uint64_t rot1(uint64_t p)
{
    return ((p >> 1) & REP(0x7777)) | ((p << 3) & REP(0x8888));
}

static uint64_t rot2(uint64_t p)
{
    return ((p >> 2) & REP(0x3333)) | ((p << 2) & REP(0xCCCC));
}

static void mix_columns(uint64_t *x)
{
    /* b = 2(a_r + a_r+1) + a_r+1 + a_r+2 + a_r+3, per column. */
    uint64_t r1[8], s[8], t[8];
    for (int i = 0; i < 8; i++) {
        r1[i] = rot1(x[i]);
        s[i] = x[i] ^ r1[i];
        t[i] = r1[i] ^ rot2(x[i] ^ r1[i]);      /* a_r+1 + a_r+2 + a_r+3 */
    }
    uint64_t hi = s[7];                         /* 2s: shift up a bit, reduce */
    x[7] = s[6] ^ t[7];
    x[6] = s[5] ^ t[6];
    x[5] = s[4] ^ t[5];
    x[4] = s[3] ^ hi ^ t[4];
    x[3] = s[2] ^ hi ^ t[3];
    x[2] = s[1] ^ t[2];
    x[1] = s[0] ^ hi ^ t[1];
    x[0] = hi ^ t[0];
}

/* InvMixColumns: a_r += 4(a_r + a_r+2), then MixColumns. */
static void inv_mix_columns(uint64_t *x)
{
    uint64_t t[8], u[8];
    for (int i = 0; i < 8; i++)
        t[i] = x[i] ^ rot2(x[i]);
    for (int twice = 0; twice < 2; twice++) {   /* t = 2t, twice */
        uint64_t hi = t[7];
        u[7] = t[6], u[6] = t[5], u[5] = t[4], u[4] = t[3] ^ hi;
        u[3] = t[2] ^ hi, u[2] = t[1], u[1] = t[0] ^ hi, u[0] = hi;
        memcpy(t, u, sizeof(t));
    }
    for (int i = 0; i < 8; i++)
        x[i] ^= t[i];
    mix_columns(x);
}

/* 64 bytes (four blocks) to planes and back; fixed positions only. */
static void to_planes(const uint8_t in[64], uint64_t *x)
{
    memset(x, 0, 8 * sizeof(uint64_t));
    for (int i = 0; i < 64; i++)
        for (int k = 0; k < 8; k++)
            x[k] |= (uint64_t)((in[i] >> k) & 1) << i;
}

static void from_planes(const uint64_t *x, uint8_t out[64])
{
    for (int i = 0; i < 64; i++) {
        uint8_t b = 0;
        for (int k = 0; k < 8; k++)
            b |= (uint8_t)(((x[k] >> i) & 1) << k);
        out[i] = b;
    }
}

/* The S-box of four bytes (the key schedule), the same way. */
static uint32_t sub_word(uint32_t w)
{
    uint8_t b[64] = { 0 };
    for (int i = 0; i < 4; i++)
        b[i] = (uint8_t)(w >> (24 - 8 * i));
    uint64_t x[8];
    to_planes(b, x);
    sub_bytes(x);
    from_planes(x, b);
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

void aes_init(struct aes *a, const uint8_t *key, size_t length)
{
    int nk = length == 32 ? 8 : 4;
    a->rounds = nk + 6;
    uint32_t w[60];
    for (int i = 0; i < nk; i++)
        w[i] = (uint32_t)key[4 * i] << 24 | (uint32_t)key[4 * i + 1] << 16 |
               (uint32_t)key[4 * i + 2] << 8 | key[4 * i + 3];
    uint32_t rcon = 1;
    for (int i = nk; i < 4 * (a->rounds + 1); i++) {
        uint32_t t = w[i - 1];
        if (i % nk == 0) {
            t = sub_word(t << 8 | t >> 24) ^ (rcon << 24);
            rcon = (rcon << 1) ^ (rcon & 0x80 ? 0x11B : 0);    /* (public data) */
        } else if (nk == 8 && i % nk == 4) {
            t = sub_word(t);
        }
        w[i] = w[i - nk] ^ t;
    }
    for (int r = 0; r <= a->rounds; r++) {      /* each round key, as planes, four times */
        uint8_t b[64];
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++) {
                uint8_t byte = (uint8_t)(w[4 * r + j] >> (24 - 8 * i));
                for (int blk = 0; blk < 4; blk++)
                    b[16 * blk + 4 * j + i] = byte;
            }
        to_planes(b, a->key[r]);
    }
    memset(w, 0, sizeof(w));
}

void aes_encrypt4(const struct aes *a, const uint8_t in[64], uint8_t out[64])
{
    uint64_t x[8];
    to_planes(in, x);
    for (int k = 0; k < 8; k++)
        x[k] ^= a->key[0][k];
    for (int r = 1; r <= a->rounds; r++) {
        sub_bytes(x);
        for (int k = 0; k < 8; k++)
            x[k] = shift_rows1(x[k]);
        if (r != a->rounds)
            mix_columns(x);
        for (int k = 0; k < 8; k++)
            x[k] ^= a->key[r][k];
    }
    from_planes(x, out);
}

void aes_encrypt(const struct aes *a, const uint8_t in[16], uint8_t out[16])
{
    uint8_t b[64] = { 0 }, o[64];
    memcpy(b, in, 16);
    aes_encrypt4(a, b, o);
    memcpy(out, o, 16);
}

void aes_decrypt4(const struct aes *a, const uint8_t in[64], uint8_t out[64])
{
    uint64_t x[8];
    to_planes(in, x);
    for (int k = 0; k < 8; k++)
        x[k] ^= a->key[a->rounds][k];
    for (int r = a->rounds - 1; r >= 0; r--) {
        for (int k = 0; k < 8; k++)
            x[k] = inv_shift_rows1(x[k]);
        inv_sub_bytes(x);
        for (int k = 0; k < 8; k++)
            x[k] ^= a->key[r][k];
        if (r)
            inv_mix_columns(x);
    }
    from_planes(x, out);
}

void aes_decrypt(const struct aes *a, const uint8_t in[16], uint8_t out[16])
{
    uint8_t b[64] = { 0 }, o[64];
    memcpy(b, in, 16);
    aes_decrypt4(a, b, o);
    memcpy(out, o, 16);
}

/* --- GCM --- */

/* x * y in GF(2^128), GCM's bit order (the first bit is x^0); no branches. */
static void gf128_mul(uint8_t x[16], const uint8_t y[16])
{
    uint64_t zh = 0, zl = 0, vh = 0, vl = 0;
    for (int i = 0; i < 8; i++) {
        vh = vh << 8 | y[i];
        vl = vl << 8 | y[8 + i];
    }
    for (int i = 0; i < 128; i++) {
        uint64_t bit = (uint64_t)((x[i / 8] >> (7 - i % 8)) & 1);
        uint64_t mask = 0 - bit;
        zh ^= vh & mask;
        zl ^= vl & mask;
        uint64_t low = 0 - (vl & 1);
        vl = (vl >> 1) | (vh << 63);
        vh = (vh >> 1) ^ (0xE100000000000000ULL & low);
    }
    for (int i = 0; i < 8; i++) {
        x[i] = (uint8_t)(zh >> (56 - 8 * i));
        x[8 + i] = (uint8_t)(zl >> (56 - 8 * i));
    }
}

static void ghash(uint8_t y[16], const uint8_t h[16], const uint8_t *data, size_t n)
{
    while (n) {
        size_t k = n < 16 ? n : 16;
        for (size_t i = 0; i < k; i++)
            y[i] ^= data[i];
        gf128_mul(y, h);
        data += k;
        n -= k;
    }
}

/* data XOR the key stream from counter block j (incremented first). */
static void ctr(const struct aes *a, uint8_t j[16], const uint8_t *in, uint8_t *out, size_t n)
{
    uint8_t blocks[64], stream[64];
    while (n) {
        for (int b = 0; b < 4; b++) {
            for (int i = 15; i >= 12; i--)      /* the last 32 bits count */
                if (++j[i])
                    break;
            memcpy(blocks + 16 * b, j, 16);
        }
        aes_encrypt4(a, blocks, stream);
        size_t k = n < 64 ? n : 64;
        for (size_t i = 0; i < k; i++)
            out[i] = in[i] ^ stream[i];
        if (k < 64) {                           /* the counter blocks not used */
            for (size_t used = (k + 15) / 16; used < 4; used++)
                for (int i = 15; i >= 12; i--)
                    if (j[i]--)
                        break;
        }
        in += k;
        out += k;
        n -= k;
    }
}

static void gcm_tag(const struct aes *a, const uint8_t j0[16], const uint8_t *aad, size_t aad_n,
                    const uint8_t *c, size_t n, uint8_t tag[16])
{
    uint8_t h[16] = { 0 }, y[16] = { 0 }, lengths[16], ek[16];
    aes_encrypt(a, h, h);
    ghash(y, h, aad, aad_n);
    ghash(y, h, c, n);
    uint64_t abits = (uint64_t)aad_n * 8, cbits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) {
        lengths[i] = (uint8_t)(abits >> (56 - 8 * i));
        lengths[8 + i] = (uint8_t)(cbits >> (56 - 8 * i));
    }
    ghash(y, h, lengths, 16);
    aes_encrypt(a, j0, ek);
    for (int i = 0; i < 16; i++)
        tag[i] = y[i] ^ ek[i];
}

void aes_gcm_seal(const struct aes *a, const uint8_t iv[12], const void *aad, size_t aad_n,
                  const void *in, void *out, size_t n, uint8_t tag[16])
{
    uint8_t j0[16], j[16];
    memcpy(j0, iv, 12);
    j0[12] = j0[13] = j0[14] = 0;
    j0[15] = 1;
    memcpy(j, j0, 16);
    ctr(a, j, in, out, n);
    gcm_tag(a, j0, aad, aad_n, out, n, tag);
}

bool aes_gcm_open(const struct aes *a, const uint8_t iv[12], const void *aad, size_t aad_n,
                  const void *in, void *out, size_t n, const uint8_t tag[16])
{
    uint8_t j0[16], j[16], want[16];
    memcpy(j0, iv, 12);
    j0[12] = j0[13] = j0[14] = 0;
    j0[15] = 1;
    gcm_tag(a, j0, aad, aad_n, in, n, want);
    if (!same_bytes(want, tag, 16))
        return false;                           /* (nothing decrypted) */
    memcpy(j, j0, 16);
    ctr(a, j, in, out, n);
    return true;
}

/* --- AES-CCM (L = 2) --- */

/* The CBC-MAC of the message's B0, its associated data and its text. */
static void ccm_mac(const struct aes *a, const uint8_t nonce[13], const uint8_t *aad,
                    size_t aad_n, const uint8_t *text, size_t n, size_t tag_length,
                    uint8_t x[16])
{
    x[0] = (uint8_t)((aad_n ? 0x40 : 0) | ((tag_length - 2) / 2) << 3 | 1);
    memcpy(x + 1, nonce, 13);
    x[14] = (uint8_t)(n >> 8), x[15] = (uint8_t)n;
    aes_encrypt(a, x, x);
    if (aad_n) {                                /* its length (2 bytes), the data, zeros */
        uint8_t block[16] = { 0 };
        size_t used = 2;
        block[0] = (uint8_t)(aad_n >> 8), block[1] = (uint8_t)aad_n;
        for (size_t i = 0; i < aad_n; i++) {
            block[used++] = aad[i];
            if (used == 16) {
                for (int k = 0; k < 16; k++)
                    x[k] ^= block[k];
                aes_encrypt(a, x, x);
                memset(block, 0, 16);
                used = 0;
            }
        }
        if (used) {
            for (int k = 0; k < 16; k++)
                x[k] ^= block[k];
            aes_encrypt(a, x, x);
        }
    }
    for (size_t i = 0; i < n; i += 16) {
        size_t k = n - i < 16 ? n - i : 16;
        for (size_t j = 0; j < k; j++)
            x[j] ^= text[i + j];
        aes_encrypt(a, x, x);
    }
}

/* in XOR the key stream S_1, S_2, ...; s0 = S_0 (for the tag). */
static void ccm_ctr(const struct aes *a, const uint8_t nonce[13], const uint8_t *in,
                    uint8_t *out, size_t n, uint8_t s0[16])
{
    uint8_t blocks[64], stream[64];
    blocks[0] = 1;
    memcpy(blocks + 1, nonce, 13);
    blocks[14] = blocks[15] = 0;
    aes_encrypt(a, blocks, s0);
    uint32_t counter = 1;
    for (size_t i = 0; i < n; i += 64) {
        for (int b = 0; b < 4; b++, counter++) {
            blocks[16 * b] = 1;
            memcpy(blocks + 16 * b + 1, nonce, 13);
            blocks[16 * b + 14] = (uint8_t)(counter >> 8), blocks[16 * b + 15] = (uint8_t)counter;
        }
        aes_encrypt4(a, blocks, stream);
        size_t k = n - i < 64 ? n - i : 64;
        for (size_t j = 0; j < k; j++)
            out[i + j] = in[i + j] ^ stream[j];
    }
    memset(stream, 0, sizeof(stream));
}

void aes_ccm_seal(const struct aes *a, const uint8_t nonce[13], const void *aad, size_t aad_n,
                  const void *in, void *out, size_t n, uint8_t *tag, size_t tag_length)
{
    uint8_t x[16], s0[16];
    ccm_mac(a, nonce, aad, aad_n, in, n, tag_length, x);
    ccm_ctr(a, nonce, in, out, n, s0);
    for (size_t k = 0; k < tag_length; k++)
        tag[k] = x[k] ^ s0[k];
}

bool aes_ccm_open(const struct aes *a, const uint8_t nonce[13], const void *aad, size_t aad_n,
                  const void *in, void *out, size_t n, const uint8_t *tag, size_t tag_length)
{
    /* The MAC is over the text: decrypt first, check, and take the text
     * back (zeros) if the tag is wrong. */
    uint8_t x[16], s0[16], want[16];
    ccm_ctr(a, nonce, in, out, n, s0);
    ccm_mac(a, nonce, aad, aad_n, out, n, tag_length, x);
    for (size_t k = 0; k < tag_length; k++)
        want[k] = x[k] ^ s0[k];
    bool ok = same_bytes(want, tag, tag_length);
    if (!ok)
        memset(out, 0, n);
    return ok;
}

/* --- AES-CMAC --- */

/* x * 2 in GF(2^128) (CMAC's subkeys), the carry as a mask. */
static void double_block(const uint8_t in[16], uint8_t out[16])
{
    uint8_t carry = (uint8_t)(0 - (in[0] >> 7));
    for (int i = 0; i < 15; i++)
        out[i] = (uint8_t)(in[i] << 1 | in[i + 1] >> 7);
    out[15] = (uint8_t)(in[15] << 1) ^ (carry & 0x87);
}

void aes_cmac(const struct aes *a, const void *data, size_t n, uint8_t tag[16])
{
    const uint8_t *p = data;
    uint8_t l[16] = { 0 }, k1[16], k2[16], x[16] = { 0 }, last[16] = { 0 };
    aes_encrypt(a, l, l);
    double_block(l, k1);
    double_block(k1, k2);
    size_t full = n ? (n - 1) / 16 : 0;         /* the blocks before the last */
    for (size_t i = 0; i < full; i++) {
        for (int k = 0; k < 16; k++)
            x[k] ^= p[16 * i + k];
        aes_encrypt(a, x, x);
    }
    size_t rest = n - 16 * full;                /* 1-16, or 0 for an empty message */
    memcpy(last, p + 16 * full, rest);
    if (rest == 16) {
        for (int k = 0; k < 16; k++)
            last[k] ^= k1[k];
    } else {
        last[rest] = 0x80;
        for (int k = 0; k < 16; k++)
            last[k] ^= k2[k];
    }
    for (int k = 0; k < 16; k++)
        x[k] ^= last[k];
    aes_encrypt(a, x, tag);
    memset(l, 0, sizeof(l));
    memset(k1, 0, sizeof(k1));
    memset(k2, 0, sizeof(k2));
}

/*
 * Random bytes for programs (salts for passwords, later more): ChaCha20
 * (RFC 8439) as a generator. The key starts from the firmware's random
 * seed (boot_info) and takes in the CPU's counter on every request; after
 * each request the key is replaced by fresh output, so earlier output can
 * not be recomputed from a later state ("fast key erasure").
 */
#include "random.h"
#include "arch_api.h"
#include "string.h"
#include "sync.h"

static uint32_t key[8];
static uint64_t requests;
static struct spinlock lock = SPINLOCK_INIT("random");

static uint32_t rotl(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

#define QUARTER(a, b, c, d)                                                    \
    do {                                                                       \
        a += b, d ^= a, d = rotl(d, 16);                                       \
        c += d, b ^= c, b = rotl(b, 12);                                       \
        a += b, d ^= a, d = rotl(d, 8);                                        \
        c += d, b ^= c, b = rotl(b, 7);                                        \
    } while (0)

/* One 64-byte ChaCha20 block for (key, counter, nonce). */
static void block(const uint32_t k[8], uint32_t counter, const uint32_t nonce[3],
                  uint32_t out[16])
{
    uint32_t x[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
                       k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7],
                       counter, nonce[0], nonce[1], nonce[2] };
    uint32_t start[16];
    memcpy(start, x, sizeof(x));
    for (int i = 0; i < 10; i++) {
        QUARTER(x[0], x[4], x[8], x[12]);
        QUARTER(x[1], x[5], x[9], x[13]);
        QUARTER(x[2], x[6], x[10], x[14]);
        QUARTER(x[3], x[7], x[11], x[15]);
        QUARTER(x[0], x[5], x[10], x[15]);
        QUARTER(x[1], x[6], x[11], x[12]);
        QUARTER(x[2], x[7], x[8], x[13]);
        QUARTER(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++)
        out[i] = x[i] + start[i];
}

void random_init(const uint8_t *seed, size_t size)
{
    uint8_t k[32] = {0};
    for (size_t i = 0; i < size; i++)
        k[i % 32] ^= seed[i];
    uint64_t now = arch_counter();
    memcpy(&key, k, sizeof(k));
    key[0] ^= (uint32_t)now;
    key[1] ^= (uint32_t)(now >> 32);
}

bool random_selftest(void)
{
    /* RFC 8439 section 2.3.2: key 00 01 .. 1f, counter 1, nonce 00 00 00 09
     * 00 00 00 4a 00 00 00 00: the block begins e4 e7 f1 10. */
    uint32_t k[8], out[16];
    const uint32_t nonce[3] = { 0x09000000, 0x4a000000, 0 };
    for (int i = 0; i < 8; i++)
        k[i] = (uint32_t)(4 * i) | (uint32_t)(4 * i + 1) << 8 | (uint32_t)(4 * i + 2) << 16 |
               (uint32_t)(4 * i + 3) << 24;
    block(k, 1, nonce, out);
    return out[0] == 0xe4e7f110 && out[15] == 0x4e3c50a2;
}

void random_bytes(void *buf, size_t n)
{
    uint64_t state = spin_lock(&lock);
    uint64_t now = arch_counter();
    uint32_t nonce[3] = { (uint32_t)now, (uint32_t)(now >> 32), (uint32_t)++requests };
    uint8_t *p = buf;
    uint32_t out[16];
    uint32_t counter = 1;
    while (n) {
        block(key, counter++, nonce, out);
        size_t take = n < sizeof(out) ? n : sizeof(out);
        memcpy(p, out, take);
        p += take, n -= take;
    }
    block(key, 0, nonce, out);                      /* block 0: the next key */
    memcpy(key, out, sizeof(key));
    memset(out, 0, sizeof(out));
    spin_unlock(&lock, state);
}

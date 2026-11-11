/*
 * X25519 (RFC 7748) and Ed25519 (RFC 8032): the curve 25519 for key
 * exchange and signatures.
 *
 * Both compute with numbers modulo p = 2^255 - 19 (the field). A field
 * element is 16 limbs of 16 bits in int64_t, value sum(limb[i] * 2^(16i)):
 * a product of two limbs is at most 2^34, so the 31 partial sums of a
 * multiplication fit easily. Limbs may grow or go negative between
 * multiplications; carry() brings them back to 16 bits, folding what
 * comes out of the top back in as 38 times as much (2^256 = 38 mod p).
 *
 * X25519: a point is given by its u coordinate only; scalar times point
 * by the Montgomery ladder, two values that step through the scalar's bits
 * together and are swapped by a mask (no branch on the secret).
 *
 * Ed25519: points (x, y) on -x^2 + y^2 = 1 + d x^2 y^2 in extended
 * coordinates (X:Y:Z:T, x = X/Z, y = Y/Z, xy = T/Z), where one formula
 * adds any two points, doubling included. A point is stored as y with
 * x's lowest bit on top. The secret scalar comes from a hash of the seed;
 * a signature is R = r*B and S = r + H(R, A, message) * a modulo the
 * group's order L (B: the base point, A = a*B the public key).
 */
#include <string.h>
#include "crypto.h"

typedef int64_t fe[16];

static void copy(fe o, const fe a)
{
    memcpy(o, a, sizeof(fe));
}

static void set(fe o, int64_t v)
{
    memset(o, 0, sizeof(fe));
    o[0] = v;
}

static void carry(fe o)
{
    for (int i = 0; i < 16; i++) {
        int64_t c = o[i] >> 16;                     /* rounds down, also when negative */
        o[i] -= c * 65536;
        if (i < 15)
            o[i + 1] += c;
        else
            o[0] += 38 * c;
    }
}

static void add(fe o, const fe a, const fe b)
{
    for (int i = 0; i < 16; i++)
        o[i] = a[i] + b[i];
}

static void sub(fe o, const fe a, const fe b)
{
    for (int i = 0; i < 16; i++)
        o[i] = a[i] - b[i];
}

static void mul(fe o, const fe a, const fe b)
{
    int64_t t[31] = { 0 };
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++)
        t[i] += 38 * t[i + 16];                     /* 2^256 = 38 */
    for (int i = 0; i < 16; i++)
        o[i] = t[i];
    carry(o);
    carry(o);
}

static void square(fe o, const fe a)
{
    mul(o, a, a);
}

/* Swap a and b if bit is 1, without a branch. */
static void swap_if(fe a, fe b, int64_t bit)
{
    int64_t mask = -bit;
    for (int i = 0; i < 16; i++) {
        int64_t t = mask & (a[i] ^ b[i]);
        a[i] ^= t;
        b[i] ^= t;
    }
}

/* a^e for a public exponent e (32 bytes, little-endian). */
static void power(fe o, const fe a, const uint8_t e[32])
{
    fe r;
    set(r, 1);
    for (int i = 255; i >= 0; i--) {
        square(r, r);
        if ((e[i >> 3] >> (i & 7)) & 1)
            mul(r, r, a);
    }
    copy(o, r);
}

static const uint8_t P_MINUS_2[32] = {                    /* 2^255 - 21: the inverse */
    0xeb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f,
};
static const uint8_t P_MINUS_5_OVER_8[32] = {             /* 2^252 - 3: square roots */
    0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0f,
};
static const uint8_t P_MINUS_1_OVER_4[32] = {             /* 2^253 - 5: sqrt(-1) = 2^that */
    0xfb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f,
};

static void invert(fe o, const fe a)
{
    power(o, a, P_MINUS_2);
}

/* The 32 bytes of a, fully reduced (0 <= a < p). */
static void pack(uint8_t out[32], const fe a)
{
    fe t, m;
    copy(t, a);
    for (int i = 0; i < 4; i++)
        carry(t);                                   /* now 0 <= t < 2^256 - 38, limbs exact */
    for (int k = 0; k < 2; k++) {                   /* t - p, if that is not negative */
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int64_t borrow = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        m[15] &= 0xffff;
        swap_if(t, m, 1 - borrow);
    }
    for (int i = 0; i < 16; i++) {
        out[2 * i] = (uint8_t)t[i];
        out[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static void unpack(fe o, const uint8_t in[32])
{
    for (int i = 0; i < 16; i++)
        o[i] = in[2 * i] | (int64_t)in[2 * i + 1] << 8;
    o[15] &= 0x7fff;                                /* the top bit is not part of it */
}

static bool equal(const fe a, const fe b)
{
    uint8_t x[32], y[32];
    pack(x, a);
    pack(y, b);
    return crypto_equal(x, y, 32);
}

static int parity(const fe a)
{
    uint8_t x[32];
    pack(x, a);
    return x[0] & 1;
}

/* --- X25519 ----------------------------------------------------------------- */

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    uint8_t k[32];
    memcpy(k, scalar, 32);
    k[0] &= 248;                                    /* a multiple of 8 (the cofactor) */
    k[31] = (k[31] & 127) | 64;                     /* bit 254 set: the same time for all */
    static const fe a24 = { 0xdb41, 1 };            /* 121665 = (486662 - 2) / 4 */
    fe x1, x2, z2, x3, z3, a, aa, b, bb, e, c, d, da, cb;
    unpack(x1, point);
    set(x2, 1);
    set(z2, 0);
    copy(x3, x1);
    set(z3, 1);
    int64_t swap = 0;
    for (int t = 254; t >= 0; t--) {
        int64_t bit = (k[t >> 3] >> (t & 7)) & 1;
        swap ^= bit;
        swap_if(x2, x3, swap);
        swap_if(z2, z3, swap);
        swap = bit;
        add(a, x2, z2);
        square(aa, a);
        sub(b, x2, z2);
        square(bb, b);
        sub(e, aa, bb);
        add(c, x3, z3);
        sub(d, x3, z3);
        mul(da, d, a);
        mul(cb, c, b);
        add(x3, da, cb);
        square(x3, x3);
        sub(z3, da, cb);
        square(z3, z3);
        mul(z3, z3, x1);
        mul(x2, aa, bb);
        mul(z2, e, a24);
        add(z2, z2, aa);
        mul(z2, z2, e);
    }
    swap_if(x2, x3, swap);
    swap_if(z2, z3, swap);
    invert(z2, z2);
    mul(x2, x2, z2);
    pack(out, x2);
    memset(k, 0, sizeof(k));
}

void x25519_base(uint8_t out[32], const uint8_t scalar[32])
{
    static const uint8_t nine[32] = { 9 };
    x25519(out, scalar, nine);
}

/* --- Ed25519 points ------------------------------------------------------------ */

struct point {
    fe x, y, z, t;
};

static fe curve_d, curve_2d, sqrt_m1;
static struct point base;
static bool ready;

static void point_add(struct point *r, const struct point *p, const struct point *q)
{
    fe a, b, c, d, e, f, g, h, t;
    sub(a, p->y, p->x);
    sub(t, q->y, q->x);
    mul(a, a, t);
    add(b, p->y, p->x);
    add(t, q->y, q->x);
    mul(b, b, t);
    mul(c, p->t, q->t);
    mul(c, c, curve_2d);
    mul(d, p->z, q->z);
    add(d, d, d);
    sub(e, b, a);
    sub(f, d, c);
    add(g, d, c);
    add(h, b, a);
    mul(r->x, e, f);
    mul(r->y, g, h);
    mul(r->z, f, g);
    mul(r->t, e, h);
}

static void point_swap_if(struct point *p, struct point *q, int64_t bit)
{
    swap_if(p->x, q->x, bit);
    swap_if(p->y, q->y, bit);
    swap_if(p->z, q->z, bit);
    swap_if(p->t, q->t, bit);
}

/* r = s * p (s: 32 bytes, little-endian), the same steps for every s. */
static void point_multiply(struct point *r, const struct point *p, const uint8_t s[32])
{
    struct point q = *p;
    set(r->x, 0);
    set(r->y, 1);
    set(r->z, 1);
    set(r->t, 0);
    for (int i = 255; i >= 0; i--) {
        int64_t bit = (s[i >> 3] >> (i & 7)) & 1;
        point_swap_if(r, &q, bit);
        point_add(&q, &q, r);
        point_add(r, r, r);
        point_swap_if(r, &q, bit);
    }
}

static void encode(uint8_t out[32], const struct point *p)
{
    fe zi, x, y;
    invert(zi, p->z);
    mul(x, p->x, zi);
    mul(y, p->y, zi);
    pack(out, y);
    out[31] ^= (uint8_t)(parity(x) << 7);
}

/* The point stored in 32 bytes; false if they are none (RFC 8032 5.1.3). */
static bool decode(struct point *p, const uint8_t in[32])
{
    fe one, u, v, v3, x, t;
    unpack(p->y, in);
    uint8_t check[32];
    pack(check, p->y);
    check[31] |= in[31] & 0x80;
    if (!crypto_equal(check, in, 32))
        return false;                               /* y >= p */
    set(one, 1);
    square(u, p->y);
    mul(v, u, curve_d);
    sub(u, u, one);                                 /* u = y^2 - 1 */
    add(v, v, one);                                 /* v = d y^2 + 1 */
    square(v3, v);
    mul(v3, v3, v);                                 /* v^3 */
    square(x, v3);
    mul(x, x, v);
    mul(x, x, u);                                   /* u v^7 */
    power(x, x, P_MINUS_5_OVER_8);
    mul(x, x, v3);
    mul(x, x, u);                                   /* x = u v^3 (u v^7)^((p-5)/8) */
    square(t, x);
    mul(t, t, v);
    if (!equal(t, u)) {
        fe minus_u, zero;
        set(zero, 0);
        sub(minus_u, zero, u);
        if (!equal(t, minus_u))
            return false;                           /* not on the curve */
        mul(x, x, sqrt_m1);
    }
    int sign = in[31] >> 7;
    fe zero;
    set(zero, 0);
    if (equal(x, zero) && sign)
        return false;
    if (parity(x) != sign)
        sub(x, zero, x);
    copy(p->x, x);
    set(p->z, 1);
    mul(p->t, p->x, p->y);
    return true;
}

/* The curve's constants, made at the first use. Threads may get here at
 * once (64n): each computes them in its own variables and only then copies
 * the finished values out, so a thread reading them while another copies
 * sees the same bytes either way (the arithmetic writes its destination in
 * steps: computed in place, a reader could catch a half-done value). The
 * base point is decoded with the constants already in place. */
static void prepare(void)
{
    if (__atomic_load_n(&ready, __ATOMIC_ACQUIRE))
        return;
    fe a, b, zero, d, d2, root;
    set(a, 121665);
    set(b, 121666);
    invert(b, b);
    mul(a, a, b);
    set(zero, 0);
    sub(d, zero, a);                                /* d = -121665 / 121666 */
    add(d2, d, d);
    set(a, 2);
    power(root, a, P_MINUS_1_OVER_4);
    memcpy(curve_d, d, sizeof(fe));
    memcpy(curve_2d, d2, sizeof(fe));
    memcpy(sqrt_m1, root, sizeof(fe));
    /* The base point: y = 4/5, x positive (even). */
    uint8_t y[32];
    set(a, 4);
    set(b, 5);
    invert(b, b);
    mul(a, a, b);
    pack(y, a);
    struct point p;
    decode(&p, y);
    memcpy(&base, &p, sizeof(p));
    __atomic_store_n(&ready, true, __ATOMIC_RELEASE);
}

/* --- scalars modulo L = 2^252 + 27742317777372353535851937790883648493 ---------- */

static const uint32_t L[8] = { 0x5cf5d3ed, 0x5812631a, 0xa2f79cd6, 0x14def9de, 0, 0, 0, 0x10000000 };

/* out = in mod L (in: n bytes, little-endian), bit by bit: the remainder
 * doubles and takes the next bit; L is subtracted when it fits (once is
 * enough: the remainder was below L). */
static void reduce(uint8_t out[32], const uint8_t *in, size_t n)
{
    uint32_t r[9] = { 0 };
    for (int i = (int)n * 8 - 1; i >= 0; i--) {
        for (int j = 8; j > 0; j--)
            r[j] = r[j] << 1 | r[j - 1] >> 31;
        r[0] = r[0] << 1 | ((in[i >> 3] >> (i & 7)) & 1);
        uint32_t m[9];
        int64_t borrow = 0;
        for (int j = 0; j < 9; j++) {
            int64_t d = (int64_t)r[j] - (j < 8 ? L[j] : 0) - borrow;
            m[j] = (uint32_t)d;
            borrow = d < 0;
        }
        uint32_t keep = (uint32_t)-borrow;          /* all ones: r < L, keep r */
        for (int j = 0; j < 9; j++)
            r[j] = (r[j] & keep) | (m[j] & ~keep);
    }
    for (int j = 0; j < 8; j++)
        for (int k = 0; k < 4; k++)
            out[4 * j + k] = (uint8_t)(r[j] >> (8 * k));
}

static void words(uint32_t w[8], const uint8_t b[32])
{
    for (int j = 0; j < 8; j++)
        w[j] = (uint32_t)b[4 * j] | (uint32_t)b[4 * j + 1] << 8 | (uint32_t)b[4 * j + 2] << 16 |
               (uint32_t)b[4 * j + 3] << 24;
}

/* out = (r + k * a) mod L. */
static void multiply_add(uint8_t out[32], const uint8_t r[32], const uint8_t k[32],
                         const uint8_t a[32])
{
    uint32_t x[8], y[8], z[8], p[16] = { 0 };
    words(x, k);
    words(y, a);
    words(z, r);
    for (int i = 0; i < 8; i++) {
        uint64_t c = 0;
        for (int j = 0; j < 8; j++) {
            uint64_t t = (uint64_t)x[i] * y[j] + p[i + j] + c;
            p[i + j] = (uint32_t)t;
            c = t >> 32;
        }
        p[i + 8] = (uint32_t)c;
    }
    uint64_t c = 0;
    for (int i = 0; i < 16; i++) {
        uint64_t t = (uint64_t)p[i] + (i < 8 ? z[i] : 0) + c;
        p[i] = (uint32_t)t;
        c = t >> 32;
    }
    uint8_t bytes[64];
    for (int j = 0; j < 16; j++)
        for (int b = 0; b < 4; b++)
            bytes[4 * j + b] = (uint8_t)(p[j] >> (8 * b));
    reduce(out, bytes, 64);
}

static bool below_l(const uint8_t s[32])
{
    uint32_t w[8];
    words(w, s);
    for (int j = 7; j >= 0; j--)
        if (w[j] != L[j])
            return w[j] < L[j];
    return false;
}

/* --- Ed25519 ------------------------------------------------------------------- */

static void expand(const uint8_t seed[32], uint8_t a[32], uint8_t prefix[32])
{
    uint8_t h[64];
    sha512(seed, 32, h);
    memcpy(a, h, 32);
    a[0] &= 248;
    a[31] = (a[31] & 127) | 64;
    memcpy(prefix, h + 32, 32);
    memset(h, 0, sizeof(h));
}

void ed25519_public(uint8_t public_key[32], const uint8_t seed[32])
{
    prepare();
    uint8_t a[32], prefix[32];
    expand(seed, a, prefix);
    struct point p;
    point_multiply(&p, &base, a);
    encode(public_key, &p);
    memset(a, 0, sizeof(a));
    memset(prefix, 0, sizeof(prefix));
}

void ed25519_sign(uint8_t signature[64], const uint8_t seed[32], const uint8_t public_key[32],
                  const void *message, size_t n)
{
    prepare();
    uint8_t a[32], prefix[32], h[64], r[32], k[32];
    expand(seed, a, prefix);
    struct sha512 s;
    sha512_init(&s);
    sha512_update(&s, prefix, 32);
    sha512_update(&s, message, n);
    sha512_final(&s, h);
    reduce(r, h, 64);                               /* r = H(prefix, message) mod L */
    struct point p;
    point_multiply(&p, &base, r);
    encode(signature, &p);                          /* R = r B */
    sha512_init(&s);
    sha512_update(&s, signature, 32);
    sha512_update(&s, public_key, 32);
    sha512_update(&s, message, n);
    sha512_final(&s, h);
    reduce(k, h, 64);                               /* k = H(R, A, message) mod L */
    multiply_add(signature + 32, r, k, a);          /* S = r + k a mod L */
    memset(a, 0, sizeof(a));
    memset(prefix, 0, sizeof(prefix));
    memset(r, 0, sizeof(r));
}

bool ed25519_verify(const uint8_t signature[64], const uint8_t public_key[32],
                    const void *message, size_t n)
{
    prepare();
    struct point a, p, q;
    if (!below_l(signature + 32) || !decode(&a, public_key))
        return false;
    uint8_t h[64], k[32], check[32];
    struct sha512 s;
    sha512_init(&s);
    sha512_update(&s, signature, 32);
    sha512_update(&s, public_key, 32);
    sha512_update(&s, message, n);
    sha512_final(&s, h);
    reduce(k, h, 64);
    /* S B - k A must be R. */
    fe zero;
    set(zero, 0);
    sub(a.x, zero, a.x);
    sub(a.t, zero, a.t);
    point_multiply(&p, &base, signature + 32);
    point_multiply(&q, &a, k);
    point_add(&p, &p, &q);
    encode(check, &p);
    return crypto_equal(check, signature, 32);
}

/*
 * SAE, WPA3-Personal's password key exchange (IEEE 802.11-2020 12.4; step
 * 24b), on group 19 (NIST P-256), our own code. Both stations turn the
 * password into a secret point, the password element (PWE): by "hunting
 * and pecking" (40 tries of a hash until it lands on the curve) or by
 * "hash to element" (RFC 9380's simplified SWU map, from a PT made once per
 * network). Then each sends a commit (a scalar and a point hiding a random
 * number), both compute the same shared point K from the other's commit,
 * and from it the KCK (to confirm) and the PMK (the 4-way handshake's key).
 *
 * The password decides the PWE, so everything that touches it takes the
 * same steps for every password: field arithmetic in Montgomery form on four
 * 64-bit limbs with masked carries, inversions and square roots as fixed
 * exponentiations, points in projective form with Renes, Costello and
 * Batina's complete addition (2016, a = -3: no special case for doubling or
 * the point at infinity), scalar multiplication by a fixed 4-bit window
 * whose table entry is picked by masks, and all 40 hunting rounds always
 * run. Only public values branch: exponents, the peer's scalar range,
 * whether a check failed.
 */
#include <string.h>
#include "crypto.h"
#include "ritteros.h"

typedef uint64_t fe[4];                 /* a field element, little-endian limbs */
typedef unsigned __int128 u128;

struct point {                          /* projective (X : Y : Z), Montgomery form */
    fe x, y, z;
};

static const fe P = { 0xffffffffffffffffULL, 0x00000000ffffffffULL, 0x0000000000000000ULL,
                      0xffffffff00000001ULL };
static const fe R2 = { 0x0000000000000003ULL, 0xfffffffbffffffffULL, 0xfffffffffffffffeULL,
                       0x00000004fffffffdULL };          /* 2^512 mod p */
static const fe ONE = { 0x0000000000000001ULL, 0xffffffff00000000ULL, 0xffffffffffffffffULL,
                        0x00000000fffffffeULL };         /* 1 (Montgomery: 2^256 mod p) */
static const fe B = { 0xd89cdf6229c4bddfULL, 0xacf005cd78843090ULL, 0xe5a220abf7212ed6ULL,
                      0xdc30061d04874834ULL };
static const fe Z = { 0xfffffffffffffff5ULL, 0x0000000affffffffULL, 0x0000000000000000ULL,
                      0xfffffff50000000bULL };           /* -10, SSWU's Z for P-256 */
static const fe MINUS_B_OVER_A = { 0x9d899fcb6341949fULL, 0x8efaac9a7d816585ULL,
                                   0xa1e0b58ea7b5ba47ULL, 0xf410020901826d67ULL };
static const fe B_OVER_ZA = { 0x5c8dc32df0535ba9ULL, 0xc17f77a98c8cf08dULL,
                              0x7696788e43f892a0ULL, 0x9868003399c03e24ULL };
static const fe N = { 0xf3b9cac2fc632551ULL, 0xbce6faada7179e84ULL, 0xffffffffffffffffULL,
                      0xffffffff00000000ULL };           /* the group's order r */
static const fe N_MINUS_1 = { 0xf3b9cac2fc632550ULL, 0xbce6faada7179e84ULL,
                              0xffffffffffffffffULL, 0xffffffff00000000ULL };

/* --- 256-bit numbers --- */

/* out = a - m if a (with a carry limb hi) >= m, else a; by masks. */
static void reduce_once(uint64_t out[4], const uint64_t a[4], uint64_t hi, const fe m)
{
    uint64_t d[4], borrow = 0;
    for (int i = 0; i < 4; i++) {
        u128 t = (u128)a[i] - m[i] - borrow;
        d[i] = (uint64_t)t;
        borrow = (uint64_t)(t >> 64) & 1;
    }
    uint64_t take = 0 - ((hi | (borrow ^ 1)) & 1);
    for (int i = 0; i < 4; i++)
        out[i] = (d[i] & take) | (a[i] & ~take);
}

static void add_mod(uint64_t out[4], const uint64_t a[4], const uint64_t b[4], const fe m)
{
    uint64_t s[4], carry = 0;
    for (int i = 0; i < 4; i++) {
        u128 t = (u128)a[i] + b[i] + carry;
        s[i] = (uint64_t)t;
        carry = (uint64_t)(t >> 64);
    }
    reduce_once(out, s, carry, m);
}

static void fe_sub(fe out, const fe a, const fe b)
{
    uint64_t d[4], borrow = 0, carry = 0;
    for (int i = 0; i < 4; i++) {
        u128 t = (u128)a[i] - b[i] - borrow;
        d[i] = (uint64_t)t;
        borrow = (uint64_t)(t >> 64) & 1;
    }
    uint64_t mask = 0 - borrow;                 /* below zero: add p back */
    for (int i = 0; i < 4; i++) {
        u128 t = (u128)d[i] + (P[i] & mask) + carry;
        out[i] = (uint64_t)t;
        carry = (uint64_t)(t >> 64);
    }
}

static void fe_add(fe out, const fe a, const fe b)
{
    add_mod(out, a, b, P);
}

/* Montgomery multiplication: a * b / 2^256 mod p (p's -1/p mod 2^64 is 1). */
static void fe_mul(fe out, const fe a, const fe b)
{
    uint64_t t[6] = { 0 };
    for (int i = 0; i < 4; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 4; j++) {
            u128 s = (u128)a[j] * b[i] + t[j] + carry;
            t[j] = (uint64_t)s;
            carry = (uint64_t)(s >> 64);
        }
        u128 s = (u128)t[4] + carry;
        t[4] = (uint64_t)s;
        t[5] = (uint64_t)(s >> 64);
        uint64_t q = t[0];
        s = (u128)q * P[0] + t[0];
        carry = (uint64_t)(s >> 64);
        for (int j = 1; j < 4; j++) {
            s = (u128)q * P[j] + t[j] + carry;
            t[j - 1] = (uint64_t)s;
            carry = (uint64_t)(s >> 64);
        }
        s = (u128)t[4] + carry;
        t[3] = (uint64_t)s;
        t[4] = t[5] + (uint64_t)(s >> 64);
    }
    reduce_once(out, t, t[4], P);
}

static void fe_sqr(fe out, const fe a)
{
    fe_mul(out, a, a);
}

/* a^e for a public exponent e (big-endian bytes). */
static void fe_pow(fe out, const fe a, const uint8_t e[32])
{
    fe r;
    memcpy(r, ONE, sizeof(fe));
    for (int i = 0; i < 256; i++) {
        fe_sqr(r, r);
        if ((e[i / 8] >> (7 - i % 8)) & 1)
            fe_mul(r, r, a);
    }
    memcpy(out, r, sizeof(fe));
}

static const uint8_t EXP_INV[32] = {            /* p - 2 */
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfd };
static const uint8_t EXP_SQRT[32] = {           /* (p + 1) / 4 */
    0x3f, 0xff, 0xff, 0xff, 0xc0, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t EXP_LEGENDRE[32] = {       /* (p - 1) / 2 */
    0x7f, 0xff, 0xff, 0xff, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* All ones if a == b, else 0; no branch. */
static uint64_t fe_equal(const fe a, const fe b)
{
    uint64_t d = 0;
    for (int i = 0; i < 4; i++)
        d |= a[i] ^ b[i];
    return ((d | (0 - d)) >> 63) - 1;
}

static uint64_t fe_is_zero(const fe a)
{
    static const fe zero = { 0 };
    return fe_equal(a, zero);
}

/* out = mask ? a : b */
static void fe_select(fe out, uint64_t mask, const fe a, const fe b)
{
    for (int i = 0; i < 4; i++)
        out[i] = (a[i] & mask) | (b[i] & ~mask);
}

/* All ones if a is a square (or zero), by Euler's criterion. */
static uint64_t fe_is_square(const fe a)
{
    fe l;
    fe_pow(l, a, EXP_LEGENDRE);
    return fe_equal(l, ONE) | fe_is_zero(a);
}

static void fe_from_bytes(fe out, const uint8_t in[32])       /* to Montgomery form */
{
    fe a;
    for (int i = 0; i < 4; i++) {
        a[i] = 0;
        for (int j = 0; j < 8; j++)
            a[i] |= (uint64_t)in[31 - 8 * i - j] << (8 * j);
    }
    fe_mul(out, a, R2);
}

static void fe_to_bytes(uint8_t out[32], const fe a)          /* from Montgomery form */
{
    static const fe one = { 1, 0, 0, 0 };
    fe n;
    fe_mul(n, a, one);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++)
            out[31 - 8 * i - j] = (uint8_t)(n[i] >> (8 * j));
}

static void num_from_bytes(uint64_t out[4], const uint8_t in[32])  /* a plain number */
{
    for (int i = 0; i < 4; i++) {
        out[i] = 0;
        for (int j = 0; j < 8; j++)
            out[i] |= (uint64_t)in[31 - 8 * i - j] << (8 * j);
    }
}

static void num_to_bytes(uint8_t out[32], const uint64_t a[4])
{
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++)
            out[31 - 8 * i - j] = (uint8_t)(a[i] >> (8 * j));
}

/* All ones if a < m. */
static uint64_t num_less(const uint64_t a[4], const fe m)
{
    uint64_t borrow = 0;
    for (int i = 0; i < 4; i++) {
        u128 t = (u128)a[i] - m[i] - borrow;
        borrow = (uint64_t)(t >> 64) & 1;
    }
    return 0 - borrow;
}

/* y^2 = x^3 - 3x + b's right side. */
static void curve_rhs(fe out, const fe x)
{
    fe t;
    fe_sqr(t, x);
    fe_mul(t, t, x);
    fe_add(t, t, B);
    fe_sub(t, t, x);
    fe_sub(t, t, x);
    fe_sub(out, t, x);
}

/* --- points --- */

static void point_infinity(struct point *r)
{
    memset(r->x, 0, sizeof(fe));
    memcpy(r->y, ONE, sizeof(fe));
    memset(r->z, 0, sizeof(fe));
}

/* r = p + q: Renes, Costello, Batina 2016, algorithm 4 (complete, a = -3). */
static void point_add(struct point *r, const struct point *p, const struct point *q)
{
    fe t0, t1, t2, t3, t4, x3, y3, z3;
    fe_mul(t0, p->x, q->x);
    fe_mul(t1, p->y, q->y);
    fe_mul(t2, p->z, q->z);
    fe_add(t3, p->x, p->y);
    fe_add(t4, q->x, q->y);
    fe_mul(t3, t3, t4);
    fe_add(t4, t0, t1);
    fe_sub(t3, t3, t4);
    fe_add(t4, p->y, p->z);
    fe_add(x3, q->y, q->z);
    fe_mul(t4, t4, x3);
    fe_add(x3, t1, t2);
    fe_sub(t4, t4, x3);
    fe_add(x3, p->x, p->z);
    fe_add(y3, q->x, q->z);
    fe_mul(x3, x3, y3);
    fe_add(y3, t0, t2);
    fe_sub(y3, x3, y3);
    fe_mul(z3, B, t2);
    fe_sub(x3, y3, z3);
    fe_add(z3, x3, x3);
    fe_add(x3, x3, z3);
    fe_sub(z3, t1, x3);
    fe_add(x3, t1, x3);
    fe_mul(y3, B, y3);
    fe_add(t1, t2, t2);
    fe_add(t2, t1, t2);
    fe_sub(y3, y3, t2);
    fe_sub(y3, y3, t0);
    fe_add(t1, y3, y3);
    fe_add(y3, t1, y3);
    fe_add(t1, t0, t0);
    fe_add(t0, t1, t0);
    fe_sub(t0, t0, t2);
    fe_mul(t1, t4, y3);
    fe_mul(t2, t0, y3);
    fe_mul(y3, x3, z3);
    fe_add(y3, y3, t2);
    fe_mul(x3, t3, x3);
    fe_sub(x3, x3, t1);
    fe_mul(z3, t4, z3);
    fe_mul(t1, t3, t0);
    fe_add(z3, z3, t1);
    memcpy(r->x, x3, sizeof(fe));
    memcpy(r->y, y3, sizeof(fe));
    memcpy(r->z, z3, sizeof(fe));
}

/* r = k * p, k a 256-bit number (big-endian bytes), secret or not. */
static void point_mul(struct point *r, const uint8_t k[32], const struct point *p)
{
    struct point table[16], acc, pick;
    point_infinity(&table[0]);
    table[1] = *p;
    for (int i = 2; i < 16; i++)
        point_add(&table[i], &table[i - 1], p);
    point_infinity(&acc);
    for (int w = 63; w >= 0; w--) {
        for (int d = 0; d < 4; d++)
            point_add(&acc, &acc, &acc);
        uint64_t nibble = (uint64_t)((k[31 - w / 2] >> (4 * (w & 1))) & 15);
        point_infinity(&pick);
        for (uint64_t i = 0; i < 16; i++) {     /* every entry looked at, one kept */
            uint64_t mask = 0 - (((i ^ nibble) - 1) >> 63);
            fe_select(pick.x, mask, table[i].x, pick.x);
            fe_select(pick.y, mask, table[i].y, pick.y);
            fe_select(pick.z, mask, table[i].z, pick.z);
        }
        point_add(&acc, &acc, &pick);
    }
    *r = acc;
    memset(table, 0, sizeof(table));
}

/* To affine coordinates (x || y, big-endian); all ones if not infinity. */
static uint64_t point_to_bytes(uint8_t out[64], const struct point *p)
{
    fe zi, x, y;
    fe_pow(zi, p->z, EXP_INV);
    fe_mul(x, p->x, zi);
    fe_mul(y, p->y, zi);
    fe_to_bytes(out, x);
    fe_to_bytes(out + 32, y);
    return ~fe_is_zero(p->z);
}

/* From x || y; false if not a point of the curve (public data). */
static bool point_from_bytes(struct point *r, const uint8_t in[64])
{
    uint64_t x[4], y[4];
    num_from_bytes(x, in);
    num_from_bytes(y, in + 32);
    if (!num_less(x, P) || !num_less(y, P))
        return false;
    fe_from_bytes(r->x, in);
    fe_from_bytes(r->y, in + 32);
    memcpy(r->z, ONE, sizeof(fe));
    fe lhs, rhs;
    fe_sqr(lhs, r->y);
    curve_rhs(rhs, r->x);
    return fe_equal(lhs, rhs) != 0;
}

/* SSWU (RFC 9380 6.6.2) of u, a field element (Montgomery form). */
static void sswu(struct point *r, const fe u)
{
    fe u2, zu2, tv, tv1, x1, x2, gx1, gx2, x, y, g, t, minus_y;
    fe_sqr(u2, u);
    fe_mul(zu2, Z, u2);
    fe_add(t, zu2, ONE);
    fe_mul(tv, zu2, t);                         /* Z^2 u^4 + Z u^2 */
    fe_pow(tv1, tv, EXP_INV);                   /* inv0: 0 for 0 */
    fe_add(t, tv1, ONE);
    fe_mul(x1, MINUS_B_OVER_A, t);
    fe_select(x1, fe_is_zero(tv1), B_OVER_ZA, x1);
    curve_rhs(gx1, x1);
    fe_mul(x2, zu2, x1);
    curve_rhs(gx2, x2);
    uint64_t square = fe_is_square(gx1);
    fe_select(x, square, x1, x2);
    fe_select(g, square, gx1, gx2);
    fe_pow(y, g, EXP_SQRT);
    uint8_t ub[32], yb[32];
    fe_to_bytes(ub, u);
    fe_to_bytes(yb, y);
    uint64_t flip = 0 - (uint64_t)((ub[31] ^ yb[31]) & 1);   /* sgn0(u) != sgn0(y) */
    static const fe zero = { 0 };
    fe_sub(minus_y, zero, y);
    fe_select(y, flip, minus_y, y);
    memcpy(r->x, x, sizeof(fe));
    memcpy(r->y, y, sizeof(fe));
    memcpy(r->z, ONE, sizeof(fe));
}

/* A 384-bit big-endian number mod p, as a field element (Montgomery form):
 * hi * 2^256 + lo, where hi * 2^256 mod p is hi in Montgomery form. */
static void fe_from_48(fe out, const uint8_t in[48])
{
    uint8_t hi[32] = { 0 };
    uint64_t lo[4], l[4];
    memcpy(hi + 16, in, 16);
    num_from_bytes(lo, in + 16);
    reduce_once(l, lo, 0, P);                   /* lo < 2^256 < 2p */
    fe h, lm;
    fe_from_bytes(h, hi);                       /* hi's Montgomery form, hi * 2^256 mod p: */
    uint64_t sum[4];                            /* read as a plain number, the high part */
    add_mod(sum, h, l, P);
    fe_mul(lm, sum, R2);
    memcpy(out, lm, sizeof(fe));
}

/* --- SAE --- */

static void macs(uint8_t out[12], const uint8_t a[6], const uint8_t b[6])
{
    int a_first = memcmp(a, b, 6) > 0;          /* max || min (public) */
    memcpy(out, a_first ? a : b, 6);
    memcpy(out + 6, a_first ? b : a, 6);
}

static const uint8_t P_BYTES[32] = {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

bool sae_pwe_hunt(struct sae *s, const uint8_t mac_a[6], const uint8_t mac_b[6],
                  const void *password, size_t n)
{
    uint8_t key[12], in[SAE_PASSWORD_MAX + 1], seed[SHA256_SIZE], value[32];
    if (n > SAE_PASSWORD_MAX)
        return false;
    macs(key, mac_a, mac_b);
    memcpy(in, password, n);
    fe x = { 0 }, saved = { 0 };
    uint64_t found = 0, seed_odd = 0;
    for (int counter = 1; counter <= SAE_HUNT_ROUNDS; counter++) {
        in[n] = (uint8_t)counter;
        hmac_sha256(key, sizeof(key), in, n + 1, seed);
        ieee80211_kdf_sha256(seed, sizeof(seed), "SAE Hunting and Pecking", P_BYTES, 32,
                             value, 32);
        uint64_t v[4];
        num_from_bytes(v, value);
        uint64_t below = num_less(v, P);
        fe_from_bytes(x, value);
        fe rhs;
        curve_rhs(rhs, x);
        uint64_t take = below & fe_is_square(rhs) & ~fe_is_zero(rhs) & ~found;
        fe_select(saved, take, x, saved);
        seed_odd = (seed_odd & ~take) | ((uint64_t)(seed[31] & 1) & take);
        found |= take;
    }
    memset(in, 0, sizeof(in));
    memset(seed, 0, sizeof(seed));
    memset(value, 0, sizeof(value));
    if (!found)
        return false;                           /* (about 2^-40) */
    fe rhs, y, minus_y;
    curve_rhs(rhs, saved);
    fe_pow(y, rhs, EXP_SQRT);
    uint8_t yb[32];
    fe_to_bytes(yb, y);
    uint64_t flip = 0 - (uint64_t)((yb[31] ^ seed_odd) & 1);
    static const fe zero = { 0 };
    fe_sub(minus_y, zero, y);
    fe_select(y, flip, minus_y, y);
    fe_to_bytes(s->pwe, saved);
    fe_to_bytes(s->pwe + 32, y);
    memset(saved, 0, sizeof(saved));
    memset(y, 0, sizeof(y));
    return true;
}

void sae_pt(uint8_t pt[64], const void *ssid, size_t ssid_n, const void *password, size_t n,
            const void *id, size_t id_n)
{
    uint8_t ikm[2 * SAE_PASSWORD_MAX], prk[SHA256_SIZE], u[48];
    if (n > SAE_PASSWORD_MAX)
        n = SAE_PASSWORD_MAX;
    if (id_n > SAE_PASSWORD_MAX)
        id_n = SAE_PASSWORD_MAX;
    memcpy(ikm, password, n);
    memcpy(ikm + n, id, id_n);
    hkdf_extract(SHA256_SIZE, ssid, ssid_n, ikm, n + id_n, prk);
    struct point p1, p2;
    fe u1;
    hkdf_expand(SHA256_SIZE, prk, "SAE Hash to Element u1 P1", 25, u, sizeof(u));
    fe_from_48(u1, u);
    sswu(&p1, u1);
    hkdf_expand(SHA256_SIZE, prk, "SAE Hash to Element u2 P2", 25, u, sizeof(u));
    fe_from_48(u1, u);
    sswu(&p2, u1);
    point_add(&p1, &p1, &p2);
    point_to_bytes(pt, &p1);
    memset(ikm, 0, sizeof(ikm));
    memset(prk, 0, sizeof(prk));
    memset(u, 0, sizeof(u));
}

bool sae_pwe_from_pt(struct sae *s, const uint8_t pt[64], const uint8_t mac_a[6],
                     const uint8_t mac_b[6])
{
    uint8_t key[12], zeros[SHA256_SIZE] = { 0 }, val[SHA256_SIZE];
    macs(key, mac_a, mac_b);
    hkdf_extract(SHA256_SIZE, zeros, sizeof(zeros), key, sizeof(key), val);
    uint64_t v[4], one[4] = { 1, 0, 0, 0 };
    num_from_bytes(v, val);
    reduce_once(v, v, 0, N_MINUS_1);            /* mod (r - 1): v < 2^256 < 2(r - 1) */
    add_mod(v, v, one, N);                      /* + 1 */
    num_to_bytes(val, v);
    struct point t, pwe;
    if (!point_from_bytes(&t, pt))
        return false;
    point_mul(&pwe, val, &t);
    bool ok = point_to_bytes(s->pwe, &pwe) != 0;
    memset(&t, 0, sizeof(t));
    memset(&pwe, 0, sizeof(pwe));
    return ok;
}

/* A random number in [2, r - 1]. */
static bool random_scalar(uint8_t out[32])
{
    static const uint64_t two[4] = { 2, 0, 0, 0 };
    for (int tries = 0; tries < 64; tries++) {
        if (ritteros_random(out, 32) != 32)
            return false;
        uint64_t v[4];
        num_from_bytes(v, out);
        if (num_less(v, N) && !num_less(v, two))       /* (rejected draws say nothing) */
            return true;
    }
    return false;
}

bool sae_commit(struct sae *s, const uint8_t *rand, const uint8_t *mask)
{
    uint8_t m[32];
    struct point pwe, e;
    if (!point_from_bytes(&pwe, s->pwe))
        return false;
    for (int tries = 0; tries < 8; tries++) {
        if (rand)
            memcpy(s->rand, rand, 32);
        else if (!random_scalar(s->rand))
            return false;
        if (mask)
            memcpy(m, mask, 32);
        else if (!random_scalar(m))
            return false;
        uint64_t a[4], b[4], sum[4];
        static const uint64_t two[4] = { 2, 0, 0, 0 };
        num_from_bytes(a, s->rand);
        num_from_bytes(b, m);
        add_mod(sum, a, b, N);
        num_to_bytes(s->scalar, sum);
        if (num_less(sum, two) && !rand)
            continue;                           /* scalar 0 or 1: draw again */
        point_mul(&e, m, &pwe);
        uint8_t xy[64];
        point_to_bytes(xy, &e);
        uint64_t y[4], neg[4];
        num_from_bytes(y, xy + 32);
        uint64_t borrow = 0;                    /* -(mask * PWE): y = p - y */
        for (int i = 0; i < 4; i++) {
            u128 t = (u128)P[i] - y[i] - borrow;
            neg[i] = (uint64_t)t;
            borrow = (uint64_t)(t >> 64) & 1;
        }
        memcpy(s->element, xy, 32);
        num_to_bytes(s->element + 32, neg);
        memset(m, 0, sizeof(m));
        memset(&e, 0, sizeof(e));
        memset(&pwe, 0, sizeof(pwe));
        return true;
    }
    return false;
}

bool sae_process(struct sae *s, const uint8_t peer_scalar[32], const uint8_t peer_element[64])
{
    static const uint64_t two[4] = { 2, 0, 0, 0 };
    uint64_t ps[4], own[4], ctx[4];
    num_from_bytes(ps, peer_scalar);
    if (!num_less(ps, N) || num_less(ps, two))
        return false;                           /* outside (1, r) */
    struct point pe, pwe, k;
    if (!point_from_bytes(&pe, peer_element) || !point_from_bytes(&pwe, s->pwe))
        return false;
    if (!memcmp(peer_scalar, s->scalar, 32) && !memcmp(peer_element, s->element, 64))
        return false;                           /* our own commit reflected */
    memcpy(s->peer_scalar, peer_scalar, 32);
    memcpy(s->peer_element, peer_element, 64);
    point_mul(&k, peer_scalar, &pwe);           /* rand * (peer_scalar * PWE + PE) */
    point_add(&k, &k, &pe);
    point_mul(&k, s->rand, &k);
    uint8_t kxy[64];
    bool finite = point_to_bytes(kxy, &k) != 0;
    memset(&k, 0, sizeof(k));
    memset(&pwe, 0, sizeof(pwe));
    if (!finite)
        return false;
    uint8_t zeros[SHA256_SIZE] = { 0 }, keyseed[SHA256_SIZE], context[32], keys[64];
    hmac_sha256(zeros, sizeof(zeros), kxy, 32, keyseed);
    num_from_bytes(own, s->scalar);
    add_mod(ctx, own, ps, N);
    num_to_bytes(context, ctx);
    ieee80211_kdf_sha256(keyseed, sizeof(keyseed), "SAE KCK and PMK", context, 32, keys, 64);
    memcpy(s->kck, keys, 32);
    memcpy(s->pmk, keys + 32, 32);
    memcpy(s->pmkid, context, 16);
    memset(kxy, 0, sizeof(kxy));
    memset(keyseed, 0, sizeof(keyseed));
    memset(keys, 0, sizeof(keys));
    return true;
}

/* CN(KCK, send-confirm, scalar1, element1, scalar2, element2). */
static void confirm_of(const uint8_t kck[32], uint16_t send_confirm, const uint8_t *s1,
                       const uint8_t *e1, const uint8_t *s2, const uint8_t *e2, uint8_t out[32])
{
    uint8_t in[2 + 32 + 64 + 32 + 64];
    in[0] = (uint8_t)send_confirm, in[1] = (uint8_t)(send_confirm >> 8);
    memcpy(in + 2, s1, 32);
    memcpy(in + 34, e1, 64);
    memcpy(in + 98, s2, 32);
    memcpy(in + 130, e2, 64);
    hmac_sha256(kck, 32, in, sizeof(in), out);
}

void sae_confirm(const struct sae *s, uint16_t send_confirm, uint8_t out[32])
{
    confirm_of(s->kck, send_confirm, s->scalar, s->element, s->peer_scalar, s->peer_element,
               out);
}

bool sae_check_confirm(const struct sae *s, uint16_t peer_send_confirm, const uint8_t confirm[32])
{
    uint8_t want[32];
    confirm_of(s->kck, peer_send_confirm, s->peer_scalar, s->peer_element, s->scalar,
               s->element, want);
    return crypto_equal(want, confirm, 32);
}

/* For the tests (RFC 9380's vectors): SSWU of a field element u and the
 * sum of two points, affine x || y. */
void p256_sswu(const uint8_t u[32], uint8_t point[64])
{
    fe f;
    struct point r;
    fe_from_bytes(f, u);
    sswu(&r, f);
    point_to_bytes(point, &r);
}

bool p256_add(const uint8_t a[64], const uint8_t b[64], uint8_t sum[64])
{
    struct point p, q;
    if (!point_from_bytes(&p, a) || !point_from_bytes(&q, b))
        return false;
    point_add(&p, &p, &q);
    return point_to_bytes(sum, &p) != 0;
}

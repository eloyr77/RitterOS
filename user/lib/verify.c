/*
 * Checking signatures, for TLS certificates and handshakes (step 23b): RSA
 * (PKCS #1 v1.5 and PSS, RFC 8017) and ECDSA on P-256 and P-384 (FIPS 186-4,
 * SEC 1). Our own code. Only public data goes in -- keys, signatures, hashes
 * of public messages -- so, unlike the rest of crypto.h, this need not take
 * the same time for every input. Numbers are little-endian arrays of 32-bit
 * words; products modulo m use Montgomery multiplication. Nothing is kept
 * between calls (64h): threads check signatures at the same time.
 */
#include <string.h>
#include "crypto.h"
#include "ritteros.h"

#define WORDS 132                       /* 4224 bits: RSA up to 4096 */

struct mod {                            /* a modulus and what Montgomery needs */
    uint32_t m[WORDS];
    int n;                              /* words */
    uint32_t inv;                       /* -m^-1 mod 2^32 */
    uint32_t rr[WORDS];                 /* R^2 mod m, R = 2^(32n) */
};

/* Big-endian bytes to n words (0 if it does not fit). */
static int load(uint32_t *x, int n, const uint8_t *b, size_t len)
{
    while (len && !*b)
        b++, len--;
    if (len > (size_t)n * 4)
        return 0;
    memset(x, 0, (size_t)n * 4);
    for (size_t i = 0; i < len; i++)
        x[i / 4] |= (uint32_t)b[len - 1 - i] << (8 * (i % 4));
    return 1;
}

static void store(uint8_t *b, size_t len, const uint32_t *x)
{
    for (size_t i = 0; i < len; i++)
        b[len - 1 - i] = (uint8_t)(x[i / 4] >> (8 * (i % 4)));
}

static int cmp(const uint32_t *a, const uint32_t *b, int n)
{
    for (int i = n - 1; i >= 0; i--)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

static uint32_t sub(uint32_t *r, const uint32_t *a, const uint32_t *b, int n)
{
    uint64_t borrow = 0;
    for (int i = 0; i < n; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        r[i] = (uint32_t)d;
        borrow = (d >> 63) & 1;
    }
    return (uint32_t)borrow;
}

static uint32_t add(uint32_t *r, const uint32_t *a, const uint32_t *b, int n)
{
    uint64_t carry = 0;
    for (int i = 0; i < n; i++) {
        uint64_t s = (uint64_t)a[i] + b[i] + carry;
        r[i] = (uint32_t)s;
        carry = s >> 32;
    }
    return (uint32_t)carry;
}

static int is_zero(const uint32_t *a, int n)
{
    for (int i = 0; i < n; i++)
        if (a[i])
            return 0;
    return 1;
}

/* r = a b R^-1 mod m (a, b < m). */
static void mont_mul(const struct mod *m, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    uint32_t t[WORDS + 2] = { 0 };
    int n = m->n;
    for (int i = 0; i < n; i++) {
        uint64_t c = 0;
        for (int j = 0; j < n; j++) {
            c += (uint64_t)t[j] + (uint64_t)a[j] * b[i];
            t[j] = (uint32_t)c;
            c >>= 32;
        }
        c += t[n];
        t[n] = (uint32_t)c;
        t[n + 1] = (uint32_t)(c >> 32);
        uint32_t q = t[0] * m->inv;
        c = (uint64_t)t[0] + (uint64_t)q * m->m[0];
        c >>= 32;
        for (int j = 1; j < n; j++) {
            c += (uint64_t)t[j] + (uint64_t)q * m->m[j];
            t[j - 1] = (uint32_t)c;
            c >>= 32;
        }
        c += t[n];
        t[n - 1] = (uint32_t)c;
        t[n] = t[n + 1] + (uint32_t)(c >> 32);
    }
    if (t[n] || cmp(t, m->m, n) >= 0)
        sub(t, t, m->m, n);
    memcpy(r, t, (size_t)n * 4);
}

/* A modulus from n words (odd, top word not 0). */
static int mod_init(struct mod *m, const uint32_t *v, int n)
{
    while (n > 1 && !v[n - 1])
        n--;
    if (!(v[0] & 1) || n > WORDS)
        return 0;
    memcpy(m->m, v, (size_t)n * 4);
    m->n = n;
    uint32_t x = 1;                             /* m^-1 mod 2^32 by Newton */
    for (int i = 0; i < 5; i++)
        x *= 2 - v[0] * x;
    m->inv = 0 - x;
    memset(m->rr, 0, sizeof(m->rr));            /* R^2 mod m: 1 doubled 64n times */
    uint32_t one[WORDS] = { 1 };
    memcpy(m->rr, one, (size_t)n * 4);
    for (int i = 0; i < 64 * n; i++) {
        uint32_t carry = add(m->rr, m->rr, m->rr, n);
        if (carry || cmp(m->rr, m->m, n) >= 0)
            sub(m->rr, m->rr, m->m, n);
    }
    return 1;
}

/* a mod m into Montgomery form (a < 2^(32n)) and back. */
static void to_mont(const struct mod *m, uint32_t *r, const uint32_t *a)
{
    uint32_t t[WORDS];
    memcpy(t, a, (size_t)m->n * 4);
    while (cmp(t, m->m, m->n) >= 0)             /* (a is at most a little over m) */
        sub(t, t, m->m, m->n);
    mont_mul(m, r, t, m->rr);
}

static void from_mont(const struct mod *m, uint32_t *r, const uint32_t *a)
{
    uint32_t one[WORDS] = { 1 };
    mont_mul(m, r, a, one);
}

/* r = a^e mod m, all in Montgomery form but e (bytes, big-endian). */
static void mont_pow(const struct mod *m, uint32_t *r, const uint32_t *a, const uint8_t *e,
                     size_t elen)
{
    uint32_t x[WORDS], one[WORDS] = { 1 };
    to_mont(m, x, one);                         /* 1 */
    for (size_t i = 0; i < elen; i++)
        for (int bit = 7; bit >= 0; bit--) {
            mont_mul(m, x, x, x);
            if ((e[i] >> bit) & 1)
                mont_mul(m, x, x, a);
        }
    memcpy(r, x, (size_t)m->n * 4);
}

/* --- RSA --- */

static void hash_of(size_t size, const void *data, size_t n, uint8_t *out)
{
    if (size == SHA384_SIZE)
        sha384(data, n, out);
    else if (size == SHA512_SIZE)
        sha512(data, n, out);
    else
        sha256(data, n, out);
}

/* sig^e mod n as k bytes (k = the modulus's length); 0 if out of range. */
static int rsa_open(const uint8_t *nb, size_t nlen, const uint8_t *eb, size_t elen,
                    const uint8_t *sig, size_t slen, uint8_t *em, size_t *k)
{
    while (nlen && !*nb)
        nb++, nlen--;
    if (nlen < 128 || nlen > 512 || slen != nlen)
        return 0;                               /* 1024 to 4096 bits, a signature as long */
    struct mod m;                               /* (64h: it was static, shared by threads) */
    uint32_t nw[WORDS], s[WORDS], x[WORDS];
    int words = (int)((nlen + 3) / 4);
    if (!load(nw, words, nb, nlen) || !mod_init(&m, nw, words) || !load(s, words, sig, slen) ||
        cmp(s, m.m, m.n) >= 0)
        return 0;
    to_mont(&m, x, s);
    mont_pow(&m, x, x, eb, elen);
    from_mont(&m, x, x);
    store(em, nlen, x);
    *k = nlen;
    return 1;
}

bool rsa_verify_pkcs1(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                      const uint8_t *digest, size_t dlen, const uint8_t *sig, size_t slen)
{
    /* EM = 00 01 FF..FF 00 DigestInfo(hash OID, digest) */
    static const uint8_t prefix256[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                         0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
    static const uint8_t prefix384[] = { 0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                         0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30 };
    static const uint8_t prefix512[] = { 0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                         0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40 };
    const uint8_t *prefix = dlen == 32 ? prefix256 : dlen == 48 ? prefix384 : dlen == 64 ? prefix512 : 0;
    uint8_t em[512];
    size_t k;
    if (!prefix || !rsa_open(n, nlen, e, elen, sig, slen, em, &k))
        return false;
    size_t t = sizeof(prefix256) + dlen;
    if (k < t + 11 || em[0] != 0 || em[1] != 1 || em[k - t - 1] != 0)
        return false;
    for (size_t i = 2; i < k - t - 1; i++)
        if (em[i] != 0xFF)
            return false;
    return memcmp(em + k - t, prefix, sizeof(prefix256)) == 0 &&
           memcmp(em + k - dlen, digest, dlen) == 0;
}

bool rsa_verify_pss(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                    const uint8_t *digest, size_t dlen, const uint8_t *sig, size_t slen)
{
    /* EM = maskedDB | H | BC; DB = 00..00 01 salt (salt as long as the hash) */
    uint8_t em[512], db[512], mask[512 + 64];
    size_t k;
    if ((dlen != 32 && dlen != 48 && dlen != 64) || !rsa_open(n, nlen, e, elen, sig, slen, em, &k))
        return false;
    while (nlen && !*n)
        n++, nlen--;
    int top_bits = 0;                           /* emBits = modBits - 1 */
    for (uint8_t b = n[0]; b; b >>= 1)
        top_bits++;
    size_t embits = (k - 1) * 8 + (size_t)top_bits - 1, emlen = (embits + 7) / 8;
    const uint8_t *e2 = em + (k - emlen);       /* (k - emlen is 0 or 1; that byte must be 0) */
    if ((k > emlen && em[0]) || emlen < 2 * dlen + 2 || e2[emlen - 1] != 0xBC)
        return false;
    size_t dblen = emlen - dlen - 1;
    const uint8_t *h = e2 + dblen;
    for (size_t done = 0, counter = 0; done < dblen; counter++) {    /* MGF1 */
        uint8_t in[64 + 4];
        memcpy(in, h, dlen);
        in[dlen] = (uint8_t)(counter >> 24), in[dlen + 1] = (uint8_t)(counter >> 16);
        in[dlen + 2] = (uint8_t)(counter >> 8), in[dlen + 3] = (uint8_t)counter;
        hash_of(dlen, in, dlen + 4, mask + done);
        done += dlen;
    }
    for (size_t i = 0; i < dblen; i++)
        db[i] = e2[i] ^ mask[i];
    db[0] &= (uint8_t)(0xFF >> (8 * emlen - embits));
    size_t ps = dblen - dlen - 1;               /* zeros, then 01, then the salt */
    for (size_t i = 0; i < ps; i++)
        if (db[i])
            return false;
    if (db[ps] != 1)
        return false;
    uint8_t mprime[8 + 64 + 64], hh[64];
    memset(mprime, 0, 8);
    memcpy(mprime + 8, digest, dlen);
    memcpy(mprime + 8 + dlen, db + ps + 1, dlen);
    hash_of(dlen, mprime, 8 + 2 * dlen, hh);
    return memcmp(hh, h, dlen) == 0;
}

/* --- ECDSA on P-256 and P-384 (a = -3) --- */

struct curve {
    int words, bytes;
    const char *p, *n, *b, *gx, *gy;            /* big-endian hex */
};

static const struct curve P256 = { 8, 32,
    "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",
    "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
    "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
    "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
    "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5" };
static const struct curve P384 = { 12, 48,
    "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
    "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",
    "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
    "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
    "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f" };

static void from_hex(uint32_t *x, int words, const char *hex)
{
    uint8_t b[64];
    size_t len = strlen(hex) / 2;
    for (size_t i = 0; i < len; i++) {
        int hi = hex[2 * i], lo = hex[2 * i + 1];
        hi = hi <= '9' ? hi - '0' : (hi | 32) - 'a' + 10;
        lo = lo <= '9' ? lo - '0' : (lo | 32) - 'a' + 10;
        b[i] = (uint8_t)(hi << 4 | lo);
    }
    load(x, words, b, len);
}

struct point {                                  /* Jacobian: x / z^2, y / z^3; z 0: infinity */
    uint32_t x[12], y[12], z[12];
};

struct field {
    struct mod p;
    uint32_t b[12], three[12];
};

static void fadd(const struct field *f, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    int n = f->p.n;
    if (add(r, a, b, n) || cmp(r, f->p.m, n) >= 0)
        sub(r, r, f->p.m, n);
}

static void fsub(const struct field *f, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    int n = f->p.n;
    if (sub(r, a, b, n))
        add(r, r, f->p.m, n);
}

#define FMUL(r, a, b) mont_mul(&f->p, r, a, b)

/* 2P (a = -3: dbl-2001-b). */
static void point_double(const struct field *f, struct point *r, const struct point *p)
{
    int n = f->p.n;
    if (is_zero(p->z, n) || is_zero(p->y, n)) {
        memset(r, 0, sizeof(*r));
        return;
    }
    uint32_t delta[12], gamma[12], beta[12], alpha[12], t1[12], t2[12], x3[12], y3[12], z3[12];
    FMUL(delta, p->z, p->z);
    FMUL(gamma, p->y, p->y);
    FMUL(beta, p->x, gamma);
    fsub(f, t1, p->x, delta);
    fadd(f, t2, p->x, delta);
    FMUL(alpha, t1, t2);
    FMUL(alpha, alpha, f->three);               /* 3 (x - delta)(x + delta) */
    FMUL(x3, alpha, alpha);
    fadd(f, t1, beta, beta);
    fadd(f, t1, t1, t1);                        /* 4 beta */
    fadd(f, t2, t1, t1);                        /* 8 beta */
    fsub(f, x3, x3, t2);
    fadd(f, z3, p->y, p->z);
    FMUL(z3, z3, z3);
    fsub(f, z3, z3, gamma);
    fsub(f, z3, z3, delta);
    fsub(f, t1, t1, x3);                        /* 4 beta - x3 */
    FMUL(y3, alpha, t1);
    FMUL(t2, gamma, gamma);
    fadd(f, t2, t2, t2);
    fadd(f, t2, t2, t2);
    fadd(f, t2, t2, t2);                        /* 8 gamma^2 */
    fsub(f, y3, y3, t2);
    memcpy(r->x, x3, sizeof(x3));
    memcpy(r->y, y3, sizeof(y3));
    memcpy(r->z, z3, sizeof(z3));
}

/* P + Q (add-2007-bl), doubling when they are the same point. */
static void point_add(const struct field *f, struct point *r, const struct point *p,
                      const struct point *q)
{
    int n = f->p.n;
    if (is_zero(p->z, n)) {
        *r = *q;
        return;
    }
    if (is_zero(q->z, n)) {
        *r = *p;
        return;
    }
    uint32_t z1z1[12], z2z2[12], u1[12], u2[12], s1[12], s2[12], h[12], i[12], j[12], rr[12],
        v[12], t[12];
    FMUL(z1z1, p->z, p->z);
    FMUL(z2z2, q->z, q->z);
    FMUL(u1, p->x, z2z2);
    FMUL(u2, q->x, z1z1);
    FMUL(s1, p->y, q->z);
    FMUL(s1, s1, z2z2);
    FMUL(s2, q->y, p->z);
    FMUL(s2, s2, z1z1);
    fsub(f, h, u2, u1);
    fsub(f, rr, s2, s1);
    if (is_zero(h, n)) {
        if (is_zero(rr, n))
            point_double(f, r, p);
        else
            memset(r, 0, sizeof(*r));           /* P = -Q */
        return;
    }
    fadd(f, rr, rr, rr);                        /* r = 2 (s2 - s1) */
    fadd(f, i, h, h);
    FMUL(i, i, i);                              /* (2h)^2 */
    FMUL(j, h, i);
    FMUL(v, u1, i);
    struct point out;
    FMUL(out.x, rr, rr);
    fsub(f, out.x, out.x, j);
    fsub(f, out.x, out.x, v);
    fsub(f, out.x, out.x, v);
    fsub(f, t, v, out.x);
    FMUL(out.y, rr, t);
    FMUL(t, s1, j);
    fadd(f, t, t, t);
    fsub(f, out.y, out.y, t);
    fadd(f, t, p->z, q->z);
    FMUL(t, t, t);
    fsub(f, t, t, z1z1);
    fsub(f, t, t, z2z2);
    FMUL(out.z, t, h);
    *r = out;
}

/* Is (x, y) on the curve (Montgomery form)? */
static int on_curve(const struct field *f, const uint32_t *x, const uint32_t *y)
{
    uint32_t l[12], r[12], t[12];
    FMUL(l, y, y);
    FMUL(r, x, x);
    FMUL(r, r, x);                              /* x^3 */
    FMUL(t, x, f->three);
    fsub(f, r, r, t);                           /* - 3x */
    fadd(f, r, r, f->b);
    return cmp(l, r, f->p.n) == 0;
}

bool ecdsa_verify(int bits, const uint8_t *pub, size_t pub_len, const uint8_t *digest,
                  size_t dlen, const uint8_t *rb, size_t rlen, const uint8_t *sb, size_t slen)
{
    const struct curve *c = bits == 256 ? &P256 : bits == 384 ? &P384 : 0;
    if (!c || pub_len != (size_t)(1 + 2 * c->bytes) || pub[0] != 4)
        return false;                           /* an uncompressed point only */
    int w = c->words;
    struct field fld, *f = &fld;
    struct mod nm;
    uint32_t pw[12], nw[12], t[12], one[12] = { 1 }, r[12], s[12], e[12];
    from_hex(pw, w, c->p);
    from_hex(nw, w, c->n);
    mod_init(&f->p, pw, w);
    mod_init(&nm, nw, w);
    from_hex(t, w, c->b);
    to_mont(&f->p, f->b, t);
    uint32_t three[12] = { 3 };
    to_mont(&f->p, f->three, three);
    /* r and s in [1, n - 1] */
    if (!load(r, w, rb, rlen) || !load(s, w, sb, slen) || is_zero(r, w) || is_zero(s, w) ||
        cmp(r, nw, w) >= 0 || cmp(s, nw, w) >= 0)
        return false;
    /* e: the digest's leftmost bits as many as n's (both curves: whole bytes) */
    size_t take = dlen < (size_t)c->bytes ? dlen : (size_t)c->bytes;
    if (!load(e, w, digest, take))
        return false;
    /* w = s^-1 (Fermat: s^(n-2)), u1 = e w, u2 = r w, all mod n */
    uint32_t sm[12], wm[12], u1[12], u2[12], nm2[12], two[12] = { 2 };
    uint8_t exp[48];
    sub(nm2, nw, two, w);
    store(exp, (size_t)c->bytes, nm2);
    to_mont(&nm, sm, s);
    mont_pow(&nm, wm, sm, exp, (size_t)c->bytes);
    to_mont(&nm, t, e);
    mont_mul(&nm, u1, t, wm);
    from_mont(&nm, u1, u1);
    to_mont(&nm, t, r);
    mont_mul(&nm, u2, t, wm);
    from_mont(&nm, u2, u2);
    /* G and Q (checked on the curve), then u1 G + u2 Q (Shamir's trick) */
    struct point g, q, gq, x = { 0 };
    from_hex(t, w, c->gx);
    to_mont(&f->p, g.x, t);
    from_hex(t, w, c->gy);
    to_mont(&f->p, g.y, t);
    to_mont(&f->p, g.z, one);
    if (!load(t, w, pub + 1, (size_t)c->bytes) || cmp(t, pw, w) >= 0)
        return false;
    to_mont(&f->p, q.x, t);
    if (!load(t, w, pub + 1 + c->bytes, (size_t)c->bytes) || cmp(t, pw, w) >= 0)
        return false;
    to_mont(&f->p, q.y, t);
    to_mont(&f->p, q.z, one);
    if (!on_curve(f, q.x, q.y))
        return false;
    point_add(f, &gq, &g, &q);
    for (int i = c->bytes * 8 - 1; i >= 0; i--) {
        point_double(f, &x, &x);
        int a = (u1[i / 32] >> (i % 32)) & 1, b = (u2[i / 32] >> (i % 32)) & 1;
        if (a && b)
            point_add(f, &x, &x, &gq);
        else if (a)
            point_add(f, &x, &x, &g);
        else if (b)
            point_add(f, &x, &x, &q);
    }
    if (is_zero(x.z, w))
        return false;
    /* x / z^2 mod p (z^-1 by Fermat), then mod n, against r */
    uint32_t zi[12], pm2[12], ax[12];
    sub(pm2, pw, two, w);
    store(exp, (size_t)c->bytes, pm2);
    mont_pow(&f->p, zi, x.z, exp, (size_t)c->bytes);
    mont_mul(&f->p, t, zi, zi);
    mont_mul(&f->p, ax, x.x, t);
    from_mont(&f->p, ax, ax);
    while (cmp(ax, nw, w) >= 0)
        sub(ax, ax, nw, w);
    return cmp(ax, r, w) == 0;
}

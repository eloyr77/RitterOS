/*
 * A TLS 1.3 client (RFC 8446), our own code (step 23d): the handshake with an
 * X25519 key share, the key schedule, records protected with AES-128-GCM,
 * AES-256-GCM or ChaCha20-Poly1305, the server's certificate chain checked
 * against the trusted roots (x509.c) and its CertificateVerify, then
 * application data, session tickets passed over, key updates followed,
 * close_notify. Not here: TLS 1.2 and older, resumption (PSK), early data,
 * client certificates (an empty one is sent if asked), and servers that want
 * another key-exchange group than X25519 (a HelloRetryRequest is refused).
 * Bytes go in and out through the caller's two functions.
 */
#include <string.h>
#include "crypto.h"
#include "ritteros.h"
#include "tls13.h"
#include "x509.h"

#define REC_MAX     (16384 + 256)
enum { CT_CCS = 20, CT_ALERT = 21, CT_HANDSHAKE = 22, CT_APP = 23 };
enum { HS_CLIENT_HELLO = 1, HS_SERVER_HELLO = 2, HS_NEW_TICKET = 4, HS_ENCRYPTED_EXT = 8,
       HS_CERTIFICATE = 11, HS_CERT_REQUEST = 13, HS_CERT_VERIFY = 15, HS_FINISHED = 20,
       HS_KEY_UPDATE = 24 };

static int fail(struct tls13 *t, const char *what)
{
    if (!t->error[0]) {
        size_t i = 0;
        for (; what[i] && i < sizeof(t->error) - 1; i++)
            t->error[i] = what[i];
        t->error[i] = 0;
    }
    t->failed = 1;
    return -1;
}

static void put16(uint8_t *p, size_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put24(uint8_t *p, size_t v) { p[0] = (uint8_t)(v >> 16); put16(p + 1, v); }
static size_t get16(const uint8_t *p) { return (size_t)p[0] << 8 | p[1]; }
static size_t get24(const uint8_t *p) { return (size_t)p[0] << 16 | get16(p + 1); }

/* --- the transcript and the key schedule --- */

static void transcript_add(struct tls13 *t, const uint8_t *msg, size_t n)
{
    sha256_update(&t->th256, msg, n);
    sha512_update(&t->th384, msg, n);
}

static void transcript_hash(struct tls13 *t, uint8_t *out)
{
    if (t->hash_len == 48) {
        struct sha512 copy = t->th384;
        sha384_final(&copy, out);
    } else {
        struct sha256 copy = t->th256;
        sha256_final(&copy, out);
    }
}

static void expand_label(struct tls13 *t, const uint8_t *secret, const char *label,
                         const uint8_t *context, size_t context_len, uint8_t *out, size_t len)
{
    uint8_t info[2 + 1 + 6 + 32 + 1 + 64];
    size_t l = strlen(label);
    put16(info, len);
    info[2] = (uint8_t)(6 + l);
    memcpy(info + 3, "tls13 ", 6);
    memcpy(info + 9, label, l);
    info[9 + l] = (uint8_t)context_len;
    memcpy(info + 10 + l, context, context_len);
    hkdf_expand(t->hash_len, secret, info, 10 + l + context_len, out, len);
}

static void derive_secret(struct tls13 *t, const uint8_t *secret, const char *label,
                          const uint8_t *hash, uint8_t *out)
{
    expand_label(t, secret, label, hash, t->hash_len, out, t->hash_len);
}

/* Keys and IV of one direction from its traffic secret. */
static void set_keys(struct tls13 *t, struct tls13_dir *d, const uint8_t *secret)
{
    uint8_t key[32];
    memcpy(d->secret, secret, t->hash_len);
    expand_label(t, secret, "key", 0, 0, key, t->key_len);
    expand_label(t, secret, "iv", 0, 0, d->iv, 12);
    if (t->suite == 0x1303)
        memcpy(d->chacha, key, 32);
    else
        aes_init(&d->aes, key, t->key_len);
    d->seq = 0;
    d->on = 1;
    memset(key, 0, sizeof(key));
}

/* --- records --- */

static void nonce(const struct tls13_dir *d, uint8_t out[12])
{
    memcpy(out, d->iv, 12);
    for (int i = 0; i < 8; i++)
        out[11 - i] ^= (uint8_t)(d->seq >> (8 * i));
}

/* ChaCha20-Poly1305 (RFC 8439 section 2.8) on one record. */
static void chacha_tag(struct tls13 *t, const uint8_t key[32], const uint8_t n12[12],
                       const uint8_t *aad, size_t aad_n, const uint8_t *c, size_t len, uint8_t tag[16])
{
    uint8_t block[16] = { 0 }, otk[32] = { 0 };
    memcpy(block + 4, n12, 12);                 /* counter 0: the one-time key */
    chacha20(key, block, otk, otk, 32);
    uint8_t *m = t->mac;
    size_t at = 0;
    memcpy(m, aad, aad_n), at += aad_n;
    while (at % 16)
        m[at++] = 0;
    memcpy(m + at, c, len), at += len;
    while (at % 16)
        m[at++] = 0;
    for (int i = 0; i < 8; i++)
        m[at + i] = (uint8_t)((uint64_t)aad_n >> (8 * i));
    for (int i = 0; i < 8; i++)
        m[at + 8 + i] = (uint8_t)((uint64_t)len >> (8 * i));
    at += 16;
    poly1305(otk, m, at, tag);
}

static void chacha_crypt(const uint8_t key[32], const uint8_t n12[12], const uint8_t *in,
                         uint8_t *out, size_t len)
{
    uint8_t block[16] = { 1 };                  /* counter 1 on */
    memcpy(block + 4, n12, 12);
    chacha20(key, block, in, out, len);
}

static int send_all(struct tls13 *t, const uint8_t *p, size_t n)
{
    if (t->send(t->io, p, n) != (long)n)
        return fail(t, "the connection failed (sending)");
    return 0;
}

/* One record of content type `type`: protected once the keys are on. */
static int send_record(struct tls13 *t, int type, const uint8_t *data, size_t n)
{
    uint8_t *r = t->out;
    struct tls13_dir *w = &t->write;
    if (!w->on) {
        r[0] = (uint8_t)type, put16(r + 1, 0x0303), put16(r + 3, n);
        memcpy(r + 5, data, n);
        return send_all(t, r, 5 + n);
    }
    size_t inner = n + 1, total = inner + 16;
    r[0] = CT_APP, put16(r + 1, 0x0303), put16(r + 3, total);
    memcpy(r + 5, data, n);
    r[5 + n] = (uint8_t)type;
    uint8_t n12[12];
    nonce(w, n12);
    if (t->suite == 0x1303) {
        chacha_crypt(w->chacha, n12, r + 5, r + 5, inner);
        chacha_tag(t, w->chacha, n12, r, 5, r + 5, inner, r + 5 + inner);
    } else {
        aes_gcm_seal(&w->aes, n12, r, 5, r + 5, r + 5, inner, r + 5 + inner);
    }
    w->seq++;
    return send_all(t, r, 5 + total);
}

static int recv_exact(struct tls13 *t, uint8_t *p, size_t n)
{
    while (n) {
        long k = t->recv(t->io, p, n);
        if (k <= 0)
            return k == 0 ? 0 : -1;
        p += k;
        n -= (size_t)k;
    }
    return 1;
}

/* The next record's plaintext into t->plain (its type in t->plain_type). */
static int read_record(struct tls13 *t)
{
    uint8_t *h = t->in;
    int r = recv_exact(t, h, 5);
    if (r == 0 && t->done) {
        t->cut = 1;             /* (the end for tls13_read; tls13_close reports it) */
        return 0;
    }
    if (r <= 0)
        return r == 0 ? fail(t, "the server closed the connection") : fail(t, "the connection failed");
    size_t len = get16(h + 3);
    if (len > REC_MAX)
        return fail(t, "a record too long");
    if (recv_exact(t, h + 5, len) <= 0)
        return fail(t, "the connection failed (a record cut short)");
    int type = h[0];
    struct tls13_dir *d = &t->read;
    if (type == CT_CCS && !t->done)             /* (compatibility: passed over) */
        return read_record(t);
    if (!d->on || type != CT_APP) {
        if (d->on)
            return fail(t, "an unprotected record after the keys");
        memcpy(t->plain, h + 5, len);
        t->plain_len = len;
        t->plain_type = type;
        return 1;
    }
    if (len < 17)
        return fail(t, "a record too short");
    size_t inner = len - 16;
    uint8_t n12[12];
    nonce(d, n12);
    if (t->suite == 0x1303) {
        uint8_t tag[16];
        chacha_tag(t, d->chacha, n12, h, 5, h + 5, inner, tag);
        if (!crypto_equal(tag, h + 5 + inner, 16))
            return fail(t, "a record failed its check (bad_record_mac)");
        chacha_crypt(d->chacha, n12, h + 5, t->plain, inner);
    } else if (!aes_gcm_open(&d->aes, n12, h, 5, h + 5, t->plain, inner, h + 5 + inner)) {
        return fail(t, "a record failed its check (bad_record_mac)");
    }
    d->seq++;
    while (inner && !t->plain[inner - 1])       /* the padding, then the real type */
        inner--;
    if (!inner)
        return fail(t, "a record without a type");
    t->plain_type = t->plain[inner - 1];
    t->plain_len = inner - 1;
    return 1;
}

/* The next whole handshake message (type and body in t->hs; its whole bytes
 * hashed into the transcript by the caller as needed). */
static int read_handshake(struct tls13 *t, int *type, size_t *len)
{
    for (;;) {
        if (t->hs_have >= 4) {
            size_t n = get24(t->hs + 1);
            if (n > sizeof(t->hs) - 4)
                return fail(t, "a handshake message too long");
            if (t->hs_have >= 4 + n) {
                *type = t->hs[0];
                *len = n;
                return 1;
            }
        }
        int r = read_record(t);
        if (r <= 0)
            return r < 0 ? -1 : fail(t, "the connection ended in a handshake message");
        if (t->plain_type == CT_ALERT)
            return fail(t, t->plain_len >= 2 && t->plain[1] == 40 ? "the server refused the handshake"
                                                                    : "the server sent an alert");
        if (t->plain_type != CT_HANDSHAKE)
            return fail(t, "not a handshake message");
        if (t->hs_have + t->plain_len > sizeof(t->hs))
            return fail(t, "handshake messages too long");
        memcpy(t->hs + t->hs_have, t->plain, t->plain_len);
        t->hs_have += t->plain_len;
    }
}

static void consume_handshake(struct tls13 *t, size_t len)
{
    size_t used = 4 + len;
    memmove(t->hs, t->hs + used, t->hs_have - used);
    t->hs_have -= used;
}

/* --- the handshake --- */

static int send_handshake(struct tls13 *t, int type, const uint8_t *body, size_t n)
{
    uint8_t *m = t->msg;
    m[0] = (uint8_t)type;
    put24(m + 1, n);
    memcpy(m + 4, body, n);
    transcript_add(t, m, 4 + n);
    return send_record(t, CT_HANDSHAKE, m, 4 + n);
}

static int client_hello(struct tls13 *t)
{
    uint8_t b[512], *p = b;
    size_t host_len = strlen(t->host);
    if (ritteros_random(t->x25519_secret, 32) < 0)
        return fail(t, "no random numbers");
    uint8_t share[32], random[32], session[32];
    x25519_base(share, t->x25519_secret);
    ritteros_random(random, 32);
    ritteros_random(session, 32);
    put16(p, 0x0303), p += 2;
    memcpy(p, random, 32), p += 32;
    *p++ = 32;
    memcpy(p, session, 32), p += 32;                /* (compatibility mode) */
    put16(p, 6), p += 2;
    put16(p, 0x1301), put16(p + 2, 0x1302), put16(p + 4, 0x1303), p += 6;
    *p++ = 1, *p++ = 0;                             /* no compression */
    uint8_t *ext = p;
    p += 2;
    put16(p, 0), put16(p + 2, host_len + 5), put16(p + 4, host_len + 3), p[6] = 0;
    put16(p + 7, host_len), memcpy(p + 9, t->host, host_len), p += 9 + host_len;    /* server_name */
    put16(p, 43), put16(p + 2, 3), p[4] = 2, put16(p + 5, 0x0304), p += 7;          /* supported_versions */
    put16(p, 10), put16(p + 2, 4), put16(p + 4, 2), put16(p + 6, 0x001d), p += 8;   /* supported_groups */
    static const uint16_t schemes[] = { 0x0403, 0x0503, 0x0804, 0x0805, 0x0806, 0x0401, 0x0501, 0x0601 };
    put16(p, 13), put16(p + 2, 2 + 2 * 8), put16(p + 4, 2 * 8), p += 6;              /* signature_algorithms */
    for (int i = 0; i < 8; i++)
        put16(p, schemes[i]), p += 2;
    put16(p, 51), put16(p + 2, 38), put16(p + 4, 36), put16(p + 6, 0x001d), put16(p + 8, 32);   /* key_share */
    memcpy(p + 10, share, 32), p += 42;
    put16(ext, (size_t)(p - ext - 2));
    return send_handshake(t, HS_CLIENT_HELLO, b, (size_t)(p - b));
}

static int server_hello(struct tls13 *t, const uint8_t *m, size_t n)
{
    static const uint8_t hrr[32] = { 0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C,
                                     0x02, 0x1E, 0x65, 0xB8, 0x91, 0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB,
                                     0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C };
    if (n < 2 + 32 + 1)
        return fail(t, "a broken ServerHello");
    if (!memcmp(m + 2, hrr, 32))
        return fail(t, "the server wants another key exchange (only X25519 here)");
    size_t at = 34, sid = m[at++];
    at += sid;
    if (at + 3 + 2 > n)
        return fail(t, "a broken ServerHello");
    t->suite = (int)get16(m + at);
    at += 3;                                    /* (and compression 0) */
    if (t->suite == 0x1301 || t->suite == 0x1303)
        t->hash_len = 32, t->key_len = t->suite == 0x1301 ? 16 : 32;
    else if (t->suite == 0x1302)
        t->hash_len = 48, t->key_len = 32;
    else
        return fail(t, "the server chose a cipher not offered");
    size_t elen = get16(m + at), version = 0;
    at += 2;
    const uint8_t *key = 0;
    for (size_t e = at; e + 4 <= at + elen && e + 4 <= n;) {
        size_t type = get16(m + e), len = get16(m + e + 2);
        const uint8_t *v = m + e + 4;
        if (type == 43 && len == 2)
            version = get16(v);
        if (type == 51 && len == 36 && get16(v) == 0x001d && get16(v + 2) == 32)
            key = v + 4;
        e += 4 + len;
    }
    if (version != 0x0304)
        return fail(t, "the server does not speak TLS 1.3");
    if (!key)
        return fail(t, "the server sent no X25519 key share");
    x25519(t->shared, t->x25519_secret, key);
    static const uint8_t zero32[32];
    if (crypto_equal(t->shared, zero32, 32))
        return fail(t, "a bad key share");
    return 0;
}

static int handshake_keys(struct tls13 *t)
{
    uint8_t zeros[48] = { 0 }, early[48], derived[48], empty[48], hash[48], c[48], s[48];
    struct sha256 e256;
    struct sha512 e384;
    if (t->hash_len == 48) {
        sha384_init(&e384);
        sha384_final(&e384, empty);
    } else {
        sha256_init(&e256);
        sha256_final(&e256, empty);
    }
    hkdf_extract(t->hash_len, 0, 0, zeros, t->hash_len, early);
    derive_secret(t, early, "derived", empty, derived);
    hkdf_extract(t->hash_len, derived, t->hash_len, t->shared, 32, t->handshake_secret);
    transcript_hash(t, hash);
    derive_secret(t, t->handshake_secret, "c hs traffic", hash, c);
    derive_secret(t, t->handshake_secret, "s hs traffic", hash, s);
    set_keys(t, &t->read, s);
    memcpy(t->client_hs, c, t->hash_len);
    derive_secret(t, t->handshake_secret, "derived", empty, derived);
    hkdf_extract(t->hash_len, derived, t->hash_len, zeros, t->hash_len, t->master);
    memset(t->shared, 0, sizeof(t->shared));
    return 0;
}

static int finished_ok(struct tls13 *t, const uint8_t *base, const uint8_t *verify, size_t n)
{
    uint8_t key[48], hash[48], want[48];
    expand_label(t, base, "finished", 0, 0, key, t->hash_len);
    transcript_hash(t, hash);
    if (t->hash_len == 48)
        hmac_sha384(key, 48, hash, 48, want);
    else
        hmac_sha256(key, 32, hash, 32, want);
    return n == t->hash_len && crypto_equal(want, verify, n);
}

static int certificate(struct tls13 *t, const uint8_t *m, size_t n)
{
    if (n < 4 || m[0] != 0)
        return fail(t, "a broken Certificate");
    size_t list = get24(m + 1), at = 4;
    t->ncerts = 0;
    t->cert_bytes = 0;
    while (at + 3 <= 4 + list && at + 3 <= n && t->ncerts < TLS13_CERTS) {
        size_t len = get24(m + at);
        at += 3;
        if (at + len + 2 > n || t->cert_bytes + len > sizeof(t->certs))
            return fail(t, "a broken Certificate");
        memcpy(t->certs + t->cert_bytes, m + at, len);
        if (!x509_parse(&t->chain[t->ncerts], t->certs + t->cert_bytes, len))
            return fail(t, "a certificate this client cannot read");
        t->cert_bytes += len;
        t->ncerts++;
        at += len;
        at += 2 + get16(m + at);                /* its extensions */
    }
    if (!t->ncerts)
        return fail(t, "the server sent no certificate");
    int e = x509_verify_chain(t->chain, t->ncerts, t->roots, t->nroots,
                              t->check_name ? t->host : 0, t->now);
    if (e != X509_OK && !t->insecure)
        return fail(t, x509_error(e));
    return 0;
}

static int certificate_verify(struct tls13 *t, const uint8_t *m, size_t n)
{
    if (n < 4)
        return fail(t, "a broken CertificateVerify");
    size_t scheme = get16(m), len = get16(m + 2);
    if (4 + len > n)
        return fail(t, "a broken CertificateVerify");
    uint8_t content[64 + 34 + 48], digest[64], hash[48];
    memset(content, 0x20, 64);
    memcpy(content + 64, "TLS 1.3, server CertificateVerify", 33);
    content[97] = 0;
    transcript_hash(t, hash);
    memcpy(content + 98, hash, t->hash_len);
    size_t clen = 98 + t->hash_len, dlen;
    const struct x509 *leaf = &t->chain[0];
    const uint8_t *sig = m + 4;
    int ok = 0;
    if (scheme == 0x0403 || scheme == 0x0804)
        sha256(content, clen, digest), dlen = 32;
    else if (scheme == 0x0503 || scheme == 0x0805)
        sha384(content, clen, digest), dlen = 48;
    else if (scheme == 0x0806)
        sha512(content, clen, digest), dlen = 64;
    else
        return fail(t, "a signature scheme not offered");
    if (scheme == 0x0804 || scheme == 0x0805 || scheme == 0x0806) {
        ok = leaf->key_type == X509_KEY_RSA &&
             rsa_verify_pss(leaf->rsa_n, leaf->rsa_n_len, leaf->rsa_e, leaf->rsa_e_len, digest, dlen,
                            sig, len);
    } else {
        int bits = scheme == 0x0403 ? 256 : 384;
        int want = bits == 256 ? X509_KEY_P256 : X509_KEY_P384;
        /* DER: SEQUENCE { INTEGER r, INTEGER s } */
        if (leaf->key_type == want && len > 8 && sig[0] == 0x30) {
            size_t at = 2, rl, sl;
            if (sig[1] & 0x80)
                at = 2 + (sig[1] & 0x7F);
            if (at + 2 <= len && sig[at] == 2 && (rl = sig[at + 1]) && at + 2 + rl + 2 <= len &&
                sig[at + 2 + rl] == 2 && (sl = sig[at + 3 + rl]) && at + 4 + rl + sl <= len)
                ok = ecdsa_verify(bits, leaf->ec_point, leaf->ec_point_len, digest, dlen,
                                  sig + at + 2, rl, sig + at + 4 + rl, sl);
        }
    }
    if (!ok)
        return fail(t, "the server's handshake signature is wrong");
    return 0;
}

int tls13_handshake(struct tls13 *t)
{
    sha256_init(&t->th256);
    sha384_init(&t->th384);
    if (client_hello(t) < 0)
        return -1;
    int type;
    size_t len;
    if (read_handshake(t, &type, &len) < 0)
        return -1;
    if (type != HS_SERVER_HELLO)
        return fail(t, "no ServerHello");
    if (server_hello(t, t->hs + 4, len) < 0)
        return -1;
    transcript_add(t, t->hs, 4 + len);
    consume_handshake(t, len);
    if (t->hs_have)
        return fail(t, "messages after the ServerHello in its record");
    handshake_keys(t);
    int asked_cert = 0, step = 0;           /* 0: EncryptedExtensions, 1: certificate (request), 2: verify, 3: finished */
    for (;;) {
        if (read_handshake(t, &type, &len) < 0)
            return -1;
        const uint8_t *m = t->hs + 4;
        if (type == HS_ENCRYPTED_EXT && step == 0) {
            step = 1;
        } else if (type == HS_CERT_REQUEST && step == 1) {
            asked_cert = 1;
        } else if (type == HS_CERTIFICATE && step == 1) {
            if (certificate(t, m, len) < 0)
                return -1;
            step = 2;
        } else if (type == HS_CERT_VERIFY && step == 2) {
            if (certificate_verify(t, m, len) < 0)
                return -1;
            step = 3;
        } else if (type == HS_FINISHED && step == 3) {
            if (!finished_ok(t, t->read.secret, m, len))
                return fail(t, "the server's Finished is wrong");
            transcript_add(t, t->hs, 4 + len);
            consume_handshake(t, len);
            break;
        } else {
            return fail(t, "handshake messages out of order");
        }
        transcript_add(t, t->hs, 4 + len);
        consume_handshake(t, len);
    }
    uint8_t hash[48], c_ap[48], s_ap[48], verify[48], key[48];
    transcript_hash(t, hash);                   /* through the server's Finished */
    derive_secret(t, t->master, "c ap traffic", hash, c_ap);
    derive_secret(t, t->master, "s ap traffic", hash, s_ap);
    static const uint8_t ccs = 1;
    if (send_record(t, CT_CCS, &ccs, 1) < 0)    /* (compatibility) */
        return -1;
    set_keys(t, &t->write, t->client_hs);
    if (asked_cert) {                           /* no certificate of ours: an empty one */
        static const uint8_t empty[4] = { 0, 0, 0, 0 };
        if (send_handshake(t, HS_CERTIFICATE, empty, 4) < 0)
            return -1;
    }
    expand_label(t, t->client_hs, "finished", 0, 0, key, t->hash_len);
    transcript_hash(t, hash);
    if (t->hash_len == 48)
        hmac_sha384(key, 48, hash, 48, verify);
    else
        hmac_sha256(key, 32, hash, 32, verify);
    if (send_handshake(t, HS_FINISHED, verify, t->hash_len) < 0)
        return -1;
    set_keys(t, &t->read, s_ap);
    set_keys(t, &t->write, c_ap);
    t->done = 1;
    t->plain_len = t->plain_off = 0;
    return 0;
}

/* Handshake messages after the handshake: tickets, key updates. */
static int post_handshake(struct tls13 *t)
{
    if (t->hs_have + t->plain_len > sizeof(t->hs))
        return fail(t, "handshake messages too long");
    memcpy(t->hs + t->hs_have, t->plain, t->plain_len);
    t->hs_have += t->plain_len;
    t->plain_len = 0;
    while (t->hs_have >= 4 && t->hs_have >= 4 + get24(t->hs + 1)) {
        int type = t->hs[0];
        size_t len = get24(t->hs + 1);
        if (type == HS_KEY_UPDATE && len == 1) {
            int request = t->hs[4];
            uint8_t next[48];
            expand_label(t, t->read.secret, "traffic upd", 0, 0, next, t->hash_len);
            set_keys(t, &t->read, next);
            if (request) {                      /* ours too, after telling it */
                uint8_t m[5] = { HS_KEY_UPDATE, 0, 0, 1, 0 };
                if (send_record(t, CT_HANDSHAKE, m, 5) < 0)
                    return -1;
                expand_label(t, t->write.secret, "traffic upd", 0, 0, next, t->hash_len);
                set_keys(t, &t->write, next);
            }
        } else if (type != HS_NEW_TICKET) {
            return fail(t, "an unexpected handshake message");
        }
        consume_handshake(t, len);
    }
    return 0;
}

long tls13_read(struct tls13 *t, void *buf, size_t n)
{
    if (t->failed)
        return -1;
    while (t->plain_off >= t->plain_len) {
        if (t->closed)
            return 0;
        int r = read_record(t);
        if (r < 0)
            return -1;
        if (r == 0) {
            t->closed = 1;
            t->plain_len = 0;
            return 0;
        }
        t->plain_off = 0;
        if (t->plain_type == CT_HANDSHAKE) {
            if (post_handshake(t) < 0)
                return -1;
        } else if (t->plain_type == CT_ALERT) {
            if (t->plain_len >= 2 && t->plain[1] == 0) {    /* close_notify */
                t->closed = 1;
                t->plain_len = 0;
                return 0;
            }
            return fail(t, "the server sent an alert");
        } else if (t->plain_type != CT_APP) {
            return fail(t, "an unexpected record");
        }
    }
    size_t k = t->plain_len - t->plain_off < n ? t->plain_len - t->plain_off : n;
    memcpy(buf, t->plain + t->plain_off, k);
    t->plain_off += k;
    return (long)k;
}

long tls13_write(struct tls13 *t, const void *buf, size_t n)
{
    if (t->failed)
        return -1;
    size_t done = 0;
    while (done < n) {
        size_t k = n - done < 16384 ? n - done : 16384;
        if (send_record(t, CT_APP, (const uint8_t *)buf + done, k) < 0)
            return -1;
        done += k;
    }
    return (long)n;
}

int tls13_close(struct tls13 *t)
{
    static const uint8_t close_notify[2] = { 1, 0 };
    if (t->cut)
        return fail(t, "the connection ended without close_notify (perhaps cut short)");
    if (t->done && !t->failed)
        send_record(t, CT_ALERT, close_notify, 2);
    return 0;
}

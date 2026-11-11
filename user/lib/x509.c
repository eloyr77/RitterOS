/*
 * X.509 certificates (RFC 5280) for TLS (step 23c): reading one (DER), the
 * signature of one by another's key, the host name it is for, and a chain
 * up to a trusted root. Only what TLS needs: no revocation, no name
 * constraints, no policies (an intermediate with name constraints is
 * refused instead of trusted blindly). Certificates are public: no secrets.
 */
#include <string.h>
#include "crypto.h"
#include "ritteros.h"
#include "x509.h"

/* --- DER --- */

struct der {
    const uint8_t *p, *end;
};

/* The next element: its tag, its contents in *in. 0 if malformed. */
static int der_next(struct der *d, int *tag, struct der *in)
{
    if (d->end - d->p < 2)
        return 0;
    *tag = d->p[0];
    size_t len = d->p[1];
    const uint8_t *q = d->p + 2;
    if (len & 0x80) {
        int bytes = (int)(len & 0x7F);
        if (bytes < 1 || bytes > 3 || d->end - q < bytes)
            return 0;
        len = 0;
        for (int i = 0; i < bytes; i++)
            len = len << 8 | *q++;
    }
    if ((size_t)(d->end - q) < len)
        return 0;
    in->p = q;
    in->end = q + len;
    d->p = q + len;
    return 1;
}

static int der_expect(struct der *d, int want, struct der *in)
{
    int tag;
    return der_next(d, &tag, in) && tag == want;
}

#define SEQ  0x30
#define SET  0x31
#define OID  0x06
#define INT  0x02
#define BITS 0x03
#define OCTS 0x04

static int oid_is(const struct der *o, const uint8_t *want, size_t n)
{
    return (size_t)(o->end - o->p) == n && memcmp(o->p, want, n) == 0;
}

/* OIDs (their contents) */
static const uint8_t OID_RSA[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };
static const uint8_t OID_RSA_SHA256[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b };
static const uint8_t OID_RSA_SHA384[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0c };
static const uint8_t OID_RSA_SHA512[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0d };
static const uint8_t OID_EC[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01 };
static const uint8_t OID_P256[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07 };
static const uint8_t OID_P384[] = { 0x2b, 0x81, 0x04, 0x00, 0x22 };
static const uint8_t OID_ECDSA_SHA256[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02 };
static const uint8_t OID_ECDSA_SHA384[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x03 };
static const uint8_t OID_ECDSA_SHA512[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x04 };
static const uint8_t OID_BASIC[] = { 0x55, 0x1d, 0x13 };
static const uint8_t OID_SAN[] = { 0x55, 0x1d, 0x11 };
static const uint8_t OID_NAME_CONSTRAINTS[] = { 0x55, 0x1d, 0x1e };

/* "YYMMDDHHMMSSZ" (UTCTime) or "YYYYMMDDHHMMSSZ": seconds since 1970. */
static int der_time(int tag, const struct der *t, int64_t *out)
{
    size_t n = (size_t)(t->end - t->p);
    const uint8_t *s = t->p;
    int year;
    if (tag == 0x17 && n == 13) {
        year = (s[0] - '0') * 10 + (s[1] - '0');
        year += year < 50 ? 2000 : 1900;
        s += 2;
    } else if (tag == 0x18 && n == 15) {
        year = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
        s += 4;
    } else {
        return 0;
    }
    if (s[10] != 'Z')
        return 0;
    for (int i = 0; i < 10; i++)
        if (s[i] < '0' || s[i] > '9')
            return 0;
    int mon = (s[0] - '0') * 10 + (s[1] - '0'), day = (s[2] - '0') * 10 + (s[3] - '0');
    int h = (s[4] - '0') * 10 + (s[5] - '0'), mi = (s[6] - '0') * 10 + (s[7] - '0');
    int sec = (s[8] - '0') * 10 + (s[9] - '0');
    if (mon < 1 || mon > 12 || day < 1 || day > 31)
        return 0;
    /* days from 1970-01-01 (the civil-from-days inverse) */
    int y = year - (mon <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;
    *out = days * 86400 + h * 3600 + mi * 60 + sec;
    return 1;
}

static int read_key(struct x509 *c, struct der spki)
{
    struct der alg, algo, oid, params, bits;
    if (!der_expect(&spki, SEQ, &alg) || !der_expect(&alg, OID, &oid) ||
        !der_expect(&spki, BITS, &bits) || bits.end - bits.p < 2 || bits.p[0] != 0)
        return 0;
    algo = alg;
    bits.p++;                                   /* (no unused bits) */
    if (oid_is(&oid, OID_RSA, sizeof(OID_RSA))) {
        struct der key, n, e;
        if (!der_expect(&bits, SEQ, &key) || !der_expect(&key, INT, &n) || !der_expect(&key, INT, &e))
            return 0;
        c->key_type = X509_KEY_RSA;
        c->rsa_n = n.p, c->rsa_n_len = (size_t)(n.end - n.p);
        c->rsa_e = e.p, c->rsa_e_len = (size_t)(e.end - e.p);
        return 1;
    }
    if (oid_is(&oid, OID_EC, sizeof(OID_EC)) && der_expect(&algo, OID, &params)) {
        if (oid_is(&params, OID_P256, sizeof(OID_P256)))
            c->key_type = X509_KEY_P256;
        else if (oid_is(&params, OID_P384, sizeof(OID_P384)))
            c->key_type = X509_KEY_P384;
        else
            return 0;
        c->ec_point = bits.p, c->ec_point_len = (size_t)(bits.end - bits.p);
        return 1;
    }
    return 0;                                   /* a kind of key TLS here does not use */
}

static int read_sig_alg(struct der alg, int *out)
{
    struct der oid;
    if (!der_expect(&alg, OID, &oid))
        return 0;
    static const struct { const uint8_t *oid; size_t n; int alg; } known[] = {
        { OID_RSA_SHA256, sizeof(OID_RSA_SHA256), X509_SIG_RSA_SHA256 },
        { OID_RSA_SHA384, sizeof(OID_RSA_SHA384), X509_SIG_RSA_SHA384 },
        { OID_RSA_SHA512, sizeof(OID_RSA_SHA512), X509_SIG_RSA_SHA512 },
        { OID_ECDSA_SHA256, sizeof(OID_ECDSA_SHA256), X509_SIG_ECDSA_SHA256 },
        { OID_ECDSA_SHA384, sizeof(OID_ECDSA_SHA384), X509_SIG_ECDSA_SHA384 },
        { OID_ECDSA_SHA512, sizeof(OID_ECDSA_SHA512), X509_SIG_ECDSA_SHA512 },
    };
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (oid_is(&oid, known[i].oid, known[i].n)) {
            *out = known[i].alg;
            return 1;
        }
    return 0;
}

static int read_extensions(struct x509 *c, struct der exts)
{
    struct der list, ext;
    if (!der_expect(&exts, SEQ, &list))
        return 0;
    while (list.p < list.end) {
        struct der oid, value;
        int tag, critical = 0;
        if (!der_expect(&list, SEQ, &ext) || !der_expect(&ext, OID, &oid) || !der_next(&ext, &tag, &value))
            return 0;
        if (tag == 0x01) {                      /* critical BOOLEAN, then the value */
            critical = value.end > value.p && value.p[0];
            if (!der_expect(&ext, OCTS, &value))
                return 0;
        } else if (tag != OCTS) {
            return 0;
        }
        if (oid_is(&oid, OID_BASIC, sizeof(OID_BASIC))) {
            struct der bc, item;
            if (!der_expect(&value, SEQ, &bc))
                return 0;
            c->path_len = -1;
            while (bc.p < bc.end) {
                if (!der_next(&bc, &tag, &item))
                    return 0;
                if (tag == 0x01)
                    c->is_ca = item.end > item.p && item.p[0];
                else if (tag == INT && item.end - item.p == 1)
                    c->path_len = item.p[0];
            }
        } else if (oid_is(&oid, OID_SAN, sizeof(OID_SAN))) {
            struct der names;
            if (!der_expect(&value, SEQ, &names))
                return 0;
            c->san = names.p, c->san_len = (size_t)(names.end - names.p);
        } else if (oid_is(&oid, OID_NAME_CONSTRAINTS, sizeof(OID_NAME_CONSTRAINTS))) {
            c->name_constraints = 1;
        } else if (critical) {
            /* key usage and extended key usage are fine to pass over here;
             * other critical ones we do not understand refuse the certificate */
            static const uint8_t KU[] = { 0x55, 0x1d, 0x0f }, EKU[] = { 0x55, 0x1d, 0x25 };
            if (!oid_is(&oid, KU, 3) && !oid_is(&oid, EKU, 3))
                return 0;
        }
    }
    return 1;
}

int x509_parse(struct x509 *c, const uint8_t *der, size_t len)
{
    memset(c, 0, sizeof(*c));
    c->path_len = -1;
    struct der all = { der, der + len }, cert, tbs, alg, sig, field;
    int tag;
    if (!der_expect(&all, SEQ, &cert))
        return 0;
    c->der = der;
    c->der_len = (size_t)(cert.end - der);
    const uint8_t *tbs_start = cert.p;
    if (!der_expect(&cert, SEQ, &tbs))
        return 0;
    c->tbs = tbs_start, c->tbs_len = (size_t)(tbs.end - tbs_start);
    /* (An algorithm we do not take -- SHA-1 -- leaves sig_alg 0: such a
     * certificate is never accepted as signed, but a root's own signature is
     * never checked, so an old root still serves.) */
    if (!der_expect(&cert, SEQ, &alg) || !der_expect(&cert, BITS, &sig) || sig.end - sig.p < 2 ||
        sig.p[0] != 0)
        return 0;
    if (!read_sig_alg(alg, &c->sig_alg))
        c->sig_alg = 0;
    c->sig = sig.p + 1, c->sig_len = (size_t)(sig.end - sig.p - 1);
    /* tbsCertificate: [0] version, serial, alg, issuer, validity, subject, spki, ..., [3] exts */
    if (!der_next(&tbs, &tag, &field))
        return 0;
    if (tag == 0xA0 && !der_expect(&tbs, INT, &field))     /* the version, then the serial */
        return 0;
    if (!der_expect(&tbs, SEQ, &field))         /* the signature algorithm, again */
        return 0;
    const uint8_t *issuer_start = tbs.p;
    if (!der_expect(&tbs, SEQ, &field))
        return 0;
    c->issuer = issuer_start, c->issuer_len = (size_t)(field.end - issuer_start);
    struct der validity, t;
    if (!der_expect(&tbs, SEQ, &validity) || !der_next(&validity, &tag, &t) ||
        !der_time(tag, &t, &c->not_before) || !der_next(&validity, &tag, &t) ||
        !der_time(tag, &t, &c->not_after))
        return 0;
    const uint8_t *subject_start = tbs.p;
    if (!der_expect(&tbs, SEQ, &field))
        return 0;
    c->subject = subject_start, c->subject_len = (size_t)(field.end - subject_start);
    if (!der_expect(&tbs, SEQ, &field) || !read_key(c, field))
        return 0;
    while (tbs.p < tbs.end) {
        if (!der_next(&tbs, &tag, &field))
            return 0;
        if (tag == 0xA3 && !read_extensions(c, field))
            return 0;
    }
    return 1;
}

/* Was child signed with parent's key? */
int x509_signed_by(const struct x509 *child, const struct x509 *parent)
{
    uint8_t digest[64];
    size_t dlen;
    int alg = child->sig_alg;
    if (!alg)
        return 0;
    if (alg == X509_SIG_RSA_SHA256 || alg == X509_SIG_ECDSA_SHA256)
        sha256(child->tbs, child->tbs_len, digest), dlen = 32;
    else if (alg == X509_SIG_RSA_SHA384 || alg == X509_SIG_ECDSA_SHA384)
        sha384(child->tbs, child->tbs_len, digest), dlen = 48;
    else
        sha512(child->tbs, child->tbs_len, digest), dlen = 64;
    if (alg == X509_SIG_RSA_SHA256 || alg == X509_SIG_RSA_SHA384 || alg == X509_SIG_RSA_SHA512)
        return parent->key_type == X509_KEY_RSA &&
               rsa_verify_pkcs1(parent->rsa_n, parent->rsa_n_len, parent->rsa_e,
                                parent->rsa_e_len, digest, dlen, child->sig, child->sig_len);
    struct der s = { child->sig, child->sig + child->sig_len }, seq, r, ss;
    if (!der_expect(&s, SEQ, &seq) || !der_expect(&seq, INT, &r) || !der_expect(&seq, INT, &ss))
        return 0;
    int bits = parent->key_type == X509_KEY_P256 ? 256 : parent->key_type == X509_KEY_P384 ? 384 : 0;
    return bits && ecdsa_verify(bits, parent->ec_point, parent->ec_point_len, digest, dlen, r.p,
                                (size_t)(r.end - r.p), ss.p, (size_t)(ss.end - ss.p));
}

/* One DNS name against the host (case-blind; "*." only as the whole leftmost label). */
static int name_matches(const uint8_t *name, size_t n, const char *host)
{
    size_t h = strlen(host);
    if (n >= 2 && name[0] == '*' && name[1] == '.') {
        const char *dot = strchr(host, '.');
        if (!dot || dot == host)
            return 0;
        host = dot;                             /* "*" covers exactly one label */
        h = strlen(host);
        name++, n--;
    }
    if (n != h)
        return 0;
    for (size_t i = 0; i < n; i++) {
        int a = name[i], b = host[i];
        if (a >= 'A' && a <= 'Z')
            a += 32;
        if (b >= 'A' && b <= 'Z')
            b += 32;
        if (a != b)
            return 0;
    }
    return 1;
}

int x509_host_matches(const struct x509 *c, const char *host)
{
    struct der names = { c->san, c->san + c->san_len }, name;
    int tag;
    while (c->san && names.p < names.end) {
        if (!der_next(&names, &tag, &name))
            return 0;
        if (tag == 0x82 && name_matches(name.p, (size_t)(name.end - name.p), host))
            return 1;                           /* [2] dNSName */
    }
    return 0;                                   /* (the subject's CN is not used: RFC 6125) */
}

static int same_name(const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
    return an == bn && memcmp(a, b, an) == 0;
}

int x509_verify_chain(const struct x509 *chain, int count, const struct x509 *roots, int nroots,
                      const char *host, int64_t now)
{
    if (count < 1)
        return X509_ERR_EMPTY;
    if (host && !x509_host_matches(&chain[0], host))
        return X509_ERR_NAME;
    for (int i = 0; i < count; i++) {
        const struct x509 *c = &chain[i];
        if (now && (now < c->not_before || now > c->not_after))
            return X509_ERR_TIME;
        if (i > 0 && (!c->is_ca || c->name_constraints ||
                      (c->path_len >= 0 && c->path_len < i - 1)))
            return X509_ERR_NOT_CA;
        /* signed by a root? then done */
        for (int k = 0; k < nroots; k++)
            if (same_name(c->issuer, c->issuer_len, roots[k].subject, roots[k].subject_len) &&
                x509_signed_by(c, &roots[k])) {
                if (now && (now < roots[k].not_before || now > roots[k].not_after))
                    return X509_ERR_TIME;
                return X509_OK;
            }
        /* a root sent by the server itself counts only if we have it too */
        if (i + 1 < count) {
            const struct x509 *p = &chain[i + 1];
            if (!same_name(c->issuer, c->issuer_len, p->subject, p->subject_len))
                return X509_ERR_CHAIN;
            if (!x509_signed_by(c, p))
                return X509_ERR_SIGNATURE;
        }
    }
    return X509_ERR_UNTRUSTED;
}

const char *x509_error(int e)
{
    switch (e) {
    case X509_OK:            return "ok";
    case X509_ERR_EMPTY:     return "no certificate";
    case X509_ERR_NAME:      return "the certificate is not for this host name";
    case X509_ERR_TIME:      return "a certificate is expired or not yet valid";
    case X509_ERR_NOT_CA:    return "a certificate in the chain may not sign others";
    case X509_ERR_CHAIN:     return "the certificates do not form a chain";
    case X509_ERR_SIGNATURE: return "a certificate's signature is wrong";
    case X509_ERR_UNTRUSTED: return "no trusted root certificate signed the chain";
    }
    return "unknown";
}

/* --- PEM --- */

static int b64(int ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

size_t pem_next(const char **text, const char *end, uint8_t *out, size_t size)
{
    static const char begin[] = "-----BEGIN CERTIFICATE-----", stop[] = "-----END CERTIFICATE-----";
    const char *p = *text;
    for (;;) {
        while (p < end && *p != '-')
            p++;
        if ((size_t)(end - p) < sizeof(begin) - 1) {
            *text = end;
            return 0;
        }
        if (memcmp(p, begin, sizeof(begin) - 1) == 0)
            break;
        p++;
    }
    p += sizeof(begin) - 1;
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    while (p < end && *p != '-') {
        int v = b64(*p++);
        if (v < 0)
            continue;                           /* line breaks, '=' */
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n < size)
                out[n] = (uint8_t)(acc >> bits);
            n++;
        }
    }
    if ((size_t)(end - p) >= sizeof(stop) - 1 && memcmp(p, stop, sizeof(stop) - 1) == 0)
        p += sizeof(stop) - 1;
    *text = p;
    return n <= size ? n : 0;
}

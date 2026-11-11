/*
 * X.509 certificates for TLS (step 23c; x509.c): what a certificate says,
 * checked signatures, host names, chains to trusted roots, PEM bundles.
 */
#ifndef RITTEROS_X509_H
#define RITTEROS_X509_H

#include <stddef.h>
#include <stdint.h>

enum { X509_KEY_RSA = 1, X509_KEY_P256, X509_KEY_P384 };
enum { X509_SIG_RSA_SHA256 = 1, X509_SIG_RSA_SHA384, X509_SIG_RSA_SHA512,
       X509_SIG_ECDSA_SHA256, X509_SIG_ECDSA_SHA384, X509_SIG_ECDSA_SHA512 };
enum { X509_OK = 0, X509_ERR_EMPTY, X509_ERR_NAME, X509_ERR_TIME, X509_ERR_NOT_CA,
       X509_ERR_CHAIN, X509_ERR_SIGNATURE, X509_ERR_UNTRUSTED };

/* One certificate; the pointers point into its DER bytes (kept by the caller). */
struct x509 {
    const uint8_t *der, *tbs, *sig, *issuer, *subject, *san;
    size_t der_len, tbs_len, sig_len, issuer_len, subject_len, san_len;
    int sig_alg, key_type;
    const uint8_t *rsa_n, *rsa_e, *ec_point;
    size_t rsa_n_len, rsa_e_len, ec_point_len;
    int64_t not_before, not_after;              /* seconds since 1970 */
    int is_ca, path_len, name_constraints;
};

int x509_parse(struct x509 *c, const uint8_t *der, size_t len);         /* 1, or 0: refused */
int x509_signed_by(const struct x509 *child, const struct x509 *parent);
int x509_host_matches(const struct x509 *c, const char *host);
/* The server's chain (its own first), the trusted roots, the host name and
 * the time now (host 0: any name; now 0: any time): X509_OK or what is
 * wrong (x509_error). */
int x509_verify_chain(const struct x509 *chain, int count, const struct x509 *roots, int nroots,
                      const char *host, int64_t now);
const char *x509_error(int e);
/* The next certificate of a PEM text (from *text, which moves on): its DER
 * bytes in out (at most size), their length; 0 at the end or if too long. */
size_t pem_next(const char **text, const char *end, uint8_t *out, size_t size);

#endif

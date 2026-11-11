/*
 * TLS (step 23d): connections protected by TLS 1.3, as OpenBSD's libtls
 * offers them (a subset, the same names and behaviour), in libtls.so.1. A
 * client checks the server's certificate chain against the trusted roots
 * (by default /system/certs/cacert.pem), its host name and the dates. Our
 * own code (RFC 8446): TLS 1.3 only, X25519, AES-128/256-GCM and
 * ChaCha20-Poly1305, RSA and ECDSA (P-256, P-384) certificates.
 *
 *     struct tls *t = tls_client();
 *     tls_configure(t, 0);                 // (0: the defaults)
 *     if (tls_connect(t, "example.com", "443") < 0) puts(tls_error(t));
 *     tls_write(t, request, n); n = tls_read(t, buf, sizeof(buf));
 *     tls_close(t); tls_free(t);
 *
 * Calls return 0 (or a byte count), -1 on failure (tls_error says why).
 * Everything blocks: TLS_WANT_POLLIN/POLLOUT are never returned here.
 */
#ifndef RITTEROS_TLS_H
#define RITTEROS_TLS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define TLS_API             20200120    /* (OpenBSD's version of this interface) */
#define TLS_WANT_POLLIN     (-2)
#define TLS_WANT_POLLOUT    (-3)
#define TLS_DEFAULT_CA_FILE "/system/certs/cacert.pem"

struct tls;
struct tls_config;

int tls_init(void);

/* A configuration: which roots to trust, what to check. */
struct tls_config *tls_config_new(void);
void tls_config_free(struct tls_config *config);
const char *tls_config_error(struct tls_config *config);
const char *tls_default_ca_cert_file(void);
int tls_config_set_ca_file(struct tls_config *config, const char *file);       /* PEM */
int tls_config_set_ca_mem(struct tls_config *config, const uint8_t *pem, size_t length);
void tls_config_insecure_noverifycert(struct tls_config *config);     /* no chain check */
void tls_config_insecure_noverifyname(struct tls_config *config);     /* any host name */
void tls_config_insecure_noverifytime(struct tls_config *config);     /* any dates */
void tls_config_verify(struct tls_config *config);                     /* all of them again */

/* A client connection. */
struct tls *tls_client(void);
int tls_configure(struct tls *ctx, struct tls_config *config);
void tls_reset(struct tls *ctx);
void tls_free(struct tls *ctx);
int tls_connect(struct tls *ctx, const char *host, const char *port);
int tls_connect_servername(struct tls *ctx, const char *host, const char *port,
                           const char *servername);
int tls_connect_socket(struct tls *ctx, int socket, const char *servername);
int tls_handshake(struct tls *ctx);
ssize_t tls_read(struct tls *ctx, void *buf, size_t length);
ssize_t tls_write(struct tls *ctx, const void *buf, size_t length);
/* tls_read returns 0 at the end, also when the server just closed the
 * connection; tls_close then fails ("...without close_notify"): the data
 * may have been cut short (as OpenBSD's). */
int tls_close(struct tls *ctx);
const char *tls_error(struct tls *ctx);

/* After the handshake: "TLSv1.3"; the cipher suite's name; the host name. */
const char *tls_conn_version(struct tls *ctx);
const char *tls_conn_cipher(struct tls *ctx);
const char *tls_conn_servername(struct tls *ctx);

#endif

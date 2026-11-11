/*
 * The TLS 1.3 client's state (tls13.c, step 23d; libtls's tls.c is the
 * interface): one connection. Large (records, handshake messages, the
 * server's certificates): allocate it. The caller sets send/recv/io, host
 * (the name sent and checked), roots/nroots, now (seconds since 1970; 0:
 * dates not checked), check_name, and insecure (the chain not checked).
 */
#ifndef RITTEROS_TLS13_H
#define RITTEROS_TLS13_H

#include <stdint.h>
#include "crypto.h"
#include "ritteros.h"
#include "x509.h"

#define TLS13_CERTS 8
#define TLS13_REC (16384 + 256)

struct tls13_dir {                              /* one direction's keys */
    uint8_t secret[48], iv[12], chacha[32];
    struct aes aes;
    uint64_t seq;
    int on;
};

struct tls13 {
    long (*send)(void *io, const void *p, size_t n);   /* all of it, or -1 */
    long (*recv)(void *io, void *p, size_t n);         /* some, 0 at the end, -1 */
    void *io;
    char host[256];
    const struct x509 *roots;
    int nroots;
    int64_t now;
    int insecure, check_name;
    char error[160];
    int failed, done, closed, cut;     /* cut: the end came without close_notify */
    int suite;
    size_t hash_len, key_len;
    struct sha256 th256;
    struct sha512 th384;
    uint8_t x25519_secret[32], shared[32], handshake_secret[48], master[48], client_hs[48];
    struct tls13_dir read, write;
    uint8_t in[5 + TLS13_REC], out[5 + TLS13_REC], msg[1024], plain[TLS13_REC];
    size_t plain_len, plain_off;
    int plain_type;
    uint8_t hs[65536 + 4];
    size_t hs_have;
    uint8_t mac[TLS13_REC + 64];
    uint8_t certs[32768];
    size_t cert_bytes;
    struct x509 chain[TLS13_CERTS];
    int ncerts;
};

int tls13_handshake(struct tls13 *t);                   /* 0, or -1 (t->error) */
long tls13_read(struct tls13 *t, void *buf, size_t n);  /* bytes, 0 at the end, -1 */
long tls13_write(struct tls13 *t, const void *buf, size_t n);
int tls13_close(struct tls13 *t);                       /* -1: the end came without close_notify */

#endif

/*
 * libtls (step 23d): OpenBSD's libtls interface (tls.h) over our TLS 1.3
 * client (tls13.c). A configuration holds the trusted roots (read from a PEM
 * file or memory, the default file on first use) and what to check; a
 * connection holds one tls13 state (large: allocated), its socket, and its
 * last error.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "ritteros.h"
#include "tls.h"
#include "tls13.h"
#include "x509.h"

#define ROOTS_MAX 256

struct tls_config {
    uint8_t *der;                       /* every root's DER, one after another */
    struct x509 *roots;
    int nroots;
    int loaded;                         /* roots read (or tried) */
    int noverifycert, noverifyname, noverifytime;
    char error[160];
};

struct tls {
    struct tls_config *config;
    struct tls_config own;              /* when configured with 0 */
    struct tls13 *t;
    int fd, own_fd, connected, handshaken;
    char servername[256];
    char error[200];
};

static struct tls_config defaults;

int tls_init(void)
{
    return 0;
}

struct tls_config *tls_config_new(void)
{
    return calloc(1, sizeof(struct tls_config));
}

static void roots_free(struct tls_config *c)
{
    free(c->der);
    free(c->roots);
    c->der = 0;
    c->roots = 0;
    c->nroots = 0;
    c->loaded = 0;
}

void tls_config_free(struct tls_config *c)
{
    if (!c)
        return;
    roots_free(c);
    free(c);
}

const char *tls_config_error(struct tls_config *c)
{
    return c && c->error[0] ? c->error : 0;
}

const char *tls_default_ca_cert_file(void)
{
    return TLS_DEFAULT_CA_FILE;
}

int tls_config_set_ca_mem(struct tls_config *c, const uint8_t *pem, size_t length)
{
    roots_free(c);
    c->der = malloc(length);            /* (DER is shorter than its PEM) */
    c->roots = malloc(ROOTS_MAX * sizeof(struct x509));
    if (!c->der || !c->roots) {
        snprintf(c->error, sizeof(c->error), "out of memory");
        return -1;
    }
    const char *text = (const char *)pem, *end = text + length;
    size_t used = 0, n;
    while (c->nroots < ROOTS_MAX && (n = pem_next(&text, end, c->der + used, length - used))) {
        if (x509_parse(&c->roots[c->nroots], c->der + used, n))
            c->nroots++;                /* (a root of a kind we cannot use is left out) */
        used += n;
    }
    c->loaded = 1;
    if (!c->nroots) {
        snprintf(c->error, sizeof(c->error), "no certificate found");
        return -1;
    }
    return 0;
}

int tls_config_set_ca_file(struct tls_config *c, const char *file)
{
    int fd = open(file, O_RDONLY);
    if (fd < 0) {
        snprintf(c->error, sizeof(c->error), "%s: %s", file, strerror(errno));
        c->loaded = 1;
        return -1;
    }
    size_t size = 0, cap = 65536;
    uint8_t *buf = malloc(cap);
    ssize_t k;
    while (buf && (k = read(fd, buf + size, cap - size)) > 0) {
        size += (size_t)k;
        if (size == cap) {
            uint8_t *more = realloc(buf, cap * 2);
            if (!more) {
                free(buf);
                buf = 0;
                break;
            }
            buf = more;
            cap *= 2;
        }
    }
    close(fd);
    if (!buf) {
        snprintf(c->error, sizeof(c->error), "out of memory");
        return -1;
    }
    int r = tls_config_set_ca_mem(c, buf, size);
    free(buf);
    if (r < 0 && !strcmp(c->error, "no certificate found"))
        snprintf(c->error, sizeof(c->error), "%s: no certificate found", file);
    return r;
}

void tls_config_insecure_noverifycert(struct tls_config *c) { c->noverifycert = 1; }
void tls_config_insecure_noverifyname(struct tls_config *c) { c->noverifyname = 1; }
void tls_config_insecure_noverifytime(struct tls_config *c) { c->noverifytime = 1; }

void tls_config_verify(struct tls_config *c)
{
    c->noverifycert = c->noverifyname = c->noverifytime = 0;
}

struct tls *tls_client(void)
{
    struct tls *ctx = calloc(1, sizeof(*ctx));
    if (ctx) {
        ctx->fd = -1;
        ctx->config = &defaults;
    }
    return ctx;
}

int tls_configure(struct tls *ctx, struct tls_config *config)
{
    ctx->config = config ? config : &defaults;
    return 0;
}

const char *tls_error(struct tls *ctx)
{
    return ctx && ctx->error[0] ? ctx->error : 0;
}

static int error(struct tls *ctx, const char *what)
{
    snprintf(ctx->error, sizeof(ctx->error), "%s", what);
    return -1;
}

static long io_send(void *io, const void *p, size_t n)
{
    int fd = *(int *)io;
    size_t done = 0;
    while (done < n) {
        ssize_t k = write(fd, (const char *)p + done, n - done);
        if (k <= 0)
            return -1;
        done += (size_t)k;
    }
    return (long)n;
}

static long io_recv(void *io, void *p, size_t n)
{
    return (long)read(*(int *)io, p, n);
}

int tls_connect_socket(struct tls *ctx, int s, const char *servername)
{
    if (ctx->connected)
        return error(ctx, "already connected");
    if (!servername || strlen(servername) >= sizeof(ctx->servername))
        return error(ctx, "a server name is needed");
    ctx->fd = s;
    snprintf(ctx->servername, sizeof(ctx->servername), "%s", servername);
    ctx->connected = 1;
    return 0;
}

/* (64d) A TCP connection to host's port: each of getaddrinfo's addresses
 * in its order (IPv6 first where the machine has it) until one answers; a
 * descriptor, or -1 with errno (ENONAME and the like for the name, the
 * last connect's otherwise). A host in brackets is an IPv6 address. */
static int connect_name(const char *host, const char *service, bool *by_name)
{
    *by_name = true;
    char name[256];
    size_t n = strlen(host);
    if (n >= 2 && host[0] == '[' && host[n - 1] == ']' && n - 2 < sizeof(name)) {
        memcpy(name, host + 1, n - 2);
        name[n - 2] = 0;
        host = name;
    }
    struct addrinfo hints, *list;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    errno = 0;
    int r = getaddrinfo(host, service, &hints, &list);
    if (r) {
        if (!errno)
            errno = r == EAI_NONAME ? ENONAME : EINVAL;
        return -1;
    }
    int fd = -1;
    *by_name = false;                               /* (from here on: the connection's) */
    for (struct addrinfo *a = list; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) < 0) {
            int e = errno;
            close(fd);
            fd = -1;
            errno = e;
        }
    }
    freeaddrinfo(list);
    return fd;
}

int tls_connect_servername(struct tls *ctx, const char *host, const char *port,
                           const char *servername)
{
    bool name;
    int fd = connect_name(host, port ? port : "443", &name);
    if (fd < 0) {
        if (name)                                   /* (the name, not the connection) */
            snprintf(ctx->error, sizeof(ctx->error), "%s: %s", host, strerror(errno));
        else
            snprintf(ctx->error, sizeof(ctx->error), "%s port %s: %s", host, port ? port : "443",
                     strerror(errno));
        return -1;
    }
    if (tls_connect_socket(ctx, fd, servername ? servername : host) < 0) {
        close(fd);
        return -1;
    }
    ctx->own_fd = 1;
    return tls_handshake(ctx);
}

int tls_connect(struct tls *ctx, const char *host, const char *port)
{
    return tls_connect_servername(ctx, host, port, host);
}

int tls_handshake(struct tls *ctx)
{
    if (ctx->handshaken)
        return 0;
    if (!ctx->connected)
        return error(ctx, "not connected");
    struct tls_config *c = ctx->config;
    if (!c->loaded && !c->noverifycert && tls_config_set_ca_file(c, TLS_DEFAULT_CA_FILE) < 0)
        return error(ctx, c->error);
    if (!c->noverifycert && !c->nroots)
        return error(ctx, c->error[0] ? c->error : "no trusted roots");
    if (!ctx->t && !(ctx->t = calloc(1, sizeof(struct tls13))))
        return error(ctx, "out of memory");
    struct tls13 *t = ctx->t;
    t->send = io_send;
    t->recv = io_recv;
    t->io = &ctx->fd;
    snprintf(t->host, sizeof(t->host), "%s", ctx->servername);
    t->roots = c->roots;
    t->nroots = c->nroots;
    t->now = c->noverifytime ? 0 : (int64_t)(ritteros_time() / 1000000000ULL);
    t->insecure = c->noverifycert;
    t->check_name = !c->noverifyname;
    if (tls13_handshake(t) < 0)
        return error(ctx, t->error);
    ctx->handshaken = 1;
    return 0;
}

ssize_t tls_read(struct tls *ctx, void *buf, size_t length)
{
    if (tls_handshake(ctx) < 0)
        return -1;
    long n = tls13_read(ctx->t, buf, length);
    if (n < 0)
        return error(ctx, ctx->t->error);
    return (ssize_t)n;
}

ssize_t tls_write(struct tls *ctx, const void *buf, size_t length)
{
    if (tls_handshake(ctx) < 0)
        return -1;
    long n = tls13_write(ctx->t, buf, length);
    if (n < 0)
        return error(ctx, ctx->t->error);
    return (ssize_t)n;
}

int tls_close(struct tls *ctx)
{
    int r = 0;
    if (ctx->t && ctx->handshaken && tls13_close(ctx->t) < 0)
        r = error(ctx, ctx->t->error);  /* (as OpenBSD's: the data may have been cut short) */
    if (ctx->own_fd && ctx->fd >= 0)
        close(ctx->fd);
    ctx->fd = -1;
    ctx->own_fd = ctx->connected = ctx->handshaken = 0;
    return r;
}

void tls_reset(struct tls *ctx)
{
    struct tls_config *config = ctx->config;
    tls_close(ctx);
    free(ctx->t);
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;
    ctx->config = config;
}

void tls_free(struct tls *ctx)
{
    if (!ctx)
        return;
    tls_close(ctx);
    free(ctx->t);
    free(ctx);
}

const char *tls_conn_version(struct tls *ctx)
{
    return ctx->handshaken ? "TLSv1.3" : 0;
}

const char *tls_conn_cipher(struct tls *ctx)
{
    if (!ctx->handshaken)
        return 0;
    return ctx->t->suite == 0x1301 ? "TLS_AES_128_GCM_SHA256" :
           ctx->t->suite == 0x1302 ? "TLS_AES_256_GCM_SHA384" : "TLS_CHACHA20_POLY1305_SHA256";
}

const char *tls_conn_servername(struct tls *ctx)
{
    return ctx->connected ? ctx->servername : 0;
}

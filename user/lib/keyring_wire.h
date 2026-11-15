/*
 * (70b, item 48) What keyringd and its clients say to each other over the
 * local socket /run/keyring/UID: libkeyring (user/lib/keyring.c), the
 * keyring command, login and passwd. Not an interface of its own: a
 * program uses keyring.h. Each message is a 32-bit length (of what
 * follows, at most KW_MESSAGE_MAX), a byte (the request, or the answer's
 * status) and fields, each a 32-bit length and its bytes; numbers are
 * little-endian. A name says nothing about who bound it (any program may
 * bind any name), so a client first checks that its own user is at the
 * other end (kw_connect). Static inline functions, as keyring_db.h.
 */
#ifndef KEYRING_WIRE_H
#define KEYRING_WIRE_H

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

#define KW_SOCKET        "/run/keyring/%u"
#define KW_SYSTEM_SOCKET "/run/keyring/system"  /* (70d) keyringd -s, root's */
#define KW_MESSAGE_MAX   65536
#define KW_FIELDS_MAX    513            /* a list's total and 256 pairs */
#define KW_WAIT_MS       60000          /* an answer, an unlock's rounds included */

enum {                                  /* requests */
    KW_GET = 1,                         /* service, account -> the secret */
    KW_SET = 2,                         /* service, account, secret */
    KW_REMOVE = 3,                      /* service, account */
    KW_LIST = 4,                        /* the first entry's index (4 bytes) -> total
                                         * (4 bytes), then service, account pairs */
    KW_LOCK = 5,
    KW_UNLOCK = 6,                      /* password */
    KW_PASSWORD = 7,                    /* the present password, the new one */
    KW_SESSION = 8,                     /* a login's: kept open while it lasts */
    KW_STATUS = 9,                      /* -> "open" or "locked" */
    KW_ALLOW = 10,                      /* (70c) password, program, service -> how many entries */
};

enum {                                  /* answers */
    KW_OK = 0,
    KW_NOT_FOUND = 1,
    KW_LOCKED = 2,
    KW_WRONG = 3,                       /* a wrong password */
    KW_INVALID = 4,                     /* a name, a size or a request refused */
    KW_FAILED = 5,                      /* not saved: errno's number in a field */
    KW_REFUSED = 6,                     /* not this keyring's user */
    KW_NOT_ALLOWED = 7,                 /* (70c) not for this program (the user did not allow it) */
};

struct kw_field {
    const void *data;
    uint32_t length;
};

struct kw_message {
    uint8_t code;
    int count;
    struct kw_field field[KW_FIELDS_MAX];
    uint8_t bytes[KW_MESSAGE_MAX];
};

static inline void kw_put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static inline uint32_t kw_get32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline bool kw_write_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return false;
        p += w, n -= (size_t)w;
    }
    return true;
}

/* Bytes, waiting at most wait_ms for each part (-1: for ever). */
static inline bool kw_read_all(int fd, uint8_t *p, size_t n, int wait_ms)
{
    while (n) {
        struct pollfd pf = { fd, POLLIN, 0 };
        int r = poll(&pf, 1, wait_ms);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            errno = r == 0 ? ETIMEDOUT : errno;
            return false;
        }
        ssize_t got = read(fd, p, n);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            errno = got == 0 ? ECONNRESET : errno;
            return false;
        }
        p += got, n -= (size_t)got;
    }
    return true;
}

/* A message of code and count fields; false (errno) if it was not sent
 * whole or would be too long (EMSGSIZE). */
static inline bool kw_send(int fd, uint8_t code, const struct kw_field *fields, int count)
{
    size_t n = 1;
    for (int i = 0; i < count; i++)
        n += 4 + fields[i].length;
    if (n > KW_MESSAGE_MAX) {
        errno = EMSGSIZE;
        return false;
    }
    uint8_t *out = malloc(4 + n);             /* (no static buffer: threads share the library) */
    if (!out)
        return false;
    kw_put32(out, (uint32_t)n);
    out[4] = code;
    size_t at = 5;
    for (int i = 0; i < count; i++) {
        kw_put32(out + at, fields[i].length);
        memcpy(out + at + 4, fields[i].data, fields[i].length);
        at += 4 + fields[i].length;
    }
    bool ok = kw_write_all(fd, out, at);
    explicit_bzero(out, at);                    /* (secrets and passwords pass here) */
    free(out);
    return ok;
}

/* The next message into m; false (errno) on an end, a wait run out or a
 * message not well formed (EPROTO). Fields past KW_FIELDS_MAX are refused. */
static inline bool kw_receive(int fd, struct kw_message *m, int wait_ms)
{
    uint8_t head[4];
    if (!kw_read_all(fd, head, 4, wait_ms))
        return false;
    uint32_t n = kw_get32(head);
    if (n < 1 || n > KW_MESSAGE_MAX) {
        errno = EPROTO;
        return false;
    }
    if (!kw_read_all(fd, m->bytes, n, wait_ms))
        return false;
    m->code = m->bytes[0];
    m->count = 0;
    uint32_t at = 1;
    while (at < n) {
        if (m->count == KW_FIELDS_MAX || n - at < 4 || kw_get32(m->bytes + at) > n - at - 4) {
            errno = EPROTO;
            return false;
        }
        uint32_t length = kw_get32(m->bytes + at);
        m->field[m->count++] = (struct kw_field){ m->bytes + at + 4, length };
        at += 4 + length;
    }
    return true;
}

/* Field i as text (copied, NUL-terminated, into out); false if it is not
 * there, too long or holds a NUL. */
static inline bool kw_text(const struct kw_message *m, int i, char *out, size_t size)
{
    if (i >= m->count || m->field[i].length >= size ||
        memchr(m->field[i].data, 0, m->field[i].length))
        return false;
    memcpy(out, m->field[i].data, m->field[i].length);
    out[m->field[i].length] = 0;
    return true;
}

/* A connection to uid's keyringd, its user checked: the descriptor, or -1
 * (errno: ECONNREFUSED when none runs, EADDRINUSE when another user holds
 * the name). Close-on-exec: a program it starts does not keep it. */
static inline int kw_connect(unsigned uid)
{
    char name[40];
    snprintf(name, sizeof(name), KW_SOCKET, uid);
    int fd = ritteros_local_connect(name);
    if (fd < 0)
        return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (ritteros_peer_uid(fd) != (int)uid) {
        close(fd);
        errno = EADDRINUSE;
        return -1;
    }
    return fd;
}

/* (70d) A connection to the system keyringd (root's: checked). */
static inline int kw_connect_system(void)
{
    int fd = ritteros_local_connect(KW_SYSTEM_SOCKET);
    if (fd < 0)
        return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (ritteros_peer_uid(fd) != 0) {
        close(fd);
        errno = EADDRINUSE;
        return -1;
    }
    return fd;
}

/* One request and its answer over a connection (m holds the answer). */
static inline bool kw_ask(int fd, uint8_t code, const struct kw_field *fields, int count,
                   struct kw_message *m)
{
    return kw_send(fd, code, fields, count) && kw_receive(fd, m, KW_WAIT_MS);
}

#endif

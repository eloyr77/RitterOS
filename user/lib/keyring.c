/*
 * libkeyring.so.1 (70b, item 48; keyring.h): a program's requests to its
 * user's keyringd over the local socket /run/keyring/UID (keyring_wire.h):
 * one connection per call, keyringd's user checked first. (70d) After
 * keyring_system(), to the system keyring's /run/keyring/system (root's).
 */
#include <stdlib.h>
#include "keyring.h"
#include "keyring_wire.h"

static _Atomic int system_chosen;        /* (70d) keyring_system(): the program's choice */

void keyring_system(void)
{
    system_chosen = 1;
}

static bool name_ok(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n > KEYRING_NAME_MAX)
        return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 0x20 || s[i] == 0x7F)
            return false;
    return true;
}

/* errno for an answer that is not KW_OK. */
static void set_errno(const struct kw_message *m)
{
    char text[16];
    switch (m->code) {
    case KW_NOT_FOUND: errno = ENOENT; break;
    case KW_LOCKED: case KW_WRONG: errno = EACCES; break;
    case KW_REFUSED: errno = EACCES; break;
    case KW_NOT_ALLOWED: errno = EPERM; break;
    case KW_FAILED:
        errno = kw_text(m, 0, text, sizeof(text)) ? atoi(text) : EIO;
        if (errno <= 0)
            errno = EIO;
        break;
    default: errno = EINVAL; break;
    }
}

/* One request: the answer in a new message (the caller frees it), or 0
 * with errno; a message whose answer is not KW_OK is 0 too. */
static struct kw_message *request(uint8_t code, const struct kw_field *f, int count)
{
    struct kw_message *m = malloc(sizeof(*m));
    if (!m)
        return 0;
    int fd = system_chosen ? kw_connect_system() : kw_connect((unsigned)getuid());
    bool asked = fd >= 0 && kw_ask(fd, code, f, count, m);
    int e = errno;
    if (fd >= 0)
        close(fd);
    if (asked && m->code == KW_OK)
        return m;
    if (asked)
        set_errno(m);
    else
        errno = e;
    explicit_bzero(m, sizeof(*m));
    free(m);
    return 0;
}

static void done(struct kw_message *m)
{
    explicit_bzero(m, sizeof(*m));
    free(m);
}

ssize_t keyring_get(const char *service, const char *account, void *secret, size_t size)
{
    if (!name_ok(service) || !name_ok(account)) {
        errno = EINVAL;
        return -1;
    }
    struct kw_field f[2] = { { service, (uint32_t)strlen(service) },
                             { account, (uint32_t)strlen(account) } };
    struct kw_message *m = request(KW_GET, f, 2);
    if (!m)
        return -1;
    if (m->count != 1) {
        done(m);
        errno = EPROTO;
        return -1;
    }
    size_t n = m->field[0].length;
    memcpy(secret, m->field[0].data, n < size ? n : size);
    done(m);
    return (ssize_t)n;
}

int keyring_set(const char *service, const char *account, const void *secret, size_t length)
{
    if (!name_ok(service) || !name_ok(account) || length > KEYRING_SECRET_MAX ||
        (length && !secret)) {
        errno = EINVAL;
        return -1;
    }
    struct kw_field f[3] = { { service, (uint32_t)strlen(service) },
                             { account, (uint32_t)strlen(account) },
                             { secret, (uint32_t)length } };
    struct kw_message *m = request(KW_SET, f, 3);
    if (!m)
        return -1;
    done(m);
    return 0;
}

int keyring_remove(const char *service, const char *account)
{
    if (!name_ok(service) || !name_ok(account)) {
        errno = EINVAL;
        return -1;
    }
    struct kw_field f[2] = { { service, (uint32_t)strlen(service) },
                             { account, (uint32_t)strlen(account) } };
    struct kw_message *m = request(KW_REMOVE, f, 2);
    if (!m)
        return -1;
    done(m);
    return 0;
}

int keyring_list(void (*each)(const char *service, const char *account, void *arg), void *arg)
{
    uint32_t first = 0, total = 0;
    do {                                /* a page at a time, as many as an answer holds */
        uint8_t at[4];
        kw_put32(at, first);
        struct kw_field f = { at, 4 };
        struct kw_message *m = request(KW_LIST, &f, 1);
        if (!m)
            return -1;
        if (m->count < 1 || m->field[0].length != 4 || m->count % 2 != 1) {
            done(m);
            errno = EPROTO;
            return -1;
        }
        total = kw_get32(m->field[0].data);
        char service[KEYRING_NAME_MAX + 1], account[KEYRING_NAME_MAX + 1];
        for (int i = 1; i + 1 < m->count; i += 2) {
            if (!kw_text(m, i, service, sizeof(service)) ||
                !kw_text(m, i + 1, account, sizeof(account))) {
                done(m);
                errno = EPROTO;
                return -1;
            }
            if (each)
                each(service, account, arg);
        }
        bool none = m->count == 1;
        first += (uint32_t)(m->count - 1) / 2;
        done(m);
        if (none)
            break;
    } while (first < total);
    return (int)total;
}

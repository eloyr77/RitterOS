/*
 * The keyring's file (70a, item 48; docs/keyring.md is its format, and
 * scripts/keyring.py reads it on the Mac with code of its own): a user's
 * passwords in ~/.config/keyring.db, encrypted. A random 32-byte data key
 * seals the entries with ChaCha20-Poly1305 (RFC 8439); the data key itself
 * is sealed (wrapped) by a key made from the password with
 * PBKDF2-HMAC-SHA256, so a new password wraps it again and the entries
 * stay as they are. Every save seals them anew under a fresh random nonce;
 * the header is the seals' additional data, so nothing in it can be
 * changed unseen. Shared by the programs that open the file (keyring;
 * keyringd, passwd): static inline functions, as settings_file.h.
 */
#ifndef KEYRING_DB_H
#define KEYRING_DB_H

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "crypto.h"
#include "ritteros.h"

#define KEYRING_MAGIC        "RKEYRING"
#define KEYRING_VERSION      1
#define KEYRING_BY_PASSWORD  1          /* the wrapping key from a password */
#define KEYRING_BY_KEY       2          /* (70d) the wrapping key a file's 32 bytes */
#define KEYRING_ROUNDS       600000     /* PBKDF2's (OWASP's 2023 count for SHA-256) */
#define KEYRING_HEADER       116        /* the body's ciphertext starts here */
#define KEYRING_TAG          16
#define KEYRING_NAME_MAX     255        /* a service's or an account's bytes */
#define KEYRING_SECRET_MAX   4096
#define KEYRING_ENTRIES_MAX  10000
#define KEYRING_FILE_MAX     (16u << 20)
#define KEYRING_PROGRAMS_MAX 64         /* (70c) programs allowed one entry */

/* An entry's fields: a tag byte, a 32-bit length, the bytes. */
enum {
    KEYRING_FIELD_SERVICE = 1,
    KEYRING_FIELD_ACCOUNT = 2,
    KEYRING_FIELD_SECRET  = 3,
    KEYRING_FIELD_CHANGED = 4,          /* 8 bytes: nanoseconds since 1970 */
    KEYRING_FIELD_PROGRAM = 5,          /* (70c) a program allowed it, its path; repeated */
};

struct keyring_entry {
    char *service, *account;            /* text without control characters */
    uint8_t *secret;
    size_t secret_length;
    uint64_t changed;
    char **programs;                    /* (70c) who may use it (none: every program) */
    int program_count;
    uint8_t *other;                     /* fields of a later version, kept as read */
    size_t other_length;
};

struct keyring {
    uint8_t header[KEYRING_HEADER];
    uint8_t data_key[32];
    int count;
    struct keyring_entry *entry;
};

static inline uint32_t kr_get32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void kr_put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* ChaCha20-Poly1305 (RFC 8439 section 2.8): the one-time key from block
 * 0, the data from block 1 on, the tag over aad, padding, the ciphertext,
 * padding and both lengths. */
static inline bool kr_tag(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_n,
                   const uint8_t *c, size_t n, uint8_t tag[16])
{
    uint8_t block[16] = { 0 }, otk[32] = { 0 };
    memcpy(block + 4, nonce, 12);
    chacha20(key, block, otk, otk, 32);
    size_t size = (aad_n + 15) / 16 * 16 + (n + 15) / 16 * 16 + 16;
    uint8_t *m = calloc(1, size);
    if (!m)
        return false;
    memcpy(m, aad, aad_n);
    size_t at = (aad_n + 15) / 16 * 16;
    memcpy(m + at, c, n);
    at += (n + 15) / 16 * 16;
    for (int i = 0; i < 8; i++) {
        m[at + i] = (uint8_t)((uint64_t)aad_n >> (8 * i));
        m[at + 8 + i] = (uint8_t)((uint64_t)n >> (8 * i));
    }
    poly1305(otk, m, size, tag);
    explicit_bzero(otk, sizeof(otk));
    free(m);
    return true;
}

static inline bool kr_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_n,
                    const uint8_t *in, uint8_t *out, size_t n, uint8_t tag[16])
{
    uint8_t block[16] = { 1 };
    memcpy(block + 4, nonce, 12);
    chacha20(key, block, in, out, n);
    return kr_tag(key, nonce, aad, aad_n, out, n, tag);
}

/* The tag checked first: nothing is decrypted under a wrong one. */
static inline bool kr_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_n,
                    const uint8_t *in, uint8_t *out, size_t n, const uint8_t tag[16])
{
    uint8_t want[16];
    if (!kr_tag(key, nonce, aad, aad_n, in, n, want) || !crypto_equal(want, tag, 16))
        return false;
    uint8_t block[16] = { 1 };
    memcpy(block + 4, nonce, 12);
    chacha20(key, block, in, out, n);
    return true;
}

/* The header's places (docs/keyring.md). */
#define KR_VERSION_AT   8
#define KR_LOCK_AT      12
#define KR_ROUNDS_AT    16
#define KR_SALT_AT      24
#define KR_WRAP_NONCE   40
#define KR_WRAPPED_AT   52
#define KR_WRAP_TAG     84
#define KR_BODY_NONCE   100
#define KR_BODY_LENGTH  112

/* The wrapping key: made from the password and the header's salt and
 * rounds, or (70d, KEYRING_BY_KEY) the key file's 32 bytes themselves;
 * the data key is sealed under it (the header's first 40 bytes its
 * additional data). */
static inline void kr_wrapping_key(const struct keyring *k, const char *password, uint8_t out[32])
{
    pbkdf2_sha256(password, strlen(password), k->header + KR_SALT_AT, 16,
                  kr_get32(k->header + KR_ROUNDS_AT), out, 32);
}

static inline bool kr_wrap_with(struct keyring *k, const char *password, const uint8_t *key)
{
    uint8_t *h = k->header, kek[32];
    memcpy(h, KEYRING_MAGIC, 8);
    kr_put32(h + KR_VERSION_AT, KEYRING_VERSION);
    kr_put32(h + KR_LOCK_AT, key ? KEYRING_BY_KEY : KEYRING_BY_PASSWORD);
    kr_put32(h + KR_ROUNDS_AT, key ? 0 : KEYRING_ROUNDS);
    kr_put32(h + 20, 0);
    memset(h + KR_SALT_AT, 0, 16);
    if ((!key && ritteros_random(h + KR_SALT_AT, 16) < 0) || ritteros_random(h + KR_WRAP_NONCE, 12) < 0)
        return false;
    if (key)
        memcpy(kek, key, 32);
    else
        kr_wrapping_key(k, password, kek);
    bool ok = kr_seal(kek, h + KR_WRAP_NONCE, h, KR_WRAP_NONCE, k->data_key, h + KR_WRAPPED_AT,
                      32, h + KR_WRAP_TAG);
    explicit_bzero(kek, sizeof(kek));
    return ok;
}

/* An entry's memory, its secret zeroed first. */
static inline void kr_entry_free(struct keyring_entry *e)
{
    if (e->secret)
        explicit_bzero(e->secret, e->secret_length);
    for (int p = 0; p < e->program_count; p++)
        free(e->programs[p]);
    free(e->programs);
    free(e->service), free(e->account), free(e->secret), free(e->other);
}

static inline void keyring_free(struct keyring *k)
{
    for (int i = 0; i < k->count; i++)
        kr_entry_free(&k->entry[i]);
    free(k->entry);
    explicit_bzero(k, sizeof(*k));
}

static inline bool kr_wrap(struct keyring *k, const char *password)
{
    return kr_wrap_with(k, password, 0);
}

/* A new, empty keyring under a password (saved by keyring_save). */
static inline bool keyring_new(struct keyring *k, const char *password)
{
    memset(k, 0, sizeof(*k));
    if (ritteros_random(k->data_key, 32) < 0)
        return false;
    return kr_wrap(k, password);
}

/* (70d) A new, empty keyring under a key file's 32 bytes. */
static inline bool keyring_new_key(struct keyring *k, const uint8_t key[32])
{
    memset(k, 0, sizeof(*k));
    if (ritteros_random(k->data_key, 32) < 0)
        return false;
    return kr_wrap_with(k, 0, key);
}

/* The same data key under another password (a new salt and nonce). */
static inline bool keyring_rewrap(struct keyring *k, const char *password)
{
    return kr_wrap(k, password);
}

/* (70b) Does password open this keyring (its data key unwrapped from
 * the header as it stands)? Takes PBKDF2's time, as opening. */
static inline bool keyring_password_is(const struct keyring *k, const char *password)
{
    uint8_t kek[32], key[32];
    kr_wrapping_key(k, password, kek);
    const uint8_t *h = k->header;
    bool ok = kr_open(kek, h + KR_WRAP_NONCE, h, KR_WRAP_NONCE, h + KR_WRAPPED_AT, key, 32,
                      h + KR_WRAP_TAG) && crypto_equal(key, k->data_key, 32);
    explicit_bzero(kek, sizeof(kek));
    explicit_bzero(key, sizeof(key));
    return ok;
}

/* (70c) May program use entry e: none named (every program), or one of
 * them. */
static inline bool keyring_program_may(const struct keyring_entry *e, const char *program)
{
    if (e->program_count == 0)
        return true;
    for (int p = 0; p < e->program_count; p++)
        if (!strcmp(e->programs[p], program))
            return true;
    return false;
}

/* (70c) program added to e's (once): true, or false (out of memory, too
 * many, a path the format does not take). */
static inline bool keyring_program_add(struct keyring_entry *e, const char *program)
{
    for (int p = 0; p < e->program_count; p++)
        if (!strcmp(e->programs[p], program))
            return true;
    size_t n = strlen(program);
    if (n == 0 || n > KEYRING_NAME_MAX || e->program_count == KEYRING_PROGRAMS_MAX)
        return false;
    char **more = realloc(e->programs, (size_t)(e->program_count + 1) * sizeof(*more));
    char *copy = strdup(program);
    if (!more || !copy) {
        if (more)
            e->programs = more;
        free(copy);
        return false;
    }
    e->programs = more;
    e->programs[e->program_count++] = copy;
    return true;
}

/* A name as the format takes it: 1-255 bytes, no control characters. */
static inline bool keyring_name_ok(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > KEYRING_NAME_MAX)
        return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 0x20 || s[i] == 0x7F)
            return false;
    return true;
}

static inline char *kr_text(const uint8_t *p, uint32_t n)
{
    char *s = malloc(n + 1);
    if (s) {
        memcpy(s, p, n);
        s[n] = 0;
    }
    return s;
}

/* The entries from the decrypted body; false if it is not well formed. */
static inline bool kr_parse(struct keyring *k, const uint8_t *b, size_t n)
{
    if (n < 4)
        return false;
    uint32_t count = kr_get32(b);
    if (count > KEYRING_ENTRIES_MAX)
        return false;
    k->entry = calloc(count ? count : 1, sizeof(*k->entry));
    if (!k->entry)
        return false;
    size_t at = 4;
    for (uint32_t i = 0; i < count; i++) {
        if (n - at < 4)
            return false;
        uint32_t length = kr_get32(b + at);
        at += 4;
        if (length > n - at)
            return false;
        struct keyring_entry *e = &k->entry[k->count++];
        size_t end = at + length;
        while (at < end) {
            if (end - at < 5)
                return false;
            uint8_t tag = b[at];
            uint32_t fn = kr_get32(b + at + 1);
            if (fn > end - at - 5)
                return false;
            const uint8_t *f = b + at + 5;
            if (tag == KEYRING_FIELD_SERVICE && !e->service) {
                if (!(e->service = kr_text(f, fn)))
                    return false;
            } else if (tag == KEYRING_FIELD_ACCOUNT && !e->account) {
                if (!(e->account = kr_text(f, fn)))
                    return false;
            } else if (tag == KEYRING_FIELD_SECRET && !e->secret) {
                if (!(e->secret = malloc(fn ? fn : 1)))
                    return false;
                memcpy(e->secret, f, fn);
                e->secret_length = fn;
            } else if (tag == KEYRING_FIELD_CHANGED && fn == 8) {
                e->changed = kr_get32(f) | (uint64_t)kr_get32(f + 4) << 32;
            } else if (tag == KEYRING_FIELD_PROGRAM && fn && fn <= KEYRING_NAME_MAX &&
                       !memchr(f, 0, fn)) {
                char *program = kr_text(f, fn);
                bool added = program && keyring_program_add(e, program);
                free(program);
                if (!added)
                    return false;
            } else {                    /* a later version's field: kept */
                uint8_t *o = realloc(e->other, e->other_length + 5 + fn);
                if (!o)
                    return false;
                memcpy(o + e->other_length, b + at, 5 + fn);
                e->other = o;
                e->other_length += 5 + fn;
            }
            at += 5 + fn;
        }
        if (!e->service || !e->account || !e->secret || !keyring_name_ok(e->service) ||
            !keyring_name_ok(e->account))
            return false;
    }
    return at == n;
}

/*
 * Opens the file at path with the password: 1 opened, 0 there is none
 * (k empty), -1 not opened (*why says why: a wrong password and a damaged
 * file look the same, as the tag cannot tell them apart).
 */
static inline int kr_load(const char *path, const char *password, const uint8_t *key,
                          struct keyring *k, const char **why)
{
    memset(k, 0, sizeof(*k));
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT)
            return 0;
        *why = strerror(errno);
        return -1;
    }
    struct stat st;
    uint8_t *file = NULL, *body = NULL;
    size_t size = 0, got = 0;
    uint32_t length = 0;
    int r = -1;
    *why = "not a keyring file";
    if (fstat(fd, &st) < 0 || st.st_size < KEYRING_HEADER + KEYRING_TAG ||
        st.st_size > KEYRING_FILE_MAX)
        goto out;
    size = (size_t)st.st_size;
    if (!(file = malloc(size))) {
        *why = "out of memory";
        goto out;
    }
    while (got < size) {
        ssize_t n = read(fd, file + got, size - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    if (got != size || memcmp(file, KEYRING_MAGIC, 8) != 0)
        goto out;
    if (kr_get32(file + KR_VERSION_AT) != KEYRING_VERSION) {
        *why = "a keyring of another version";
        goto out;
    }
    if (kr_get32(file + KR_LOCK_AT) != (key ? KEYRING_BY_KEY : KEYRING_BY_PASSWORD)) {
        *why = "a keyring locked another way";
        goto out;
    }
    length = kr_get32(file + KR_BODY_LENGTH);
    if (length != size - KEYRING_HEADER - KEYRING_TAG || (!key && kr_get32(file + KR_ROUNDS_AT) == 0))
        goto out;
    memcpy(k->header, file, KEYRING_HEADER);
    uint8_t kek[32];
    if (key)
        memcpy(kek, key, 32);
    else
        kr_wrapping_key(k, password, kek);
    bool unwrapped = kr_open(kek, file + KR_WRAP_NONCE, file, KR_WRAP_NONCE, file + KR_WRAPPED_AT,
                             k->data_key, 32, file + KR_WRAP_TAG);
    explicit_bzero(kek, sizeof(kek));
    if (!unwrapped) {
        *why = "wrong password (or the file is damaged)";
        goto out;
    }
    if (!(body = malloc(length ? length : 1))) {
        *why = "out of memory";
        goto out;
    }
    if (!kr_open(k->data_key, file + KR_BODY_NONCE, file, KEYRING_HEADER, file + KEYRING_HEADER,
                 body, length, file + KEYRING_HEADER + length)) {
        *why = "the file is damaged";
        goto out;
    }
    if (!kr_parse(k, body, length)) {
        *why = "the file is damaged (its entries)";
        goto out;
    }
    r = 1;
out:
    close(fd);
    if (body) {
        explicit_bzero(body, length);
        free(body);
    }
    free(file);
    if (r < 0)
        keyring_free(k);
    return r;
}

static inline int keyring_load(const char *path, const char *password, struct keyring *k, const char **why)
{
    return kr_load(path, password, 0, k, why);
}

/* (70d) The same for a keyring locked by a key file's 32 bytes (a wrong
 * key: "wrong password (or the file is damaged)" too). */
static inline int keyring_load_key(const char *path, const uint8_t key[32], struct keyring *k,
                                   const char **why)
{
    return kr_load(path, 0, key, k, why);
}

static inline void kr_field(uint8_t *b, size_t *at, int tag, const void *data, size_t n)
{
    b[*at] = (uint8_t)tag;
    kr_put32(b + *at + 1, (uint32_t)n);
    memcpy(b + *at + 5, data, n);
    *at += 5 + n;
}

/*
 * Writes the keyring to path: PATH.new (mode 0600) written whole and
 * committed, then renamed over the old one, so a crash leaves one or the
 * other. The entries are sealed under a new random nonce. 0, or -1 with
 * errno.
 */
static inline int keyring_save(const char *path, struct keyring *k)
{
    size_t length = 4;
    for (int i = 0; i < k->count; i++) {
        struct keyring_entry *e = &k->entry[i];
        length += 4 + 5 + strlen(e->service) + 5 + strlen(e->account) + 5 + e->secret_length +
                  5 + 8 + e->other_length;
        for (int p = 0; p < e->program_count; p++)
            length += 5 + strlen(e->programs[p]);
    }
    size_t size = KEYRING_HEADER + length + KEYRING_TAG;
    uint8_t *file = malloc(size), *body = malloc(length);
    if (!file || !body) {
        free(file), free(body);
        errno = ENOMEM;
        return -1;
    }
    size_t at = 4;
    kr_put32(body, (uint32_t)k->count);
    for (int i = 0; i < k->count; i++) {
        struct keyring_entry *e = &k->entry[i];
        size_t start = at;
        at += 4;
        kr_field(body, &at, KEYRING_FIELD_SERVICE, e->service, strlen(e->service));
        kr_field(body, &at, KEYRING_FIELD_ACCOUNT, e->account, strlen(e->account));
        kr_field(body, &at, KEYRING_FIELD_SECRET, e->secret, e->secret_length);
        uint8_t changed[8];
        kr_put32(changed, (uint32_t)e->changed);
        kr_put32(changed + 4, (uint32_t)(e->changed >> 32));
        kr_field(body, &at, KEYRING_FIELD_CHANGED, changed, 8);
        for (int p = 0; p < e->program_count; p++)
            kr_field(body, &at, KEYRING_FIELD_PROGRAM, e->programs[p], strlen(e->programs[p]));
        memcpy(body + at, e->other, e->other_length);
        at += e->other_length;
        kr_put32(body + start, (uint32_t)(at - start - 4));
    }
    int r = -1;
    if (ritteros_random(k->header + KR_BODY_NONCE, 12) < 0)
        goto out;
    kr_put32(k->header + KR_BODY_LENGTH, (uint32_t)length);
    memcpy(file, k->header, KEYRING_HEADER);
    if (!kr_seal(k->data_key, k->header + KR_BODY_NONCE, k->header, KEYRING_HEADER, body,
                 file + KEYRING_HEADER, length, file + KEYRING_HEADER + length)) {
        errno = ENOMEM;
        goto out;
    }
    char temp[512];
    if (snprintf(temp, sizeof(temp), "%s.new", path) >= (int)sizeof(temp)) {
        errno = ENAMETOOLONG;
        goto out;
    }
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        goto out;
    size_t done = 0;
    int failed = 0;
    while (done < size) {
        ssize_t n = write(fd, file + done, size - done);
        if (n <= 0) {
            failed = n < 0 ? errno : EIO;
            break;
        }
        done += (size_t)n;
    }
    if (!failed && fsync(fd) < 0)
        failed = errno;
    close(fd);
    if (!failed && rename(temp, path) < 0)
        failed = errno;
    if (failed) {
        unlink(temp);
        errno = failed;
        goto out;
    }
    r = 0;
out:
    explicit_bzero(body, length);
    free(body);
    free(file);
    return r;
}

/* The entry of (service, account), or -1. */
static inline int keyring_find(const struct keyring *k, const char *service, const char *account)
{
    for (int i = 0; i < k->count; i++)
        if (!strcmp(k->entry[i].service, service) && !strcmp(k->entry[i].account, account))
            return i;
    return -1;
}

/* Sets (service, account)'s secret, a new entry or the old one's: 1 new,
 * 0 replaced, -1 not (out of memory, too many, a name or secret refused). */
static inline int keyring_put(struct keyring *k, const char *service, const char *account,
                       const void *secret, size_t n)
{
    if (!keyring_name_ok(service) || !keyring_name_ok(account) || n > KEYRING_SECRET_MAX)
        return -1;
    uint8_t *copy = malloc(n ? n : 1);
    if (!copy)
        return -1;
    memcpy(copy, secret, n);
    int i = keyring_find(k, service, account);
    if (i >= 0) {
        struct keyring_entry *e = &k->entry[i];
        explicit_bzero(e->secret, e->secret_length);
        free(e->secret);
        e->secret = copy;
        e->secret_length = n;
        e->changed = ritteros_time();
        return 0;
    }
    if (k->count >= KEYRING_ENTRIES_MAX) {
        free(copy);
        return -1;
    }
    struct keyring_entry *more = realloc(k->entry, (k->count + 1) * sizeof(*more));
    char *s = strdup(service), *a = strdup(account);
    if (!more || !s || !a) {
        if (more)
            k->entry = more;
        free(copy), free(s), free(a);
        return -1;
    }
    k->entry = more;
    struct keyring_entry *e = &k->entry[k->count++];
    memset(e, 0, sizeof(*e));
    e->service = s, e->account = a, e->secret = copy, e->secret_length = n;
    e->changed = ritteros_time();
    return 1;
}

/* Takes entry i out (its secret zeroed). */
static inline void keyring_drop(struct keyring *k, int i)
{
    struct keyring_entry *e = &k->entry[i];
    kr_entry_free(e);
    memmove(e, e + 1, (size_t)(k->count - i - 1) * sizeof(*e));
    k->count--;
}

#endif

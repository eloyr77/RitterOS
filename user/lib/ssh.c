/*
 * SSH's transport (see ssh.h).
 *
 * A connection starts with both sides sending a version line
 * ("SSH-2.0-RitterOS_1.0"), then binary packets: a 32-bit length, a
 * padding length, the message, random padding. The key exchange: each
 * side lists what it can (KEXINIT); the client sends an X25519 public key,
 * the server answers with its own, its host key and a signature of the
 * exchange's hash H (over both versions, both KEXINITs, the host key, both
 * public keys and the shared secret K); both derive the keys from K and H,
 * and NEWKEYS switches them on. The first H is the session's ID.
 *
 * chacha20-poly1305@openssh.com: each direction has a 64-byte key, a main
 * key and a header key; the packet's sequence number is the nonce. The
 * length is encrypted with the header key, the rest with the main key
 * from block 1 on, and Poly1305 (its key: the main key's block 0) tags
 * the encrypted length and rest. The receiver decrypts the length first
 * to know how much to read.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "crypto.h"
#include "ritteros.h"
#include "ssh.h"

#define VERSION "SSH-2.0-RitterOS_1.0"

/* --- building and reading messages ------------------------------------------- */

void ssh_buf_init(struct ssh_buf *b, uint8_t type)
{
    b->length = 0;
    ssh_put_u8(b, type);
}

void ssh_put_u8(struct ssh_buf *b, uint8_t v)
{
    if (b->length < sizeof(b->data))
        b->data[b->length++] = v;
}

void ssh_put_bool(struct ssh_buf *b, bool v)
{
    ssh_put_u8(b, v ? 1 : 0);
}

void ssh_put_u32(struct ssh_buf *b, uint32_t v)
{
    for (int i = 3; i >= 0; i--)
        ssh_put_u8(b, (uint8_t)(v >> (8 * i)));
}

void ssh_put_string(struct ssh_buf *b, const void *data, size_t n)
{
    if (b->length + 4 + n > sizeof(b->data))
        n = 0;
    ssh_put_u32(b, (uint32_t)n);
    memcpy(b->data + b->length, data, n);
    b->length += n;
}

void ssh_put_text(struct ssh_buf *b, const char *s)
{
    ssh_put_string(b, s, strlen(s));
}

/* A big-endian magnitude as mpint: leading zeros dropped, a zero byte in
 * front if the top bit is set (it would mean negative). */
void ssh_put_mpint(struct ssh_buf *b, const uint8_t *v, size_t n)
{
    while (n > 0 && v[0] == 0)
        v++, n--;
    bool pad = n > 0 && (v[0] & 0x80);
    ssh_put_u32(b, (uint32_t)(n + pad));
    if (pad)
        ssh_put_u8(b, 0);
    for (size_t i = 0; i < n; i++)
        ssh_put_u8(b, v[i]);
}

void ssh_reader_init(struct ssh_reader *r, const uint8_t *data, size_t n)
{
    r->data = data, r->length = n, r->at = 0, r->bad = false;
}

uint8_t ssh_get_u8(struct ssh_reader *r)
{
    if (r->bad || r->at >= r->length) {
        r->bad = true;
        return 0;
    }
    return r->data[r->at++];
}

bool ssh_get_bool(struct ssh_reader *r)
{
    return ssh_get_u8(r) != 0;
}

uint32_t ssh_get_u32(struct ssh_reader *r)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v = v << 8 | ssh_get_u8(r);
    return v;
}

const uint8_t *ssh_get_string(struct ssh_reader *r, size_t *n)
{
    uint32_t length = ssh_get_u32(r);
    if (r->bad || length > r->length - r->at) {
        r->bad = true;
        *n = 0;
        return (const uint8_t *)"";
    }
    const uint8_t *p = r->data + r->at;
    r->at += length;
    *n = length;
    return p;
}

bool ssh_get_text(struct ssh_reader *r, char *out, size_t size)
{
    size_t n;
    const uint8_t *p = ssh_get_string(r, &n);
    if (n >= size) {
        out[0] = 0;
        return false;
    }
    memcpy(out, p, n);
    out[n] = 0;
    return !r->bad;
}

bool ssh_string_is(const uint8_t *s, size_t n, const char *text)
{
    return n == strlen(text) && memcmp(s, text, n) == 0;
}

/* --- base64 and fingerprints ---------------------------------------------------- */

static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64_encode(const uint8_t *in, size_t n, char *out, bool padding)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) |
                     (i + 2 < n ? in[i + 2] : 0);
        out[o++] = ALPHABET[v >> 18 & 63];
        out[o++] = ALPHABET[v >> 12 & 63];
        if (i + 1 < n)
            out[o++] = ALPHABET[v >> 6 & 63];
        else if (padding)
            out[o++] = '=';
        if (i + 2 < n)
            out[o++] = ALPHABET[v & 63];
        else if (padding)
            out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

long base64_decode(const char *in, size_t n, uint8_t *out, size_t size)
{
    uint32_t v = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n && in[i] != '='; i++) {
        const char *c = strchr(ALPHABET, in[i]);
        if (!c || !in[i])
            return -1;
        v = v << 6 | (uint32_t)(c - ALPHABET);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o == size)
                return -1;
            out[o++] = (uint8_t)(v >> bits);
        }
    }
    return (long)o;
}

size_t ssh_key_blob(const uint8_t public_key[32], uint8_t out[51])
{
    static const char type[] = "ssh-ed25519";
    out[0] = out[1] = out[2] = 0, out[3] = 11;
    memcpy(out + 4, type, 11);
    out[15] = out[16] = out[17] = 0, out[18] = 32;
    memcpy(out + 19, public_key, 32);
    return 51;
}

void ssh_fingerprint(const uint8_t public_key[32], char *out, size_t size)
{
    uint8_t blob[51], hash[SHA256_SIZE];
    sha256(blob, ssh_key_blob(public_key, blob), hash);
    char text[64];
    base64_encode(hash, sizeof(hash), text, false);
    snprintf(out, size, "SHA256:%s", text);
}

/* --- the wire ---------------------------------------------------------------------- */

/* The longest KEXINIT taken (OpenSSH's are about 1.5 KB): two of them and
 * the rest of the exchange hash's input stay well inside SSH_PACKET_MAX. */
#define KEXINIT_MAX 16384

static int fail(struct ssh *s, const char *why)
{
    if (!s->error[0])
        snprintf(s->error, sizeof(s->error), "%s", why);
    return -1;
}

static int read_exact(struct ssh *s, uint8_t *out, size_t n)
{
    while (n) {
        if (s->in_at == s->in_length) {
            ssize_t r = read(s->fd, s->in, sizeof(s->in));
            if (r == 0)
                return fail(s, "the connection was closed");
            if (r < 0)
                return fail(s, strerror(errno));
            s->in_at = 0, s->in_length = (size_t)r;
        }
        size_t take = s->in_length - s->in_at < n ? s->in_length - s->in_at : n;
        memcpy(out, s->in + s->in_at, take);
        s->in_at += take, out += take, n -= take;
    }
    return 0;
}

static int write_all(struct ssh *s, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t w = write(s->fd, p, n);
        if (w <= 0)
            return fail(s, w < 0 ? strerror(errno) : "the connection was closed");
        p += w, n -= (size_t)w;
    }
    return 0;
}

static void nonce(uint8_t block[16], uint64_t counter, uint32_t seq)
{
    for (int i = 0; i < 8; i++)
        block[i] = (uint8_t)(counter >> (8 * i));   /* little-endian, as the state's words */
    for (int i = 0; i < 8; i++)
        block[8 + i] = (uint8_t)((uint64_t)seq >> (56 - 8 * i));   /* the sequence number, big-endian */
}

int ssh_send(struct ssh *s, const struct ssh_buf *b)
{
    while (__atomic_exchange_n(&s->send_lock, 1, __ATOMIC_ACQUIRE))
        usleep(200);
    static uint8_t out[SSH_PACKET_MAX + 64];
    size_t n = b->length;
    /* Padding: at least 4 bytes, to a multiple of 8 (with the cipher, the
     * length field is left out of that count: it is encrypted apart). */
    size_t counted = (s->encrypt_out ? 0 : 4) + 1 + n;
    size_t pad = 8 - counted % 8;
    if (pad < 4)
        pad += 8;
    uint32_t length = (uint32_t)(1 + n + pad);
    out[0] = (uint8_t)(length >> 24), out[1] = (uint8_t)(length >> 16);
    out[2] = (uint8_t)(length >> 8), out[3] = (uint8_t)length;
    out[4] = (uint8_t)pad;
    memcpy(out + 5, b->data, n);
    ritteros_random(out + 5 + n, pad);
    size_t total = 4 + length;
    if (s->encrypt_out) {
        uint8_t block[16], poly_key[64] = { 0 };
        nonce(block, 0, s->seq_out);
        chacha20(s->key_out + 32, block, out, out, 4);           /* the length: header key */
        chacha20(s->key_out, block, poly_key, poly_key, 64);    /* block 0: Poly1305's key */
        nonce(block, 1, s->seq_out);
        chacha20(s->key_out, block, out + 4, out + 4, length);
        poly1305(poly_key, out, total, out + total);
        total += 16;
    }
    s->seq_out++;
    int r = write_all(s, out, total);
    __atomic_store_n(&s->send_lock, 0, __ATOMIC_RELEASE);
    return r;
}

/* One packet, whatever its type: 0 (s->packet, s->packet_length) or -1. */
static int receive_packet(struct ssh *s)
{
    uint8_t head[4];
    if (read_exact(s, head, 4) < 0)
        return -1;
    uint8_t block[16], poly_key[64] = { 0 }, encrypted_head[4];
    memcpy(encrypted_head, head, 4);
    if (s->encrypt_in) {
        nonce(block, 0, s->seq_in);
        chacha20(s->key_in + 32, block, head, head, 4);
    }
    uint32_t length = (uint32_t)head[0] << 24 | (uint32_t)head[1] << 16 | (uint32_t)head[2] << 8 |
                      head[3];
    if (length < 8 || length > SSH_PACKET_MAX || (length + (s->encrypt_in ? 0 : 4)) % 8)
        return fail(s, "a packet of a bad length");
    static uint8_t body[SSH_PACKET_MAX + 16];
    if (read_exact(s, body, length + (s->encrypt_in ? 16 : 0)) < 0)
        return -1;
    if (s->encrypt_in) {
        chacha20(s->key_in, block, poly_key, poly_key, 64);
        static uint8_t tagged[SSH_PACKET_MAX + 4];
        memcpy(tagged, encrypted_head, 4);
        memcpy(tagged + 4, body, length);
        uint8_t tag[16];
        poly1305(poly_key, tagged, 4 + length, tag);
        if (!crypto_equal(tag, body + length, 16))
            return fail(s, "a packet was changed on the way (its tag is wrong)");
        nonce(block, 1, s->seq_in);
        chacha20(s->key_in, block, body, body, length);
    }
    s->seq_in++;
    uint8_t pad = body[0];
    if (pad < 4 || (size_t)pad + 1 >= length)
        return fail(s, "a packet with bad padding");
    s->packet_length = length - 1 - pad;
    memcpy(s->packet, body + 1, s->packet_length);
    return 0;
}

int ssh_receive(struct ssh *s)
{
    for (;;) {
        if (receive_packet(s) < 0)
            return -1;
        uint8_t type = s->packet[0];
        if (type == SSH_IGNORE || type == SSH_DEBUG || type == SSH_UNIMPLEMENTED)
            continue;
        if (type == SSH_DISCONNECT) {
            struct ssh_reader r;
            ssh_reader_init(&r, s->packet + 1, s->packet_length - 1);
            ssh_get_u32(&r);
            char why[120];
            ssh_get_text(&r, why, sizeof(why));
            snprintf(s->error, sizeof(s->error), "the other side disconnected: %s", why);
            return -1;
        }
        return type;
    }
}

void ssh_disconnect(struct ssh *s, uint32_t reason, const char *text)
{
    static struct ssh_buf b_buffer;   /* too big for the stack */
    struct ssh_buf *b = &b_buffer;
    ssh_buf_init(b, SSH_DISCONNECT);
    ssh_put_u32(b, reason);
    ssh_put_text(b, text);
    ssh_put_text(b, "");
    ssh_send(s, b);
}

/* --- the key exchange ------------------------------------------------------------- */

static bool listed(const uint8_t *list, size_t n, const char *name)
{
    size_t want = strlen(name);
    for (size_t at = 0; at < n;) {
        size_t end = at;
        while (end < n && list[end] != ',')
            end++;
        if (end - at == want && memcmp(list + at, name, want) == 0)
            return true;
        at = end + 1;
    }
    return false;
}

static int version_exchange(struct ssh *s)
{
    snprintf(s->ours, sizeof(s->ours), "%s", VERSION);
    char line[300];
    snprintf(line, sizeof(line), "%s\r\n", s->ours);
    if (write_all(s, (const uint8_t *)line, strlen(line)) < 0)
        return -1;
    /* Lines before the version are allowed (from a server). */
    for (int lines = 0; lines < 20; lines++) {
        size_t n = 0;
        for (;;) {
            uint8_t c;
            if (read_exact(s, &c, 1) < 0)
                return -1;
            if (c == '\n')
                break;
            if (n < sizeof(line) - 1)
                line[n++] = (char)c;
        }
        if (n && line[n - 1] == '\r')
            n--;
        line[n] = 0;
        if (strncmp(line, "SSH-", 4) == 0) {
            if (strncmp(line, "SSH-2.0-", 8) != 0 && strncmp(line, "SSH-1.99-", 9) != 0)
                return fail(s, "the other side does not speak SSH 2");
            snprintf(s->theirs, sizeof(s->theirs), "%s", line);
            return 0;
        }
    }
    return fail(s, "no SSH version line");
}

static void derive(const struct ssh_buf *k, const uint8_t h[32], const uint8_t session_id[32],
                   char letter, uint8_t out[64])
{
    struct sha256 c;
    sha256_init(&c);
    sha256_update(&c, k->data, k->length);
    sha256_update(&c, h, 32);
    sha256_update(&c, &letter, 1);
    sha256_update(&c, session_id, 32);
    sha256_final(&c, out);
    sha256_init(&c);                                /* 64 bytes: K1, then HASH(K, H, K1) */
    sha256_update(&c, k->data, k->length);
    sha256_update(&c, h, 32);
    sha256_update(&c, out, 32);
    sha256_final(&c, out + 32);
}

/* Receive a message of the type wanted during the key exchange (IGNORE
 * only without strict mode). */
static int expect(struct ssh *s, uint8_t type)
{
    for (;;) {
        if (receive_packet(s) < 0)
            return -1;
        uint8_t got = s->packet[0];
        if (got == type)
            return 0;
        if (!s->strict && (got == SSH_IGNORE || got == SSH_DEBUG))
            continue;
        if (got == SSH_DISCONNECT)
            return fail(s, "the other side disconnected during the key exchange");
        char why[80];
        snprintf(why, sizeof(why), "message %u during the key exchange (wanted %u)", got, type);
        return fail(s, why);
    }
}

int ssh_start(struct ssh *s, int fd, bool server)
{
    s->fd = fd;
    s->server = server;
    if (version_exchange(s) < 0)
        return -1;
    /* KEXINIT, ours and theirs (both are hashed). */
    static struct ssh_buf ours, theirs, h_input, k;
    ssh_buf_init(&ours, SSH_KEXINIT);
    uint8_t cookie[16];
    ritteros_random(cookie, sizeof(cookie));
    for (int i = 0; i < 16; i++)
        ssh_put_u8(&ours, cookie[i]);
    ssh_put_text(&ours, server ? "curve25519-sha256,curve25519-sha256@libssh.org,"
                                 "kex-strict-s-v00@openssh.com"
                               : "curve25519-sha256,curve25519-sha256@libssh.org,"
                                 "kex-strict-c-v00@openssh.com");
    ssh_put_text(&ours, "ssh-ed25519");
    ssh_put_text(&ours, "chacha20-poly1305@openssh.com");
    ssh_put_text(&ours, "chacha20-poly1305@openssh.com");
    ssh_put_text(&ours, "hmac-sha2-256");           /* not used: the cipher tags itself */
    ssh_put_text(&ours, "hmac-sha2-256");
    ssh_put_text(&ours, "none");
    ssh_put_text(&ours, "none");
    ssh_put_text(&ours, "");
    ssh_put_text(&ours, "");
    ssh_put_bool(&ours, false);
    ssh_put_u32(&ours, 0);
    if (ssh_send(s, &ours) < 0 || expect(s, SSH_KEXINIT) < 0)
        return -1;
    /* Its fixed part (type, cookie, ten name lists, a flag, a word) must be
     * there; and it is kept short enough that the exchange hash's input
     * (both KEXINITs and the rest) always fits its buffer (49l: a KEXINIT
     * sized to fill it let the shared secret run past its end). */
    if (s->packet_length < 17 + 10 * 4 + 5)
        return fail(s, "a KEXINIT too short");
    if (s->packet_length > KEXINIT_MAX)
        return fail(s, "a KEXINIT too long");
    theirs.length = s->packet_length;
    memcpy(theirs.data, s->packet, s->packet_length);
    struct ssh_reader r;
    ssh_reader_init(&r, theirs.data + 17, theirs.length - 17);
    size_t n;
    const uint8_t *kex = ssh_get_string(&r, &n);
    size_t kex_n = n;
    const uint8_t *host = ssh_get_string(&r, &n);
    bool host_ok = listed(host, n, "ssh-ed25519");
    const uint8_t *c2s = ssh_get_string(&r, &n);
    bool c2s_ok = listed(c2s, n, "chacha20-poly1305@openssh.com");
    const uint8_t *s2c = ssh_get_string(&r, &n);
    bool s2c_ok = listed(s2c, n, "chacha20-poly1305@openssh.com");
    ssh_get_string(&r, &n), ssh_get_string(&r, &n);          /* MACs */
    const uint8_t *z1 = ssh_get_string(&r, &n);
    bool z_ok = listed(z1, n, "none");
    const uint8_t *z2 = ssh_get_string(&r, &n);
    z_ok = z_ok && listed(z2, n, "none");
    if (r.bad)
        return fail(s, "a broken KEXINIT");
    if (!listed(kex, kex_n, "curve25519-sha256") && !listed(kex, kex_n, "curve25519-sha256@libssh.org"))
        return fail(s, "no common key exchange (we have curve25519-sha256)");
    if (!host_ok)
        return fail(s, "no common host key type (we have ssh-ed25519)");
    if (!c2s_ok || !s2c_ok)
        return fail(s, "no common cipher (we have chacha20-poly1305@openssh.com)");
    if (!z_ok)
        return fail(s, "no common compression (we have none)");
    s->strict = listed(kex, kex_n, server ? "kex-strict-c-v00@openssh.com"
                                          : "kex-strict-s-v00@openssh.com");

    /* X25519: our secret and public key, theirs, the shared secret. */
    uint8_t secret[32], our_public[32], their_public[32], shared[32];
    ritteros_random(secret, sizeof(secret));
    x25519_base(our_public, secret);
    uint8_t host_blob[51], signature[64];
    struct ssh_buf *b = &h_input;                   /* also used for the messages */
    if (server) {
        if (expect(s, SSH_KEX_ECDH_INIT) < 0)
            return -1;
        ssh_reader_init(&r, s->packet + 1, s->packet_length - 1);
        const uint8_t *q = ssh_get_string(&r, &n);
        if (r.bad || n != 32)
            return fail(s, "a client key of the wrong size");
        memcpy(their_public, q, 32);
    } else {
        ssh_buf_init(b, SSH_KEX_ECDH_INIT);
        ssh_put_string(b, our_public, 32);
        if (ssh_send(s, b) < 0 || expect(s, SSH_KEX_ECDH_REPLY) < 0)
            return -1;
        ssh_reader_init(&r, s->packet + 1, s->packet_length - 1);
        const uint8_t *blob = ssh_get_string(&r, &n);
        struct ssh_reader kr;
        ssh_reader_init(&kr, blob, n);
        size_t tn, kn;
        const uint8_t *type = ssh_get_string(&kr, &tn);
        const uint8_t *key = ssh_get_string(&kr, &kn);
        if (kr.bad || !ssh_string_is(type, tn, "ssh-ed25519") || kn != 32)
            return fail(s, "the server's host key is not an Ed25519 key");
        memcpy(s->server_key, key, 32);
        const uint8_t *q = ssh_get_string(&r, &n);
        if (n != 32)
            return fail(s, "a server key of the wrong size");
        memcpy(their_public, q, 32);
        const uint8_t *sig_blob = ssh_get_string(&r, &n);
        struct ssh_reader sr;
        ssh_reader_init(&sr, sig_blob, n);
        type = ssh_get_string(&sr, &tn);
        const uint8_t *sig = ssh_get_string(&sr, &kn);
        if (r.bad || sr.bad || !ssh_string_is(type, tn, "ssh-ed25519") || kn != 64)
            return fail(s, "a broken signature from the server");
        memcpy(signature, sig, 64);
    }
    x25519(shared, secret, their_public);
    uint8_t zero = 0;
    for (int i = 0; i < 32; i++)
        zero |= shared[i];
    if (!zero)
        return fail(s, "a key that gives no shared secret");
    memset(secret, 0, sizeof(secret));

    /* H = SHA-256(V_C, V_S, I_C, I_S, K_S, Q_C, Q_S, K). */
    const uint8_t *host_key = server ? s->host_public : s->server_key;
    ssh_key_blob(host_key, host_blob);
    struct ssh_buf *client_init = server ? &theirs : &ours, *server_init = server ? &ours : &theirs;
    b->length = 0;
    ssh_put_text(b, server ? s->theirs : s->ours);
    ssh_put_text(b, server ? s->ours : s->theirs);
    ssh_put_string(b, client_init->data, client_init->length);
    ssh_put_string(b, server_init->data, server_init->length);
    ssh_put_string(b, host_blob, sizeof(host_blob));
    ssh_put_string(b, server ? their_public : our_public, 32);
    ssh_put_string(b, server ? our_public : their_public, 32);
    k.length = 0;
    ssh_put_mpint(&k, shared, 32);
    memset(shared, 0, sizeof(shared));
    if (b->length + k.length > sizeof(b->data))
        return fail(s, "the exchange hash's input does not fit");
    memcpy(b->data + b->length, k.data, k.length);
    b->length += k.length;
    uint8_t h[32];
    sha256(b->data, b->length, h);
    if (!s->keyed)
        memcpy(s->session_id, h, 32);

    if (server) {
        ed25519_sign(signature, s->host_seed, s->host_public, h, 32);
        ssh_buf_init(b, SSH_KEX_ECDH_REPLY);
        ssh_put_string(b, host_blob, sizeof(host_blob));
        ssh_put_string(b, our_public, 32);
        uint8_t sig_blob[4 + 11 + 4 + 64];
        struct ssh_buf *sb = &theirs;               /* free now */
        sb->length = 0;
        ssh_put_text(sb, "ssh-ed25519");
        ssh_put_string(sb, signature, 64);
        memcpy(sig_blob, sb->data, sizeof(sig_blob));
        ssh_put_string(b, sig_blob, sizeof(sig_blob));
        if (ssh_send(s, b) < 0)
            return -1;
    } else if (!ed25519_verify(signature, s->server_key, h, 32)) {
        return fail(s, "the server's signature is wrong");
    }

    /* NEWKEYS both ways; then the keys: C and D are the two directions'
     * encryption keys (client to server, server to client). */
    ssh_buf_init(b, SSH_NEWKEYS);
    if (ssh_send(s, b) < 0)
        return -1;
    derive(&k, h, s->session_id, server ? 'D' : 'C', s->key_out);
    s->encrypt_out = true;
    if (s->strict)
        s->seq_out = 0;
    if (expect(s, SSH_NEWKEYS) < 0)
        return -1;
    derive(&k, h, s->session_id, server ? 'C' : 'D', s->key_in);
    s->encrypt_in = true;
    if (s->strict)
        s->seq_in = 0;
    memset(k.data, 0, sizeof(k.data));
    s->keyed = true;
    return 0;
}

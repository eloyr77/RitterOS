/*
 * SSH's transport (user/lib/ssh.c, RFC 4253), for sshd and ssh: the
 * version exchange, binary packets, the key exchange curve25519-sha256
 * (RFC 8731) with an ssh-ed25519 host key, and the cipher
 * chacha20-poly1305@openssh.com. Plus the wire format's pieces: numbers
 * big-endian, strings with a 32-bit length in front.
 */
#ifndef RITTEROS_SSH_H
#define RITTEROS_SSH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Message numbers (RFC 4250). */
enum {
    SSH_DISCONNECT = 1, SSH_IGNORE = 2, SSH_UNIMPLEMENTED = 3, SSH_DEBUG = 4,
    SSH_SERVICE_REQUEST = 5, SSH_SERVICE_ACCEPT = 6, SSH_EXT_INFO = 7,
    SSH_KEXINIT = 20, SSH_NEWKEYS = 21, SSH_KEX_ECDH_INIT = 30, SSH_KEX_ECDH_REPLY = 31,
    SSH_USERAUTH_REQUEST = 50, SSH_USERAUTH_FAILURE = 51, SSH_USERAUTH_SUCCESS = 52,
    SSH_USERAUTH_BANNER = 53, SSH_USERAUTH_PK_OK = 60,
    SSH_GLOBAL_REQUEST = 80, SSH_REQUEST_SUCCESS = 81, SSH_REQUEST_FAILURE = 82,
    SSH_CHANNEL_OPEN = 90, SSH_CHANNEL_OPEN_CONFIRMATION = 91, SSH_CHANNEL_OPEN_FAILURE = 92,
    SSH_CHANNEL_WINDOW_ADJUST = 93, SSH_CHANNEL_DATA = 94, SSH_CHANNEL_EXTENDED_DATA = 95,
    SSH_CHANNEL_EOF = 96, SSH_CHANNEL_CLOSE = 97, SSH_CHANNEL_REQUEST = 98,
    SSH_CHANNEL_SUCCESS = 99, SSH_CHANNEL_FAILURE = 100,
};

#define SSH_PACKET_MAX 35000            /* RFC 4253 6.1: at least 35000 */

/* A message being built. */
struct ssh_buf {
    uint8_t data[SSH_PACKET_MAX];
    size_t length;
};
void ssh_buf_init(struct ssh_buf *b, uint8_t type);
void ssh_put_u8(struct ssh_buf *b, uint8_t v);
void ssh_put_bool(struct ssh_buf *b, bool v);
void ssh_put_u32(struct ssh_buf *b, uint32_t v);
void ssh_put_string(struct ssh_buf *b, const void *data, size_t n);
void ssh_put_text(struct ssh_buf *b, const char *s);
void ssh_put_mpint(struct ssh_buf *b, const uint8_t *big_endian, size_t n);

/* A message being read: every get fails (and keeps failing: bad) at the
 * end of the data. */
struct ssh_reader {
    const uint8_t *data;
    size_t length, at;
    bool bad;
};
void ssh_reader_init(struct ssh_reader *r, const uint8_t *data, size_t n);
uint8_t ssh_get_u8(struct ssh_reader *r);
bool ssh_get_bool(struct ssh_reader *r);
uint32_t ssh_get_u32(struct ssh_reader *r);
/* A string: where it starts in the message, its length in *n. */
const uint8_t *ssh_get_string(struct ssh_reader *r, size_t *n);
/* A string as text (NUL-terminated, cut to size); false if too long. */
bool ssh_get_text(struct ssh_reader *r, char *out, size_t size);
/* Is the string equal to text? */
bool ssh_string_is(const uint8_t *s, size_t n, const char *text);

/* One side of a connection. */
struct ssh {
    int fd;                             /* the TCP socket */
    bool server;
    char ours[64], theirs[256];         /* version strings, without CR LF */
    uint8_t session_id[32];
    bool keyed;                         /* the first key exchange is done */
    bool encrypt_out, encrypt_in;
    uint8_t key_out[64], key_in[64];    /* chacha20-poly1305: main key, then header key */
    uint32_t seq_out, seq_in;
    bool strict;                        /* strict key exchange (OpenSSH, the Terrapin fix) */
    uint8_t host_seed[32], host_public[32];   /* the server's host key */
    uint8_t server_key[32];             /* the client: the host key the server showed */
    uint8_t in[16384];                  /* bytes received, not used yet */
    size_t in_at, in_length;
    uint8_t packet[SSH_PACKET_MAX + 64];   /* the last message received */
    size_t packet_length;
    volatile int send_lock;             /* two threads send (sshd: input and output) */
    char error[160];                    /* why it failed */
};

/* Exchange versions, then keys: the server signs with its host key, the
 * client checks the signature (and then compares server_key with what
 * it knows). 0, or -1 with s->error set. */
int ssh_start(struct ssh *s, int fd, bool server);

/* Send one message (encrypted once keys are there). 0 or -1. */
int ssh_send(struct ssh *s, const struct ssh_buf *b);

/* The next message into s->packet (IGNORE and DEBUG are skipped); its
 * type, or -1 (the connection ended or broke: s->error). */
int ssh_receive(struct ssh *s);

/* Send DISCONNECT with a reason (2: protocol error, 11: by application). */
void ssh_disconnect(struct ssh *s, uint32_t reason, const char *text);

/* "SHA256:" and the unpadded base64 of SHA-256 over a key's blob, as
 * OpenSSH shows fingerprints. */
void ssh_fingerprint(const uint8_t public_key[32], char *out, size_t size);
/* The blob of an Ed25519 public key: string "ssh-ed25519", string key. */
size_t ssh_key_blob(const uint8_t public_key[32], uint8_t out[51]);

/* Base64 (RFC 4648): the encoded length, or for decoding the decoded
 * length (-1 if it is not base64). */
size_t base64_encode(const uint8_t *in, size_t n, char *out, bool padding);
long base64_decode(const char *in, size_t n, uint8_t *out, size_t size);

#endif

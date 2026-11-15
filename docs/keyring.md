# The keyring's file

A user's keyring is the file `~/.config/keyring.db` (70a, item 48),
written and read by `keyringd` (70b; `passwd` too, sealing its key again
when keyringd does not run). The code is
`user/bin/keyring_db.h`; `scripts/keyring.py` reads the file on the Mac
from this description alone (`keyring_shell_test` checks that both agree).

## Keys

- A random 32-byte **data key** seals the entries.
- The **wrapping key** is PBKDF2-HMAC-SHA256 of the password (its UTF-8
  bytes as typed) with the header's 16-byte salt and round count (600,000
  when written), 32 bytes long. It seals the data key.
- (70d) The system keyring, `/system/config/keyring.db` (`keyringd -s`),
  has no password: its wrapping key is the 32 random bytes of
  `/system/config/keyring.key` (root's, mode 0600), its header's lock 2,
  rounds 0 and the salt zeros. Whoever can read the key file can open it:
  it keeps root's programs' secrets from anyone who copies the database
  alone (the whole disk tells it until the system disk is encrypted).
- Both seals are ChaCha20-Poly1305 as RFC 8439 section 2.8 (a 12-byte
  nonce, a 16-byte tag over the additional data and the ciphertext).

A new password seals the same data key again under a new salt and nonce;
the entries are not touched. Every save seals the entries under a new
random nonce.

## Layout

Numbers are little-endian.

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `RKEYRING` |
| 8 | 4 | version: 1 |
| 12 | 4 | how it is locked: 1 = a password, 2 = a key file (70d) |
| 16 | 4 | PBKDF2 rounds (0 with a key file) |
| 20 | 4 | 0 |
| 24 | 16 | salt (zeros with a key file) |
| 40 | 12 | the data key's nonce |
| 52 | 32 | the data key, sealed |
| 84 | 16 | its tag; additional data: bytes 0-39 |
| 100 | 12 | the entries' nonce |
| 112 | 4 | N: the entries' length |
| 116 | N | the entries, sealed |
| 116 + N | 16 | their tag; additional data: bytes 0-115 |

The file is exactly 132 + N bytes. A wrong password fails the data key's
tag; a changed header or body fails a tag too, so the two look the same.

## The entries

Decrypted, the entries are a 32-bit count, then per entry its 32-bit
length and its fields, each a tag byte, a 32-bit length and the bytes:

| Tag | Field |
|---|---|
| 1 | service: 1-255 bytes, no control characters |
| 2 | account: the same |
| 3 | secret: up to 4,096 bytes, any |
| 4 | when it changed: 8 bytes, nanoseconds since 1970 (UTC) |
| 5 | (70c) a program allowed it: its full path, 1-255 bytes; repeated, at most 64 |

1, 2 and 3 are in every entry, once each. The first program (5) is the
one that stored the entry, the others the ones the user allowed; an entry
without one (stored before 70c) is every program's. A field with another tag (a
later version's) is kept as it is when the file is saved again. No two
entries have the same service and account. Nothing follows the last
entry.

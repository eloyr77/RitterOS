#!/usr/bin/env python3
"""The keyring's file read on the Mac (70a, item 48), by docs/keyring.md
alone: code of its own (ChaCha20-Poly1305 from RFC 8439, checked against
the RFC's AEAD example at every start; PBKDF2 from Python's hashlib), so a
file RitterOS wrote and this reads proves the format as written down.

    keyring.py dump FILE [KEYFILE]
                              the password on standard input (one line), or
                              (70d) a system keyring's key file;
                              prints the header, then each entry as
                              SERVICE<tab>ACCOUNT<tab>SECRET, then (70c) a
                              tab and the programs allowed it, commas between
"""
import hashlib
import hmac
import struct
import sys

MAGIC = b"RKEYRING"
HEADER = 116


def rotl(x, n):
    return ((x << n) | (x >> (32 - n))) & 0xFFFFFFFF


def quarter(s, a, b, c, d):
    s[a] = (s[a] + s[b]) & 0xFFFFFFFF; s[d] = rotl(s[d] ^ s[a], 16)
    s[c] = (s[c] + s[d]) & 0xFFFFFFFF; s[b] = rotl(s[b] ^ s[c], 12)
    s[a] = (s[a] + s[b]) & 0xFFFFFFFF; s[d] = rotl(s[d] ^ s[a], 8)
    s[c] = (s[c] + s[d]) & 0xFFFFFFFF; s[b] = rotl(s[b] ^ s[c], 7)


def chacha_block(key, counter, nonce):
    state = [0x61707865, 0x3320646E, 0x79622D32, 0x6B206574]
    state += list(struct.unpack("<8I", key)) + [counter] + list(struct.unpack("<3I", nonce))
    w = state[:]
    for _ in range(10):
        quarter(w, 0, 4, 8, 12); quarter(w, 1, 5, 9, 13)
        quarter(w, 2, 6, 10, 14); quarter(w, 3, 7, 11, 15)
        quarter(w, 0, 5, 10, 15); quarter(w, 1, 6, 11, 12)
        quarter(w, 2, 7, 8, 13); quarter(w, 3, 4, 9, 14)
    return struct.pack("<16I", *[(w[i] + state[i]) & 0xFFFFFFFF for i in range(16)])


def chacha_xor(key, counter, nonce, data):
    out = bytearray()
    for i in range(0, len(data), 64):
        stream = chacha_block(key, counter + i // 64, nonce)
        out += bytes(a ^ b for a, b in zip(data[i:i + 64], stream))
    return bytes(out)


def poly1305(key, msg):
    r = int.from_bytes(key[:16], "little") & 0x0FFFFFFC0FFFFFFC0FFFFFFC0FFFFFFF
    s = int.from_bytes(key[16:], "little")
    p = (1 << 130) - 5
    acc = 0
    for i in range(0, len(msg), 16):
        block = msg[i:i + 16] + b"\x01"
        acc = (acc + int.from_bytes(block, "little")) * r % p
    return ((acc + s) & ((1 << 128) - 1)).to_bytes(16, "little")


def pad16(b):
    return b"\0" * (-len(b) % 16)


def aead_open(key, nonce, aad, ciphertext, tag):
    otk = chacha_block(key, 0, nonce)[:32]
    mac = aad + pad16(aad) + ciphertext + pad16(ciphertext) + \
        struct.pack("<QQ", len(aad), len(ciphertext))
    if not hmac.compare_digest(poly1305(otk, mac), tag):
        return None
    return chacha_xor(key, 1, nonce, ciphertext)


def self_check():
    """RFC 8439 section 2.8.2's example."""
    key = bytes(range(0x80, 0xA0))
    nonce = bytes.fromhex("070000004041424344454647")
    aad = bytes.fromhex("50515253c0c1c2c3c4c5c6c7")
    plain = (b"Ladies and Gentlemen of the class of '99: If I could offer you only one "
             b"tip for the future, sunscreen would be it.")
    cipher = chacha_xor(key, 1, nonce, plain)
    tag = bytes.fromhex("1ae10b594f09e26a7e902ecbd0600691")
    assert cipher[:16] == bytes.fromhex("d31a8d34648e60db7b86afbc53ef7ec2"), "chacha20"
    assert aead_open(key, nonce, aad, cipher, tag) == plain, "poly1305"


def dump(path, password, key=None):
    data = open(path, "rb").read()
    if len(data) < HEADER + 16 or data[:8] != MAGIC:
        sys.exit("keyring.py: not a keyring file")
    version, lock, rounds, zero = struct.unpack_from("<4I", data, 8)
    salt = data[24:40]
    length = struct.unpack_from("<I", data, 112)[0]
    print(f"version {version} lock {lock} rounds {rounds} zero {zero} body {length}")
    if version != 1 or lock != (2 if key else 1) or zero != 0 or length != len(data) - HEADER - 16:
        sys.exit("keyring.py: a header this reader does not know")
    if key:
        if len(key) != 32 or rounds != 0:
            sys.exit("keyring.py: a key file is 32 bytes; a key's keyring has no rounds")
        kek = key
    else:
        kek = hashlib.pbkdf2_hmac("sha256", password, salt, rounds, 32)
    data_key = aead_open(kek, data[40:52], data[:40], data[52:84], data[84:100])
    if data_key is None:
        sys.exit("keyring.py: wrong password (the data key's tag)")
    body = aead_open(data_key, data[100:112], data[:HEADER], data[HEADER:HEADER + length],
                     data[HEADER + length:])
    if body is None:
        sys.exit("keyring.py: the entries' tag is wrong")
    count = struct.unpack_from("<I", body, 0)[0]
    at = 4
    for _ in range(count):
        n = struct.unpack_from("<I", body, at)[0]
        at += 4
        end = at + n
        fields, programs = {}, []
        while at < end:
            tag, fn = body[at], struct.unpack_from("<I", body, at + 1)[0]
            if tag == 5:
                programs.append(body[at + 5:at + 5 + fn].decode())
            else:
                fields[tag] = body[at + 5:at + 5 + fn]
            at += 5 + fn
        if at != end or 1 not in fields or 2 not in fields or 3 not in fields or \
                len(fields.get(4, b"")) != 8:
            sys.exit("keyring.py: an entry without its fields")
        line = "\t".join(fields[t].decode("utf-8", "backslashreplace") for t in (1, 2, 3))
        print(line + ("\t" + ",".join(programs) if programs else ""))
    if at != len(body):
        sys.exit("keyring.py: bytes after the last entry")


def main():
    self_check()
    if len(sys.argv) not in (3, 4) or sys.argv[1] != "dump":
        sys.exit(__doc__)
    if len(sys.argv) == 4:
        dump(sys.argv[2], None, open(sys.argv[3], "rb").read())
    else:
        dump(sys.argv[2], sys.stdin.buffer.readline().rstrip(b"\r\n"))


if __name__ == "__main__":
    main()

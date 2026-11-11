#!/usr/bin/env python3
"""Reference cryptography for the tests, in plain Python with big integers,
written from the RFCs' definitions (slow, simple, not constant time):
ChaCha20 and Poly1305 (RFC 8439), X25519 (RFC 7748), Ed25519 (RFC 8032).
SHA-512 is hashlib's.

Usage:
  crypto_ref.py vectors        check the RFCs' test vectors (prints ok/FAILED)
  crypto_ref.py expected       what user/test/sshcrypto prints (the test
                               compares the two)
  crypto_ref.py derived        only its lines for derived inputs
"""
import hashlib
import struct
import sys

P = 2**255 - 19
L = 2**252 + 27742317777372353535851937790883648493


# --- ChaCha20 (RFC 8439 2.3; the counter may be 64 bits, as OpenSSH's) -------------

def _rotl(x, n):
    return ((x << n) | (x >> (32 - n))) & 0xFFFFFFFF


def chacha20_block(key, block16):
    """block16: words 12-15 (counter and nonce, 16 bytes little-endian)."""
    s = [0x61707865, 0x3320646e, 0x79622d32, 0x6b206574]
    s += list(struct.unpack("<8I", key)) + list(struct.unpack("<4I", block16))
    x = s[:]

    def qr(a, b, c, d):
        x[a] = (x[a] + x[b]) & 0xFFFFFFFF; x[d] = _rotl(x[d] ^ x[a], 16)
        x[c] = (x[c] + x[d]) & 0xFFFFFFFF; x[b] = _rotl(x[b] ^ x[c], 12)
        x[a] = (x[a] + x[b]) & 0xFFFFFFFF; x[d] = _rotl(x[d] ^ x[a], 8)
        x[c] = (x[c] + x[d]) & 0xFFFFFFFF; x[b] = _rotl(x[b] ^ x[c], 7)
    for _ in range(10):
        qr(0, 4, 8, 12); qr(1, 5, 9, 13); qr(2, 6, 10, 14); qr(3, 7, 11, 15)
        qr(0, 5, 10, 15); qr(1, 6, 11, 12); qr(2, 7, 8, 13); qr(3, 4, 9, 14)
    return struct.pack("<16I", *[(x[i] + s[i]) & 0xFFFFFFFF for i in range(16)])


def chacha20(key, block16, data):
    out = bytearray()
    counter = struct.unpack("<Q", block16[:8])[0]
    for i in range(0, len(data), 64):
        ks = chacha20_block(key, struct.pack("<Q", counter) + block16[8:])
        out += bytes(a ^ b for a, b in zip(data[i:i + 64], ks))
        counter = (counter + 1) & 0xFFFFFFFFFFFFFFFF
    return bytes(out)


# --- Poly1305 (RFC 8439 2.5) ------------------------------------------------------

def poly1305(key, msg):
    r = int.from_bytes(key[:16], "little") & 0x0ffffffc0ffffffc0ffffffc0fffffff
    s = int.from_bytes(key[16:], "little")
    acc = 0
    for i in range(0, len(msg), 16):
        n = int.from_bytes(msg[i:i + 16] + b"\x01", "little")
        acc = (acc + n) * r % (2**130 - 5)
    return ((acc + s) % 2**128).to_bytes(16, "little")


# --- X25519 (RFC 7748 5) ------------------------------------------------------------

def x25519(k, u):
    k = bytearray(k)
    k[0] &= 248; k[31] &= 127; k[31] |= 64
    k = int.from_bytes(k, "little")
    u = int.from_bytes(u, "little") & (2**255 - 1)
    x1, x2, z2, x3, z3, swap = u, 1, 0, u, 1, 0
    for t in range(254, -1, -1):
        kt = (k >> t) & 1
        swap ^= kt
        if swap:
            x2, x3, z2, z3 = x3, x2, z3, z2
        swap = kt
        a = x2 + z2; aa = a * a; b = x2 - z2; bb = b * b; e = aa - bb
        c = x3 + z3; d = x3 - z3; da = d * a; cb = c * b
        x3 = (da + cb) ** 2 % P
        z3 = x1 * (da - cb) ** 2 % P
        x2 = aa * bb % P
        z2 = e * (aa + 121665 * e) % P
    if swap:
        x2, z2 = x3, z3
    return (x2 * pow(z2, P - 2, P) % P).to_bytes(32, "little")


# --- Ed25519 (RFC 8032 5.1) ---------------------------------------------------------

D = -121665 * pow(121666, P - 2, P) % P
SQRT_M1 = pow(2, (P - 1) // 4, P)


def _add(p, q):
    x1, y1, z1, t1 = p
    x2, y2, z2, t2 = q
    a = (y1 - x1) * (y2 - x2) % P
    b = (y1 + x1) * (y2 + x2) % P
    c = 2 * t1 * t2 * D % P
    d = 2 * z1 * z2 % P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % P, g * h % P, f * g % P, e * h % P)


def _mul(s, p):
    q = (0, 1, 1, 0)
    while s:
        if s & 1:
            q = _add(q, p)
        p = _add(p, p)
        s >>= 1
    return q


def _recover_x(y, sign):
    if y >= P:
        return None
    x2 = (y * y - 1) * pow(D * y * y + 1, P - 2, P)
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P:
        x = x * SQRT_M1 % P
    if (x * x - x2) % P:
        return None
    if (x & 1) != sign:
        x = P - x
    return x


_BY = 4 * pow(5, P - 2, P) % P
_BX = _recover_x(_BY, 0)
B = (_BX, _BY, 1, _BX * _BY % P)


def _encode(p):
    zi = pow(p[2], P - 2, P)
    x, y = p[0] * zi % P, p[1] * zi % P
    return (y | (x & 1) << 255).to_bytes(32, "little")


def _decode(s):
    y = int.from_bytes(s, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    return None if x is None else (x, y, 1, x * y % P)


def _h(m):
    return int.from_bytes(hashlib.sha512(m).digest(), "little")


def ed25519_public(seed):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return _encode(_mul(a, B))


def ed25519_sign(seed, msg):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    A = _encode(_mul(a, B))
    r = _h(h[32:] + msg) % L
    R = _encode(_mul(r, B))
    k = _h(R + A + msg) % L
    return R + ((r + k * a) % L).to_bytes(32, "little")


def ed25519_verify(public, msg, sig):
    A, R = _decode(public), _decode(sig[:32])
    s = int.from_bytes(sig[32:], "little")
    if A is None or R is None or s >= L:
        return False
    k = _h(sig[:32] + public + msg) % L
    return _encode(_mul(s, B)) == _encode(_add(R, _mul(k, A)))


# --- the checks --------------------------------------------------------------------

def x(h):
    return bytes.fromhex(h.replace(":", "").replace(" ", ""))


def vectors():
    ok = True

    def check(name, got, want):
        nonlocal ok
        good = got == want
        ok &= good
        print(f"{name}: {'ok' if good else 'FAILED: ' + got.hex()}")

    key = bytes(range(32))
    check("chacha20 block (RFC 8439 2.3.2)",
          chacha20_block(key, struct.pack("<I", 1) + x("000000090000004a00000000"))[:16],
          x("10f1e7e4d13b5915500fdd1fa32071c4"))
    sun = (b"Ladies and Gentlemen of the class of '99: If I could offer you only one tip "
           b"for the future, sunscreen would be it.")
    check("chacha20 encryption (RFC 8439 2.4.2), first 16 bytes",
          chacha20(key, struct.pack("<I", 1) + x("000000000000004a00000000"), sun)[:16],
          x("6e2e359a2568f98041ba0728dd0d6981"))
    check("poly1305 (RFC 8439 2.5.2)",
          poly1305(x("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b"),
                   b"Cryptographic Forum Research Group"),
          x("a8061dc1305136c6c22b8baf0c0127a9"))
    check("x25519 (RFC 7748 5.2, 1)",
          x25519(x("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
                 x("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c")),
          x("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"))
    k = u = (9).to_bytes(32, "little")
    k, u = x25519(k, u), k
    check("x25519 (RFC 7748 5.2, 1 iteration)", k,
          x("422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079"))
    alice = x("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
    bob = x("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb")
    nine = (9).to_bytes(32, "little")
    check("x25519 Alice's public (RFC 7748 6.1)", x25519(alice, nine),
          x("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"))
    check("x25519 Bob's public (RFC 7748 6.1)", x25519(bob, nine),
          x("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"))
    check("x25519 shared (RFC 7748 6.1)", x25519(alice, x25519(bob, nine)),
          x("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"))
    for name, seed, pub, msg, sig in ED25519:
        check(f"ed25519 public ({name})", ed25519_public(x(seed)), x(pub))
        check(f"ed25519 signature ({name})", ed25519_sign(x(seed), x(msg)), x(sig))
        check(f"ed25519 verify ({name})", bytes([ed25519_verify(x(pub), x(msg), x(sig))]), b"\1")
    return ok


ED25519 = [
    ("RFC 8032 7.1 TEST 1",
     "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"),
    ("RFC 8032 7.1 TEST 2",
     "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
     "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"),
    ("RFC 8032 7.1 TEST 3",
     "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
     "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"),
]


def derived():
    """Inputs both sides derive the same way: SHA-512 of a label."""
    for i in range(4):
        seed = hashlib.sha512(b"seed %d" % i).digest()
        k, u = seed[:32], seed[32:]
        msg = hashlib.sha512(b"message %d" % i).digest()[: 17 * i]
        print(f"x25519 {i}: {x25519(k, u).hex()}")
        sig = ed25519_sign(k, msg)
        print(f"ed25519 {i}: {ed25519_public(k).hex()} {sig.hex()}")
        print(f"poly1305 {i}: {poly1305(seed[:32], msg * 5).hex()}")
        print(f"chacha20 {i}: {chacha20(k, u[:16], msg * 9).hex()}")
        print(f"sha512 {i}: {hashlib.sha512(msg * 11).hexdigest()}")


def expected():
    """What user/test/sshcrypto prints (but its timings)."""
    key = bytes(range(32))
    print("sha512(''): " + hashlib.sha512(b"").hexdigest())
    print("sha512('abc'): " + hashlib.sha512(b"abc").hexdigest())
    print("sha512(a million 'a'): " + hashlib.sha512(b"a" * 1000000).hexdigest())
    print("chacha20 RFC 8439 2.3.2: " +
          chacha20(key, struct.pack("<I", 1) + x("000000090000004a00000000"), bytes(64)).hex())
    sun = (b"Ladies and Gentlemen of the class of '99: If I could offer you only one tip "
           b"for the future, sunscreen would be it.")
    print("chacha20 RFC 8439 2.4.2: " +
          chacha20(key, struct.pack("<I", 1) + x("000000000000004a00000000"), sun).hex())
    print("chacha20 64-bit counter: " +
          chacha20(key, struct.pack("<Q", 0xFFFFFFFF) + bytes(8), bytes(128)).hex())
    print("poly1305 RFC 8439 2.5.2: " +
          poly1305(x("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b"),
                   b"Cryptographic Forum Research Group").hex())
    print("x25519 RFC 7748 5.2: " +
          x25519(x("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
                 x("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c")).hex())
    nine = (9).to_bytes(32, "little")
    print("x25519 RFC 7748 5.2, 1 iteration: " + x25519(nine, nine).hex())
    alice = x("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
    bob = x("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb")
    print("x25519 RFC 7748 6.1 Alice: " + x25519(alice, nine).hex())
    print("x25519 RFC 7748 6.1 Bob: " + x25519(bob, nine).hex())
    print("x25519 RFC 7748 6.1 shared (Alice): " + x25519(alice, x25519(bob, nine)).hex())
    print("x25519 RFC 7748 6.1 shared (Bob): " + x25519(bob, x25519(alice, nine)).hex())
    for n, (name, seed, pub, msg, sig) in enumerate(ED25519, 1):
        print(f"ed25519 TEST {n} public: " + ed25519_public(x(seed)).hex())
        print(f"ed25519 TEST {n} signature: " + ed25519_sign(x(seed), x(msg)).hex())
        print(f"ed25519 TEST {n} verify: yes")
    for what in ("a changed signature", "a changed message", "S + L", "a key not on the curve"):
        print(f"ed25519 {what}: rejected")
    derived()


if __name__ == "__main__":
    if sys.argv[1:] == ["vectors"]:
        sys.exit(0 if vectors() else 1)
    elif sys.argv[1:] == ["derived"]:
        derived()
    elif sys.argv[1:] == ["expected"]:
        expected()
    else:
        sys.exit(__doc__)

"""
The cryptography the test access point needs (step 24e), pure Python so the
tests pull in no host crypto library: AES-128 (encryption only -- CCMP,
AES-CMAC, the 4-way handshake's MIC and key wrap, and the station's SAE all
need only the forward cipher on the AP side), and on it CCMP, CMAC, RFC 3394
key wrap, the 802.11 PRF, and SAE for group 19 (P-256). This is a reference
for the tests, not production code.
"""
import hashlib
import hmac
import struct

# --- AES-128 (forward only) ------------------------------------------------

_SBOX = []


def _init_sbox():
    p = q = 1
    sbox = [0] * 256
    while True:
        p = p ^ ((p << 1) & 0xff) ^ (0x1b if p & 0x80 else 0)
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xff
        if q & 0x80:
            q ^= 0x09
        xformed = q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ \
            ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4))
        sbox[p] = (xformed ^ 0x63) & 0xff
        if p == 1:
            break
    sbox[0] = 0x63
    return sbox


_SBOX = _init_sbox()


def _xtime(a):
    return ((a << 1) ^ 0x1b) & 0xff if a & 0x80 else (a << 1)


def _key_schedule(key):
    rcon = 1
    w = [list(key[4 * i:4 * i + 4]) for i in range(4)]
    for i in range(4, 44):
        t = list(w[i - 1])
        if i % 4 == 0:
            t = t[1:] + t[:1]
            t = [_SBOX[b] for b in t]
            t[0] ^= rcon
            rcon = _xtime(rcon)
        w.append([w[i - 4][j] ^ t[j] for j in range(4)])
    return [sum((w[4 * r + c][row] for c in range(4) for row in ()), []) if False else
            bytes(w[4 * r + c][row] for c in range(4) for row in range(4)) for r in range(11)]


def aes_encrypt(key, block):
    rk = _key_schedule(key)
    s = bytes(a ^ b for a, b in zip(block, rk[0]))
    for rnd in range(1, 11):
        t = [_SBOX[b] for b in s]
        # ShiftRows (column-major state: byte i = row i%4, col i//4)
        t = [t[(i + 4 * (i % 4)) % 16] for i in range(16)]
        if rnd != 10:
            out = [0] * 16
            for c in range(4):
                col = t[4 * c:4 * c + 4]
                for r in range(4):
                    out[4 * c + r] = (_xtime(col[r]) ^ _xtime(col[(r + 1) % 4]) ^
                                      col[(r + 1) % 4] ^ col[(r + 2) % 4] ^ col[(r + 3) % 4]) & 0xff
            t = out
        s = bytes(a ^ b for a, b in zip(t, rk[rnd]))
    return s


# --- AES-CMAC (RFC 4493) ---------------------------------------------------

def _dbl(b):
    out = bytearray(16)
    carry = b[0] >> 7
    for i in range(16):
        out[i] = ((b[i] << 1) | (b[i + 1] >> 7 if i < 15 else 0)) & 0xff
    if carry:
        out[15] ^= 0x87
    return bytes(out)


def aes_cmac(key, msg):
    k1 = _dbl(aes_encrypt(key, bytes(16)))
    k2 = _dbl(k1)
    n = max(1, (len(msg) + 15) // 16)
    last = msg[(n - 1) * 16:]
    if len(last) == 16:
        last = bytes(a ^ b for a, b in zip(last, k1))
    else:
        last = last + b"\x80" + bytes(15 - len(last))
        last = bytes(a ^ b for a, b in zip(last, k2))
    x = bytes(16)
    for i in range(n - 1):
        x = aes_encrypt(key, bytes(a ^ b for a, b in zip(x, msg[i * 16:i * 16 + 16])))
    return aes_encrypt(key, bytes(a ^ b for a, b in zip(x, last)))


# --- AES key wrap (RFC 3394) -----------------------------------------------

def aes_key_wrap(kek, data):
    n = len(data) // 8
    a = bytearray(b"\xa6" * 8)
    r = [data[i * 8:i * 8 + 8] for i in range(n)]
    for j in range(6):
        for i in range(n):
            b = aes_encrypt(kek, bytes(a) + r[i])
            t = n * j + i + 1
            a = bytearray(b[:8])
            for k in range(8):
                a[k] ^= (t >> (56 - 8 * k)) & 0xff
            r[i] = b[8:]
    return bytes(a) + b"".join(r)


# --- CCMP (CTR + CBC-MAC) --------------------------------------------------

def _ctr(key, nonce, data):
    out = bytearray()
    counter = 1
    for i in range(0, len(data), 16):
        block = bytes([1]) + nonce + struct.pack(">H", counter)
        stream = aes_encrypt(key, block)
        chunk = data[i:i + 16]
        out += bytes(a ^ b for a, b in zip(chunk, stream))
        counter += 1
    return bytes(out)


def _cbc_mac(key, nonce, aad, text, tag_len):
    flags = (0x40 if aad else 0) | (((tag_len - 2) // 2) << 3) | 1
    x = aes_encrypt(key, bytes([flags]) + nonce + struct.pack(">H", len(text)))
    if aad:
        block = struct.pack(">H", len(aad)) + aad
        block += bytes((-len(block)) % 16)
        for i in range(0, len(block), 16):
            x = aes_encrypt(key, bytes(a ^ b for a, b in zip(x, block[i:i + 16])))
    for i in range(0, len(text), 16):
        chunk = text[i:i + 16]
        chunk += bytes(16 - len(chunk))
        x = aes_encrypt(key, bytes(a ^ b for a, b in zip(x, chunk)))
    s0 = aes_encrypt(key, bytes([1]) + nonce + b"\x00\x00")
    return bytes(a ^ b for a, b in zip(x[:tag_len], s0))


def ccm_encrypt(key, nonce, aad, text, tag_len=8):
    tag = _cbc_mac(key, nonce, aad, text, tag_len)
    return _ctr(key, nonce, text) + tag


def ccm_decrypt(key, nonce, aad, data, tag_len=8):
    text, tag = data[:-tag_len], data[-tag_len:]
    plain = _ctr(key, nonce, text)
    if _cbc_mac(key, nonce, aad, plain, tag_len) != tag:
        return None
    return plain


# --- 802.11 PRF and PMK ----------------------------------------------------

def prf(key, label, data, bits):
    out = b""
    i = 0
    while len(out) * 8 < bits:
        out += hmac.new(key, label + b"\x00" + data + bytes([i]), hashlib.sha1).digest()
        i += 1
    return out[:bits // 8]


def kdf_sha256(key, label, data, bits):
    out = b""
    i = 1
    while len(out) * 8 < bits:
        out += hmac.new(key, struct.pack("<H", i) + label + data + struct.pack("<H", bits),
                        hashlib.sha256).digest()
        i += 1
    return out[:bits // 8]


def pmk_psk(passphrase, ssid):
    return hashlib.pbkdf2_hmac("sha1", passphrase.encode(), ssid.encode(), 4096, 32)


def ptk(pmk, aa, spa, anonce, snonce, sha256=False):
    data = min(aa, spa) + max(aa, spa) + min(anonce, snonce) + max(anonce, snonce)
    if sha256:
        return kdf_sha256(pmk, b"Pairwise key expansion", data, 384)
    return prf(pmk, b"Pairwise key expansion", data, 384)


# --- SAE (group 19, P-256), the access point's side ------------------------

_P = 2**256 - 2**224 + 2**192 + 2**96 - 1
_A = _P - 3
_B = 0x5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b
_R = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551


def _pt_add(p, q):
    if p is None:
        return q
    if q is None:
        return p
    if p[0] == q[0] and (p[1] + q[1]) % _P == 0:
        return None
    if p == q:
        l = (3 * p[0] * p[0] + _A) * pow(2 * p[1], -1, _P) % _P
    else:
        l = (q[1] - p[1]) * pow(q[0] - p[0], -1, _P) % _P
    x = (l * l - p[0] - q[0]) % _P
    return (x, (l * (p[0] - x) - p[1]) % _P)


def _pt_mul(k, p):
    r = None
    while k:
        if k & 1:
            r = _pt_add(r, p)
        p = _pt_add(p, p)
        k >>= 1
    return r


def _sswu(u):
    z = _P - 10
    t1 = pow(z * z * pow(u, 4, _P) + z * u * u, _P - 2, _P)
    x1 = (-_B * pow(_A, _P - 2, _P)) * (1 + t1) % _P
    if t1 == 0:
        x1 = _B * pow(z * _A % _P, _P - 2, _P) % _P
    gx1 = (x1**3 + _A * x1 + _B) % _P
    x2 = z * u * u * x1 % _P
    gx2 = (x2**3 + _A * x2 + _B) % _P
    if pow(gx1, (_P - 1) // 2, _P) == 1:
        x, y = x1, pow(gx1, (_P + 1) // 4, _P)
    else:
        x, y = x2, pow(gx2, (_P + 1) // 4, _P)
    if (u & 1) != (y & 1):
        y = _P - y
    return (x, y)


def sae_pt(ssid, password, pwid=b""):
    prk = hmac.new(ssid, password + pwid, hashlib.sha256).digest()

    def expand(info, n):
        out, t, i = b"", b"", 1
        while len(out) < n:
            t = hmac.new(prk, t + info + bytes([i]), hashlib.sha256).digest()
            out += t
            i += 1
        return out[:n]
    u1 = int.from_bytes(expand(b"SAE Hash to Element u1 P1", 48), "big") % _P
    u2 = int.from_bytes(expand(b"SAE Hash to Element u2 P2", 48), "big") % _P
    return _pt_add(_sswu(u1), _sswu(u2))


def sae_pwe(pt, mac_a, mac_b):
    val = int.from_bytes(hmac.new(bytes(32), max(mac_a, mac_b) + min(mac_a, mac_b),
                                  hashlib.sha256).digest(), "big")
    return _pt_mul(val % (_R - 1) + 1, pt)


class SAE:
    """The access point's end of an SAE exchange."""

    def __init__(self, pt, ap_mac, sta_mac):
        self.pwe = sae_pwe(pt, ap_mac, sta_mac)
        self.rand = int.from_bytes(hashlib.sha256(ap_mac + sta_mac + b"r").digest(), "big") % _R
        mask = int.from_bytes(hashlib.sha256(ap_mac + sta_mac + b"m").digest(), "big") % _R
        self.scalar = (self.rand + mask) % _R
        e = _pt_mul(mask, self.pwe)
        self.element = (e[0], (_P - e[1]) % _P)
        self.pmk = self.kck = None

    def commit(self):
        return (self.scalar.to_bytes(32, "big") + self.element[0].to_bytes(32, "big") +
                self.element[1].to_bytes(32, "big"))

    def process(self, peer_scalar, peer_element):
        self.peer_scalar = peer_scalar
        self.peer_element = peer_element
        ps = int.from_bytes(peer_scalar, "big")
        pe = (int.from_bytes(peer_element[:32], "big"), int.from_bytes(peer_element[32:], "big"))
        k = _pt_mul(self.rand, _pt_add(_pt_mul(ps, self.pwe), pe))
        keyseed = hmac.new(bytes(32), k[0].to_bytes(32, "big"), hashlib.sha256).digest()
        context = ((self.scalar + ps) % _R).to_bytes(32, "big")
        keys = kdf_sha256(keyseed, b"SAE KCK and PMK", context, 512)
        self.kck, self.pmk = keys[:32], keys[32:]
        self.pmkid = context[:16]
        return True

    def confirm(self, send, s1, e1, s2, e2):
        msg = struct.pack("<H", send) + s1 + e1 + s2 + e2
        return hmac.new(self.kck, msg, hashlib.sha256).digest()

#!/usr/bin/env python3
# Make PNG test images for libimage (step 25a) in every variant the decoder
# must handle: each colour type and bit depth, palettes with transparency,
# the grey and RGB transparency keys, Adam7 interlacing, all five row filters
# (cycling row by row; random samples too, for the Paeth filter's ties),
# stored blocks (zlib level 0) and a large picture
# (dynamic Huffman codes). Writes DIR/NAME.png for each and prints one line
# per file: "NAME WxH SHA256" -- the SHA-256 of the RGBA8888 pixels the
# decoder must produce (16-bit samples by their high byte, smaller depths
# scaled as v * 255 / (2^depth - 1)). Also writes two damaged files the
# decoder must refuse (not listed): zcrc.png (an IDAT's CRC wrong) and
# zshort.png (the image data cut short, CRCs right).
#   mkpng.py DIR
import hashlib
import os
import struct
import sys
import zlib

ADAM7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2),
         (0, 1, 1, 2)]
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + \
        struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def filter_rows(rows, bpp):
    # each row filtered with type y % 5 against the row above (in its pass)
    out = bytearray()
    prior = None
    for y, raw in enumerate(rows):
        f = y % 5
        line = bytearray([f])
        for i, v in enumerate(raw):
            left = raw[i - bpp] if i >= bpp else 0
            up = prior[i] if prior is not None else 0
            corner = prior[i - bpp] if prior is not None and i >= bpp else 0
            pred = [0, left, up, (left + up) >> 1, paeth(left, up, corner)][f]
            line.append((v - pred) & 0xFF)
        out += line
        prior = raw
    return bytes(out)


def pack_samples(samples, depth):
    if depth == 8:
        return bytes(samples)
    if depth == 16:
        return b"".join(struct.pack(">H", s) for s in samples)
    out = bytearray()
    acc, bits = 0, 0
    for s in samples:
        acc = (acc << depth) | s
        bits += depth
        if bits == 8:
            out.append(acc)
            acc, bits = 0, 0
    if bits:
        out.append(acc << (8 - bits))
    return bytes(out)


def to8(v, depth):
    if depth == 16:
        return v >> 8
    if depth == 8:
        return v
    return v * 255 // ((1 << depth) - 1)


def make(name, w, h, ctype, depth, interlace=False, trns=False, level=6, outdir=".", noise=False):
    top = (1 << depth) - 1
    ch = CHANNELS[ctype]
    palette = [((i * 37) & 255, (i * 91) & 255, (i * 151) & 255) for i in range(1 << min(depth, 8))] \
        if ctype == 3 else []
    alphas = [(i * 17) & 255 for i in range(min(len(palette), 16))] if ctype == 3 and trns else []

    def samples(x, y):
        if noise:                                       # (random samples: every Paeth tie)
            v = (x * 73856093 ^ y * 19349663) * 2654435761
            return [(v >> (8 + 5 * c)) & top for c in range(ch)]
        if ctype == 3:
            return [(x + 2 * y) % len(palette)]
        return [((x * 997 + y * 1601 + c * 3001) if depth == 16 else
                 (x * 7 + y * 13 + c * 50)) & top for c in range(ch)]

    key = None
    if trns and ctype == 0:
        key = samples(0, 0)
    if trns and ctype == 2:
        key = samples(1, 1)

    def rgba(x, y):
        s = samples(x, y)
        if ctype == 0:
            g = to8(s[0], depth)
            return (g, g, g, 0 if key is not None and s == key else 255)
        if ctype == 2:
            return tuple(to8(v, depth) for v in s) + (0 if key is not None and s == key else 255,)
        if ctype == 3:
            i = s[0]
            return palette[i] + (alphas[i] if i < len(alphas) else 255,)
        if ctype == 4:
            g = to8(s[0], depth)
            return (g, g, g, to8(s[1], depth))
        return tuple(to8(v, depth) for v in s)

    bpp = max(1, ch * depth // 8)
    passes = ADAM7 if interlace else [(0, 0, 1, 1)]
    raw = b""
    for x0, y0, dx, dy in passes:
        pw = (w - x0 + dx - 1) // dx if w > x0 else 0
        ph = (h - y0 + dy - 1) // dy if h > y0 else 0
        if not pw or not ph:
            continue
        rows = []
        for py in range(ph):
            line = []
            for px in range(pw):
                line += samples(x0 + px * dx, y0 + py * dy)
            rows.append(pack_samples(line, depth))
        raw += filter_rows(rows, bpp)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, 1 if interlace else 0))
    if palette:
        png += chunk(b"PLTE", b"".join(bytes(c) for c in palette))
    if alphas:
        png += chunk(b"tRNS", bytes(alphas))
    if key is not None:
        png += chunk(b"tRNS", b"".join(struct.pack(">H", v) for v in key))
    png += chunk(b"tEXt", b"Comment\x00RitterOS test")     # (an ancillary chunk: skipped)
    data = zlib.compress(raw, level)
    third = len(data) // 3                              # (split over several IDATs)
    for part in (data[:third], data[third:2 * third], data[2 * third:]):
        png += chunk(b"IDAT", part)
    png += chunk(b"IEND", b"")
    with open(os.path.join(outdir, name + ".png"), "wb") as f:
        f.write(png)
    pixels = bytearray()
    for y in range(h):
        for x in range(w):
            pixels += bytes(rgba(x, y))
    print("%s %dx%d %s" % (name, w, h, hashlib.sha256(pixels).hexdigest()))


def main():
    outdir = sys.argv[1]
    W, H = 37, 29
    for name, ctype, depth, interlace, trns, level in [
            ("g1", 0, 1, False, False, 6), ("g2", 0, 2, False, False, 6),
            ("g4", 0, 4, False, False, 6), ("g8", 0, 8, False, False, 6),
            ("g16", 0, 16, False, False, 6), ("g8t", 0, 8, False, True, 6),
            ("rgb8", 2, 8, False, False, 6), ("rgb16", 2, 16, False, False, 6),
            ("rgb8t", 2, 8, False, True, 6), ("p1", 3, 1, False, False, 6),
            ("p2", 3, 2, False, False, 6), ("p4", 3, 4, False, False, 6),
            ("p8", 3, 8, False, False, 6), ("p8t", 3, 8, False, True, 6),
            ("ga8", 4, 8, False, False, 6), ("ga16", 4, 16, False, False, 6),
            ("rgba8", 6, 8, False, False, 6), ("rgba16", 6, 16, False, False, 6),
            ("rgba8i", 6, 8, True, False, 6), ("p4i", 3, 4, True, False, 6),
            ("g1i", 0, 1, True, False, 6), ("rgb16i", 2, 16, True, False, 6),
            ("rgb8z0", 2, 8, False, False, 0)]:
        make(name, W, H, ctype, depth, interlace, trns, level, outdir)
    make("big", 400, 300, 2, 8, False, False, 9, outdir)
    make("noise", 64, 48, 6, 8, False, False, 6, outdir, noise=True)
    make("noise16i", 64, 48, 2, 16, True, False, 6, outdir, noise=True)
    good = open(os.path.join(outdir, "rgb8.png"), "rb").read()
    at = good.index(b"IDAT")                            # the first IDAT's type
    n = struct.unpack(">I", good[at - 4:at])[0]
    bad = bytearray(good)
    bad[at + 4 + n] ^= 1                                # its CRC's first byte
    with open(os.path.join(outdir, "zcrc.png"), "wb") as f:
        f.write(bad)
    cut = good[:at - 4] + chunk(b"IDAT", good[at + 4:at + 4 + n // 2]) + chunk(b"IEND", b"")
    with open(os.path.join(outdir, "zshort.png"), "wb") as f:
        f.write(cut)


if __name__ == "__main__":
    main()

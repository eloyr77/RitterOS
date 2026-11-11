#!/usr/bin/env python3
"""pngcheck (41h-2): libimage's DEFLATE compressor and PNG writer, built on
the Mac as pngenc_host (scripts/pngenc_host.c), read back by inflaters not
ours: Python's zlib (every stream; the PNG's chunks, CRCs, filters and
pixels decoded here, compared with the pixels written) and the Mac's ImageIO
(sips must read each PNG's size).

    pngcheck.py PNGENC_HOST WORKDIR

Prints a line per case and a summary; exit 1 on any failure.
"""
import os
import random
import struct
import subprocess
import sys
import zlib

failures = 0


def report(ok, text):
    global failures
    print(("pngcheck: " if ok else "pngcheck: FAILED: ") + text)
    failures += not ok


def deflate_cases():
    r = random.Random(41)
    block = bytes(r.getrandbits(8) for _ in range(32768))
    block2 = bytes(r.getrandbits(8) for _ in range(32769))
    text = b"the window system's screen, saved as a picture; " * 2000
    return [
        ("empty", b""),
        ("one byte", b"a"),
        ("a run of 258", b"x" * 258),
        ("a run of 259", b"x" * 259),
        ("a run of 1000", b"y" * 1000),
        ("zeros, 1 MiB", bytes(1 << 20)),
        ("random, 200 KiB", bytes(r.getrandbits(8) for _ in range(200 * 1024))),
        ("text", text),
        ("a block again 32768 back", block * 3),
        ("a block again 32769 back", block2 * 2),
        ("every byte value", bytes(range(256)) * 300),
    ]


def unfilter(raw, w, h, bpp):
    rb = w * bpp
    out, prior, at = [], bytearray(rb), 0
    for _ in range(h):
        f, row = raw[at], bytearray(raw[at + 1:at + 1 + rb])
        at += rb + 1
        for i in range(rb):
            a = row[i - bpp] if i >= bpp else 0
            b = prior[i]
            c = prior[i - bpp] if i >= bpp else 0
            if f == 0:
                p = 0
            elif f == 1:
                p = a
            elif f == 2:
                p = b
            elif f == 3:
                p = (a + b) // 2
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                p = a if pa <= pb and pa <= pc else b if pb <= pc else c
            else:
                raise ValueError(f"filter {f}")
            row[i] = (row[i] + p) & 255
        out.append(bytes(row))
        prior = row
    if at != len(raw):
        raise ValueError(f"{len(raw) - at} bytes after the last row")
    return b"".join(out)


def decode_png(data):
    """(w, h, channels, pixels) by the specification, every check on."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("no PNG signature")
    at, idat, ihdr, ended = 8, b"", None, False
    while at < len(data):
        n, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + n]
        crc = struct.unpack(">I", data[at + 8 + n:at + 12 + n])[0]
        if zlib.crc32(kind + body) != crc:
            raise ValueError(f"{kind!r}: CRC wrong")
        if kind == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            ended = True
        at += 12 + n
    if not ihdr or not ended or at != len(data):
        raise ValueError("chunks wrong")
    w, h, depth, ctype, comp, filt, interlace = ihdr
    if depth != 8 or ctype not in (2, 6) or comp or filt or interlace:
        raise ValueError(f"header {ihdr}")
    ch = 4 if ctype == 6 else 3
    return w, h, ch, unfilter(zlib.decompress(idat), w, h, ch)


def main():
    host, work = sys.argv[1], sys.argv[2]
    os.makedirs(work, exist_ok=True)
    for name, data in deflate_cases():
        src, dst = os.path.join(work, "in.bin"), os.path.join(work, "out.z")
        with open(src, "wb") as f:
            f.write(data)
        rc = subprocess.run([host, "deflate", src, dst]).returncode
        try:
            z = open(dst, "rb").read() if rc == 0 else b""
            back = zlib.decompress(z)
            report(back == data, f"deflate {name}: {len(data)} -> {len(z)} bytes"
                   + ("" if back == data else ", inflated differently"))
        except Exception as e:                         # (zlib.error, missing file)
            report(False, f"deflate {name}: rc {rc}, {e}")
    pictures = [("noise", 37, 23), ("alpha", 64, 64), ("flat", 1, 1), ("flat", 640, 480),
                ("gradient", 255, 100), ("stripes", 333, 17), ("screen", 1280, 800),
                ("far", 3000, 12), ("noise", 1, 300), ("alpha", 3, 1)]
    for kind, w, h in pictures:
        png, raw = os.path.join(work, f"{kind}-{w}x{h}.png"), os.path.join(work, "raw.rgba")
        rc = subprocess.run([host, "png", kind, str(w), str(h), "7", png, raw]).returncode
        try:
            data, rgba = open(png, "rb").read(), open(raw, "rb").read()
            pw, ph, ch, pix = decode_png(data)
            want = rgba if ch == 4 else b"".join(rgba[i:i + 3] for i in range(0, len(rgba), 4))
            opaque = all(rgba[i] == 255 for i in range(3, len(rgba), 4))
            ok = (pw, ph) == (w, h) and pix == want and (ch == 3) == opaque
            sips = subprocess.run(["sips", "-g", "pixelWidth", "-g", "pixelHeight", png],
                                  capture_output=True, text=True).stdout
            seen = f"pixelWidth: {w}" in sips and f"pixelHeight: {h}" in sips
            report(ok and seen, f"png {kind} {w}x{h}: {len(rgba)} bytes of pixels -> {len(data)}"
                   + ("" if ok else ", pixels differ") + ("" if seen else ", ImageIO cannot read it"))
            if kind == "screen":
                report(len(data) * 20 < len(rgba), f"a screen compresses over 20 times "
                       f"({len(rgba) / len(data):.0f})")
        except Exception as e:
            report(False, f"png {kind} {w}x{h}: rc {rc}, {e}")
    print(f"pngcheck: {failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()

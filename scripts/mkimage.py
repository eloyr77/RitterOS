#!/usr/bin/env python3
# Make test images for the libimage test: a patterned picture as PPM and BMP
# (same pixels), and a solid-colour PPM. Prints the SHA-256 of the patterned
# picture's RGBA8888 pixels, which the RitterOS test compares against.
import hashlib
import struct
import sys

W, H = 200, 150


def pattern(x, y):
    return ((x * 7) & 0xFF, (y * 13) & 0xFF, ((x + y) * 5) & 0xFF)


def write_ppm(path, pixel):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (W, H))
        for y in range(H):
            for x in range(W):
                f.write(bytes(pixel(x, y)))


def write_bmp(path, pixel):
    row = (W * 3 + 3) & ~3
    data = bytearray()
    for y in range(H - 1, -1, -1):          # BMP rows are bottom-up
        line = bytearray()
        for x in range(W):
            r, g, b = pixel(x, y)
            line += bytes((b, g, r))         # BGR
        line += b"\x00" * (row - W * 3)
        data += line
    total = 54 + len(data)
    hdr = struct.pack("<2sIHHI", b"BM", total, 0, 0, 54)
    dib = struct.pack("<IiiHHIIiiII", 40, W, H, 1, 24, 0, len(data), 2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(hdr + dib + bytes(data))


def rgba_sha(pixel):
    h = hashlib.sha256()
    for y in range(H):
        for x in range(W):
            r, g, b = pixel(x, y)
            h.update(bytes((r, g, b, 255)))
    return h.hexdigest()


def main():
    ppm, bmp, solid = sys.argv[1], sys.argv[2], sys.argv[3]
    write_ppm(ppm, pattern)
    write_bmp(bmp, pattern)
    write_ppm(solid, lambda x, y: (200, 80, 40))
    print(rgba_sha(pattern))


if __name__ == "__main__":
    main()

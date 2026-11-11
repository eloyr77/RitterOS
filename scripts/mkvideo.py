#!/usr/bin/env python3
# Make a test RVID video (solid-colour frames + a tone) for the libvideo/reel
# test, and print the SHA-256 of its frames' RGBA8888 pixels.
#   mkvideo.py PATH [WIDTH HEIGHT]     (160 x 120 if not given)
import hashlib
import struct
import sys

W, H, FPS = 160, 120, 2
COLORS = [(200, 40, 40), (40, 200, 40), (40, 40, 200)]      # red, green, blue
ARATE, ACH = 22050, 1
AFRAMES = ARATE                                             # ~1 second of audio


MARK = (255, 128, 0)            # a 20x20 top-left marker, the same in every frame
MS = 20


def pixel(x, y, color):
    return MARK if x < MS and y < MS else color


def frames_rgb():
    out = bytearray()
    for c in COLORS:
        for y in range(H):
            for x in range(W):
                out += bytes(pixel(x, y, c))
    return bytes(out)


def frames_rgba_sha():
    h = hashlib.sha256()
    for c in COLORS:
        for y in range(H):
            for x in range(W):
                r, g, b = pixel(x, y, c)
                h.update(bytes((r, g, b, 255)))
    return h.hexdigest()


def audio():
    s = bytearray()
    for i in range(AFRAMES):
        s += struct.pack("<h", (i * 97) % 65536 - 32768)    # a loud sawtooth
    return bytes(s)


def main():
    global W, H
    path = sys.argv[1]
    if len(sys.argv) == 4:
        W, H = int(sys.argv[2]), int(sys.argv[3])
    aud = audio()
    hdr = b"RVID" + struct.pack("<IIIIIIII", 1, W, H, FPS, len(COLORS), ARATE, ACH, AFRAMES)
    with open(path, "wb") as f:
        f.write(hdr + aud + frames_rgb())
    print(frames_rgba_sha())


if __name__ == "__main__":
    main()

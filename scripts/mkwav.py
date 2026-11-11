#!/usr/bin/env python3
# Make a test WAV (16-bit stereo PCM) for the libaudio test and print the
# SHA-256 of its 16-bit samples, which the RitterOS test compares against.
import hashlib
import struct
import sys

RATE = 22050
FRAMES = 4000
CHANNELS = 2


def samples():
    s = bytearray()
    for i in range(FRAMES):
        left = (i * 37) % 65536 - 32768         # a deterministic sawtooth-ish pattern
        right = (i * 53 + 1000) % 65536 - 32768
        s += struct.pack("<hh", left, right)
    return bytes(s)


def main():
    path = sys.argv[1]
    data = samples()
    hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, CHANNELS, RATE,
                                 RATE * CHANNELS * 2, CHANNELS * 2, 16)
    hdr += b"data" + struct.pack("<I", len(data))
    with open(path, "wb") as f:
        f.write(hdr + data)
    print(hashlib.sha256(data).hexdigest())


if __name__ == "__main__":
    main()

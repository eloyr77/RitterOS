#!/usr/bin/env python3
# Print the peak absolute 16-bit sample in a WAV file (used to tell a tone
# from silence in the HDA test). Parses the data chunk; tolerant of a header
# QEMU did not finalise (falls back to everything after 44 bytes).
import struct
import sys


def data_bytes(b):
    if len(b) >= 12 and b[:4] == b"RIFF" and b[8:12] == b"WAVE":
        at = 12
        while at + 8 <= len(b):
            cid = b[at:at + 4]
            sz = struct.unpack("<I", b[at + 4:at + 8])[0]
            body = at + 8
            if cid == b"data":
                end = body + sz if 0 < sz <= len(b) - body else len(b)
                return b[body:end]
            at = body + sz + (sz & 1)
            if sz == 0:
                break
    return b[44:]


def main():
    b = open(sys.argv[1], "rb").read()
    d = data_bytes(b)
    peak = 0
    for i in range(0, len(d) - 1, 2):
        v = struct.unpack("<h", d[i:i + 2])[0]
        if abs(v) > peak:
            peak = abs(v)
    print(peak)


if __name__ == "__main__":
    main()

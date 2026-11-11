#!/usr/bin/env python3
# Make JPEG test images for libimage (step 25b) and their reference pictures.
# A small baseline encoder of our own: it does not compress a picture but
# writes chosen quantized coefficients (smooth DC, sparse AC with long zero
# runs, the largest categories, values that clip), so the reference pixels
# come from those same coefficients through an exact floating-point inverse
# DCT -- independent of any decoder's parsing, which is what the test checks.
# Variants: grey, 4:4:4, 4:2:2, 4:2:0, 4:4:0 and 4:1:1 sampling, restart
# markers, one scan per component, 16-bit quantization tables (SOF1), Adobe's
# RGB (no transform), sizes that are not whole MCUs, fill bytes and segments
# to skip; and progressive files (25c: T.81 G.1.2, with Huffman tables made
# for each scan) of the same coefficients as baseline twins, so the same
# pictures. Writes DIR/NAME.jpg and DIR/NAME.ppm (the reference, P6) and
# prints the names (two damaged files too, below). The reference upsamples
# halved chroma with the triangle filter (3/4 of the nearer sample, 1/4 of
# the next), as libjpeg's "fancy" upsampling and the decoder do, other ratios
# by repeating samples, and converts YCbCr with JFIF's formulas; a decoder in
# integers lands within a few levels of it.
#   mkjpeg.py DIR
import heapq
import math
import os
import random
import struct
import sys

ZIGZAG = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
          12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
          35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
          58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63]

# T.81 Annex K: example quantization tables (natural order) and Huffman tables.
QUANT_LUMA = [16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
              14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
              18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
              49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99]
QUANT_CHROMA = [17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
                24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99] + [99] * 32
DC_LUMA = ([0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0], list(range(12)))
DC_CHROMA = ([0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0], list(range(12)))
AC_LUMA = ([0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d], [
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61,
    0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52,
    0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25,
    0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
    0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3,
    0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8,
    0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa])
AC_CHROMA = ([0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77], [
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61,
    0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33,
    0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18,
    0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
    0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63,
    0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
    0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
    0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa])


def codes(table):
    counts, values = table
    out, code, k = {}, 0, 0
    for length in range(1, 17):
        for _ in range(counts[length - 1]):
            out[values[k]] = (code, length)
            code += 1
            k += 1
        code <<= 1
    return out


class Bits:
    def __init__(self):
        self.out, self.acc, self.n = bytearray(), 0, 0

    def put(self, value, length):
        for i in range(length - 1, -1, -1):
            self.acc = self.acc << 1 | (value >> i) & 1
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                if self.acc == 0xFF:
                    self.out.append(0)                  # (stuffed)
                self.acc, self.n = 0, 0

    def flush(self):                                    # pad with ones to the byte
        while self.n:
            self.put(1, 1)


def category(v):
    return 0 if v == 0 else abs(v).bit_length()


def put_value(bits, v, s):
    bits.put(v if v >= 0 else v + (1 << s) - 1, s)


def encode_block(bits, block, pred, dc, ac):
    diff = block[0] - pred
    s = category(diff)
    bits.put(*dc[s])
    if s:
        put_value(bits, diff, s)
    run = 0
    for k in range(1, 64):
        v = block[k]
        if v == 0:
            run += 1
            continue
        while run > 15:
            bits.put(*ac[0xF0])                         # ZRL: sixteen zeros
            run -= 16
        s = category(v)
        bits.put(*ac[run << 4 | s])
        put_value(bits, v, s)
        run = 0
    if run:
        bits.put(*ac[0x00])                             # EOB
    return block[0]


def segment(marker, data):
    return struct.pack(">BBH", 0xFF, marker, len(data) + 2) + data


def table_segment(cls, index, table):
    counts, values = table
    return bytes([cls << 4 | index]) + bytes(counts) + bytes(values)


def idct(coef):
    # exact: f(x,y) = 1/4 sum C(u) C(v) F(u,v) cos((2x+1)u pi/16) cos((2y+1)v pi/16), + 128
    cosines = [[math.cos((2 * x + 1) * u * math.pi / 16) * (math.sqrt(0.5) if u == 0 else 1)
                for u in range(8)] for x in range(8)]
    rows = [[sum(coef[v * 8 + u] * cosines[x][u] for u in range(8)) for x in range(8)]
            for v in range(8)]
    return [[min(255, max(0, math.floor(sum(rows[v][x] * cosines[y][v] for v in range(8)) / 4
                                        + 128 + 0.5))) for x in range(8)] for y in range(8)]


def coefficients(rng, bx, by, comp):
    # quantized coefficients in zigzag order: a smooth DC field, sparse AC
    block = [0] * 64
    block[0] = int(30 * math.sin(bx * 0.7 + comp) + 25 * math.cos(by * 0.5 - comp))
    kind = (bx * 7 + by * 3 + comp) % 9
    if kind == 0:
        block[0] = 127 if (bx + by) % 2 else -127       # (large DC differences, clipping)
    for k in range(1, 64):
        if rng.random() < (0.5 if k < 6 else 0.08):
            block[k] = rng.randint(-12, 12)
    if kind == 3:
        block[1:40] = [0] * 39                          # long runs: ZRLs before 40
        block[40] = rng.choice([-1, 1]) * rng.randint(1, 3)
    if kind == 5:
        block[63] = 1                                   # the very last coefficient
    if kind == 7 and comp == 0:
        block[1] = rng.choice([-1, 1]) * rng.randint(40, 63)   # (AC of category 6+)
    return block


def optimal_table(freq):
    # T.81 K.2-K.3: code lengths by Huffman's method with a reserved symbol
    # (so no code is all ones), limited to 16 bits; the most frequent symbols
    # get the shortest codes.
    symbols = sorted(freq)
    size = {sym: 0 for sym in symbols + [256]}
    items = [(freq[sym], i, [sym]) for i, sym in enumerate(symbols)] + [(1, len(symbols), [256])]
    heapq.heapify(items)
    serial = len(items)
    while len(items) > 1:
        f1, _, a = heapq.heappop(items)
        f2, _, b = heapq.heappop(items)
        for sym in a + b:
            size[sym] += 1
        heapq.heappush(items, (f1 + f2, serial, a + b))
        serial += 1
    bits = [0] * 64
    for length in size.values():
        bits[length] += 1
    i = max(size.values())
    while i > 16:
        while bits[i] > 0:
            j = i - 2
            while bits[j] == 0:
                j -= 1
            bits[i] -= 2
            bits[i - 1] += 1
            bits[j + 1] += 2
            bits[j] -= 1
        i -= 1
    while bits[i] == 0:
        i -= 1
    bits[i] -= 1                                        # (the reserved symbol's code)
    return bits[1:17], sorted(symbols, key=lambda sym: (-freq[sym], sym))


def progressive_ops(blocks, units, ncomp, ss, se, ah, al, restart):
    # One progressive scan (T.81 G.1.2) as a list of operations: ("h", class,
    # symbol) a Huffman-coded symbol, ("b", value, n) n plain bits, ("m", m) a
    # restart marker. The decoder must undo exactly this.
    ops, preds = [], [0] * ncomp
    run = {"eob": 0, "bits": []}                        # the pending end-of-band run

    def bits_of(v, n):
        ops.append(("b", v if v >= 0 else v + (1 << n) - 1, n))

    def flush():                                        # EOBn, its count, the run's corrections
        e = run["eob"]
        if e:
            n = e.bit_length() - 1
            ops.append(("h", 1, n << 4))
            if n:
                ops.append(("b", e - (1 << n), n))
            ops.extend(("b", bit, 1) for bit in run["bits"])
            run["eob"], run["bits"] = 0, []

    for i, unit in enumerate(units):
        if restart and i and i % restart == 0:
            flush()
            ops.append(("m", 0xD0 + (i // restart - 1) % 8))
            preds = [0] * ncomp
        for c, bx, by in unit:
            z = blocks[c][(bx, by)]
            if ss == 0 and ah == 0:                     # DC first: the shifted difference
                v = z[0] >> al
                diff, preds[c] = v - preds[c], v
                cat = category(diff)
                ops.append(("h", 0, cat))
                if cat:
                    bits_of(diff, cat)
            elif ss == 0:                               # DC refinement: one bit
                ops.append(("b", (z[0] >> al) & 1, 1))
            elif ah == 0:                               # AC first, the band ss..se
                zeros = 0
                for k in range(ss, se + 1):
                    a = abs(z[k]) >> al
                    if a == 0:
                        zeros += 1
                        continue
                    flush()
                    while zeros > 15:
                        ops.append(("h", 1, 0xF0))
                        zeros -= 16
                    cat = a.bit_length()
                    ops.append(("h", 1, zeros << 4 | cat))
                    bits_of(a if z[k] > 0 else -a, cat)
                    zeros = 0
                if zeros:
                    run["eob"] += 1
                    if run["eob"] == 0x7FFF:
                        flush()
            else:                                       # AC refinement (as libjpeg's encoder)
                a = [abs(z[k]) >> al for k in range(64)]
                last_new = max([k for k in range(ss, se + 1) if a[k] == 1], default=0)
                zeros, corrections = 0, []
                for k in range(ss, se + 1):
                    if a[k] == 0:
                        zeros += 1
                        continue
                    while zeros > 15 and k <= last_new:
                        flush()
                        ops.append(("h", 1, 0xF0))
                        zeros -= 16
                        ops.extend(("b", bit, 1) for bit in corrections)
                        corrections = []
                    if a[k] > 1:                        # nonzero before: its next bit
                        corrections.append(a[k] & 1)
                        continue
                    flush()                             # newly nonzero: run, sign, corrections
                    ops.append(("h", 1, zeros << 4 | 1))
                    ops.append(("b", 1 if z[k] > 0 else 0, 1))
                    ops.extend(("b", bit, 1) for bit in corrections)
                    zeros, corrections = 0, []
                if zeros or corrections:
                    run["eob"] += 1
                    run["bits"] += corrections
                    if run["eob"] == 0x7FFF or len(run["bits"]) > 937:
                        flush()
    flush()
    return ops


def make(outdir, name, w, h, sampling, restart=0, separate=False, sixteen=False, rgb=False,
         extras=False, seed=1, script=None, orientation=0):
    rng = random.Random(seed)
    ncomp = len(sampling)
    hmax = max(s[0] for s in sampling)
    vmax = max(s[1] for s in sampling)
    mcux = (w + 8 * hmax - 1) // (8 * hmax)
    mcuy = (h + 8 * vmax - 1) // (8 * vmax)
    quants = [QUANT_LUMA, QUANT_CHROMA]
    tq = [0] + [1] * (ncomp - 1) if not rgb else [0] * ncomp
    if sixteen:
        quants = [[q * 3 for q in QUANT_LUMA], [q * 300 // 99 + 1 for q in QUANT_CHROMA]]
    # every block's coefficients (zigzag order, quantized) and its samples
    blocks, planes = [], []
    for c, (hs, vs) in enumerate(sampling):
        bw, bh = mcux * hs, mcuy * vs
        q = quants[tq[c]]
        comp_blocks = {}
        plane = [[0] * (bw * 8) for _ in range(bh * 8)]
        for by in range(bh):
            for bx in range(bw):
                z = coefficients(rng, bx, by, c)
                if sixteen:                     # (the same pictures, coarser steps)
                    z = [v // 3 for v in z]
                comp_blocks[(bx, by)] = z
                natural = [0] * 64
                for k in range(64):
                    natural[ZIGZAG[k]] = z[k] * q[ZIGZAG[k]]
                for y, row in enumerate(idct(natural)):
                    plane[by * 8 + y][bx * 8:bx * 8 + 8] = row
        blocks.append(comp_blocks)
        planes.append(plane)

    out = bytearray(b"\xFF\xD8")
    out += segment(0xE0, b"JFIF\x00\x01\x02\x00\x00\x01\x00\x01\x00\x00") if not rgb else \
        segment(0xEE, b"Adobe\x00\x64\x00\x00\x00\x00\x00")   # transform 0: RGB
    if extras:
        out += segment(0xFE, b"RitterOS test")                  # COM
        out += segment(0xE1, b"Exif\x00\x00" + bytes(40))       # APP1, skipped
    if orientation:                                             # EXIF: tag 0x0112, a SHORT
        tiff = b"MM\x00\x2a" + struct.pack(">I", 8) + struct.pack(">H", 2)
        tiff += struct.pack(">HHIHH", 0x010F, 2, 4, 0, 0)        # (Make, before it: skipped)
        tiff += struct.pack(">HHIHH", 0x0112, 3, 1, orientation, 0) + struct.pack(">I", 0)
        out += segment(0xE1, b"Exif\x00\x00" + tiff)
    for i, q in enumerate(quants[:1 + (ncomp > 1 and not rgb)]):
        table = [q[ZIGZAG[k]] for k in range(64)]
        out += segment(0xDB, bytes([0x10 | i]) + b"".join(struct.pack(">H", v) for v in table)
                       if sixteen else bytes([i]) + bytes(table))
    frame = struct.pack(">BHHB", 8, h, w, ncomp)
    for c, (hs, vs) in enumerate(sampling):
        frame += bytes([c + 1, hs << 4 | vs, tq[c]])
    out += segment(0xC2 if script else 0xC1 if sixteen else 0xC0, frame)
    if not script:
        huff = table_segment(0, 0, DC_LUMA) + table_segment(1, 0, AC_LUMA)
        if ncomp > 1:
            huff += table_segment(0, 1, DC_CHROMA) + table_segment(1, 1, AC_CHROMA)
        out += segment(0xC4, huff)
    if restart:
        out += segment(0xDD, struct.pack(">H", restart))
    if extras:
        out += b"\xFF\xFF"                                      # fill bytes before a marker
    def units_of(scan):                                 # blocks one by one, or whole MCUs
        if len(scan) == 1:
            hs, vs = sampling[scan[0]]
            cw = (w * hs + hmax - 1) // hmax
            ch = (h * vs + vmax - 1) // vmax
            return [[(scan[0], bx, by)] for by in range((ch + 7) // 8) for bx in range((cw + 7) // 8)]
        return [[(c, mx * sampling[c][0] + x, my * sampling[c][1] + y)
                 for c in scan for y in range(sampling[c][1]) for x in range(sampling[c][0])]
                for my in range(mcuy) for mx in range(mcux)]

    # Progressive: each scan gets Huffman tables made for it (DC class 0 or AC
    # class 1, table 0, defined again before the scan).
    for scan, ss, se, ah, al in script or []:
        ops = progressive_ops(blocks, units_of(scan), ncomp, ss, se, ah, al, restart)
        freq = {}
        for op in ops:
            if op[0] == "h":
                freq[op[2]] = freq.get(op[2], 0) + 1
        header = bytes([len(scan)])
        for c in scan:
            header += bytes([c + 1, 0x00])
        if freq:
            table = optimal_table(freq)
            out += segment(0xC4, table_segment(0 if ss == 0 else 1, 0, table))
            code = codes(table)
        out += segment(0xDA, header + bytes([ss, se, ah << 4 | al]))
        bits = Bits()
        for op in ops:
            if op[0] == "h":
                bits.put(*code[op[2]])
            elif op[0] == "b":
                bits.put(op[1], op[2])
            else:
                bits.flush()
                bits.out += bytes([0xFF, op[1]])
        bits.flush()
        out += bits.out

    tables = [(codes(DC_LUMA), codes(AC_LUMA))] + [(codes(DC_CHROMA), codes(AC_CHROMA))] * 2
    if rgb:
        tables = [tables[0]] * 3
    tid = [0] + [1] * (ncomp - 1) if not rgb else [0] * ncomp
    scans = [[c] for c in range(ncomp)] if separate else [list(range(ncomp))]
    for scan in [] if script else scans:
        header = bytes([len(scan)])
        for c in scan:
            header += bytes([c + 1, tid[c] << 4 | tid[c]])
        out += segment(0xDA, header + b"\x00\x3F\x00")
        bits = Bits()
        units = units_of(scan)
        preds = [0] * ncomp
        for n, unit in enumerate(units):
            if restart and n and n % restart == 0:
                bits.flush()
                bits.out += bytes([0xFF, 0xD0 + (n // restart - 1) % 8])
                preds = [0] * ncomp
            for c, bx, by in unit:
                preds[c] = encode_block(bits, blocks[c][(bx, by)], preds[c], *tables[c])
        bits.flush()
        out += bits.out
    out += b"\xFF\xD9"
    with open(os.path.join(outdir, name + ".jpg"), "wb") as f:
        f.write(out)

    # the reference: upsample, convert
    pixels = bytearray()
    for y in range(h):
        rows = []
        for c, (hs, vs) in enumerate(sampling):
            sx, sy = hmax // hs, vmax // vs
            plane = planes[c]
            cw = (w * hs + hmax - 1) // hmax
            ch = (h * vs + vmax - 1) // vmax
            halves = sx * hs == hmax and sy * vs == vmax and sx <= 2 and sy <= 2
            if not halves:
                rows.append([plane[y * vs // vmax][x * hs // hmax] for x in range(w)])
                continue
            near = plane[y // sy]
            if sy == 2:
                far_y = min(max(y // 2 + 1 if y & 1 else y // 2 - 1, 0), ch - 1)
                col = [(3 * near[x] + plane[far_y][x]) / 4 for x in range(cw)]
            else:
                col = [near[x] for x in range(cw)]
            if sx == 2:
                row = []
                for x in range(w):
                    i = x >> 1
                    other = min(i + 1, cw - 1) if x & 1 else max(i - 1, 0)
                    row.append((3 * col[i] + col[other]) / 4)
                rows.append(row)
            else:
                rows.append(col[:w])
        for x in range(w):
            if ncomp == 1:
                g = round(rows[0][x])
                pixels += bytes([g, g, g])
            elif rgb:
                pixels += bytes(round(rows[c][x]) for c in range(3))
            else:
                yy, cb, cr = rows[0][x], rows[1][x] - 128, rows[2][x] - 128
                rgbv = (yy + 1.402 * cr, yy - 0.344136 * cb - 0.714136 * cr, yy + 1.772 * cb)
                pixels += bytes(min(255, max(0, math.floor(v + 0.5))) for v in rgbv)
    if orientation > 1:                                         # as it is to be shown
        turned = bytearray()
        tw, th = (h, w) if orientation >= 5 else (w, h)
        for y in range(th):
            for x in range(tw):
                sx, sy = {2: (w - 1 - x, y), 3: (w - 1 - x, h - 1 - y), 4: (x, h - 1 - y),
                          5: (y, x), 6: (y, h - 1 - x), 7: (w - 1 - y, h - 1 - x),
                          8: (w - 1 - y, x)}[orientation]
                turned += pixels[(sy * w + sx) * 3:(sy * w + sx) * 3 + 3]
        pixels, w, h = turned, tw, th
    with open(os.path.join(outdir, name + ".ppm"), "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h) + pixels)
    print(name)


def main():
    d = sys.argv[1]
    make(d, "grey", 61, 45, [(1, 1)], extras=True)
    make(d, "s444", 61, 45, [(1, 1), (1, 1), (1, 1)])
    make(d, "s422", 61, 45, [(2, 1), (1, 1), (1, 1)])
    make(d, "s420", 61, 45, [(2, 2), (1, 1), (1, 1)], extras=True)
    make(d, "s440", 61, 45, [(1, 2), (1, 1), (1, 1)])
    make(d, "s411", 61, 45, [(4, 1), (1, 1), (1, 1)])
    make(d, "s420r", 61, 45, [(2, 2), (1, 1), (1, 1)], restart=3)
    make(d, "s420s", 61, 45, [(2, 2), (1, 1), (1, 1)], separate=True, restart=5)
    make(d, "greyr", 61, 45, [(1, 1)], restart=1)
    make(d, "s444q16", 61, 45, [(1, 1), (1, 1), (1, 1)], sixteen=True)
    make(d, "rgb", 61, 45, [(1, 1), (1, 1), (1, 1)], rgb=True)
    make(d, "big420", 320, 240, [(2, 2), (1, 1), (1, 1)], restart=40, seed=7)
    # Progressive (25c): the same coefficients as above, so the same pictures.
    # libjpeg's usual script: DC with Al 1 interleaved, AC bands with
    # successive approximation, refinements.
    usual = [([0, 1, 2], 0, 0, 0, 1), ([0], 1, 5, 0, 2), ([2], 1, 63, 0, 1), ([1], 1, 63, 0, 1),
             ([0], 6, 63, 0, 2), ([0], 1, 63, 2, 1), ([0, 1, 2], 0, 0, 1, 0), ([2], 1, 63, 1, 0),
             ([1], 1, 63, 1, 0), ([0], 1, 63, 1, 0)]
    make(d, "p420", 61, 45, [(2, 2), (1, 1), (1, 1)], extras=True, script=usual)
    # grey, spectral selection only (no refinement), several bands
    make(d, "pgrey", 61, 45, [(1, 1)], extras=True,
         script=[([0], 0, 0, 0, 0), ([0], 1, 2, 0, 0), ([0], 3, 9, 0, 0), ([0], 10, 63, 0, 0)])
    # 4:4:4, DC one component at a time, AC refined over three more bits,
    # restart markers (runs and predictions start again)
    deep = [([0], 0, 0, 0, 2), ([1], 0, 0, 0, 2), ([2], 0, 0, 0, 2)]
    for c in range(3):
        deep += [([c], 1, 63, 0, 3), ([c], 1, 63, 3, 2), ([c], 1, 63, 2, 1), ([c], 1, 63, 1, 0)]
    deep += [([0, 1, 2], 0, 0, 2, 1), ([0, 1, 2], 0, 0, 1, 0)]
    make(d, "p444r", 61, 45, [(1, 1), (1, 1), (1, 1)], restart=2, script=deep)
    make(d, "pbig420", 320, 240, [(2, 2), (1, 1), (1, 1)], seed=7, script=usual)
    # EXIF orientations (cameras store portrait photos landscape, with a tag
    # saying how to turn them): every one of the eight
    for o in range(1, 9):
        make(d, "o%d" % o, 61, 45, [(2, 2), (1, 1), (1, 1)], orientation=o)
    # Damaged (not listed): zcut.jpg, the 4:2:0 file cut short in its data
    # (decoded anyway, the rest grey, as other decoders do); zsof.jpg, a
    # frame of width 0 (refused).
    good = open(os.path.join(d, "s420.jpg"), "rb").read()
    sos = good.index(b"\xFF\xDA")
    with open(os.path.join(d, "zcut.jpg"), "wb") as f:
        f.write(good[:sos + (len(good) - sos) // 2])
    sof = good.index(b"\xFF\xC0")
    bad = bytearray(good)
    bad[sof + 7:sof + 9] = b"\x00\x00"                    # the width
    with open(os.path.join(d, "zsof.jpg"), "wb") as f:
        f.write(bad)


if __name__ == "__main__":
    main()

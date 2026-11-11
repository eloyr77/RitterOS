#!/usr/bin/env python3
"""exFAT on the host, for the tests (independent of kernel/exfat.c).

Usage:
  exfat.py make IMAGE FIRST_SECTOR SECTORS LABEL TREE.json [mbr]
      make an exFAT in the image's sectors FIRST..; TREE is a JSON object:
      names to file contents (strings, {"repeat": TEXT, "times": N}, or
      {"text": TEXT, "valid": N, "size": M}: only N bytes written of a
      file of M, the rest reads as zeros) or to objects (directories).
      A name starting with "+" (the "+" dropped) is laid out as one run of
      clusters (NoFatChain); the others are spread over the heap in pieces
      (every other cluster), so that reading must follow the FAT. "mbr":
      also write an MBR with one partition of type 0x07.
  exfat.py ls TREE.json PATH
      what RitterOS's ls prints for PATH ("" the root) in that tree
  exfat.py tree IMAGE FIRST_SECTOR
      every file and directory: "PATH/" or "PATH SIZE SHA-256 (16 digits)"
  exfat.py cat IMAGE FIRST_SECTOR PATH
  exfat.py date IMAGE FIRST_SECTOR PATH
      when it was last written: "YYYY-MM-DD HH:MM" (UTC, from the stored zone)
  exfat.py check IMAGE FIRST_SECTOR
      "exFAT ok", or the problems: the boot checksum, entry set checksums,
      name hashes, chains not matching their lengths, clusters used twice
      or not marked in the bitmap, marked but unused

Sectors of 512 bytes, clusters of 1 KiB (small images still have many
clusters, directories span several). The up-case table is the full one,
made from Python's Unicode data (each character's capital where it is a
single character of the Basic Multilingual Plane), stored compressed.
"""
import hashlib
import json
import struct
import sys

SECTOR = 512
CLUSTER = 1024
CLUSTER_SECTORS = CLUSTER // SECTOR
# 2024-05-17 13:45:30, local time two hours east of UTC (stored with it).
STAMP = ((2024 - 1980) << 25) | (5 << 21) | (17 << 16) | (13 << 11) | (45 << 5) | (30 // 2)
ZONE = 0x80 | 8                                 # valid, +8 quarter hours


def content(value):
    if isinstance(value, str):
        return value.encode()
    if "repeat" in value:
        return value["repeat"].encode() * value["times"]
    data = value["text"].encode()
    return data[:value["valid"]] + bytes(value["size"] - value["valid"])


def is_dir(value):
    return isinstance(value, dict) and "repeat" not in value and "text" not in value


def upcase_table():
    """The full table (65536 units) and its compressed form."""
    table = []
    for c in range(65536):
        u = chr(c).upper() if not 0xD800 <= c < 0xE000 else chr(c)
        table.append(ord(u) if len(u) == 1 and ord(u) < 0x10000 else c)
    out, i = [], 0
    while i < 65536:
        j = i
        while j < 65536 and table[j] == j:
            j += 1
        if j - i >= 4:                          # a run of units that are their own capitals
            out += [0xFFFF, j - i]
            i = j
        else:
            out.append(table[i])
            i += 1
    return table, struct.pack("<%dH" % len(out), *out)


UPCASE, UPCASE_DATA = upcase_table()


def checksum32(data):
    s = 0
    for b in data:
        s = (((s & 1) << 31) | (s >> 1)) + b & 0xFFFFFFFF
    return s


def set_checksum(entries):
    s = 0
    for i, b in enumerate(entries):
        if i in (2, 3):
            continue
        s = (((s & 1) << 15) | (s >> 1)) + b & 0xFFFF
    return s


def name_hash(name):
    s = 0
    data = name.encode("utf-16-le")
    for i in range(0, len(data), 2):
        u = UPCASE[struct.unpack_from("<H", data, i)[0]]
        for b in (u & 0xFF, u >> 8):
            s = (((s & 1) << 15) | (s >> 1)) + b & 0xFFFF
    return s


def boot_checksum(region):
    s = 0
    for i, b in enumerate(region[:11 * SECTOR]):
        if i in (106, 107, 112):
            continue
        s = (((s & 1) << 31) | (s >> 1)) + b & 0xFFFFFFFF
    return s


def entry_set(name, attributes, first, length, valid, contiguous):
    units = name.encode("utf-16-le")
    count = len(units) // 2
    names = (count + 14) // 15
    file = bytearray(32)
    file[0] = 0x85
    file[1] = 1 + names
    struct.pack_into("<H", file, 4, attributes)
    struct.pack_into("<III", file, 8, STAMP, STAMP, STAMP)
    file[22] = file[23] = file[24] = ZONE
    stream = bytearray(32)
    stream[0] = 0xC0
    stream[1] = 0x01 | (0x02 if contiguous else 0)          # allocation possible, NoFatChain
    stream[3] = count
    struct.pack_into("<H", stream, 4, name_hash(name))
    struct.pack_into("<Q", stream, 8, valid)
    struct.pack_into("<I", stream, 20, first)
    struct.pack_into("<Q", stream, 24, length)
    out = file + stream
    for k in range(names):
        e = bytearray(32)
        e[0] = 0xC1
        part = units[k * 30:(k + 1) * 30]
        e[2:2 + len(part)] = part
        out += e
    struct.pack_into("<H", out, 2, set_checksum(out))
    return bytes(out)


class Volume:
    def __init__(self, sectors):
        self.sectors = sectors
        self.fat_offset = 24
        clusters = (sectors - 24) // CLUSTER_SECTORS
        self.fat_length = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
        self.heap_offset = self.fat_offset + self.fat_length
        self.heap_offset += (-self.heap_offset) % CLUSTER_SECTORS
        self.clusters = (sectors - self.heap_offset) // CLUSTER_SECTORS
        self.fat = [0] * (self.clusters + 2)
        self.fat[0], self.fat[1] = 0xFFFFFFF8, 0xFFFFFFFF
        self.used = [False] * (self.clusters + 2)
        self.data = {}                          # cluster -> bytes
        self.next = 2

    def run(self, n):
        """n clusters in one run (and chained in the FAT too)."""
        first = self.next
        self.next += n
        for c in range(first, first + n):
            self.used[c] = True
        return list(range(first, first + n))

    def pieces(self, n):
        """n clusters spread out: every other one."""
        out = []
        while len(out) < n:
            out.append(self.next)
            self.used[self.next] = True
            self.next += 2
        return out

    def chain(self, clusters):
        for a, b in zip(clusters, clusters[1:]):
            self.fat[a] = b
        if clusters:
            self.fat[clusters[-1]] = 0xFFFFFFFF

    def store(self, clusters, data):
        for i, c in enumerate(clusters):
            self.data[c] = data[i * CLUSTER:(i + 1) * CLUSTER].ljust(CLUSTER, b"\0")


def place(vol, name, value):
    """Clusters for one object; returns its entry set."""
    contiguous = name.startswith("+")
    name = name.lstrip("+")
    if is_dir(value):
        body = b"".join(place(vol, k, v) for k, v in value.items())
        n = max(1, (len(body) + CLUSTER - 1) // CLUSTER)
        clusters = vol.run(n) if contiguous else vol.pieces(n)
        if not contiguous:
            vol.chain(clusters)
        vol.store(clusters, body)
        return entry_set(name, 0x10, clusters[0], n * CLUSTER, n * CLUSTER, contiguous)
    data = content(value)
    valid = value["valid"] if isinstance(value, dict) and "valid" in value else len(data)
    n = (len(data) + CLUSTER - 1) // CLUSTER
    if n == 0:
        return entry_set(name, 0x20, 0, 0, 0, False)
    clusters = vol.run(n) if contiguous else vol.pieces(n)
    if not contiguous:
        vol.chain(clusters)
    vol.store(clusters, data[:valid])
    return entry_set(name, 0x21 if name.startswith("readonly") else 0x20, clusters[0], len(data),
                     valid, contiguous)


def make(path, first, sectors, label, tree, mbr):
    vol = Volume(sectors)
    # The bitmap and the up-case table first, one run each.
    bitmap_clusters = vol.run((vol.clusters + 7) // 8 // CLUSTER + 1)
    upcase_clusters = vol.run((len(UPCASE_DATA) + CLUSTER - 1) // CLUSTER)
    vol.chain(bitmap_clusters)
    vol.chain(upcase_clusters)
    vol.store(upcase_clusters, UPCASE_DATA)
    root_clusters = vol.run(1)
    vol.next += 1                               # (the root's later clusters elsewhere)
    entries = b"".join(place(vol, k, v) for k, v in tree.items())
    label_entry = bytearray(32)
    label_entry[0] = 0x83
    units = label.encode("utf-16-le")
    label_entry[1] = len(units) // 2
    label_entry[2:2 + len(units)] = units
    bitmap_entry = bytearray(32)
    bitmap_entry[0] = 0x81
    struct.pack_into("<IQ", bitmap_entry, 20, bitmap_clusters[0], (vol.clusters + 7) // 8)
    upcase_entry = bytearray(32)
    upcase_entry[0] = 0x82
    struct.pack_into("<I", upcase_entry, 4, checksum32(UPCASE_DATA))
    struct.pack_into("<IQ", upcase_entry, 20, upcase_clusters[0], len(UPCASE_DATA))
    root = bytes(label_entry + bitmap_entry + upcase_entry) + entries
    n = max(1, (len(root) + 32 + CLUSTER - 1) // CLUSTER)
    root_clusters += vol.pieces(n - 1)
    vol.chain(root_clusters)
    vol.store(root_clusters, root)
    bitmap = bytearray((vol.clusters + 7) // 8)
    for c in range(2, vol.clusters + 2):
        if vol.used[c]:
            bitmap[(c - 2) // 8] |= 1 << ((c - 2) % 8)
    vol.store(bitmap_clusters, bytes(bitmap))

    boot = bytearray(SECTOR)
    boot[0:3] = b"\xEB\x76\x90"
    boot[3:11] = b"EXFAT   "
    struct.pack_into("<QQIIIIIIHH", boot, 64, first, sectors, vol.fat_offset, vol.fat_length,
                     vol.heap_offset, vol.clusters, root_clusters[0], 0x52495454, 0x0100, 0)
    boot[108], boot[109], boot[110], boot[111] = 9, 1, 1, 0x80
    boot[112] = sum(vol.used[2:]) * 100 // vol.clusters
    boot[510:512] = b"\x55\xAA"
    region = bytearray(boot)
    for _ in range(8):
        extended = bytearray(SECTOR)
        extended[508:512] = b"\x00\x00\x55\xAA"
        region += extended
    region += bytes(2 * SECTOR)                 # OEM parameters, reserved
    region += struct.pack("<I", boot_checksum(region)) * (SECTOR // 4)

    with open(path, "r+b") as f:
        base = first * SECTOR
        f.seek(base)
        f.write(region)
        f.write(region)                         # the backup copy
        f.seek(base + vol.fat_offset * SECTOR)
        f.write(struct.pack("<%dI" % len(vol.fat), *vol.fat))
        for c, data in vol.data.items():
            f.seek(base + (vol.heap_offset + (c - 2) * CLUSTER_SECTORS) * SECTOR)
            f.write(data)
        if mbr:
            m = bytearray(512)
            m[446 + 4] = 0x07
            struct.pack_into("<II", m, 446 + 8, first, sectors)
            m[510:512] = b"\x55\xAA"
            f.seek(0)
            f.write(m)


class Reader:
    def __init__(self, path, first):
        self.f = open(path, "rb")
        self.base = first * SECTOR
        b = self.sector(0)
        assert b[3:11] == b"EXFAT   ", "no exFAT there"
        self.sector_size = 1 << b[108]
        self.cluster_bytes = self.sector_size << b[109]
        (self.fat_offset, self.fat_length, self.heap_offset, self.clusters,
         self.root) = struct.unpack_from("<IIIII", b, 80)
        self.boot_ok = boot_checksum(self.read(0, 11 * SECTOR)) == struct.unpack_from(
            "<I", self.read(11 * SECTOR, 4))[0]

    def read(self, offset, n):
        self.f.seek(self.base + offset)
        return self.f.read(n)

    def sector(self, n):
        return self.read(n * SECTOR, SECTOR)

    def fat(self, c):
        return struct.unpack_from("<I", self.read(self.fat_offset * SECTOR + c * 4, 4))[0]

    def clusters_of(self, first, length, contiguous):
        if first == 0:
            return []
        n = (length + self.cluster_bytes - 1) // self.cluster_bytes if length is not None else None
        if contiguous:
            return list(range(first, first + n))
        out, c = [], first
        while c != 0xFFFFFFFF and (n is None or len(out) < n):
            out.append(c)
            if len(out) > self.clusters:
                raise ValueError("a loop in the FAT")
            c = self.fat(c)
        return out

    def cluster(self, c):
        return self.read((self.heap_offset * SECTOR) + (c - 2) * self.cluster_bytes,
                         self.cluster_bytes)

    def stream(self, first, length, contiguous):
        return b"".join(self.cluster(c) for c in self.clusters_of(first, length, contiguous))

    def entries(self, data):
        """(name, set bytes, is_dir, first, length, valid, contiguous) of a directory's data."""
        out, i = [], 0
        while i + 32 <= len(data):
            t = data[i]
            if t == 0:
                break
            if t != 0x85:
                i += 32
                continue
            count = data[i + 1]
            s = data[i:i + 32 * (count + 1)]
            stream = s[32:64]
            units = stream[3]
            name = b"".join(s[64 + 32 * k + 2:64 + 32 * k + 32] for k in range(count - 1))
            name = name[:units * 2].decode("utf-16-le")
            out.append((name, s, bool(struct.unpack_from("<H", s, 4)[0] & 0x10),
                        struct.unpack_from("<I", stream, 20)[0],
                        struct.unpack_from("<Q", stream, 24)[0],
                        struct.unpack_from("<Q", stream, 8)[0], bool(stream[1] & 2)))
            i += 32 * (count + 1)
        return out

    def root_data(self):
        return self.stream(self.root, None, False)

    def find(self, path):
        data, found = self.root_data(), None
        for part in [p for p in path.split("/") if p]:
            found = next(e for e in self.entries(data) if e[0].upper() == part.upper())
            data = self.stream(found[3], found[4], found[6]) if found[2] else None
        return found

    def content(self, e):
        data = self.stream(e[3], e[4], e[6])[:e[4]]
        return data[:e[5]] + bytes(e[4] - e[5])

    def walk(self, data=None, prefix=""):
        for e in self.entries(self.root_data() if data is None else data):
            if e[2]:
                yield prefix + e[0] + "/", e
                yield from self.walk(self.stream(e[3], e[4], e[6]), prefix + e[0] + "/")
            else:
                yield prefix + e[0], e

    def check(self):
        problems = []
        if not self.boot_ok:
            problems.append("the boot region's checksum is wrong")
        owner = {}
        root = self.clusters_of(self.root, None, False)
        special = []
        for e in self.root_entries_special():
            special.append(e)
        for c in root:
            owner.setdefault(c, []).append("/")
        for kind, first, length in special:
            for c in self.clusters_of(first, length, False):
                owner.setdefault(c, []).append(kind)
        for path, e in self.walk():
            name, s, d, first, length, valid, contiguous = e
            if struct.unpack_from("<H", s, 2)[0] != set_checksum(s):
                problems.append(f"{path}: entry set checksum wrong")
            if struct.unpack_from("<H", s, 36)[0] != name_hash(name):
                problems.append(f"{path}: name hash wrong")
            if valid > length:
                problems.append(f"{path}: valid data length beyond the data length")
            try:
                cs = self.clusters_of(first, length, contiguous)
            except ValueError as err:
                problems.append(f"{path}: {err}")
                continue
            if len(cs) * self.cluster_bytes < length:
                problems.append(f"{path}: its chain is shorter than its length")
            for c in cs:
                owner.setdefault(c, []).append(path)
        bitmap_first, bitmap_length = next((f, l) for k, f, l in special if k == "bitmap")
        bitmap = self.stream(bitmap_first, bitmap_length, False)
        for c in range(2, self.clusters + 2):
            marked = bool(bitmap[(c - 2) // 8] >> ((c - 2) % 8) & 1)
            if c in owner and len(owner[c]) > 1:
                problems.append(f"cluster {c} used twice: {', '.join(owner[c])}")
            if c in owner and not marked:
                problems.append(f"cluster {c} used ({owner[c][0]}) but free in the bitmap")
            if c not in owner and marked:
                problems.append(f"cluster {c} marked in the bitmap but unused")
        return problems

    def label(self):
        data, i = self.root_data(), 0
        while i + 32 <= len(data) and data[i]:
            if data[i] == 0x83:
                return data[i + 2:i + 2 + 2 * data[i + 1]].decode("utf-16-le")
            i += 32
        return ""

    def root_entries_special(self):
        data, out, i = self.root_data(), [], 0
        while i + 32 <= len(data) and data[i]:
            t = data[i]
            if t in (0x81, 0x82):
                out.append(("bitmap" if t == 0x81 else "upcase",
                            struct.unpack_from("<I", data, i + 20)[0],
                            struct.unpack_from("<Q", data, i + 24)[0]))
            i += 32
        return out


def ls(tree, path):
    node = tree
    for part in [p for p in path.split("/") if p]:
        node = next(v for k, v in node.items() if k.lstrip("+").lower() == part.lower())
    lines = []
    for name, value in node.items():
        name = name.lstrip("+")
        if is_dir(value):
            lines.append(f"     <dir>  {name}/")
        else:
            lines.append(f"{len(content(value)):10d}  {name}")
    return "\n".join(lines)


def main():
    a = sys.argv
    if len(a) >= 7 and a[1] == "make":
        with open(a[6]) as f:
            tree = json.load(f)
        make(a[2], int(a[3]), int(a[4]), a[5], tree, len(a) > 7 and a[7] == "mbr")
    elif len(a) == 4 and a[1] == "ls":
        with open(a[2]) as f:
            print(ls(json.load(f), a[3]))
    elif len(a) == 4 and a[1] == "tree":
        r = Reader(a[2], int(a[3]))
        for path, e in r.walk():
            if e[2]:
                print(path)
            else:
                print(path, e[4], hashlib.sha256(r.content(e)).hexdigest()[:16])
    elif len(a) == 5 and a[1] == "cat":
        r = Reader(a[2], int(a[3]))
        sys.stdout.buffer.write(r.content(r.find(a[4])))
    elif len(a) == 5 and a[1] == "date":
        s = Reader(a[2], int(a[3])).find(a[4])[1]
        t, zone = struct.unpack_from("<I", s, 12)[0], s[23]
        import datetime
        when = datetime.datetime(1980 + (t >> 25), t >> 21 & 15, t >> 16 & 31, t >> 11 & 31,
                                 t >> 5 & 63, (t & 31) * 2)
        if zone & 0x80:
            offset = zone & 0x7F
            when -= datetime.timedelta(minutes=15 * (offset - 128 if offset >= 64 else offset))
        print(when.strftime("%Y-%m-%d %H:%M"))
    elif len(a) == 4 and a[1] == "check":
        problems = Reader(a[2], int(a[3])).check()
        print("\n".join(problems) if problems else "exFAT ok")
        sys.exit(1 if problems else 0)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()

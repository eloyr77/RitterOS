#!/usr/bin/env python3
"""FAT12/16/32 on the host, for the tests (independent of kernel/fat.c).

Usage:
  fat.py make IMAGE FIRST_SECTOR SECTORS BITS LABEL TREE.json [mbr]
      make a FAT with BITS 12, 16 or 32 in the image's sectors FIRST..; TREE
      is a JSON object: names to file contents (strings, or {"repeat": TEXT,
      "times": N}) or to objects (directories). Names that are no plain 8.3
      upper-case name get a long name (VFAT) and a short alias (NAME~1.EXT),
      or, if they fit 8.3 in one case per part, the case flags Windows uses.
      Files are spread over the disk in pieces (every other cluster), so
      that reading must follow the FAT. "mbr": also write an MBR with one
      partition of the type for BITS.
  fat.py ls TREE.json PATH
      what RitterOS's ls prints for PATH ("" the root) in that tree
  fat.py tree IMAGE FIRST_SECTOR
      every file and directory of the FAT there: "PATH/" or "PATH SIZE
      SHA-256 (16 digits)"
  fat.py cat IMAGE FIRST_SECTOR PATH
  fat.py date IMAGE FIRST_SECTOR PATH
      when it was last written: "YYYY-MM-DD HH:MM" (as stored: UTC from RitterOS)
  fat.py check IMAGE FIRST_SECTOR
      "FAT ok", or the problems: FAT copies that differ, chains not
      matching their sizes, clusters in two chains, lost clusters, a wrong
      FSInfo free count

Sizes: a cluster of 1 KiB for FAT12, 2 KiB for FAT16, 512 bytes for FAT32,
so that small images have enough clusters for each kind.
"""
import json
import struct
import sys

SECTOR = 512
CLUSTER_SECTORS = {12: 2, 16: 4, 32: 1}
ROOT_ENTRIES = {12: 224, 16: 512, 32: 0}
EOC = {12: 0xFFF, 16: 0xFFFF, 32: 0x0FFFFFFF}
# 2024-05-17 13:45:30 as a DOS date and time.
DATE = ((2024 - 1980) << 9) | (5 << 5) | 17
TIME = (13 << 11) | (45 << 5) | (30 // 2)


def content(value):
    if isinstance(value, dict) and "repeat" in value:
        return (value["repeat"] * value["times"]).encode("utf-8")
    return value.encode("utf-8")


def is_dir(value):
    return isinstance(value, dict) and "repeat" not in value


SHORT_OK = set(b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'()-@^_`{}~")


def short_form(name, taken):
    """(11-byte short name, case flags, needs a long name)."""
    base, _, ext = name.rpartition(".") if "." in name[1:] else (name, "", "")
    plain = 0 < len(base) <= 8 and len(ext) <= 3 and "." not in base
    if plain:
        b, e = base.encode(), ext.encode()
        ok = all(c in SHORT_OK for c in b.upper() + e.upper())
        one_case = lambda s: s == s.upper() or s == s.lower()  # noqa: E731
        if ok and one_case(base) and one_case(ext):
            flags = (0x08 if base != base.upper() else 0) | (0x10 if ext != ext.upper() else 0)
            short = b.upper().ljust(8) + e.upper().ljust(3)
            if short not in taken:
                return short, flags, False
    # An alias: the first letters that may appear in a short name, ~N.
    clean = lambda s: bytes(c for c in s.upper().encode("ascii", "ignore") if c in SHORT_OK)  # noqa: E731
    b, e = clean(base.replace(" ", "")) or b"FILE", clean(ext)[:3]
    for n in range(1, 1000):
        tail = b"~%d" % n
        short = (b[:8 - len(tail)] + tail).ljust(8) + e.ljust(3)
        if short not in taken:
            return short, 0, True
    raise SystemExit("fat.py: too many similar names")


def checksum(short):
    s = 0
    for c in short:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s


def long_entries(name, short):
    units = list(struct.unpack(f"<{len(name.encode('utf-16-le')) // 2}H",
                               name.encode("utf-16-le")))
    units.append(0)
    while len(units) % 13:
        units.append(0xFFFF)
    parts = [units[i:i + 13] for i in range(0, len(units), 13)]
    out = []
    for order in range(len(parts), 0, -1):
        p = parts[order - 1]
        e = bytearray(32)
        e[0] = order | (0x40 if order == len(parts) else 0)
        struct.pack_into("<5H", e, 1, *p[0:5])
        e[11], e[13] = 0x0F, checksum(short)
        struct.pack_into("<6H", e, 14, *p[5:11])
        struct.pack_into("<2H", e, 28, *p[11:13])
        out.append(bytes(e))
    return out


def entry(short, attr, cluster, size, flags=0):
    return struct.pack("<11sBBBHHHHHHHI", short, attr, flags, 0, TIME, DATE, DATE,
                       cluster >> 16, TIME, DATE, cluster & 0xFFFF, size)


def make(path, first, sectors, bits, label, tree, mbr):
    spc = CLUSTER_SECTORS[bits]
    cluster = spc * SECTOR
    reserved = 32 if bits == 32 else 1
    root_sectors = ROOT_ENTRIES[bits] * 32 // SECTOR
    fat_sectors = 1
    while True:
        clusters = (sectors - reserved - 2 * fat_sectors - root_sectors) // spc
        need = -(-(clusters + 2) * bits // 8 // SECTOR) + 1
        if need <= fat_sectors:
            break
        fat_sectors = need
    kind = 12 if clusters < 4085 else 16 if clusters < 65525 else 32
    if kind != bits:
        raise SystemExit(f"fat.py: {sectors} sectors make FAT{kind}, not FAT{bits}")
    data_start = reserved + 2 * fat_sectors + root_sectors
    fat = [0] * (clusters + 2)
    fat[0], fat[1] = (0xFFFFFF8 & EOC[bits]), EOC[bits]
    image = {}                                      # sector -> bytes (sparse)
    cursor = [2]

    def allocate(count, spread):
        """count clusters, every other one if spread; their chain."""
        chain = []
        c = cursor[0]
        while len(chain) < count:
            if c >= clusters + 2:
                raise SystemExit("fat.py: full")
            if fat[c] == 0:
                chain.append(c)
                fat[c] = -1
                c += 2 if spread else 1
            else:
                c += 1
        if not spread:
            cursor[0] = c
        for a, b in zip(chain, chain[1:]):
            fat[a] = b
        fat[chain[-1]] = EOC[bits]
        return chain

    def put(chain, data):
        for i, c in enumerate(chain):
            part = data[i * cluster:(i + 1) * cluster].ljust(cluster, b"\0")
            for s in range(spc):
                image[data_start + (c - 2) * spc + s] = part[s * SECTOR:(s + 1) * SECTOR]

    def directory(node, own, parent):
        """The entries of a directory (own: its first cluster, 0 for a fixed root)."""
        entries, taken = [], set()
        if own is None:
            entries.append(struct.pack("<11sB20x", label.encode().ljust(11), 0x08))
        else:
            entries.append(entry(b".".ljust(11), 0x10, own, 0))
            entries.append(entry(b"..".ljust(11), 0x10, parent, 0))
        children = []
        for name, value in node.items():
            short, flags, needs_long = short_form(name, taken)
            taken.add(short)
            if is_dir(value):
                chain = allocate(1, False)
                children.append((value, chain))
                first_cluster, size, attr = chain[0], 0, 0x10
            else:
                data = content(value)
                chain = allocate(-(-len(data) // cluster), len(data) > 4 * cluster) if data else []
                if data:
                    put(chain, data)
                first_cluster, size, attr = (chain[0] if chain else 0), len(data), 0x20
                if name.startswith("readonly"):
                    attr |= 0x01
            if needs_long:
                entries.extend(long_entries(name, short))
            entries.append(entry(short, attr, first_cluster, size, flags))
        return entries, children

    def write_dir(node, chain, own, parent):
        entries, children = directory(node, own, parent)
        data = b"".join(entries)
        need = max(1, -(-len(data) // cluster))
        if len(chain) < need:                       # a directory longer than a cluster
            more = allocate(need - len(chain), False)
            fat[chain[-1]] = more[0]
            chain = chain + more
        put(chain, data)
        for value, child_chain in children:
            write_dir(value, child_chain, child_chain[0], own or 0)

    if bits == 32:
        root = allocate(1, False)
        write_dir(tree, root, None, 0)
        root_cluster = root[0]
    else:
        entries, children = directory(tree, None, 0)
        data = b"".join(entries).ljust(root_sectors * SECTOR, b"\0")
        for s in range(root_sectors):
            image[reserved + 2 * fat_sectors + s] = data[s * SECTOR:(s + 1) * SECTOR]
        for value, chain in children:
            write_dir(value, chain, chain[0], 0)
        root_cluster = 0

    # The FAT, twice.
    if bits == 12:
        raw = bytearray(-(-(len(fat)) * 3 // 2) + 1)
        for c, v in enumerate(fat):
            off = c * 3 // 2
            if c & 1:
                raw[off] |= (v << 4) & 0xF0
                raw[off + 1] = (v >> 4) & 0xFF
            else:
                raw[off] = v & 0xFF
                raw[off + 1] |= (v >> 8) & 0x0F
    else:
        raw = struct.pack(f"<{len(fat)}{'H' if bits == 16 else 'I'}", *fat)
    raw = bytes(raw).ljust(fat_sectors * SECTOR, b"\0")
    for copy in range(2):
        for s in range(fat_sectors):
            image[reserved + copy * fat_sectors + s] = raw[s * SECTOR:(s + 1) * SECTOR]

    boot = bytearray(SECTOR)
    boot[0:3] = b"\xeb\x58\x90"
    boot[3:11] = b"RTTRTEST"
    struct.pack_into("<HBHBHHBHHHII", boot, 11, SECTOR, spc, reserved, 2, ROOT_ENTRIES[bits],
                     sectors if sectors < 65536 else 0, 0xF8,
                     fat_sectors if bits != 32 else 0, 32, 64, first, sectors if sectors >= 65536 else 0)
    if bits == 32:
        struct.pack_into("<IHHIHH12sBBBI11s8s", boot, 36, fat_sectors, 0, 0, root_cluster, 1, 6,
                         bytes(12), 0x80, 0, 0x29, 0x12345678, label.encode().ljust(11),
                         b"FAT32   ")
    else:
        struct.pack_into("<BBBI11s8s", boot, 36, 0x80, 0, 0x29, 0x12345678,
                         label.encode().ljust(11), b"FAT%d   " % bits)
    boot[510:512] = b"\x55\xaa"
    image[0] = bytes(boot)
    if bits == 32:
        # FSInfo (sector 1): the free clusters and where to look for one;
        # a copy of the boot sector in sector 6.
        info = bytearray(SECTOR)
        free = sum(1 for v in fat[2:] if v == 0)
        struct.pack_into("<I", info, 0, 0x41615252)
        struct.pack_into("<IIII", info, 484, 0x61417272, free, cursor[0], 0)
        struct.pack_into("<I", info, 508, 0xAA550000)
        image[1] = bytes(info)
        image[6] = bytes(boot)

    with open(path, "r+b") as f:
        for s, data in image.items():
            f.seek((first + s) * SECTOR)
            f.write(data)
        if mbr:
            m = bytearray(SECTOR)
            kind_byte = {12: 0x01, 16: 0x0E, 32: 0x0C}[bits]
            m[446:462] = struct.pack("<B3sB3sII", 0, b"\x00\x02\x00", kind_byte, b"\xff\xff\xff",
                                     first, sectors)
            m[510:512] = b"\x55\xaa"
            f.seek(0)
            f.write(m)


class Reader:
    """A FAT read on the host: the boot sector, the FATs, directories with
    long names (their checksums checked), files by their chains."""

    def __init__(self, path, first):
        self.f = open(path, "rb")
        self.base = first * SECTOR
        self.bps = SECTOR                           # until the boot sector says
        b = self.sectors(0, 1)
        self.bps, self.spc, self.reserved, self.nfats, self.root_entries, total16 = \
            struct.unpack_from("<HBHBHH", b, 11)
        fat16, = struct.unpack_from("<H", b, 22)
        total32, fat32 = struct.unpack_from("<II", b, 32)
        self.total = total16 or total32
        self.fat_sectors = fat16 or fat32
        self.root_sectors = (self.root_entries * 32 + self.bps - 1) // self.bps
        self.data = self.reserved + self.nfats * self.fat_sectors + self.root_sectors
        self.clusters = (self.total - self.data) // self.spc
        self.bits = 12 if self.clusters < 4085 else 16 if self.clusters < 65525 else 32
        self.root_cluster = struct.unpack_from("<I", b, 44)[0] if self.bits == 32 else 0
        self.fsinfo = struct.unpack_from("<H", b, 48)[0] if self.bits == 32 else 0
        self.fats = [self.sectors(self.reserved + i * self.fat_sectors, self.fat_sectors)
                     for i in range(self.nfats)]

    def sectors(self, first, count):
        self.f.seek(self.base + first * self.bps)
        return self.f.read(count * self.bps)

    def entry(self, c, fat=0):
        raw = self.fats[fat]
        if self.bits == 32:
            return struct.unpack_from("<I", raw, c * 4)[0] & 0x0FFFFFFF
        if self.bits == 16:
            return struct.unpack_from("<H", raw, c * 2)[0]
        v = struct.unpack_from("<H", raw, c * 3 // 2)[0]
        return v >> 4 if c & 1 else v & 0xFFF

    def chain(self, first):
        end = {12: 0xFF8, 16: 0xFFF8, 32: 0x0FFFFFF8}[self.bits]
        out, c = [], first
        while 2 <= c < self.clusters + 2:
            if c in out:
                raise ValueError(f"a loop in the chain from {first}")
            out.append(c)
            c = self.entry(c)
        if first and c < end:
            raise ValueError(f"the chain from {first} ends in {c:#x}")
        return out

    def read_chain(self, first):
        return b"".join(self.sectors(self.data + (c - 2) * self.spc, self.spc)
                        for c in self.chain(first))

    def entries(self, first):
        """(name, short entry bytes) of a directory (0: FAT12/16's root)."""
        raw = self.sectors(self.data - self.root_sectors, self.root_sectors) \
            if first == 0 else self.read_chain(first)
        out, lfn, checksum = [], {}, None
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn = {}
                continue
            if e[11] & 0x3F == 0x0F:
                if e[0] & 0x40:
                    lfn, checksum = {}, e[13]
                units = struct.unpack_from("<5H", e, 1) + struct.unpack_from("<6H", e, 14) + \
                    struct.unpack_from("<2H", e, 28)
                lfn[e[0] & 0x1F] = units
                continue
            if e[11] & 0x08 or e[:2] in (b". ", b".."):
                lfn = {}
                continue
            name = None
            if lfn and checksum == _checksum(e[:11]):
                units = [u for k in sorted(lfn) for u in lfn[k]]
                units = units[:units.index(0)] if 0 in units else [u for u in units if u != 0xFFFF]
                name = struct.pack(f"<{len(units)}H", *units).decode("utf-16-le")
            if name is None:
                base, ext = e[:8].decode("latin-1").rstrip(), e[8:11].decode("latin-1").rstrip()
                base = base.lower() if e[12] & 0x08 else base
                ext = ext.lower() if e[12] & 0x10 else ext
                name = base + ("." + ext if ext else "")
            out.append((name, e))
            lfn = {}
        return out

    def first_of(self, e):
        return (struct.unpack_from("<H", e, 20)[0] << 16 if self.bits == 32 else 0) | \
            struct.unpack_from("<H", e, 26)[0]

    def walk(self, first=None, prefix=""):
        """(path, entry) of everything, directories before what they hold."""
        first = self.root_cluster if first is None else first
        for name, e in self.entries(first):
            yield prefix + "/" + name, e
            if e[11] & 0x10:
                yield from self.walk(self.first_of(e), prefix + "/" + name)

    def content(self, e):
        size = struct.unpack_from("<I", e, 28)[0]
        return self.read_chain(self.first_of(e))[:size] if size else b""

    def find(self, path):
        first, found = self.root_cluster, None
        for part in [p for p in path.split("/") if p]:
            found = next(e for name, e in self.entries(first) if name.lower() == part.lower())
            first = self.first_of(found)
        return found

    def check(self):
        """Problems: FAT copies that differ, chains too short or too long
        for their size, clusters in two chains, lost clusters, a wrong
        FSInfo free count."""
        problems, owner = [], {}
        for i in range(1, self.nfats):
            if self.fats[i] != self.fats[0]:
                problems.append(f"FAT copy {i + 1} differs from the first")
        roots = [("/", self.root_cluster)] if self.bits == 32 else []
        for path, e in self.walk():
            roots.append((path, self.first_of(e)))
            if not e[11] & 0x10:
                size = struct.unpack_from("<I", e, 28)[0]
                n = len(self.chain(self.first_of(e))) if self.first_of(e) else 0
                if n != -(-size // (self.spc * self.bps)):
                    problems.append(f"{path}: {n} clusters for {size} bytes")
        for path, first in roots:
            for c in (self.chain(first) if first else []):
                if c in owner:
                    problems.append(f"cluster {c} in {owner[c]} and {path}")
                owner[c] = path
        used = [c for c in range(2, self.clusters + 2) if self.entry(c) != 0]
        lost = [c for c in used if c not in owner]
        if lost:
            problems.append(f"{len(lost)} lost cluster(s), the first {lost[0]}")
        if self.fsinfo:
            info = self.sectors(self.fsinfo, 1)
            free, = struct.unpack_from("<I", info, 488)
            real = self.clusters - len(used)
            if free != 0xFFFFFFFF and free != real:
                problems.append(f"FSInfo says {free} free clusters, there are {real}")
        return problems


def _checksum(short):
    return checksum(short)


def ls(tree, path):
    node = tree
    for part in [p for p in path.split("/") if p]:
        node = next(v for k, v in node.items() if k.lower() == part.lower())
    lines = []
    for name, value in node.items():
        if is_dir(value):
            lines.append(f"     <dir>  {name}/")
        else:
            lines.append(f"{len(content(value)):10d}  {name}")
    return "\n".join(lines)


def main():
    if len(sys.argv) >= 8 and sys.argv[1] == "make":
        with open(sys.argv[7]) as f:
            tree = json.load(f)
        make(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), sys.argv[6], tree,
             len(sys.argv) > 8 and sys.argv[8] == "mbr")
    elif len(sys.argv) == 4 and sys.argv[1] == "ls":
        with open(sys.argv[2]) as f:
            print(ls(json.load(f), sys.argv[3]))
    elif len(sys.argv) == 4 and sys.argv[1] == "tree":
        import hashlib
        r = Reader(sys.argv[2], int(sys.argv[3]))
        for path, e in r.walk():
            if e[11] & 0x10:
                print(path + "/")
            else:
                data = r.content(e)
                print(f"{path} {len(data)} {hashlib.sha256(data).hexdigest()[:16]}"
                      f"{' read-only' if e[11] & 1 else ''}")
    elif len(sys.argv) == 5 and sys.argv[1] == "cat":
        r = Reader(sys.argv[2], int(sys.argv[3]))
        sys.stdout.buffer.write(r.content(r.find(sys.argv[4])))
    elif len(sys.argv) == 5 and sys.argv[1] == "date":
        e = Reader(sys.argv[2], int(sys.argv[3])).find(sys.argv[4])
        date, time = struct.unpack_from("<H", e, 24)[0], struct.unpack_from("<H", e, 22)[0]
        print(f"{1980 + (date >> 9)}-{date >> 5 & 15:02d}-{date & 31:02d} "
              f"{time >> 11:02d}:{time >> 5 & 63:02d}")
    elif len(sys.argv) == 4 and sys.argv[1] == "check":
        problems = Reader(sys.argv[2], int(sys.argv[3])).check()
        print("\n".join(problems) if problems else "FAT ok")
        sys.exit(1 if problems else 0)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()

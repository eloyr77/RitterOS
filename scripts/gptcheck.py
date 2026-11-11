#!/usr/bin/env python3
"""Check a disk image's GPT the way a careful partitioning tool would, on
the host and independently of RitterOS (scripts/test.sh runs it on images
that the program disks changed).

Usage: gptcheck.py IMAGE

Prints "GPT ok" and one line per partition ("#N FIRST-LAST TYPE 'NAME'"),
or the problems found: the protective MBR, both headers (signature,
checksum, where each says it and the other one are), both entry arrays
(checksum, the same content), the partitions (inside the usable blocks,
not overlapping). Exit status 1 if there are problems.
"""
import struct
import sys
import uuid
import zlib

SECTOR = 512


def header(f, lba, problems, which):
    f.seek(lba * SECTOR)
    raw = bytearray(f.read(SECTOR))
    if raw[:8] != b"EFI PART":
        problems.append(f"{which} header: no signature at LBA {lba}")
        return None
    size, crc = struct.unpack_from("<II", raw, 12)
    check = bytearray(raw[:size])
    check[16:20] = bytes(4)
    if zlib.crc32(check) != crc:
        problems.append(f"{which} header: bad checksum")
    fields = struct.unpack_from("<QQQQ16sQIII", raw, 24)
    names = ("my_lba", "alternate_lba", "first_usable", "last_usable", "disk_guid",
             "entries_lba", "entry_count", "entry_size", "entries_crc")
    return dict(zip(names, fields))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    problems = []
    with open(sys.argv[1], "rb") as f:
        f.seek(0, 2)
        sectors = f.tell() // SECTOR
        f.seek(0)
        mbr = f.read(SECTOR)
        kind, first, count = mbr[446 + 4], *struct.unpack_from("<II", mbr, 446 + 8)
        if mbr[510:512] != b"\x55\xaa" or kind != 0xEE or first != 1 or \
                count != min(sectors - 1, 0xFFFFFFFF):
            problems.append("protective MBR: not one 0xEE partition over the whole disk")
        primary = header(f, 1, problems, "primary")
        backup = header(f, sectors - 1, problems, "backup")
        arrays = []
        for which, h, lba in (("primary", primary, 1), ("backup", backup, sectors - 1)):
            if not h:
                continue
            other = sectors - 1 if lba == 1 else 1
            if h["my_lba"] != lba or h["alternate_lba"] != other:
                problems.append(f"{which} header: says it is at {h['my_lba']}, the other "
                                f"at {h['alternate_lba']}")
            f.seek(h["entries_lba"] * SECTOR)
            data = f.read(h["entry_count"] * h["entry_size"])
            if zlib.crc32(data) != h["entries_crc"]:
                problems.append(f"{which} entries: bad checksum")
            arrays.append(data)
        if primary and backup:
            for key in ("first_usable", "last_usable", "disk_guid", "entry_count", "entry_size"):
                if primary[key] != backup[key]:
                    problems.append(f"headers differ in {key}")
            if len(arrays) == 2 and arrays[0] != arrays[1]:
                problems.append("the two entry arrays differ")
            last_array = sectors - 1 - (len(arrays[-1]) + SECTOR - 1) // SECTOR
            if primary["entries_lba"] + (len(arrays[0]) + SECTOR - 1) // SECTOR > \
                    primary["first_usable"] or primary["last_usable"] >= last_array:
                problems.append("the usable blocks overlap the entry arrays")
        lines, taken = [], []
        if primary and arrays:
            size = primary["entry_size"]
            for i in range(primary["entry_count"]):
                e = arrays[0][i * size:(i + 1) * size]
                if e[:16] == bytes(16):
                    continue
                lo, hi = struct.unpack_from("<QQ", e, 32)
                name = e[56:128].decode("utf-16-le").split("\0")[0]
                if lo < primary["first_usable"] or hi > primary["last_usable"] or hi < lo:
                    problems.append(f"#{i + 1}: blocks {lo}-{hi} outside the usable ones")
                for a, b, n in taken:
                    if lo <= b and a <= hi:
                        problems.append(f"#{i + 1} overlaps #{n}")
                taken.append((lo, hi, i + 1))
                type_text = str(uuid.UUID(bytes_le=bytes(e[:16]))).upper()
                lines.append(f"#{i + 1} {lo}-{hi} {type_text} '{name}'")
    if problems:
        print("\n".join(problems))
        sys.exit(1)
    print("GPT ok")
    print("\n".join(lines))


if __name__ == "__main__":
    main()

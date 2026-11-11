#!/usr/bin/env python3
"""What the program disks should print for these disk images, found on the
host independently of the kernel (scripts/test.sh compares the two).

Usage: expect_disks.py NAME,KIND,MODEL,IMAGE[,system][,mounted=PATH][,pN=PATH][,whole=PATH]...
  NAME    the kernel's name for the disk (nvme0n1, usbdisk0, ...)
  KIND    NVMe, SATA or USB
  MODEL   the disk's own name ('QEMU NVMe Ctrl')
  IMAGE   its image file
  system  it holds the system partition (the RitterOS one, mounted at /;
          its EFI system partition at /boot)
  mounted=PATH  its RitterOS partition is mounted at PATH
  pN=PATH       partition N is mounted at PATH
  whole=PATH    the file system on the whole disk is mounted at PATH

The partition table, the file systems and the output format mirror
kernel/gpt.c, kernel/devices.c and user/bin/disks.c.
"""
import os
import struct
import sys
import uuid
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exfat  # noqa: E402
import ritterfs  # noqa: E402

SECTOR = 512
PROBE = 3 * 4096

GPT_TYPES = {
    "C12A7328-F81F-11D2-BA4B-00A0C93EC93B": "EFI system partition",
    "B5CA1B3E-6278-48F0-BBEF-87907326A8B3": "RitterOS",
    "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7": "Microsoft basic data",
    "0FC63DAF-8483-4772-8E79-3D69D8477DE4": "Linux filesystem",
    "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F": "Linux swap",
    "E3C9E316-0B5C-4DB8-817D-F92DF00215AE": "Microsoft reserved",
    "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC": "Windows recovery",
    "7C3457EF-0000-11AA-AA11-00306543ECAC": "Apple APFS",
    "48465300-0000-11AA-AA11-00306543ECAC": "Apple HFS+",
}
MBR_TYPES = {0x01: "FAT12", 0x04: "FAT16", 0x06: "FAT16", 0x07: "NTFS or exFAT", 0x0B: "FAT32",
             0x0C: "FAT32 (LBA)", 0x0E: "FAT16 (LBA)", 0x05: "extended",
             0x0F: "extended (LBA)", 0x82: "Linux swap", 0x83: "Linux",
             0xEF: "EFI system partition"}


def size_text(n):
    units = ["bytes", "KiB", "MiB", "GiB", "TiB"]
    whole, tenths, u = n, 0, 0
    while whole >= 1024 and u < 4:
        tenths = (whole % 1024) * 10 // 1024
        whole //= 1024
        u += 1
    return f"{n} bytes" if u == 0 else f"{whole}.{tenths} {units[u]}"


def label(data):
    text = "".join(chr(c) if 0x20 <= c < 0x7F else "?" for c in data.split(b"\0")[0])
    return text.rstrip(" ")


def content(f, first, sectors):
    """(file system, label, generation, empty) of the area."""
    f.seek(first * SECTOR)
    b = f.read(min(PROBE, sectors * SECTOR))
    empty = not any(b)
    sb, _ = ritterfs.mount(ritterfs.Partition(f, first, sectors)) if len(b) >= PROBE else (None, None)
    fs, name, gen = "", "", 0
    boot = len(b) >= 512 and b[510:512] == b"\x55\xaa"
    if sb:
        fs, name, gen = "RitterFS", label(sb.label), sb.generation
    elif boot and b[82:90] == b"FAT32   ":
        fs, name = "FAT32", label(b[71:82])
    elif boot and b[54:58] == b"FAT1":
        fs, name = b[54:59].decode(), label(b[43:54])
    elif boot and b[3:11] == b"EXFAT   ":
        fs = "exFAT"                            # (its label is in the root directory:
        name = exfat.Reader(f.name, first).label()  # shown once it is mounted)
    elif boot and b[3:11] == b"NTFS    ":
        fs = "NTFS"
    elif len(b) >= 2048 and b[1080:1082] == b"\x53\xef":
        fs, name = "ext", label(b[1144:1160])
    elif len(b) >= 36 and b[32:36] == b"NXSB":
        fs = "APFS"
    if name == "NO NAME":
        name = ""
    return fs, name, gen, empty


def describe(fs, name, gen, empty, mounted):
    if not fs:
        text = "empty" if empty else "no file system found"
    else:
        if fs == "exFAT" and not mounted:
            name = ""
        text = fs + (f" '{name}'" if name else "")
        if fs == "RitterFS":
            text += f", generation {gen}"
    return text + (f", mounted at {mounted}" if mounted else "")


def gpt_copy(f, lba):
    """(header fields, entries) of the GPT copy whose header is at lba, or None."""
    f.seek(lba * SECTOR)
    h = f.read(92)
    if h[:8] != b"EFI PART":
        return None
    size, crc = struct.unpack_from("<II", h, 12)
    f.seek(lba * SECTOR)
    raw = bytearray(f.read(size))
    raw[16:20] = b"\0\0\0\0"
    if zlib.crc32(raw) != crc:
        return None
    last_usable, = struct.unpack_from("<Q", h, 48)
    entries_lba, count, entry_size, entries_crc = struct.unpack_from("<QIII", h, 72)
    f.seek(entries_lba * SECTOR)
    entries = f.read(count * entry_size)
    if zlib.crc32(entries) != entries_crc:
        return None
    return last_usable, count, entry_size, entries


def gpt_partitions(f, disk_sectors):
    """The partitions of the GPT at sector 1, or else of its copy in the last sector."""
    copy = gpt_copy(f, 1) or gpt_copy(f, disk_sectors - 1)
    if not copy:
        return None
    last_usable, count, entry_size, entries = copy
    parts = []
    for i in range(count):
        e = entries[i * entry_size:(i + 1) * entry_size]
        if e[:16] == bytes(16):
            continue
        first, last = struct.unpack_from("<QQ", e, 32)
        if last < first or last > last_usable:
            continue
        name = e[56:128].decode("utf-16-le").split("\0")[0]
        type_text = str(uuid.UUID(bytes_le=bytes(e[:16]))).upper()
        parts.append((i + 1, first, last - first + 1, type_text, name, 0))
    return parts


def mbr_partitions(f, disk_sectors):
    f.seek(0)
    s = f.read(SECTOR)
    if s[510:512] != b"\x55\xaa":
        return None
    if s[0] in (0xEB, 0xE9) and (s[3:11] in (b"NTFS    ", b"EXFAT   ") or s[54:57] == b"FAT" or
                                 s[82:90] == b"FAT32   "):
        return None                                 # a file system's boot sector
    parts = []
    for i in range(4):
        e = s[446 + 16 * i:462 + 16 * i]
        status, kind = e[0], e[4]
        first, count = struct.unpack_from("<II", e, 8)
        if status not in (0, 0x80):
            return None
        if kind == 0 or count == 0:
            continue
        if kind == 0xEE or first == 0 or first + count > disk_sectors:
            return None
        parts.append((i + 1, first, count, None, "", kind))
    return parts


def disk_lines(name, kind, model, path, system, mounted_at=None, mounts=None):
    mounts = mounts or {}
    lines = []
    sectors = os.path.getsize(path) // SECTOR
    with open(path, "rb") as f:
        table, parts = "GPT", gpt_partitions(f, sectors)
        if parts is None:
            table, parts = "MBR", mbr_partitions(f, sectors)
        tail = "; the system disk" if system else ""
        size = size_text(sectors * SECTOR)
        if parts is None:
            what = describe(*content(f, 0, sectors), mounts.get("whole"))
            lines.append(f"{name:<14}{size:>10}  {kind} '{model}', no partition table: {what}{tail}")
            return lines
        plural = "" if len(parts) == 1 else "s"
        lines.append(f"{name:<14}{size:>10}  {kind} '{model}', {table}, {len(parts)} partition{plural}{tail}")
        p = "p" if name[-1].isdigit() else ""
        for number, first, count, type_text, pname, mbr in parts:
            if mbr:
                type_name = MBR_TYPES.get(mbr, "")
                type_shown = f"type 0x{mbr:02X} ({type_name or '?'})"
            else:
                type_name = GPT_TYPES.get(type_text, "")
                type_shown = type_name or type_text
            shown_name = f" '{pname}'" if pname and pname != type_name else ""
            ritteros = type_text == "B5CA1B3E-6278-48F0-BBEF-87907326A8B3"
            esp = type_text == "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
            mounted = ("/" if system else mounted_at) if ritteros else \
                "/boot" if system and esp else None
            mounted = mounts.get(f"p{number}", mounted)
            what = describe(*content(f, first, count), mounted)
            lines.append(f"  {name + p + str(number):<12}{size_text(count * SECTOR):>10}  "
                         f"#{number} {type_shown}{shown_name}: {what}")
    return lines


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for arg in sys.argv[1:]:
        fields = arg.split(",")
        extras = fields[4:]
        mounted, mounts = None, {}
        for extra in extras:
            if extra.startswith("mounted="):
                mounted = extra[len("mounted="):]
            elif "=" in extra:
                key, value = extra.split("=", 1)
                mounts[key] = value
        print("\n".join(disk_lines(fields[0], fields[1], fields[2], fields[3],
                                   "system" in extras, mounted, mounts)))


if __name__ == "__main__":
    main()

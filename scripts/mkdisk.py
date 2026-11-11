#!/usr/bin/env python3
"""Build the RitterOS disk image: a GPT disk with a FAT32 EFI system partition
and a RitterFS partition.

Usage: mkdisk.py [--arch ARCH] [--size MIB] IMAGE ESP_DIR SYSTEM_DIR

--arch (55a, one disk per architecture): only that architecture's part of
SYSTEM_DIR (/system/ARCH, and the shared fonts, certs, firmware) and only
its bootloader of ESP_DIR go on the disk; without it, both.

--size (68j): the image's size in MiB (1024 without it; at least 128). The
sticks are 256: written to a bigger stick, RitterOS grows its RitterFS to
the stick's end at the first start (disks grow).

Layout (512-byte sectors):
    LBA 0             protective MBR
    LBA 1             primary GPT header
    LBA 2-33          primary partition entry array (128 entries x 128 bytes)
    1 MiB             partition 1: EFI system partition, FAT32, filled from ESP_DIR
                      (the bootloaders and the kernel command line)
    after the ESP     partition 2: RitterOS data, RitterFS; SYSTEM_DIR (the
                      kernels) is installed as /system
    last 33 sectors   backup partition entries + backup GPT header

If IMAGE already exists with the right size, the partition tables and the ESP
are rewritten, but the RitterFS in partition 2 is kept: whatever the OS stored
there survives. Files of SYSTEM_DIR that differ from their copies in /system
are installed with one copy-on-write commit (scripts/ritterfs.py, install).
"""
import hashlib
import os
import struct
import sys
import time
import uuid
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ritterfs  # noqa: E402

SECTOR = 512
DISK_SIZE = 1024 * 1024 * 1024          # 1 GiB
ALIGN = 2048                            # partitions start on 1 MiB boundaries
ESP_START = 2048
ESP_SECTORS = 64 * 1024 * 1024 // SECTOR

GPT_ENTRIES = 128
GPT_ENTRY_SIZE = 128
GPT_ENTRY_SECTORS = GPT_ENTRIES * GPT_ENTRY_SIZE // SECTOR   # 32

EFI_SYSTEM_TYPE = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")   # from the UEFI spec
RITTEROS_TYPE = uuid.UUID("b5ca1b3e-6278-48f0-bbef-87907326a8b3")     # our own choice
ESP_LABEL = b"EFI        "            # the EFI system partition's volume label (11 bytes)

# Fixed GUIDs so that rebuilding produces the same disk identity.
NAMESPACE = uuid.UUID("6ba7b811-9dad-11d1-80b4-00c04fd430c8")
DISK_GUID = uuid.uuid5(NAMESPACE, "ritteros/disk")
ESP_GUID = uuid.uuid5(NAMESPACE, "ritteros/esp")
DATA_GUID = uuid.uuid5(NAMESPACE, "ritteros/data")
RITTERFS_UUID = uuid.uuid5(NAMESPACE, "ritteros/ritterfs")


# --- initial RitterFS content ------------------------------------------------------

README = (b"Welcome to RitterFS, the file system of RitterOS.\n\n"
          b"Everything here is protected by checksums, metadata is stored twice,\n"
          b"and nothing is ever overwritten in place. /test holds files that the\n"
          b"kernel verifies at boot against /test/manifest.txt.\n")


def pseudo_random(seed, size):
    out = bytearray()
    counter = 0
    while len(out) < size:
        out.extend(hashlib.sha256(f"{seed}:{counter}".encode()).digest())
        counter += 1
    return bytes(out[:size])


# The architectures and their bootloaders' names (55a: one disk each).
ARCHES = {"x86_64": "BOOTX64.EFI", "aarch64": "BOOTAA64.EFI"}


def system_files(system_dir, arch=None):
    """The files of SYSTEM_DIR as they belong in /system on RitterFS (with
    arch, the other architectures' folders left out)."""
    files = {}
    for parent, dirs, names in os.walk(system_dir):
        if arch and os.path.samefile(parent, system_dir):
            dirs[:] = [d for d in dirs if d not in ARCHES or d == arch]
        dirs.sort()
        for name in sorted(names):
            if name == ".DS_Store" or name.startswith("._"):
                continue                            # (the Mac's; 41g-6: other dot files belong)
            path = os.path.join(parent, name)
            rel = os.path.relpath(path, system_dir).replace(os.sep, "/")
            with open(path, "rb") as f:
                files["/system/" + rel] = f.read()
    files["/system/config/README"] = CONFIG_README
    return files


# Programs that run as root whoever starts them (setuid, 50b): root's, 4755.
SETUID_PROGRAMS = ("su", "sudo")


def layout(arch):
    """(57b, the user's) The Unix folders of a disk for one architecture:
    links into /system, and /tmp for everyone (sticky). osinstall makes the
    same."""
    if not arch:
        return {}
    return {"/bin": ritterfs.Link(f"/system/{arch}/bin"),
            "/usr/bin": ritterfs.Link(f"/system/{arch}/bin"),
            "/lib": ritterfs.Link(f"/system/{arch}/lib"),
            "/etc": ritterfs.Link("/system/config"),
            "/tmp": None}


def setuid_modes(system):
    """The setuid programs among the system's files, path -> mode; and the
    readme, which is no program (52g: a text file the shell would run as a
    script once it is executable)."""
    modes = {path: 0o4755 for path in system
             if path.split("/")[-2:-1] == ["bin"] and path.split("/")[-1] in SETUID_PROGRAMS}
    modes["/readme.txt"] = 0o644
    if "/tmp" in system:
        modes["/tmp"] = 0o1777                      # (57b) everyone's, sticky
    return modes


# /system/config: the system's settings (written by programs, never by
# mkdisk, which would put them back at every build); the README explains it.
CONFIG_README = b"""\
The system's settings, one file per topic, plain text:

  users       the users: name:uid:gid:home per line (useradd writes it)
  passwords   their passwords' hashes, root only (passwd writes it)
  network/    one file per network card (eth0, eth1, ...): address, gateway,
              dns (IPv6: address6, gateway6, dns6); net up applies them and
              makes a missing one (DHCP; IPv6 from the network's router)
  network/names  the cards found, NAME = MAC: each keeps its name (the elf
              netnamesd adds new ones, with their settings files: DHCP;
              net names, net forget NAME)
  routing     forward = on: pass packets on between the cards; masquerade =
              the private cards; default = the default route's cards in
              order (net up applies them)
  elves/      one file per elf, a program kept running: command, start (elf)
  ssh_host_ed25519_key  sshd's host key, root only (sshd makes it)
  sudo        users = anna, bob: who may run commands as root with sudo
  services    service names for programs (getaddrinfo): name = port
  cpu         clock = performance: the processor's clock (AMD with CPPC),
              read at the start
  displays    hdmi0 = 1920x1080@60: a mode for an output (displays lists
              them and their monitors' modes), set at the start
  power       the power button, the lid, the screen going off (the kernel
              reads it at the start)
  swap        on = yes: swap on at every start (swap on, swap off)
  windows     the window system's for everyone (optional: nothing writes
              it); each user's own are ~/.config/windows, which win
  wifi/       radio (country, powersave) and networks/, a file per saved
              network (wifi join)
  time        zone = -05:00: the local time's offset from UTC

Files of settings are written as "key = value" lines; a line starting
with # is a comment. Each file holds its settings at their defaults and
comments saying what else they can be: whoever reads it (the kernel at the
start, a program when it looks) writes it when it is missing, so a deleted
file comes back. See the manual (docs/manual.md), "Special directories".
"""


def initial_files():
    """The files mkdisk puts on a new RitterFS: a readme, and test files that
    cover every way RitterFS stores content."""
    lorem = b"".join(b"%04d Lorem ipsum dolor sit amet, consectetur adipiscing elit.\n" % i
                     for i in range(100))                       # 6200 bytes: 2 blocks
    sparse = bytearray(2 * 1024 * 1024)                          # 512 blocks, 2 with data
    sparse[0:4096] = pseudo_random("sparse-0", 4096)
    sparse[300 * 4096:301 * 4096] = pseudo_random("sparse-300", 4096)
    tests = {
        "/test/hello.txt": b"Hello from RitterFS!\n",          # inline
        "/test/empty.txt": b"",
        "/test/block.bin": pseudo_random("block", 4096),        # exactly one block
        "/test/docs/notes/deep/nested.txt": b"Found me, four directories down.\n",
        "/test/text/lorem.txt": lorem,                           # one indirect level
        "/test/data/medium.bin": pseudo_random("medium", 300 * 1024),
        "/test/data/large.bin": pseudo_random("large", 1024 * 1024 + 123),   # two levels
        "/test/data/sparse.bin": bytes(sparse),                  # holes
        "/test/Gr\u00fc\u00dfe \u2603.txt": "Gr\u00fc\u00dfe aus RitterFS \u2603\n".encode(),
    }
    for i in range(300):                                         # a two-block directory
        tests[f"/test/many/file-{i:03d}"] = b"file %d\n" % i
    manifest = "".join(f"{ritterfs.crc32c(data):08X} {len(data)} {path}\n"
                       for path, data in sorted(tests.items()))
    # (37e-1b, the user's choice) No settings files: whoever reads one
    # writes it with its defaults when it is missing (the first start).
    files = {"/readme.txt": README, "/test/manifest.txt": manifest.encode()}
    files.update(tests)
    return files

# --- GPT ----------------------------------------------------------------------

def protective_mbr(total_sectors):
    """MBR with one partition of type 0xEE covering the disk: 'this is GPT'."""
    mbr = bytearray(SECTOR)
    size = min(total_sectors - 1, 0xFFFFFFFF)
    mbr[446:462] = struct.pack("<B3sB3sII", 0x00, b"\x00\x02\x00", 0xEE,
                               b"\xff\xff\xff", 1, size)
    mbr[510:512] = b"\x55\xaa"
    return bytes(mbr)


def partition_entry(type_guid, unique_guid, first, last, name):
    entry = struct.pack("<16s16sQQQ72s", type_guid.bytes_le, unique_guid.bytes_le,
                        first, last, 0, name.encode("utf-16-le"))
    assert len(entry) == GPT_ENTRY_SIZE
    return entry


def gpt_header(current, backup, first_usable, last_usable, entries_lba, entries_crc):
    def pack(crc):
        return struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x00010000, 92, crc, 0,
                           current, backup, first_usable, last_usable,
                           DISK_GUID.bytes_le, entries_lba, GPT_ENTRIES,
                           GPT_ENTRY_SIZE, entries_crc)
    header = pack(zlib.crc32(pack(0)))
    return header + bytes(SECTOR - len(header))


def partition_layout(total_sectors):
    last_usable = total_sectors - 1 - 1 - GPT_ENTRY_SECTORS
    esp = (ESP_START, ESP_START + ESP_SECTORS - 1)
    data_first = esp[1] + 1
    data_last = (last_usable + 1) // ALIGN * ALIGN - 1
    return 2 + GPT_ENTRY_SECTORS, last_usable, esp, (data_first, data_last)


def write_gpt(f, total_sectors):
    first_usable, last_usable, esp, data = partition_layout(total_sectors)
    entries = bytearray(GPT_ENTRIES * GPT_ENTRY_SIZE)
    entries[0:128] = partition_entry(EFI_SYSTEM_TYPE, ESP_GUID, esp[0], esp[1],
                                     "EFI system partition")
    entries[128:256] = partition_entry(RITTEROS_TYPE, DATA_GUID, data[0], data[1],
                                       "RitterOS")
    entries_crc = zlib.crc32(entries)
    last = total_sectors - 1
    backup_entries_lba = last - GPT_ENTRY_SECTORS

    f.seek(0)
    f.write(protective_mbr(total_sectors))
    f.write(gpt_header(1, last, first_usable, last_usable, 2, entries_crc))
    f.write(entries)
    f.seek(backup_entries_lba * SECTOR)
    f.write(entries)
    f.write(gpt_header(last, 1, first_usable, last_usable, backup_entries_lba, entries_crc))
    return esp, data


# --- FAT32 --------------------------------------------------------------------

RESERVED_SECTORS = 32
NUM_FATS = 2
SECTORS_PER_CLUSTER = 1
CLUSTER = SECTOR * SECTORS_PER_CLUSTER
EOC = 0x0FFFFFFF                         # end of a cluster chain

ATTR_VOLUME_ID = 0x08
ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE = 0x20


def short_name(name):
    """Return the 11-byte 8.3 name and the NT case flags for a file name."""
    base, _, ext = name.partition(".")
    if not base or len(base) > 8 or len(ext) > 3 or "." in ext:
        sys.exit(f"mkdisk: '{name}' does not fit the 8.3 format (long names unsupported)")
    allowed = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-~!#$%&'()@^`{}")
    if not set(base.upper() + ext.upper()) <= allowed:
        sys.exit(f"mkdisk: '{name}' contains characters not allowed in 8.3 names")
    flags = 0
    for part, flag in ((base, 0x08), (ext, 0x10)):     # lowercase base / extension
        if part != part.upper():
            if part != part.lower():
                sys.exit(f"mkdisk: '{name}' mixes upper and lower case (needs a long name)")
            flags |= flag
    return (base.upper().ljust(8) + ext.upper().ljust(3)).encode("ascii"), flags


def fat_timestamp(t):
    lt = time.localtime(t)
    date = ((max(lt.tm_year, 1980) - 1980) << 9) | (lt.tm_mon << 5) | lt.tm_mday
    clock = (lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec // 2)
    return date, clock


def dir_entry(name11, attr, cluster, size, mtime, nt_flags=0):
    date, clock = fat_timestamp(mtime)
    return struct.pack("<11sBBBHHHHHHHI", name11, attr, nt_flags, 0, clock, date, date,
                       cluster >> 16, clock, date, cluster & 0xFFFF, size)


class Node:
    def __init__(self, path, name, skip=()):
        self.path, self.name = path, name
        self.is_dir = os.path.isdir(path)
        self.mtime = os.path.getmtime(path)
        self.children = []
        self.cluster = 0
        if self.is_dir:
            self.children = [Node(os.path.join(path, n), n, skip)
                             for n in sorted(os.listdir(path))
                             if not n.startswith(".") and n.upper() not in skip]

    def byte_size(self, is_root):
        if not self.is_dir:
            return os.path.getsize(self.path)
        # root: volume label + children; others: "." and ".." + children
        return 32 * ((1 if is_root else 2) + len(self.children))


def make_fat32(esp_dir, total_sectors, hidden_sectors, skip=()):
    # FAT size: each data cluster needs 4 bytes in each FAT copy.
    fat_sectors = 1
    while True:
        data_sectors = total_sectors - RESERVED_SECTORS - NUM_FATS * fat_sectors
        clusters = data_sectors // SECTORS_PER_CLUSTER
        needed = -(-(clusters + 2) * 4 // SECTOR)
        if needed <= fat_sectors:
            break
        fat_sectors = needed
    if clusters < 65525:
        sys.exit("mkdisk: partition too small for FAT32")

    image = bytearray(total_sectors * SECTOR)
    fat = [0] * (clusters + 2)
    fat[0], fat[1] = 0x0FFFFFF8, EOC
    next_free = 2

    def allocate(nbytes):
        nonlocal next_free
        count = max(1, -(-nbytes // CLUSTER))
        first = next_free
        if first + count > clusters + 2:
            sys.exit("mkdisk: ESP is full")
        for c in range(first, first + count):
            fat[c] = c + 1
        fat[first + count - 1] = EOC
        next_free += count
        return first

    def cluster_offset(c):
        return (RESERVED_SECTORS + NUM_FATS * fat_sectors + (c - 2) * SECTORS_PER_CLUSTER) * SECTOR

    # Allocate the root first so that it starts at cluster 2, then everything else.
    root = Node(esp_dir, "", skip)
    queue = [(root, True)]
    while queue:
        node, is_root = queue.pop(0)
        size = node.byte_size(is_root)
        if node.is_dir or size > 0:
            node.cluster = allocate(size)
        queue.extend((child, False) for child in node.children)

    # Write directory contents and file data.
    def write_tree(node, parent_cluster, is_root):
        if not node.is_dir:
            with open(node.path, "rb") as src:
                data = src.read()
            off = cluster_offset(node.cluster)
            image[off:off + len(data)] = data
            return
        entries = []
        if is_root:
            entries.append(dir_entry(ESP_LABEL, ATTR_VOLUME_ID, 0, 0, node.mtime))
        else:
            entries.append(dir_entry(b".          ", ATTR_DIRECTORY, node.cluster, 0, node.mtime))
            entries.append(dir_entry(b"..         ", ATTR_DIRECTORY, parent_cluster, 0, node.mtime))
        for child in node.children:
            name11, flags = short_name(child.name)
            if child.is_dir:
                entries.append(dir_entry(name11, ATTR_DIRECTORY, child.cluster, 0,
                                         child.mtime, flags))
            else:
                entries.append(dir_entry(name11, ATTR_ARCHIVE, child.cluster,
                                         os.path.getsize(child.path), child.mtime, flags))
        data = b"".join(entries)
        off = cluster_offset(node.cluster)
        image[off:off + len(data)] = data
        for child in node.children:
            # ".." of a directory in the root refers to cluster 0 by convention.
            write_tree(child, 0 if is_root else node.cluster, False)

    write_tree(root, 0, True)

    # Boot sector with the BIOS parameter block (BPB).
    boot = bytearray(SECTOR)
    boot[0:3] = b"\xeb\x58\x90"                       # jmp over the BPB (x86 boot code)
    boot[3:11] = b"RITTEROS"                          # OEM name
    struct.pack_into("<HBHBHHBHHHII", boot, 11,
                     SECTOR, SECTORS_PER_CLUSTER, RESERVED_SECTORS, NUM_FATS,
                     0, 0,                            # root entries, 16-bit total: 0 on FAT32
                     0xF8,                            # media: fixed disk
                     0, 32, 64,                       # 16-bit FAT size, sectors/track, heads
                     hidden_sectors, total_sectors)
    struct.pack_into("<IHHIHH12sBBBI11s8s", boot, 36,
                     fat_sectors, 0, 0,               # FAT size, flags, version
                     2,                               # root directory cluster
                     1, 6,                            # FSInfo sector, backup boot sector
                     bytes(12), 0x80, 0, 0x29,        # reserved, drive number, -, signature
                     0x52495454,                      # volume serial number
                     ESP_LABEL, b"FAT32   ")
    boot[510:512] = b"\x55\xaa"

    # FSInfo: hints about free space for the next writer.
    used = next_free - 2
    fsinfo = bytearray(SECTOR)
    struct.pack_into("<I", fsinfo, 0, 0x41615252)
    struct.pack_into("<III", fsinfo, 484, 0x61417272, clusters - used, next_free)
    struct.pack_into("<I", fsinfo, 508, 0xAA550000)

    image[0:SECTOR] = boot
    image[SECTOR:2 * SECTOR] = fsinfo
    image[6 * SECTOR:7 * SECTOR] = boot              # backup copies
    image[7 * SECTOR:8 * SECTOR] = fsinfo

    fat_bytes = struct.pack(f"<{len(fat)}I", *fat)
    for i in range(NUM_FATS):
        off = (RESERVED_SECTORS + i * fat_sectors) * SECTOR
        image[off:off + len(fat_bytes)] = fat_bytes

    return bytes(image), used, clusters


def main():
    args = sys.argv[1:]
    arch = None
    disk_size = DISK_SIZE
    while args[:1] in (["--arch"], ["--size"]) and len(args) > 1:
        if args[0] == "--arch":
            arch = args[1]
            if arch not in ARCHES:
                sys.exit(f"mkdisk: no such architecture: {arch}")
        else:
            if not args[1].isdigit() or int(args[1]) < 128:
                sys.exit(f"mkdisk: not a size in MiB (at least 128): {args[1]}")
            disk_size = int(args[1]) * 1024 * 1024
        args = args[2:]
    if len(args) != 3:
        sys.exit(__doc__)
    image_path, esp_dir, system_dir = args
    system = {**system_files(system_dir, arch), **layout(arch)}
    skip = {name for a, name in ARCHES.items() if arch and a != arch}
    total_sectors = disk_size // SECTOR

    fresh = not os.path.exists(image_path) or os.path.getsize(image_path) != disk_size
    with open(image_path, "w+b" if fresh else "r+b") as f:
        if fresh:
            f.truncate(disk_size)                     # sparse: unwritten parts use no space
        (esp_first, esp_last), (data_first, data_last) = write_gpt(f, total_sectors)
        fs, used, clusters = make_fat32(esp_dir, esp_last - esp_first + 1, esp_first, skip)
        # A fresh file reads as zeros already: skip zero blocks to keep it sparse.
        # An existing image is fully rewritten so that no old data survives.
        block = 64 * 1024
        for off in range(0, len(fs), block):
            chunk = fs[off:off + block]
            if fresh and not any(chunk):
                continue
            f.seek(esp_first * SECTOR + off)
            f.write(chunk)

        # Partition 2: format RitterFS unless it already holds one, so that
        # rebuilding never erases what RitterOS stored there.
        part = ritterfs.Partition(f, data_first, data_last - data_first + 1)
        if ritterfs.mount(part)[0]:
            written = ritterfs.install(part, system, modes=setuid_modes(system))
            sb, _ = ritterfs.mount(part)
            fs_state = (f"RitterFS kept, installed {', '.join(written)} (generation {sb.generation})"
                        if written else "RitterFS kept, /system up to date")
        else:
            # Test images (scripts/test.sh) may have the elf logd switched
            # off: most tests compare the disk's exact state, which logd's
            # writes would change. Never for build/ritteros-<arch>.img (the sticks).
            extra = {}
            if os.environ.get("RITTEROS_TEST_LOGD_OFF"):
                extra["/system/config/elves/logd"] = (
                    b"# The logs' keeper, switched off for this test image\n"
                    b"command = logd\nstart = off\n")
            # (50h) And netnamesd, which net up sets up on every machine: its
            # start would print a line into most tests' sessions.
            if os.environ.get("RITTEROS_TEST_NETNAMESD_OFF"):
                extra["/system/config/elves/netnamesd"] = (
                    b"# The network cards' names, switched off for this test image\n"
                    b"command = netnamesd\nstart = off\n")
            ritterfs.format(part, b"RitterOS", RITTERFS_UUID.bytes_le,
                            files={**initial_files(), **system, **extra},
                            modes=setuid_modes(system))
            fs_state = (f"RitterFS formatted with "
                        f"{sum(p.startswith('/system/') for p in system)} file(s) in /system")

    print(f"{image_path}: {'created' if fresh else 'updated'}, GPT, "
          f"ESP {ESP_SECTORS * SECTOR // 2**20} MiB FAT32 "
          f"({used} of {clusters} clusters used), {fs_state}")


if __name__ == "__main__":
    main()

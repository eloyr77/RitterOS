#!/usr/bin/env python3
"""RitterFS tools for the host: format, install files, check (see docs/ritterfs.md).

Usage:
  ritterfs.py check  IMAGE FIRST_LBA SECTORS
  ritterfs.py format IMAGE FIRST_LBA SECTORS [LABEL]
  ritterfs.py ls     IMAGE FIRST_LBA SECTORS [PATH]
  ritterfs.py cat    IMAGE FIRST_LBA SECTORS PATH
  ritterfs.py put    IMAGE FIRST_LBA SECTORS PATH LOCAL_FILE

IMAGE is a disk image, FIRST_LBA and SECTORS (512 bytes each) locate the
partition inside it. mkdisk.py uses format() and install(); test.sh uses
check, ls, cat and put.
"""
import os
import struct
import sys
import time
import uuid

BLOCK = 4096
SECTOR = 512
FIRST_ALLOCATABLE = 16
SLOTS = (1, 2)                          # superblock slots A and B
MAGIC = b"RITTERFS"
VERSION = 3                             # 1: no snapshots, 2: no attributes; all read
POINTERS_PER_BLOCK = BLOCK // 32
INLINE_MAX = 160

TYPE_FREE, TYPE_FILE, TYPE_DIR, TYPE_TABLE, TYPE_SYMLINK = 0, 1, 2, 3, 4
DATA_TYPES = (TYPE_FILE, TYPE_SYMLINK)  # one copy of their blocks; all else is metadata
LINKS_MAX = 40                          # symbolic links followed in one path, as the kernel


class Link:
    """A symbolic link for format() and install(): the path it names (57a)."""
    def __init__(self, target):
        self.target = target.encode("utf-8") if isinstance(target, str) else target
FLAG_INLINE = 1
FLAG_FIXED = 2                      # (35a) the swap file: data written in place, no checksums
OBJ_TABLE, OBJ_BITMAP, OBJ_ROOT = 1, 2, 3

POINTER = struct.Struct("<QQIIQ")                       # 32 bytes
INODE = struct.Struct("<HHIQQQQQBIIH5s32s160s")         # 256 bytes
MODE_SET, MODE_BITS = 0x8000, 0o5777    # mode 0: root's, 0755; 0o4000 setuid, 0o1000 sticky (57b)
SUPER = struct.Struct("<8sIIQQQQ16s64sQQQQQQ80s256s")   # 512 bytes, then padding + CRC
SNAPSHOT = struct.Struct("<64sQQ176s256s")              # 512 bytes
ATTR = struct.Struct("<QHBBHH")                         # 16 bytes + key + value
ATTR_KEY_MAX, ATTR_VALUE_MAX, ATTR_ALIGN = 63, 1024, 16
DIRENT = struct.Struct("<QHBB")                         # 12 bytes + name

# --- CRC-32C -------------------------------------------------------------------

def _crc32c_table():
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
        table.append(c)
    return table

_TABLE = _crc32c_table()


def crc32c(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc = _TABLE[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


assert crc32c(b"123456789") == 0xE3069283, "CRC-32C self-test"

# --- structures ----------------------------------------------------------------

class Pointer:
    def __init__(self, address=0, address2=0, checksum=0, birth=0):
        self.address, self.address2, self.checksum, self.birth = address, address2, checksum, birth

    def pack(self):
        return POINTER.pack(self.address, self.address2, self.checksum, 0, self.birth)

    @classmethod
    def unpack(cls, data):
        a, a2, c, _, b = POINTER.unpack(data)
        return cls(a, a2, c, b)


class Inode:
    def __init__(self, type=TYPE_FREE, flags=0, link_count=0, size=0, created=0, modified=0,
                 changed=0, generation=0, height=0, root=None, inline=b"", uid=0, gid=0, mode=0):
        self.type, self.flags, self.link_count, self.size = type, flags, link_count, size
        self.created, self.modified, self.changed = created, modified, changed
        self.generation, self.height = generation, height
        self.root = root or Pointer()
        self.inline = inline
        self.uid, self.gid, self.mode = uid, gid, mode     # owner and permissions

    def pack(self):
        return INODE.pack(self.type, self.flags, self.link_count, self.size, self.created,
                          self.modified, self.changed, self.generation, self.height, self.uid,
                          self.gid, self.mode, bytes(5), self.root.pack(),
                          self.inline.ljust(INLINE_MAX, b"\0"))

    @classmethod
    def unpack(cls, data):
        (t, f, links, size, cr, mo, ch, gen, h, uid, gid, mode, _, root,
         inline) = INODE.unpack(data)
        return cls(t, f, links, size, cr, mo, ch, gen, h, Pointer.unpack(root), inline, uid, gid,
                   mode)


class Superblock:
    def __init__(self, block_count=0, generation=0, commit_time=0, created_time=0,
                 fs_uuid=bytes(16), label=b"", used_blocks=0, next_object_id=0, table=None,
                 pinned_generation=0, deferred_blocks=0, snapshot_object=0, attribute_object=0):
        self.block_count, self.generation = block_count, generation
        self.commit_time, self.created_time = commit_time, created_time
        self.uuid, self.label = fs_uuid, label
        self.used_blocks, self.next_object_id = used_blocks, next_object_id
        self.table = table or Inode()
        # Version 2: snapshots (see docs/ritterfs.md).
        self.pinned_generation, self.deferred_blocks = pinned_generation, deferred_blocks
        self.snapshot_object = snapshot_object
        self.attribute_object = attribute_object    # version 3: attributes

    def pack(self):
        # Like the kernel: the oldest version that describes the file system.
        version = 3 if self.attribute_object else \
            2 if self.pinned_generation or self.snapshot_object else 1
        head = SUPER.pack(MAGIC, version, BLOCK, self.block_count, self.generation,
                          self.commit_time, self.created_time, self.uuid,
                          self.label.ljust(64, b"\0"), self.used_blocks, self.next_object_id,
                          self.pinned_generation, self.deferred_blocks, self.snapshot_object,
                          self.attribute_object, bytes(80), self.table.pack())
        body = head.ljust(BLOCK - 4, b"\0")
        return body + struct.pack("<I", crc32c(body))

    @classmethod
    def unpack(cls, data):
        """A Superblock, or None if the block is not a valid one."""
        if crc32c(data[:BLOCK - 4]) != struct.unpack_from("<I", data, BLOCK - 4)[0]:
            return None
        (magic, version, block_size, count, gen, commit, created, fs_uuid, label, used,
         next_id, pinned, deferred, snapshots, attributes, _, table) = SUPER.unpack_from(data)
        if magic != MAGIC or not 1 <= version <= VERSION or block_size != BLOCK:
            return None
        return cls(count, gen, commit, created, fs_uuid, label.rstrip(b"\0"), used, next_id,
                   Inode.unpack(table), pinned, deferred, snapshots, attributes)


def parse_attributes(data, errors):
    """[(object id, key, value)] of an attribute table's content (free
    records skipped); problems are appended to errors."""
    out, pos = [], 0
    while pos + ATTR.size <= len(data):
        oid, size, key_length, _, value_length, _ = ATTR.unpack_from(data, pos)
        if size < ATTR.size or size % ATTR_ALIGN or pos + size > len(data) or \
                pos // BLOCK != (pos + size - 1) // BLOCK:
            break
        if oid:
            key = data[pos + 16:pos + 16 + key_length]
            if ATTR.size + key_length + value_length > size or not 1 <= key_length <= ATTR_KEY_MAX \
                    or b"=" in key or b"\0" in key or value_length > ATTR_VALUE_MAX:
                errors.append(f"attribute record at byte {pos} is malformed")
            else:
                out.append((oid, key.decode("utf-8", "replace"),
                            data[pos + 16 + key_length:pos + 16 + key_length + value_length]))
        pos += size
    if pos != len(data):
        errors.append(f"attribute table is malformed at byte {pos} (of {len(data)})")
    return out


def parse_snapshots(data):
    """[(name, generation, created, table inode)] of a snapshot table's
    content; entries with generation 0 are free."""
    out = []
    for pos in range(0, len(data) - SNAPSHOT.size + 1, SNAPSHOT.size):
        name, gen, created, _, table = SNAPSHOT.unpack_from(data, pos)
        if gen:
            out.append((name.rstrip(b"\0").decode("utf-8", "replace"), gen, created,
                        Inode.unpack(table)))
    return out

# --- partition access ------------------------------------------------------------

class Partition:
    def __init__(self, f, first_lba, sectors):
        self.f = f
        self.offset = first_lba * SECTOR
        self.block_count = sectors * SECTOR // BLOCK

    def read(self, block):
        self.f.seek(self.offset + block * BLOCK)
        return self.f.read(BLOCK).ljust(BLOCK, b"\0")

    def write(self, block, data):
        assert len(data) == BLOCK and FIRST_ALLOCATABLE <= block < self.block_count
        self.f.seek(self.offset + block * BLOCK)
        self.f.write(data)

    def write_super(self, slot, data):
        self.f.seek(self.offset + slot * BLOCK)
        self.f.write(data)

    def flush(self):
        """Make everything written so far durable (the host's "flush the device")."""
        self.f.flush()
        os.fsync(self.f.fileno())

# --- writing (formatting) ----------------------------------------------------------

def tree_height(blocks):
    height, capacity = 0, 1
    while capacity < blocks:
        height, capacity = height + 1, capacity * POINTERS_PER_BLOCK
    return height


class Allocator:
    """First copies from the start, second copies from the middle of the
    partition, so that the two copies of metadata are far apart."""

    def __init__(self, block_count):
        self.used = set(range(FIRST_ALLOCATABLE))
        self.cursors = [FIRST_ALLOCATABLE, block_count // 2]
        self.block_count = block_count

    def one(self, which=0):
        while self.cursors[which] in self.used:
            self.cursors[which] += 1
        if self.cursors[which] >= self.block_count:
            sys.exit("ritterfs: partition full")
        self.used.add(self.cursors[which])
        return self.cursors[which]

    def pair(self, meta):
        return (self.one(0), self.one(1) if meta else 0)


def plan_tree(alloc, present, meta):
    """Allocate the blocks of a content tree. present[i] tells whether data
    block i exists (False = hole, files only). Returns (height, plan)."""
    height = tree_height(max(1, len(present)))

    def node(level, start):
        if start >= len(present):
            return None
        if level == 0:
            return ("data", start, alloc.pair(meta)) if present[start] else None
        span = POINTERS_PER_BLOCK ** (level - 1)
        children = [node(level - 1, start + i * span) for i in range(POINTERS_PER_BLOCK)]
        if not any(children):
            return None
        return ("indirect", children, alloc.pair(True))

    return height, node(height, 0)


def write_tree(part, plan, blocks, birth):
    """Write a planned tree bottom-up; returns the root Pointer."""
    if plan is None:
        return Pointer()
    if plan[0] == "data":
        data = blocks[plan[1]]
    else:
        data = b"".join(write_tree(part, c, blocks, birth).pack() for c in plan[1])
        data = data.ljust(BLOCK, b"\0")
    a1, a2 = plan[2]
    part.write(a1, data)
    if a2:
        part.write(a2, data)
    return Pointer(a1, a2, crc32c(data), birth)


def split_blocks(content):
    return [content[i:i + BLOCK].ljust(BLOCK, b"\0") for i in range(0, len(content), BLOCK)]


def encode_directory(entries):
    """Directory content for [(name bytes, object id, type)]. An entry that
    would cross a block boundary starts the next block; the previous entry is
    padded to the end of its block."""
    out = bytearray()
    last = None
    for name, oid, typ in entries:
        size = (DIRENT.size + len(name) + 7) // 8 * 8
        room = BLOCK - len(out) % BLOCK
        if size > room:
            old_size = struct.unpack_from("<H", out, last + 8)[0]
            struct.pack_into("<H", out, last + 8, old_size + room)
            out.extend(bytes(room))
        last = len(out)
        out.extend(DIRENT.pack(oid, size, len(name), typ) + name)
        out.extend(bytes(size - DIRENT.size - len(name)))
    return bytes(out)


def format(part, label=b"RitterOS", fs_uuid=None, now=None, files=None, modes=None):
    """Create a RitterFS. files maps paths ("/a/b.txt") to their content
    (bytes) or to None for an empty directory; parent directories are created
    as needed. All-zero blocks of files become holes. modes (path -> bits,
    e.g. 0o4755 for a setuid program) records those files' permissions."""
    now = now if now is not None else time.time_ns()
    fs_uuid = fs_uuid or uuid.uuid4().bytes_le
    gen = 1
    alloc = Allocator(part.block_count)

    # The directory tree: every directory is a dict of name -> subtree or bytes.
    tree = {}
    for path, content in (files or {}).items():
        parts = [p for p in path.split("/") if p]
        node = tree
        for d in parts[:-1]:
            node = node.setdefault(d, {})
        node[parts[-1]] = {} if content is None else content

    # Object IDs in breadth-first order; directory contents follow from them.
    objects = []                                    # (oid, type, content, path)
    queue, next_id = [(OBJ_ROOT, tree, "")], OBJ_ROOT + 1
    while queue:
        oid, node, at = queue.pop(0)
        entries = []
        for name in sorted(node):
            child = node[name]
            typ = TYPE_DIR if isinstance(child, dict) else \
                TYPE_SYMLINK if isinstance(child, Link) else TYPE_FILE
            entries.append((name.encode("utf-8"), next_id, typ))
            if typ == TYPE_DIR:
                queue.append((next_id, child, at + "/" + name))
            else:
                objects.append((next_id, typ, child.target if typ == TYPE_SYMLINK else child,
                                at + "/" + name))
            next_id += 1
        objects.append((oid, TYPE_DIR, encode_directory(entries), at or "/"))

    # Allocate every tree first: the bitmap must list all blocks, its own too.
    plans = {}
    for oid, typ, content, _ in objects:
        if len(content) > INLINE_MAX:
            blocks = split_blocks(content)
            present = [any(b) for b in blocks] if typ == TYPE_FILE else [True] * len(blocks)
            plans[oid] = (blocks, plan_tree(alloc, present, typ not in DATA_TYPES))
    table_bytes = next_id * INODE.size
    table_height, table_plan = plan_tree(alloc, [True] * len(split_blocks(bytes(table_bytes))), True)
    bitmap_bytes = (part.block_count + 7) // 8
    bitmap_height, bitmap_plan = plan_tree(alloc, [True] * len(split_blocks(bytes(bitmap_bytes))),
                                           True)

    bitmap = bytearray(bitmap_bytes)
    for b in alloc.used:
        bitmap[b // 8] |= 1 << (b % 8)
    table = bytearray(table_bytes)

    def put(oid, inode):
        table[oid * INODE.size:(oid + 1) * INODE.size] = inode.pack()

    put(OBJ_BITMAP, Inode(TYPE_TABLE, 0, 1, bitmap_bytes, now, now, now, gen, bitmap_height,
                          write_tree(part, bitmap_plan, split_blocks(bytes(bitmap)), gen)))
    for oid, typ, content, at in objects:
        inode = Inode(typ, 0, 1, len(content), now, now, now, gen)
        if modes and at in modes:
            inode.mode = MODE_SET | modes[at]
        if oid in plans:
            blocks, (height, plan) = plans[oid]
            inode.height, inode.root = height, write_tree(part, plan, blocks, gen)
        else:
            inode.flags, inode.inline = FLAG_INLINE, content
        put(oid, inode)
    table_inode = Inode(TYPE_TABLE, 0, 1, table_bytes, now, now, now, gen, table_height,
                        write_tree(part, table_plan, split_blocks(bytes(table)), gen))

    sb = Superblock(part.block_count, gen, now, now, fs_uuid, label, len(alloc.used),
                    next_id, table_inode)
    part.write_super(SLOTS[gen % 2], sb.pack())
    part.write_super(SLOTS[(gen + 1) % 2], bytes(BLOCK))      # the other slot: empty
    return sb

# --- reading and checking ------------------------------------------------------------

class CheckError(Exception):
    pass


def mount(part):
    """The newest valid superblock and its slot, or (None, None)."""
    best = (None, None)
    for slot in SLOTS:
        sb = Superblock.unpack(part.read(slot))
        if sb and sb.block_count <= part.block_count and (not best[0] or
                                                          sb.generation > best[0].generation):
            best = (sb, slot)
    return best


class Checker:
    """Walks every tree, verifies checksums and both copies, and compares the
    blocks in use with the bitmap."""

    def __init__(self, part, sb):
        self.part, self.sb = part, sb
        self.owner = {}                     # block -> what uses it, in any state
        self.state = {}                     # the same for the state being walked: a block
                                            # used twice in one state is an error, by two
                                            # not, nor by copies (born up to the pin)
        self.walking_live = True
        self.shared = 0                     # blocks the live state uses more than once
        self.newest = sb.generation         # newest birth allowed in the state being walked
        self.errors = []

    def claim(self, block, what, birth):
        pinned = self.sb.pinned_generation
        if not FIRST_ALLOCATABLE <= block < self.sb.block_count:
            self.errors.append(f"{what}: block {block} outside the allocatable area")
        elif block in self.state:
            if not pinned or birth > pinned:
                self.errors.append(f"{what}: block {block} also used by {self.state[block]}")
            elif self.walking_live:
                self.shared += 1
        else:
            self.state[block] = what
            self.owner.setdefault(block, what)

    def read(self, ptr, meta, what):
        if meta and not ptr.address2:
            self.errors.append(f"{what}: metadata block {ptr.address} has no second copy")
        copies = [ptr.address] + ([ptr.address2] if ptr.address2 else [])
        good = None
        for a in copies:
            self.claim(a, what, ptr.birth)
            data = self.part.read(a)
            if crc32c(data) == ptr.checksum:
                good = good or data
            else:
                self.errors.append(f"{what}: copy at block {a} has a bad checksum")
        if good is None:
            if meta:
                raise CheckError(f"{what}: no valid copy")
            self.errors.append(f"{what}: no valid copy (file data lost)")
            return bytes(BLOCK)
        if ptr.birth > self.newest:
            self.errors.append(f"{what}: born in generation {ptr.birth} > {self.newest}")
        return good

    def content(self, inode, what):
        """Read an object's whole content."""
        if inode.flags & FLAG_INLINE:
            return inode.inline[:inode.size]
        meta = inode.type not in DATA_TYPES
        blocks = (inode.size + BLOCK - 1) // BLOCK
        if tree_height(max(1, blocks)) != inode.height:
            self.errors.append(f"{what}: height {inode.height} does not fit {blocks} blocks")
        # A fixed file (35a): whole blocks, no holes; its data blocks are
        # written in place, so they are claimed but not verified (and read
        # as zeros here).
        fixed = bool(inode.flags & FLAG_FIXED)
        if fixed and (inode.type != TYPE_FILE or inode.size % BLOCK or not inode.size):
            self.errors.append(f"{what}: fixed, but not a file of whole blocks")
        out = bytearray()

        def walk(ptr, level, start):
            if start >= blocks:
                return
            span = POINTERS_PER_BLOCK ** level
            if ptr.address == 0:
                if meta or fixed:
                    self.errors.append(f"{what}: hole in {'a fixed file' if fixed else 'metadata'}"
                                       f" at block {start}")
                out.extend(bytes(min(span, blocks - start) * BLOCK))
                return
            if fixed and level == 0:
                self.claim(ptr.address, f"{what} block {start}", ptr.birth)
                if ptr.address2:
                    self.errors.append(f"{what} block {start}: fixed data with a second copy")
                out.extend(bytes(BLOCK))
                return
            data = self.read(ptr, meta or level > 0, f"{what} block {start}" if level == 0
                             else f"{what} indirect")
            if level == 0:
                out.extend(data)
                return
            for i in range(POINTERS_PER_BLOCK):
                walk(Pointer.unpack(data[i * 32:(i + 1) * 32]), level - 1,
                     start + i * POINTERS_PER_BLOCK ** (level - 1))

        walk(inode.root, inode.height, 0)
        return bytes(out[:inode.size])

    def run(self):
        sb = self.sb
        table = self.content(sb.table, "object table")
        inodes = {}
        for oid in range(len(table) // INODE.size):
            ino = Inode.unpack(table[oid * INODE.size:(oid + 1) * INODE.size])
            if oid >= sb.next_object_id and ino.type != TYPE_FREE:
                self.errors.append(f"object {oid} is in use but >= next_object_id")
            if ino.type != TYPE_FREE:
                inodes[oid] = ino
                if ino.mode and (not ino.mode & MODE_SET or ino.mode & ~(MODE_SET | MODE_BITS)):
                    self.errors.append(f"object {oid}: invalid mode {ino.mode:#o}")
                if ino.type == TYPE_SYMLINK and not 1 <= ino.size <= 255:
                    self.errors.append(f"object {oid}: a symbolic link of {ino.size} bytes")
                if ino.type == TYPE_DIR and oid != OBJ_ROOT and ino.link_count != 1:
                    self.errors.append(f"directory {oid}: link count {ino.link_count} "
                                       f"(a directory has one name)")
        for oid, name in ((OBJ_BITMAP, "bitmap"), (OBJ_ROOT, "root directory")):
            if oid not in inodes:
                raise CheckError(f"{name} (object {oid}) missing")

        contents = {oid: self.content(ino, f"object {oid}") for oid, ino in inodes.items()}
        for oid, ino in inodes.items():
            if ino.type == TYPE_SYMLINK and b"\0" in contents[oid]:
                self.errors.append(f"object {oid}: a symbolic link with a zero byte")

        # Directories: valid entries, and link counts that match them.
        refs = {}
        for oid, ino in inodes.items():
            if ino.type != TYPE_DIR:
                continue
            names = set()
            for name, child, typ in parse_directory(contents[oid], self.errors, oid):
                if name in names:
                    self.errors.append(f"directory {oid}: duplicate name {name!r}")
                names.add(name)
                if child not in inodes or child in (OBJ_BITMAP, OBJ_ROOT):
                    self.errors.append(f"directory {oid}: {name!r} refers to object {child}")
                elif inodes[child].type != typ:
                    self.errors.append(f"directory {oid}: {name!r} has the wrong type")
                refs[child] = refs.get(child, 0) + 1
        for oid, ino in inodes.items():
            if ino.type == TYPE_TABLE:
                continue                            # internal: bitmap, snapshot table
            want = 0 if oid == OBJ_ROOT else ino.link_count
            if refs.get(oid, 0) != want:
                self.errors.append(f"object {oid}: link count {ino.link_count}, "
                                   f"{refs.get(oid, 0)} directory entries")
        if inodes[OBJ_ROOT].type != TYPE_DIR or inodes[OBJ_ROOT].link_count != 1:
            self.errors.append("root directory: wrong type or link count")
        entries = parse_directory(contents[OBJ_ROOT], [], OBJ_ROOT)

        # Attributes: well-formed records of files and directories that
        # exist, no key twice for one object.
        self.attributes = None
        if sb.attribute_object:
            ino = inodes.get(sb.attribute_object)
            if not ino or ino.type != TYPE_TABLE:
                raise CheckError(f"attribute table (object {sb.attribute_object}) missing")
            seen = set()
            self.attributes = []
            for oid, key, value in parse_attributes(contents[sb.attribute_object], self.errors):
                if oid not in inodes or inodes[oid].type not in (TYPE_FILE, TYPE_DIR):
                    self.errors.append(f"attribute {key!r} of object {oid}, which does not exist")
                    continue
                if (oid, key) in seen:
                    self.errors.append(f"object {oid} has attribute {key!r} twice")
                seen.add((oid, key))
                self.attributes.append((oid, key, value))

        # Every snapshot's state: its object table and objects; blocks it
        # shares with other states are fine.
        self.snapshots, self.live = [], set(self.owner)
        self.walking_live = False
        if sb.snapshot_object:
            ino = inodes.get(sb.snapshot_object)
            if not ino or ino.type != TYPE_TABLE:
                raise CheckError(f"snapshot table (object {sb.snapshot_object}) missing")
            for name, gen, _, table_inode in parse_snapshots(contents[sb.snapshot_object]):
                if gen > sb.pinned_generation:
                    self.errors.append(f"snapshot {name!r}: generation {gen} is not pinned "
                                       f"({sb.pinned_generation})")
                self.snapshots.append(name)
                self.state, self.newest = {}, gen
                what = f"snapshot {name!r}"
                table = self.content(table_inode, f"{what} object table")
                for oid in range(2, len(table) // INODE.size):
                    old = Inode.unpack(table[oid * INODE.size:(oid + 1) * INODE.size])
                    if old.type != TYPE_FREE:
                        self.content(old, f"{what} object {oid}")
        self.kept = len(set(self.owner) - self.live)

        # The bitmap marks exactly the reserved and the reached blocks; with
        # a pinned generation, frees held back stay marked until a sweep.
        bitmap = contents[OBJ_BITMAP]
        marked = {b for b in range(sb.block_count) if bitmap[b // 8] >> (b % 8) & 1}
        expected = set(self.owner) | set(range(FIRST_ALLOCATABLE))
        for b in sorted(expected - marked)[:5]:
            self.errors.append(f"block {b} in use ({self.owner.get(b)}) but free in the bitmap")
        self.unreached = len(marked - expected)
        for b in sorted(marked - expected)[:5] if not sb.pinned_generation else ():
            self.errors.append(f"block {b} marked in the bitmap but unused")
        if len(marked) != sb.used_blocks:
            self.errors.append(f"bitmap has {len(marked)} blocks, superblock says {sb.used_blocks}")
        return len(inodes) + 1, len(marked), entries


def valid_name(name):
    return 0 < len(name) <= 255 and "/" not in name and "\0" not in name and \
        name not in (".", "..")


def parse_directory(data, errors, oid):
    """[(name, object id, type)] of a directory's content; problems are
    appended to errors."""
    entries, pos = [], 0
    while pos < len(data):
        child, size, name_size, typ = DIRENT.unpack_from(data, pos)
        if size < DIRENT.size + name_size or size % 8 or pos + size > len(data) or \
                pos // BLOCK != (pos + size - 1) // BLOCK:
            errors.append(f"directory {oid}: malformed entry at byte {pos}")
            break
        if child:
            try:
                name = data[pos + 12:pos + 12 + name_size].decode("utf-8")
            except UnicodeDecodeError:
                name = None
            if name is None or not valid_name(name):
                errors.append(f"directory {oid}: invalid name at byte {pos}")
            else:
                entries.append((name, child, typ))
        pos += size
    return entries


def check(part):
    """Returns (ok, one-line summary)."""
    sb, slot = mount(part)
    if not sb:
        return False, "no valid RitterFS superblock"
    checker = Checker(part, sb)
    try:
        objects, used, entries = checker.run()
    except CheckError as e:
        checker.errors.append(str(e))
        objects = used = 0
        entries = []
    summary = (f"generation {sb.generation} (slot {'AB'[slot - 1]}), {sb.block_count} blocks, "
               f"{used} used, {objects} objects, root directory: {len(entries)} entries")
    attributes = getattr(checker, "attributes", None)
    if attributes is not None:
        summary += (f", attributes: {len(attributes)} on "
                    f"{len({oid for oid, _, _ in attributes})} objects")
    snapshots = getattr(checker, "snapshots", None)
    unreached, kept = getattr(checker, "unreached", 0), getattr(checker, "kept", 0)
    if checker.shared or (sb.pinned_generation and not snapshots):
        summary += (f", copies: {checker.shared} blocks shared (generations up to "
                    f"{sb.pinned_generation} pinned, {unreached} awaiting a sweep)")
    if snapshots:
        summary += (f", snapshots: {len(snapshots)} ({kept} blocks kept only "
                    f"for them, {unreached} awaiting a sweep)")
    if checker.errors:
        return False, summary + "; errors: " + "; ".join(checker.errors[:5])
    return True, summary + "; all checksums ok, bitmap consistent"


class Reader:
    """Path lookup and reading, for the ls and cat commands."""

    def __init__(self, part):
        self.part = part
        self.sb, _ = mount(part)
        if not self.sb:
            raise CheckError("no valid RitterFS superblock")
        table = self.content(self.sb.table)
        self.inodes = [Inode.unpack(table[i:i + INODE.size])
                       for i in range(0, len(table), INODE.size)]

    def content(self, inode):
        return Checker(self.part, self.sb).content(inode, "object")

    def lookup(self, path, follow=False):
        """The object at path: symbolic links on the way are followed as
        the kernel follows them (57a), the last one too if follow."""
        names, done, links = [p for p in path.split("/") if p], [], 0
        oid, trail = OBJ_ROOT, [OBJ_ROOT]
        while names:
            name = names.pop(0)
            if name == ".":
                continue
            if name == "..":
                if len(trail) > 1:
                    trail.pop()
                    done.pop()
                oid = trail[-1]
                continue
            if self.inodes[oid].type != TYPE_DIR:
                raise CheckError(f"{path}: not a directory")
            found = [c for n, c, _ in parse_directory(self.content(self.inodes[oid]), [], oid)
                     if n == name]
            if not found:
                raise CheckError(f"{path}: not found")
            child = found[0]
            if self.inodes[child].type == TYPE_SYMLINK and (names or follow):
                links += 1
                if links > LINKS_MAX:
                    raise CheckError(f"{path}: too many levels of symbolic links")
                target = self.content(self.inodes[child]).decode("utf-8", "replace")
                names = [p for p in target.split("/") if p] + names
                if target.startswith("/"):
                    oid, trail, done = OBJ_ROOT, [OBJ_ROOT], []
                continue
            oid = child
            trail.append(oid)
            done.append(name)
        return oid, self.inodes[oid]


# --- changing an existing file system (one copy-on-write commit) ------------------

def read_verified(part, ptr):
    """The block a pointer refers to, from whichever copy matches its checksum."""
    for a in (ptr.address, ptr.address2):
        if a:
            data = part.read(a)
            if crc32c(data) == ptr.checksum:
                return data
    raise CheckError(f"block {ptr.address}: no valid copy")


def tree_blocks(part, inode):
    """Every block (both copies) of an object's content tree, with the
    generation that wrote it: {block: birth}."""
    blocks = {}
    if inode.flags & FLAG_INLINE:
        return blocks

    def walk(ptr, level):
        if ptr.address == 0:
            return
        blocks.update((a, ptr.birth) for a in (ptr.address, ptr.address2) if a)
        if level > 0:
            data = read_verified(part, ptr)
            for i in range(POINTERS_PER_BLOCK):
                walk(Pointer.unpack(data[i * 32:(i + 1) * 32]), level - 1)

    walk(inode.root, inode.height)
    return blocks


def install(part, files, now=None, objects=None, modes=None):
    """Store files (path -> bytes) in the existing RitterFS with one commit:
    missing directories are created, files whose content differs are
    replaced. objects (object ID -> bytes) replaces existing objects'
    content as it is (for tests and repairs: e.g. an internal table). Like the kernel's commits it is copy-on-write: no block of the
    current state is overwritten, and the new state becomes valid only with
    the superblock written last, into the other slot. modes (path -> bits,
    e.g. 0o4755 for a setuid program) records those files' permissions (root
    owns what install makes). Returns the paths written; if nothing differs
    there is no commit and nothing is written."""
    now = now if now is not None else time.time_ns()
    reader = Reader(part)
    sb, inodes = reader.sb, reader.inodes
    next_id = sb.next_object_id
    entries = {}                                    # directory oid -> [(name, oid, type)]
    contents = {}                                   # changed object -> new content
    new_objects = set()

    def dir_entries(oid):
        if oid not in entries:
            entries[oid] = parse_directory(reader.content(inodes[oid]), [], oid)
        return entries[oid]

    def child(oid, name, typ, path):
        """The object for name in directory oid, created if missing."""
        nonlocal next_id
        for n, c, t in dir_entries(oid):
            if n == name:
                if t != typ:
                    raise CheckError(f"{path}: exists, but is not a " +
                                     {TYPE_DIR: "directory", TYPE_SYMLINK: "symbolic link"}
                                     .get(typ, "file"))
                return c
        if not valid_name(name):
            raise CheckError(f"{path}: invalid name")
        new, next_id = next_id, next_id + 1
        inodes.extend(Inode() for _ in range(next_id - len(inodes)))
        inodes[new] = Inode(typ, 0, 1, 0, now, now, now)
        new_objects.add(new)
        dir_entries(oid).append((name, new, typ))
        contents[oid] = None                        # re-encoded below
        if typ == TYPE_DIR:
            entries[new] = []
            contents[new] = None
        return new

    written = []
    mode_changed = set()
    for path, data in sorted(files.items()):
        names = [p for p in path.split("/") if p]
        oid = OBJ_ROOT
        for i, name in enumerate(names[:-1]):
            oid = child(oid, name, TYPE_DIR, "/" + "/".join(names[:i + 1]))
        if data is None:                            # (57b) a folder, made if missing
            created = next_id
            oid = child(oid, names[-1], TYPE_DIR, path)
            if oid >= created:
                written.append(path)
            if modes and path in modes and inodes[oid].mode != MODE_SET | modes[path]:
                inodes[oid].mode = MODE_SET | modes[path]
                mode_changed.add(oid)
                if path not in written:
                    written.append(path)
            continue
        if isinstance(data, Link):                  # (57a) a symbolic link
            oid = child(oid, names[-1], TYPE_SYMLINK, path)
            data = data.target
        else:
            oid = child(oid, names[-1], TYPE_FILE, path)
        if oid in new_objects or reader.content(inodes[oid]) != data:
            contents[oid] = data
            written.append(path)
        if modes and path in modes and inodes[oid].mode != MODE_SET | modes[path]:
            inodes[oid].mode = MODE_SET | modes[path]
            mode_changed.add(oid)
            if path not in written:
                written.append(path)
    for oid, data in (objects or {}).items():
        if oid not in range(len(inodes)) or inodes[oid].type in (TYPE_FREE, TYPE_DIR):
            raise CheckError(f"object {oid} does not exist or is a directory")
        contents[oid] = data
        written.append(f"object {oid}")
    if not written:
        return []
    for oid in contents:
        if inodes[oid].type == TYPE_DIR:
            contents[oid] = encode_directory([(n.encode("utf-8"), c, t)
                                              for n, c, t in entries[oid]])

    gen = sb.generation + 1
    bitmap_inode = inodes[OBJ_BITMAP]
    old_bitmap = reader.content(bitmap_inode)
    in_use = {b for b in range(sb.block_count) if old_bitmap[b // 8] >> (b % 8) & 1}

    # Blocks of the trees that are replaced: free in the new bitmap, but not
    # reused in this commit (the current superblock still refers to them).
    # Blocks born up to the pinned generation are shared with snapshots:
    # their frees are held back (counted) until a sweep, as in the kernel.
    replaced = {**tree_blocks(part, sb.table), **tree_blocks(part, bitmap_inode)}
    for oid in contents:
        if oid not in new_objects:
            replaced.update(tree_blocks(part, inodes[oid]))
    released = {b for b, birth in replaced.items() if not birth or birth > sb.pinned_generation}
    deferred = sb.deferred_blocks + len(replaced) - len(released)

    alloc = Allocator(part.block_count)
    alloc.used |= in_use
    plans = {}
    for oid, content in contents.items():
        if len(content) > INLINE_MAX:
            typ = inodes[oid].type
            blocks = split_blocks(content)
            present = [any(b) for b in blocks] if typ == TYPE_FILE else [True] * len(blocks)
            plans[oid] = (blocks, plan_tree(alloc, present, typ not in DATA_TYPES))
    table_bytes = next_id * INODE.size
    table_height, table_plan = plan_tree(alloc, [True] * len(split_blocks(bytes(table_bytes))),
                                         True)
    bitmap_height, bitmap_plan = plan_tree(alloc, [True] * len(split_blocks(old_bitmap)), True)

    used = (in_use - released) | (alloc.used - in_use)
    bitmap = bytearray(len(old_bitmap))
    for b in used:
        bitmap[b // 8] |= 1 << (b % 8)

    # Children before parents: content trees, then the bitmap, then the
    # object table that holds their inodes, then the superblock.
    for oid, content in contents.items():
        ino = inodes[oid]
        ino.size, ino.modified, ino.changed, ino.generation = len(content), now, now, gen
        if oid in plans:
            blocks, (height, plan) = plans[oid]
            ino.flags, ino.inline = 0, b""
            ino.height, ino.root = height, write_tree(part, plan, blocks, gen)
        else:
            ino.flags, ino.inline, ino.height, ino.root = FLAG_INLINE, content, 0, Pointer()
    for oid in mode_changed - set(contents):        # (a mode alone: the inode changes)
        inodes[oid].changed, inodes[oid].generation = now, gen
    bitmap_inode.changed, bitmap_inode.modified, bitmap_inode.generation = now, now, gen
    bitmap_inode.height = bitmap_height
    bitmap_inode.root = write_tree(part, bitmap_plan, split_blocks(bytes(bitmap)), gen)
    table = b"".join(ino.pack() for ino in inodes[:next_id])
    table_inode = Inode(TYPE_TABLE, 0, 1, table_bytes, sb.table.created, now, now, gen,
                        table_height, write_tree(part, table_plan, split_blocks(table), gen))

    part.flush()
    new_sb = Superblock(sb.block_count, gen, now, sb.created_time, sb.uuid, sb.label, len(used),
                        next_id, table_inode, sb.pinned_generation, deferred, sb.snapshot_object,
                        sb.attribute_object)
    part.write_super(SLOTS[gen % 2], new_sb.pack())
    part.flush()
    return written


def main():
    commands = ("check", "format", "ls", "cat", "put")
    if len(sys.argv) < 5 or sys.argv[1] not in commands:
        sys.exit(__doc__)
    command, image, first, sectors = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    with open(image, "r+b") as f:
        part = Partition(f, first, sectors)
        if command in ("ls", "cat"):
            reader = Reader(part)
            oid, inode = reader.lookup(sys.argv[5] if len(sys.argv) > 5 else "/",
                                       follow=command == "cat")
            data = reader.content(inode)
            if command == "cat":
                sys.stdout.buffer.write(data)
                return
            if command == "ls" and inode.type == TYPE_SYMLINK:
                oid, inode = reader.lookup(sys.argv[5], follow=True)
                data = reader.content(inode)
            if inode.type != TYPE_DIR:
                print(f"{inode.size:>10}  {sys.argv[5]}")
                return
            for name, child, typ in parse_directory(data, [], oid):
                if typ == TYPE_SYMLINK:             # as RitterOS's ls (57a)
                    target = reader.content(reader.inodes[child]).decode("utf-8", "replace")
                    print(f"{'<link>':>10}  {name} -> {target}")
                    continue
                kind = "<dir>" if typ == TYPE_DIR else str(reader.inodes[child].size)
                print(f"{kind:>10}  {name}{'/' if typ == TYPE_DIR else ''}")
            return
        if command == "put":
            if len(sys.argv) != 7:
                sys.exit(__doc__)
            with open(sys.argv[6], "rb") as src:
                written = install(part, {sys.argv[5]: src.read()})
            print(f"{sys.argv[5]}: {'written' if written else 'unchanged'}")
        if command == "format":
            label = sys.argv[5].encode() if len(sys.argv) > 5 else b"RitterOS"
            format(part, label)
        ok, summary = check(part)
    print(f"RitterFS check: {summary}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

# RitterFS — on-disk format, version 3

RitterFS is the native file system of RitterOS. It lives in the GPT partition
with type GUID `B5CA1B3E-6278-48F0-BBEF-87907326A8B3`.

## Principles

1. **Copy-on-write.** A block that is part of the committed file system is
   never overwritten. Changes are written to free blocks; a commit then
   switches to the new tree by writing a superblock. After a crash the disk
   holds either the old or the new state, never a mix: no journal, no fsck.
2. **A checksum in every pointer.** Each block pointer stores the CRC-32C of
   the block it points to (a Merkle tree, as in ZFS). This detects bit rot and
   also misdirected or lost writes, which a checksum inside the block cannot.
3. **Metadata twice.** Every metadata block is written to two different
   locations; a damaged copy can be detected, and later repaired, from the
   other. File data is written once.
4. **Made for SSDs.** No layout tricks for spinning disks, no access times (no
   write on every read), freed space can be reported to the SSD (NVMe
   deallocate / TRIM).
5. **Everything is an object.** Like NetApp's WAFL, the file system's own
   tables — the object table and the free-space bitmap — are stored as
   ordinary objects in the tree, so they get copy-on-write and checksums for
   free.

## Conventions

- All integers are little-endian.
- The file system block size is 4096 bytes. Block numbers are 64-bit and
  count from the start of the partition. Block 0 is never used for data, so
  address 0 means "no block".
- Checksums are CRC-32C (Castagnoli polynomial 0x1EDC6F41, reflected,
  initial value and final XOR 0xFFFFFFFF), as in btrfs, ext4 and iSCSI.
  x86 CPUs with SSE4.2 compute it with the `crc32` instruction.
- Times are 64-bit nanoseconds since 1970-01-01 00:00 UTC.
- Names are UTF-8, 1-255 bytes, compared byte by byte; they must not contain
  `/` or NUL and must not be `.` or `..`.

## Partition layout

| Block      | Contents                                                     |
|------------|--------------------------------------------------------------|
| 0          | reserved (zero)                                              |
| 1          | superblock slot A                                            |
| 2          | superblock slot B                                            |
| 3 - 15     | reserved                                                     |
| 16 - end   | allocatable: all trees, data and copies                      |

## Block pointer (32 bytes)

| Offset | Size | Field      | Meaning                                              |
|--------|------|------------|------------------------------------------------------|
| 0      | 8    | `address`  | block number of the (first) copy; 0 = none (a hole)  |
| 8      | 8    | `address2` | second copy for metadata; 0 for file data            |
| 16     | 4    | `checksum` | CRC-32C of the 4096-byte block                       |
| 20     | 4    | `reserved` | 0                                                    |
| 24     | 8    | `birth`    | generation (commit number) that wrote the block      |

Both copies hold identical bytes, so one checksum covers both.

## Superblock (block 1 or 2)

A commit writes the superblock with generation `g` into slot `g % 2`
(A for even, B for odd). Mounting reads both slots and uses the valid one
with the highest generation.

| Offset | Size | Field               | Meaning                                    |
|--------|------|---------------------|--------------------------------------------|
| 0      | 8    | `magic`             | `"RITTERFS"`                                |
| 8      | 4    | `version`           | 1; 2 with snapshots or copies, 3 with attributes (below) |
| 12     | 4    | `block_size`        | 4096                                        |
| 16     | 8    | `block_count`       | blocks in the partition                     |
| 24     | 8    | `generation`        | commit number, 1 for a new file system      |
| 32     | 8    | `commit_time`       | time of this commit                         |
| 40     | 8    | `created_time`      | time the file system was created            |
| 48     | 16   | `uuid`              | identity of this file system                |
| 64     | 64   | `label`             | UTF-8, NUL-padded, e.g. `"RitterOS"`        |
| 128    | 8    | `used_blocks`       | allocated blocks, including both copies     |
| 136    | 8    | `next_object_id`    | lowest object ID never used                 |
| 144    | 8    | `pinned_generation` | version 2: see Snapshots; 0 = none          |
| 152    | 8    | `deferred_blocks`   | version 2: frees held back since the last sweep |
| 160    | 8    | `snapshot_object`   | version 2: object ID of the snapshot table; 0 = none |
| 168    | 8    | `attribute_object`  | version 3: object ID of the attribute table; 0 = none |
| 176    | 80   | reserved            | 0                                           |
| 256    | 256  | `object_table`      | the inode of the object table (see below)   |
| 512    | 3580 | reserved            | 0                                           |
| 4092   | 4    | `checksum`          | CRC-32C of bytes 0-4091                     |

The superblock is the root of the Merkle tree: through the object table's
inode it reaches, checksum by checksum, every block in use.

## Objects and inodes

Every file, directory and internal table is an *object*, identified by a
64-bit object ID. The **object table** is itself an object: its content is an
array of 256-byte inodes, where inode *n* describes object *n*. Its own inode
is stored in the superblock.

Reserved object IDs:

| ID | Object                                                   |
|----|----------------------------------------------------------|
| 0  | invalid (an all-zero inode means "free slot")            |
| 1  | the object table (its inode is in the superblock; slot 1 of the table is unused) |
| 2  | the free-space bitmap                                    |
| 3  | the root directory `/`                                   |

### Inode (256 bytes)

| Offset | Size | Field          | Meaning                                         |
|--------|------|----------------|-------------------------------------------------|
| 0      | 2    | `type`         | 0 free, 1 file, 2 directory, 3 internal table, 4 symbolic link |
| 2      | 2    | `flags`        | bit 0: content is inline (see below); bit 1: a fixed file (see Fixed files) |
| 4      | 4    | `link_count`   | directory entries referring to it               |
| 8      | 8    | `size`         | content size in bytes                           |
| 16     | 8    | `created`      | time                                            |
| 24     | 8    | `modified`     | time of the last content change                 |
| 32     | 8    | `changed`      | time of the last inode change                   |
| 40     | 8    | `generation`   | commit that last changed the object             |
| 48     | 1    | `height`       | levels of indirect blocks above the data        |
| 49     | 4    | `uid`          | owner (user ID; 0: root)                        |
| 53     | 4    | `gid`          | group                                           |
| 57     | 2    | `mode`         | 0x8000 + setuid (04000) + sticky (01000) + rwxrwxrwx bits; 0: none recorded (0755) |
| 59     | 5    | reserved       | 0                                               |
| 64     | 32   | `root`         | block pointer to the content tree               |
| 96     | 160  | `inline`       | content of up to 160 bytes, if flag 0 is set    |

A **symbolic link** (type 4, since 57a) holds the path it names as its
content: 1-255 bytes of UTF-8 without a zero byte, kept as written
(absolute, or relative to the link's directory), inline when it fits.
Its blocks, if any, are data like a file's (one copy). Its mode is not
used (a link is shown as `lrwxrwxrwx`); paths through it lead to what it
names, at most 40 links in one path. Like the setuid bit, the type came
without a new version: readers that know only types 1-3 still read every
file and directory, and older checkers report the links as invalid
objects (the bootloader only follows `/system/<arch>/kernel.elf`).

Owner, group and mode used to be reserved (zero): an inode without a
recorded mode belongs to root with the rights 0755, so file systems written
before need no change, and the version stays 3. Permissions are the Unix
ones: read, write and execute (for a directory: look up names) for the
owner, the group and everyone else; root may do everything. The setuid bit
(04000, since 50b; a valid bit, so older checkers report it as an invalid
mode) makes a program run as its file's owner, honoured only on the system's
own file system. The sticky bit (01000, since 57b) on a directory lets
only an entry's owner, the directory's owner or root remove or rename that
entry, so everyone may write in `/tmp` (mode 1777) without removing the
files of others.

### Content tree

An object's content is a sequence of 4096-byte blocks, found through a tree
of block pointers:

- `height` 0: `root` points directly to the only data block.
- `height` *h* > 0: `root` points to an indirect block holding 128 block
  pointers, each the root of a subtree of height *h* - 1.

Block *i* of the content is reached by taking the base-128 digits of *i*,
most significant first. Height 4 covers 1 TiB per object. The height is the
smallest one whose tree covers all blocks of the content.

In files, a pointer with `address` 0 is a hole: the block (or the whole
subtree) reads as zeros and uses no space. Directories and internal tables
have no holes: all their blocks are allocated.

Indirect blocks are metadata (two copies). Data blocks of files are written
once; data blocks of directories and internal tables are metadata.

Contents of at most 160 bytes are stored inline in the inode (flag bit 0);
`root` is then all zero and `height` 0. Small files and directories need no
block at all.

## Directories

A directory's content is a list of entries, each padded to a multiple of 8
bytes:

| Offset | Size | Field        | Meaning                                    |
|--------|------|--------------|--------------------------------------------|
| 0      | 8    | `object_id`  | 0 marks a deleted entry (space to reuse)   |
| 8      | 2    | `entry_size` | bytes including padding                    |
| 10     | 1    | `name_size`  | 1-255                                      |
| 11     | 1    | `type`       | copy of the object's type                  |
| 12     | n    | `name`       | UTF-8, not NUL-terminated                  |

`entry_size` may be larger than needed: when the next entry would cross a
4096-byte block boundary, the previous entry is padded to the end of its
block, so entries never cross block boundaries. The directory's `size` is the
total length of its entries. There are no `.` and `..` entries; a
directory's parent is found from the path that was used to reach it.

Every object other than the root directory, the object table and the bitmap
is referred to by exactly `link_count` directory entries; the root directory
has `link_count` 1. A file or symbolic link may have several names (hard
links, since 57a: the same object ID in several entries, on the same
file system); removing a name lowers the count, and the object goes with
its last name. A directory has exactly one name (`link_count` 1).

## Free-space bitmap (object 2)

One bit per block of the partition, block *n* at bit *n % 8* of byte *n / 8*;
1 = in use. Blocks 0-15 are always marked in use.

## Well-known paths

| Path | Contents |
|------|----------|
| `/system/x86_64/kernel.elf` | the x86-64 kernel, loaded by `BOOTX64.EFI` |
| `/system/aarch64/kernel.elf` | the AArch64 kernel, loaded by `BOOTAA64.EFI` |

The bootloaders read RitterFS without writing: they choose the superblock,
follow the trees and verify every checksum like the kernel does.

## Growing

(68j) A file system can grow when its partition does (`disks grow`): one
commit raises `block_count` and makes the bitmap (object 2) longer, its
new bits 0 (free), its tree taller where it needs more than its height
holds (the old tree its first subtree, as for any object). Nothing else
moves: both superblocks are at the start. The format version stays.

## Commits

Changes are collected in memory and written by a commit:

1. Write every changed block to newly allocated blocks (metadata twice),
   children before parents, so that each parent records its children's
   checksums. This includes the object table and the bitmap, whose updates
   may allocate further blocks; repeat until nothing changes.
2. Flush the device.
3. Write the superblock with generation + 1 into its slot; flush again.

Blocks that the new tree no longer uses become free only after the commit
that released them has completed: until then the previous superblock still
refers to them and must survive a crash.

## Snapshots (version 2)

A snapshot is a committed state kept for later: its object table's inode,
saved in the **snapshot table**, an internal object (type table, not in any
directory, `link_count` 0) named by the superblock's `snapshot_object`. Its
content is an array of 512-byte entries:

| Offset | Size | Field          | Meaning                                         |
|--------|------|----------------|-------------------------------------------------|
| 0      | 64   | `name`         | UTF-8, NUL-padded, no `/`, not `.` or `..`      |
| 64     | 8    | `generation`   | the commit the snapshot shows; 0 = free entry   |
| 72     | 8    | `created`      | time it was taken                               |
| 80     | 176  | reserved       | 0                                               |
| 256    | 256  | `object_table` | the object table's inode in that commit         |

Through that inode a snapshot reaches its whole tree, read-only, like the
superblock reaches the live one. (RitterOS shows snapshot *name* at
`/.snapshots/name`; that directory is not stored on the disk.) Blocks are shared between the live state and
the snapshots: copy-on-write never changes them, so a snapshot costs only the
blocks that later changes would have freed.

Those frees are held back by **`pinned_generation`**, the newest snapshot's
generation (or a newer one, for copies, below): a block whose pointer says it was born in a generation up to
`pinned_generation` stays marked in the bitmap when the live state stops
using it (`deferred_blocks` counts such frees). Blocks born later are not in
any snapshot and are freed as usual. Deleting a snapshot clears its entry and starts a **sweep**: every block
reachable from the live state or a remaining snapshot is marked, and the
blocks marked in the bitmap but reached by none are freed;
`deferred_blocks` becomes 0, and `pinned_generation` is set to what is
still needed: the newest remaining snapshot's generation, or the newest
birth of a block the live state uses twice (copies), whichever is higher;
0 if neither exists. A writer also sweeps by itself once enough frees were
held back.

**Instant copies** use the same pin, within the live state: a copy of a
file is a new inode with the source's tree (its root pointer and size); a
copy of a directory is a new directory whose entries are copied the same
way. No data block is written. The copy raises `pinned_generation` to the
current generation, so a block that both use is never freed while one of
them changes or is removed.

So with a pinned generation the bitmap may mark more than the trees reach,
never less. A check walks every snapshot's tree as well: between states,
sharing is the point; within one state a block may be used twice only if
it was born up to `pinned_generation` (copies), otherwise it is an error.

A writer that sees neither a pinned generation nor a snapshot table writes
version 1; readers of version 2 accept both.

## Attributes (version 3)

Files and directories can carry attributes: a key (1-63 bytes of UTF-8,
no `=` and no NUL) with a value (0-1024 bytes, any content). They are kept
in the **attribute table**, an internal object (type table, `link_count` 0)
named by the superblock's `attribute_object`. Its content is a sequence of
records like directory entries:

| Offset | Size | Field          | Meaning                                         |
|--------|------|----------------|-------------------------------------------------|
| 0      | 8    | `object_id`    | the file or directory; 0 = free space           |
| 8      | 2    | `size`         | the whole record, a multiple of 16              |
| 10     | 1    | `key_length`   | 1-63                                            |
| 11     | 1    | reserved       | 0                                               |
| 12     | 2    | `value_length` | 0-1024                                          |
| 14     | 2    | reserved       | 0                                               |
| 16     | ...  | key, value     | then padding to `size` (its content is ignored) |

A record never crosses a block: when the next one would, a free record
fills the rest of the block. Changing a value rewrites its record in place
if it is large enough, otherwise the old record becomes free space and the
value goes into a free record that is large enough, or at the end. An
object has each key at most once. When a file or directory is removed, its
records become free (object IDs are reused); an instant copy gets copies
of the source's records. Snapshots see the attributes as they were: the
table is an ordinary object of every state.

## Fixed files

A **fixed** file (inode flag bit 1, since step 35a: the swap file
`/system/swap`) is the one exception to copy-on-write: its data blocks stay
where they are and their owner (the kernel's swap) writes them in place,
past the file system. So:

- a fixed file is a file (type 1) of whole blocks (its size a multiple of
  4096, not inline) without holes; its data blocks have one copy, like any
  file's;
- the checksums in its data blocks' pointers are not kept: readers never
  verify them (a check claims the blocks and checks their places in the
  bitmap, a scrub skips them); its indirect blocks are ordinary metadata
  (two copies, checksums) and never change;
- it is never written, cut or moved through the file system, and its
  content is never read through it (RitterOS refuses to open it); a block
  of it is never relocated;
- it becomes fixed only while none of its blocks, indirect ones included,
  is shared with a snapshot or an instant copy (each born after
  `pinned_generation`), else writing it in place would change the other
  too. An instant copy of a fixed file is refused, and a copy of a
  directory leaves fixed files out. A snapshot taken later holds its blocks
  like any other's (it sees whatever was written in place since).

Removing a fixed file frees its blocks as usual. Older readers and checkers
read every other object as before; they report a fixed file's data blocks
as having bad checksums.

## Scrub and repair

A scrub reads every block of every tree and verifies each copy against the
checksum in its pointer. A damaged copy of a metadata block is rewritten from
the intact copy in place: the content is identical (same checksum, same tree)
and the intact copy is never touched, so a crash during the repair cannot make
anything worse. If the rewritten copy still does not read back correctly, the
block is relocated instead: a commit writes both copies to new places. File
data is stored once; a damaged data block can be detected and reported, not
repaired. A fixed file's data blocks are not verified (their checksums are
not kept), and its blocks are never relocated.

## Version history

- 1: initial version.
- 2: snapshots: superblock fields `pinned_generation`, `deferred_blocks`,
  `snapshot_object`; the snapshot table; instant copies (blocks shared within
  the live state, covered by the pin). Written only while something is
  pinned or a snapshot table exists.
- 3: attributes: superblock field `attribute_object`, the attribute table.
  Written once the table exists. Later additions without a new version:
  the owner, group and mode fields; the setuid bit (50b); symbolic links
  (type 4) and hard links (57a); the sticky bit (57b); fixed files (inode
  flag bit 1, 35a).

# disks: the disks and their partitions; changes to a partition table (root)

```
disks [DISK] | disks init|add|delete|rename|grow ...
disks
disks DISK
disks init DISK
disks add DISK SIZE|rest [TYPE [NAME]]
disks delete PARTITION
disks rename PARTITION NAME
disks grow [DISK]
```

`disks` alone lists every disk, one line each (its size, kind and own
name, its partition table), and one line per partition below it (size,
number and type, the file system found on it and its label, where it is
mounted). It is the kernel's report, the same as the rescue console's
`disks`. Anyone may run it. Partition tables are GPT or MBR (older disks
and many sticks); "no partition table" means a file system on the whole
disk, or nothing. Recognized file systems: RitterFS, FAT12/16/32, exFAT,
NTFS, ext2/3/4, APFS; `empty` means the start of the area is all zeros.

```
nvme0n1          1.0 GiB  NVMe 'QEMU NVMe Ctrl', GPT, 2 partitions; the system disk
  nvme0n1p1     64.0 MiB  #1 EFI system partition: FAT32 'EFI'
  nvme0n1p2    958.0 MiB  #2 RitterOS: RitterFS 'RitterOS', generation 14, mounted at /
usbdisk1        64.0 MiB  USB 'QEMU QEMU HARDDISK', MBR, 1 partition
  usbdisk1p1    63.0 MiB  #1 type 0x0C (FAT32 (LBA)): FAT32 'STICK'
usbdisk2        16.0 MiB  USB 'QEMU QEMU HARDDISK', no partition table: empty
```

After a sleep (`power sleep`, since 37g-6) each SATA controller adds a
line on how its disks came back: the link's speed and how long it took to
settle after its reset, and when the disk said it was ready (`ahci0: after
sleep 1: sata0: the link at 6 Gbit/s after 308 ms, the disk ready after
2435 ms (SStatus 0x123, task file 0x50)`), or why not; a controller with
no disk says `started again, no disk on it`.

Disks are named by kind: `nvme0n1` (NVMe SSDs), `sata0`, `sata1` (SATA
disks), `usbdisk0`, `usbdisk1` (USB sticks and disks, in the order found;
a stick plugged in again gets its old name). Partitions add their number:
`nvme0n1p2`, `usbdisk0p1`. Each is also in `/dev`, and a name may be given
as `/dev/NAME`.

`disks DISK` shows one disk in detail: its line, the block size, the
blocks partitions may use, and each partition's first and last block,
with the free areas between them (those of at least 1 MiB). A disk image
copied to a bigger stick keeps a table made for the smaller size; `disks
DISK` says so and the next change fits the table to the whole disk.

**Changing a partition table** is root's (it writes `/dev/DISK`), and
only GPT tables are changed; an MBR disk can get a new GPT with `init`.
`disks` first says what it is about to do, then writes only after the
disk's name is typed (anything else, or Ctrl-D, leaves the disk as it
was), then shows the disk again. The copy of the table at the disk's end
is written first, so a stick pulled out in the middle still has one whole
table. When the disk is closed, RitterOS reads the table again: `/dev` and
`disks` show the change at once, and a new partition holding a RitterFS is
mounted at `/volumes/<label>`. A new partition is empty: `format` makes a
file system on it. `disks` has been in RitterOS since step 23a (the list)
and step 23c (the changes).

## Options

| Command | Does |
|---|---|
| `disks` | all disks and partitions |
| `disks DISK` | one disk in detail: blocks, partitions, free areas |
| `disks init DISK` | a new, empty GPT; whatever table (or file system on the whole disk) was there is no longer found. Refused while anything on the disk is mounted |
| `disks add DISK SIZE [TYPE [NAME]]` | a partition of SIZE in the first free area large enough |
| `disks add DISK rest [TYPE [NAME]]` | a partition filling all of the largest free area |
| `disks delete PARTITION` | its entry out of the table; the data stays until something else is written there (`add` of a partition with the same start and type finds it again). Refused while it is mounted |
| `disks rename PARTITION NAME` | the partition's name in the GPT (not the file system's label) |
| `disks grow [DISK]` | (since 68j) the disk's last partition, which must hold a mounted RitterFS, to the end of the disk, and its RitterFS with it; DISK is the system disk if not given. For a disk image written to a bigger disk (the sticks are 256 MiB images): the GPT is fitted to the whole disk (its copy moves to the end), the partition stays mounted while it grows, and the file system takes the new room in one commit. It asks nothing (it only adds free room). `login` runs it at the first start when the system disk's table was made for a smaller disk |

SIZE is a number of bytes, or with `K`, `M`, `G` or `T` (binary units:
`512K`, `100M`, `2G`, `1T`). Partitions start on a 1 MiB boundary. TYPE
is one of:

| TYPE | The partition type |
|---|---|
| `ritteros` | RitterOS (the default) |
| `efi` | EFI system partition |
| `data` | Microsoft basic data (for FAT and exFAT, as Windows and macOS expect) |
| `linux` | Linux filesystem |
| `swap` | Linux swap |
| a GUID | that type GUID, as `0FC63DAF-8483-4772-8E79-3D69D8477DE4` |

NAME is at most 36 characters of plain ASCII; it defaults to the type's
name (none for a GUID).

## Exit status and messages

- `0`: shown, or the table was written. Before writing: `DISK: a new,
  empty GPT partition table` (with `, replacing its GPT with N partitions`
  and the like), `DISK: new partition #N (DISKpN): SIZE, blocks
  FIRST-LAST, TYPE 'NAME'`, `DISK: delete partition PARTITION (SIZE,
  ...)`, `DISK: partition PARTITION named 'NAME' (was 'OLD')`; then `Type
  the disk's name (DISK) to write the new table: `. `disks grow`: `disks:
  PARTITION grows from SIZE to SIZE, to the end of DISK` and `disks: the
  RitterFS on PARTITION now has SIZE (N blocks of 4 KiB)`, or `disks:
  PARTITION reaches the end of DISK already (its RitterFS has SIZE)`.
- `1`: `Nothing written.` (another name typed), or `disks: ...`:
  - `NAME: No such file or directory`
  - `PARTITION: a partition (of DISK); show its disk`, `...; this needs a
    disk`; `DISK: a disk; this needs a partition (DISKp1, ...)`
  - `PARTITION is mounted at PATH (unmount it first)`
  - `DISK: an MBR; disks changes GPT tables only (disks init DISK makes
    one)`, `DISK: no partition table (disks init DISK makes one)`,
    `PARTITION: on a disk with an MBR; disks changes GPT tables only
    (disks init DISK makes one)`
  - `SIZE: not a size (like 100M, 2G, or rest)`, `TYPE: not a type
    (ritteros, efi, data, linux, swap, or a GUID)`, `'NAME': not a name
    (at most 36 characters of plain ASCII)`
  - `DISK: no free area of SIZE`, `DISK: no free area`, `DISK: the table
    is full (128 entries)`
  - `DISK: no valid GPT found`, `DISK: the table does not fit the disk
    (smaller than it says)`, `PARTITION: not in the table on DISK
    (changed meanwhile?)`
  - `/dev/DISK: Permission denied (changing disks is root's)`, `DISK:
    writing failed: REASON`
  - `disks grow`: `no system disk found (name one: disks grow DISK)`,
    `DISK: its last partition, PARTITION holds no mounted RitterFS (disks
    grows only that)`, `PARTITION: the file system did not grow: REASON`
- `2`: a wrong command: the usage of every form.

## Examples

```
ritteros:/$ disks usbdisk0
usbdisk0        64.0 MiB  USB 'QEMU QEMU HARDDISK', GPT, 1 partition
  blocks of 512 bytes; partitions may use 34-131038
  (free)        16.0 MiB  2048-34815
  usbdisk0p2    46.9 MiB  34816-131038          #2 Microsoft basic data 'Pictures': empty
ritteros:/$ disks add usbdisk0 16M
usbdisk0: new partition #1 (usbdisk0p1): 16.0 MiB, blocks 2048-34815, RitterOS
Type the disk's name (usbdisk0) to write the new table: usbdisk0
usbdisk0        64.0 MiB  USB 'QEMU QEMU HARDDISK', GPT, 2 partitions
  blocks of 512 bytes; partitions may use 34-131038
  usbdisk0p1    16.0 MiB  2048-34815            #1 RitterOS: empty
  usbdisk0p2    46.9 MiB  34816-131038          #2 Microsoft basic data 'Pictures': empty
ritteros:/$ disks rename usbdisk0p2 Photos
ritteros:/$ disks add usbdisk1 rest data STICK
ritteros:/$ disks grow
disks: usbdisk0p2 grows from 190.0 MiB to 28.8 GiB, to the end of usbdisk0
disks: the RitterFS on usbdisk0p2 now has 28.8 GiB (7551744 blocks of 4 KiB)
```

## See also

[`format`](format.md), [`mount`](mount.md), [`unmount`](unmount.md),
[`osinstall`](osinstall.md), [`ls`](ls.md) (`ls /dev`)

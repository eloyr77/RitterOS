# format: make a new, empty RitterFS (or FAT32, exFAT) on a partition or disk (root)

```
format [-t ritterfs|fat|exfat] DEVICE [LABEL]
format DEVICE [LABEL]                  a RitterFS
format -t fat DEVICE [LABEL]           a FAT32, to share with other systems
format -t exfat DEVICE [LABEL]         an exFAT, to share them, files of any size
```

Makes a new, empty file system on a partition (`format usbdisk0p1
Photos`) or on a whole disk without partitions (`format usbdisk2`), with
LABEL as its name. DEVICE is a name as `disks` and `/dev` show it, with or
without `/dev/`. `format` first shows the size and what is about to be
lost, and formats only after you type the device's name; anything else
writes nothing. Only root may format. `format` came with step 23d
(RitterFS), `-t fat` with 24c, `-t exfat` with 46c.

The new file system is mounted at once at `/volumes/<label>` (FAT and exFAT
without a label under the device's name), and again whenever the disk is
plugged in: RitterOS finds every RitterFS by its content, whatever the
partition's type says. Formatting clears the marks of the file system that
was there before, so other systems no longer see it either; for a whole
disk also its partition table, both copies. The old data itself is not
erased, only no longer found. A device that is mounted (for a whole disk:
one of its partitions) is refused: `unmount` it first.

- **RitterFS** (the default, or `-t ritterfs`): LABEL is 1-63 plain
  characters, no `/`; `RitterFS` if not given. At least 2 MiB. The device's
  blocks must divide 4096 bytes.
- **FAT32** (`-t fat`): for a stick or partition that Windows, macOS and
  Linux should read and write too. LABEL is at most 11 letters, digits,
  spaces or ``!#$%&'()-@^_`{}~``, kept in upper case (`share` becomes
  `SHARE`); without one the volume has none. The cluster size is chosen as
  Windows does (512 bytes up to 260 MB, then 4 KiB to 8 GB, 8 KiB to 16 GB,
  16 KiB to 32 GB, 32 KiB above). From about 33 MiB to 2 TiB; a file on it
  is at most 4 GiB. Give such a partition the type `data` (`disks add DISK
  rest data`) for other systems to show it: `format` warns about one of the
  type RitterOS.
- **exFAT** (`-t exfat`): what large sticks and memory cards come with,
  without FAT32's limit of 4 GiB per file. LABEL is at most 11 characters,
  any but `"*/:<>?\|` (also beyond ASCII: `"Fotos Señor"`), kept as typed;
  without one the volume has none. Clusters of 4 KiB up to 256 MiB, 32 KiB
  up to 32 GiB, 128 KiB above. At least 1 MiB. The volume gets the full
  up-case table, the Unicode Character Database's capitals under the
  Unicode License v3 (`scripts/UNICODE-LICENSE.txt` in the source).

FAT and exFAT need blocks of 512 to 4096 bytes.

## Options

| Option | Meaning |
|---|---|
| `-t ritterfs` | a RitterFS (the same as no `-t`) |
| `-t fat` | a FAT32 |
| `-t exfat` | an exFAT |

`-t` must come first, before DEVICE.

## Exit status and messages

Before asking, `format` prints what it will make, for example
`usbdisk0p1: a new RitterFS 'Photos', 16.0 MiB (4096 blocks of 4 KiB)`,
then what is lost: `What it holds now is lost: FAT32 'STICK'.` (a RitterFS
with its generation), or for a whole disk `It replaces the whole disk's GPT
partition table with 3 partitions.`, and asks `Type the device's name
(DEVICE) to format it: `.

- `0`: formatted: `DEVICE: formatted, mounted at /volumes/LABEL` (or
  `DEVICE: formatted`).
- `1`: nothing written, or writing failed:
  - `Nothing written.`: something else was typed (or the input ended).
  - `format: DEVICE: No such file or directory`: no device of that name.
  - `format: DEVICE is mounted at PATH (unmount it first)`, `format: DISK:
    its partition PART is mounted at PATH (unmount it first)`.
  - `format: 'LABEL': not a label (1-63 plain characters, no '/')`, `not a
    FAT label (...)`, `not an exFAT label (...)`.
  - `format: DEVICE: SIZE is too small (RitterFS needs at least 2.0 MiB)`,
    `format: DEVICE: SIZE does not suit FAT32 (from about 33 MiB to 2
    TiB)`, `format: DEVICE: SIZE does not suit exFAT (from 1 MiB on)`.
  - `format: DEVICE: blocks of N bytes (RitterFS needs a size that divides
    4096)` (or `FAT needs 512 to 4096`, `exFAT needs 512 to 4096`).
  - `format: /dev/DEVICE: Permission denied (formatting is root's)`.
  - `format: DEVICE: writing failed: REASON`.
- `2`: no DEVICE, too many arguments, an unknown `-t` type or another
  option: `usage: format [-t fat|exfat] DEVICE [LABEL]`.

## Examples

```
ritteros:/$ format usbdisk0p1 Photos
usbdisk0p1: a new RitterFS 'Photos', 16.0 MiB (4096 blocks of 4 KiB)
Type the device's name (usbdisk0p1) to format it: usbdisk0p1
usbdisk0p1: formatted, mounted at /volumes/Photos
ritteros:/$ format -t fat usbdisk0p3 share
usbdisk0p3: a new FAT32 'SHARE', 27.8 GiB (1823104 clusters of 16.0 KiB)
Type the device's name (usbdisk0p3) to format it: usbdisk0p3
usbdisk0p3: formatted, mounted at /volumes/SHARE
ritteros:/$ format -t exfat usbdisk1 Photos
usbdisk1: a new exFAT 'Photos', 57.6 GiB (472028 clusters of 128.0 KiB)
Type the device's name (usbdisk1) to format it: usbdisk1
usbdisk1: formatted, mounted at /volumes/Photos
ritteros:/$ format usbdisk0p2
format: usbdisk0p2 is mounted at /volumes/STICK (unmount it first)
```

## See also

[`disks`](disks.md), [`mount`](mount.md), [`unmount`](unmount.md),
[`osinstall`](osinstall.md)

# mount: list mounted volumes, or mount one (root only)

```
mount [DEVICE [PATH]]
mount
mount DEVICE [PATH]
```

Without arguments `mount` lists what is mounted where: each device, its
place, its file system with its label, and whether it is read only. With
DEVICE (a partition's or disk's name as `disks` and `/dev` show it,
`usbdisk0p2` or `/dev/usbdisk0p2`) it mounts the RitterFS or FAT on it at
`/volumes/<label>` (two with the same label get `-2`, `-3`, ...), or at
PATH: a new name in `/volumes`, or an existing directory anywhere, whose
own content is hidden while the volume is mounted there. Mounting is
root's; anyone may list. `mount` has been in RitterOS since step 23b.

Volumes are mounted by themselves: every RitterFS, FAT and exFAT found at
the boot or plugged in later appears at `/volumes/<label>`, and the system
disk's EFI partition at `/boot`. `mount` is for a volume that was
unmounted with `unmount`, for an EFI partition (which is left alone
otherwise), or for another place; RitterFS, FAT and exFAT alike (exFAT by
hand since 50k).

## Exit status and messages

- `0`: listed, or mounted: `mount: DEVICE at PATH`.
- `1`: not mounted:
  - `mount: DEVICE: No such file or directory`: no device of that name.
  - `mount: DEVICE: holds NTFS; only RitterFS, FAT and exFAT can be
    mounted` (or `holds no file system`).
  - `mount: DEVICE: mounted already, at PATH`.
  - `mount: PATH: REASON` (or `mount: DEVICE: REASON` without PATH):
    `Operation not permitted` (not root), `Device or resource busy` (a
    program has the disk open for writing, or PATH is `/volumes` itself or
    in use), `Not a directory`, `Invalid argument` (a new name that is not
    directly in `/volumes`).
- `2`: more than two arguments: `usage: mount [DEVICE [PATH]]`.

## Examples

```
ritteros:/$ mount
nvme0n1p1    on /boot (FAT32 'EFI', read only)
nvme0n1p2    on / (RitterFS 'RitterOS', read-write)
usbdisk0p2   on /volumes/STICK (RitterFS 'STICK', read-write)
usbdisk1p1   on /volumes/CAMERA (FAT32 'CAMERA', read only)
ritteros:/$ unmount /volumes/STICK
ritteros:/$ mount usbdisk0p2
mount: usbdisk0p2 at /volumes/STICK
ritteros:/$ unmount usbdisk0p2
ritteros:/$ mount /dev/usbdisk0p2 /volumes/backup
mount: usbdisk0p2 at /volumes/backup
```

## See also

[`unmount`](unmount.md), [`disks`](disks.md), [`format`](format.md),
[`ls`](ls.md)

# unmount: unmount volumes, e.g. before pulling a stick (root only)

```
unmount PATH|DEVICE...
```

Unmounts each volume, named by where it is mounted (`unmount
/volumes/STICK`) or by its device (`unmount usbdisk0p2`, also as
`/dev/usbdisk0p2`). Everything written there is on the disk already (each
change is committed at once); unmounting also empties the disk's own
write cache, so the stick can be pulled out safely afterwards. It is
refused while a program has a file open there or its current folder there
(`cd` elsewhere first). Root only. `mount` mounts it again (or plugging the
stick in again). `unmount` has been in RitterOS since step 23b.

## Exit status and messages

- `0`: every volume was unmounted.
- `1`: one could not be (the others still are): `unmount: PATH: not
  mounted`; `unmount: PATH: in use (a program has a file open there, or
  its current directory)`; `unmount: PATH: Operation not permitted` (not
  root).
- `2`: no PATH or DEVICE: `usage: unmount PATH|DEVICE...`.

## Examples

```
ritteros:/$ cd /volumes/STICK
ritteros:/volumes/STICK$ unmount /volumes/STICK
unmount: /volumes/STICK: in use (a program has a file open there, or its current directory)
ritteros:/volumes/STICK$ cd /
ritteros:/$ unmount /volumes/STICK
ritteros:/$ unmount usbdisk0p2
unmount: usbdisk0p2: not mounted
```

## See also

[`mount`](mount.md), [`disks`](disks.md), [`format`](format.md),
[`osinstall`](osinstall.md)

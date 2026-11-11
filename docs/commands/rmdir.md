# rmdir: remove empty directories

```
rmdir DIR...
```

Removes each DIR, which must be empty: a directory with anything in it
stays (`Directory not empty`); `rm -r` removes a directory with
everything in it. The directories are removed one after the other, and
one that fails does not stop the others. There is no undo, but a
directory stays readable in snapshots taken before. `rmdir` has been in
RitterOS since step 19a.

## Exit status and messages

- `0`: every DIR was removed.
- `1`: a DIR could not be removed: `rmdir: DIR: REASON`, such as
  `Directory not empty`, `No such file or directory`, `Not a directory`,
  `Permission denied`, `Read-only file system` (in `/.snapshots`).
- `2`: no DIR given: `usage: rmdir DIR...`.

## Examples

```
ritteros:/$ mkdir /home/root/tmp
ritteros:/$ rmdir /home/root/tmp
ritteros:/$ rmdir /home/root
rmdir: /home/root: Directory not empty
```

## See also

[`mkdir`](mkdir.md), [`rm`](rm.md), [`ls`](ls.md),
[`snapshot`](snapshot.md)

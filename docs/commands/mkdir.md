# mkdir: create directories (-p: with their parents)

```
mkdir [-p] DIR...
```

Creates each DIR, with the mode `755` (`rwxr-xr-x`) less what the umask
takes away (by default `022`, so `755`), owned by you. The folder it is
made in must exist and you must be allowed to write there. With `-p` the
missing folders on the way are made too (`mkdir -p a/b/c` makes `a`, `a/b`
and `a/b/c`), and a DIR that is already there is no error. `mkdir` has
been in RitterOS since step 19a.

## Options

| Option | Meaning |
|---|---|
| `-p` | make missing parent folders too, and accept folders that exist |

Options come before the names; `--` ends them.

## Exit status and messages

- `0`: every DIR was made (or, with `-p`, is there).
- `1`: a DIR could not be made (the others still are): `mkdir: DIR: File
  exists`, `mkdir: DIR: No such file or directory` (its parent is missing:
  use `-p`), `mkdir: DIR: Permission denied`, `mkdir: DIR: Read-only file
  system` (in `/.snapshots`, `/dev` or `/boot`). With
  `-p` the message names the first folder on the way that failed. A path
  of 512 bytes or more with `-p`: `mkdir: DIR: File name too long`.
- `2`: no DIR: `usage: mkdir [-p] DIR...`.

## Examples

```
ritteros:/$ mkdir /home/root/projects
ritteros:/$ mkdir -p /home/root/projects/os/notes
ritteros:/$ mkdir /home/root/projects
mkdir: /home/root/projects: File exists
ritteros:/$ mkdir /home/root/a/b
mkdir: /home/root/a/b: No such file or directory
```

## See also

[`rmdir`](rmdir.md), [`rm`](rm.md), [`ls`](ls.md), [`umask`](sh.md) (the
shell's built-in)

# ls: list directories (default: the current one)

```
ls [-l] [PATH]...
```

Lists each directory PATH, or the current directory when none is given:
one line per entry, in the order the folder holds them (not sorted). A
line shows the size in bytes, or `<dir>` for a directory (its name then
ends in `/`), or `<link>` for a symbolic link with what it names (`bin ->
/system/x86_64/bin`, since step 57a). A file given as PATH is listed alone,
with its size; a link to a folder given as PATH lists the folder. `ls` has
been a program since step 11d.

With `-l` each line starts with the type and permissions (`d` a
directory, `l` a link, `b` and `c` devices in `/dev`; `s` for a setuid
program, `t` for the sticky bit as on `/tmp`), the number of names the
file has (hard links), and the owner's and group's numbers. A link then
shows its own length in place of `<link>`, and a link given as PATH is
shown itself, not the folder it names. An entry that cannot be looked at
shows `?` in place of what is not known.

`ls /` also lists `boot`, `dev` and `volumes`, which are not on the disk
itself; `/.snapshots` is not listed but can be listed (`ls /.snapshots`).

## Options

| Option | Meaning |
|---|---|
| `-l` | the type, permissions, number of names, owner and group in front of each entry |

Options come before the PATHs (`-l` again or `-ll` is the same; `--` ends
them, so a PATH may start with `-`).

## Exit status and messages

- `0`: everything was listed.
- `1`: a PATH could not be listed: `ls: PATH: No such file or directory`,
  `ls: PATH: Permission denied` and the like; the other PATHs are still
  listed.
- `2`: an option other than `-l`: `usage: ls [-l] [PATH]...`.

## Examples

```
ritteros:/$ ls /test/docs
     <dir>  notes/
ritteros:/$ ls /
...
    <link>  bin -> /system/x86_64/bin
     <dir>  tmp/
ritteros:/$ ls -l /home
drwx------   1     0     0      <dir>  root/
drwxr-xr-x   1  1000  1000      <dir>  anna/
ritteros:/$ ls /home/nobody
ls: /home/nobody: No such file or directory
```

## See also

[`find`](find.md), [`ln`](ln.md), [`readlink`](readlink.md),
[`chmod`](chmod.md), [`disks`](disks.md) (`ls /dev`), [`mount`](mount.md)
(`ls /volumes`)

# cp: copy a file (-r: a directory), instantly; -p: keep times and modes

```
cp [-rp] FROM TO
```

Copies the file FROM to TO, or with `-r` the directory FROM with
everything in it. If TO is a directory, the copy goes into it under
FROM's name. If TO is a file that exists, its content is replaced (since
step 59, as POSIX's cp): it keeps its permissions and its other names.

A copy to a new name on the same disk is instant: the copy shares the
data blocks with FROM until one of the two changes, so copying a large
file costs almost no space (instant copies, since step 18c). To another
volume (`/volumes/...`, FAT or exFAT sticks) the data is copied byte by
byte. FROM may be in a snapshot (`/.snapshots/NAME/...`), which restores
an old version. A copy gets the time of the moment it was made, unless
`-p` is given. `cp` has been in
RitterOS since step 19a.

## Options

| Option | Meaning |
|---|---|
| `-r`, `-R` | copy a directory with everything in it |
| `-p` | the copy (and with `-r` everything in it) keeps FROM's modification time and permissions; when root copies, also its owner and group (the setuid bit only with the owner, as POSIX's); since step 63m |

The options may be written together (`-rp`) or apart (`-r -p`). There is
no `--`: a FROM that starts with `-` is written `./-name`.

## Exit status and messages

- `0`: the copy was made.
- `1`:
  - `cp: FROM: No such file or directory` and the other reasons a file
    could not be read or written (`cp: TO: Permission denied`, `No space
    left on device`, ...)
  - `cp: FROM: Is a directory (use -r)`
  - `cp: FROM -> TO: REASON` when an instant copy failed, such as
    `Read-only file system` for a TO inside `/.snapshots`.
- `2`: an unknown option, or not exactly FROM and TO: `usage: cp [-rp]
  FROM TO`.

## Examples

```
ritteros:/$ cp /home/root/notes.txt /home/root/notes-old.txt
ritteros:/$ cp -r /home/root/project /volumes/STICK
ritteros:/$ cp /.snapshots/monday/home/root/notes.txt /home/root/notes.txt
ritteros:/$ cp -p /home/root/run.sh /home/anna
ritteros:/$ cp /home/root/project /tmp
cp: /home/root/project: Is a directory (use -r)
```

## See also

[`mv`](mv.md), [`rm`](rm.md), [`ln`](ln.md), [`tar`](tar.md),
[`snapshot`](snapshot.md), [`mount`](mount.md)

# versions: a file's versions in the snapshots and now

```
versions PATH
```

Shows a file's history: the file in every snapshot that has it
(`/.snapshots/NAME/PATH`), oldest first, then the file as it is now. Each
line gives the version's size and whether its content differs from the
version before it (`first version`, `changed`, `unchanged`). Snapshots are
made with `snapshot create NAME`; they share the blocks that did not
change, so old versions cost little room on the disk. To get an old
version back, copy it out of its snapshot: `cp
/.snapshots/monday/home/root/notes.txt notes.txt`.

PATH is the file's path as it is now (relative to the current folder, or
from `/`), never one inside `/.snapshots`. A snapshot without the file
shows `(not there)`; the next version found is then compared with nothing
before it. At most 64 snapshots are looked at. `versions` takes no
options.

## Exit status and messages

- `0`: the list was shown (also when the file is in no snapshot and gone
  now: every line says `(not there)`).
- `1`: `versions: PATH: give the path as it is now, not in a snapshot`,
  `versions: PATH: Is a directory` (folders have no versions here), or the
  snapshots could not be listed (`versions: REASON`).
- `2`: no PATH, or more than one: `usage: versions PATH`.
- A version that cannot be read for the comparison shows `(cannot be
  read)`.

## Examples

```
ritteros:/$ snapshot create monday
ritteros:/$ edit /home/root/notes.txt
ritteros:/$ versions /home/root/notes.txt
/home/root/notes.txt:
  monday (generation 12)              241 bytes  first version
  now                                  39 bytes  changed
ritteros:/$ cd /home/root
ritteros:/home/root$ versions notes.txt
```

## See also

[`snapshot`](snapshot.md), [`cp`](cp.md), [`ls`](ls.md) (`ls /.snapshots`),
[`attr`](attr.md)

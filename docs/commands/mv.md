# mv: move or rename a file or directory

```
mv FROM TO
```

Moves or renames a file or directory. If TO is a directory, FROM goes
into it under its own name (`mv notes.txt /home/root` makes
`/home/root/notes.txt`). Otherwise TO is the new name. Nothing is
copied on the same volume: only the directory entries change, in one
commit, so a large folder moves at once. A directory cannot be moved
into itself. `mv` has been in RitterOS since step 19a.

An existing TO is replaced, as POSIX's `mv` does (since step 68a): a
file by a file, an empty folder by a folder. A folder with something in
it is not replaced, nor a folder by a file or a file by a folder. On
RitterFS the old TO goes and FROM takes its name in the same commit, so
whoever opens TO finds the old file or the new one, never nothing: a
program can be updated safely by writing `prog.new` and moving it over
`prog`. FAT and exFAT volumes have no commits: there the old TO is
removed first. In a sticky folder (`/tmp`) the file being replaced must
be yours too, as for removing it. A program that still has the old file
open finds it ended at its next read, as after `rm` (Unix lets it read
the old content to its end). A running program is not affected: its
code was read into memory when it started.

To another volume (`/volumes/...`, a stick) the data cannot just be
re-entered: `mv` then removes an existing TO (by the rules above),
copies FROM byte by byte, folders with everything in them, and removes
FROM once the copy is complete. A copy that fails
halfway is removed again, so FROM is left as it was. A symbolic link is
moved itself, never what it names. A mounted volume's own folder
(`/volumes/STICK`) cannot be moved: unmount it, or mount it elsewhere.

`mv` takes no options.

## Exit status and messages

- `0`: moved.
- `1`: not moved:
  - `mv: FROM -> TO: No such file or directory`, `mv: FROM -> TO:
    Permission denied`, `mv: FROM -> TO: Operation not permitted` (someone
    else's file in a sticky folder), `mv: FROM -> TO: Invalid argument` (a
    folder into itself), `mv: FROM -> TO: Directory not empty`, `mv: FROM
    -> TO: Is a directory` (a file over a folder), `mv: FROM -> TO: Not a
    directory` (a folder over a file), `mv: FROM -> TO: Read-only file
    system`; TO here is the full target, with FROM's name added when TO is
    a directory;
  - `mv: FROM and TO are the same file` (one name moved onto itself, or
    onto another name of the same file);
  - `mv: FROM: a mounted volume (unmount it, or mount it elsewhere)`;
  - to another volume: `mv: PATH: WHY` for the file whose copy failed;
    `mv: copied to TO, but FROM: WHY` when the copy is complete but FROM
    could not be removed (both are there then).
- `2`: not two arguments: `usage: mv FROM TO`.

## Examples

```
ritteros:/$ mv notes.txt notes-old.txt
ritteros:/$ mv notes-old.txt /home/root/archive
ritteros:/$ mv /home/root/photos /volumes/STICK
ritteros:/$ mv sdrfi.new /system/x86_64/bin/sdrfi
ritteros:/$ mv /home/root/a /tmp
mv: /home/root/a -> /tmp/a: Directory not empty
```

## See also

[`cp`](cp.md), [`rm`](rm.md), [`ln`](ln.md), [`mount`](mount.md),
[`snapshot`](snapshot.md)

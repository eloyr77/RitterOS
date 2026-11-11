# rm: remove files (-r: directories with their contents; -f: missing ones are no error)

```
rm [-rf] PATH...
```

Removes each PATH. Without `-r` only files and links are removed; a
directory needs `rmdir` (when it is empty) or `-r`, which removes it with
everything in it. There is no undo, but removed files stay readable in
snapshots taken before (`/.snapshots`). A symbolic link is removed
itself, never what it names, and `-r` never follows one into a folder. A
file with several names (hard links) stays until its last name is
removed. `rm` has been in RitterOS since step 19a; `-f` came in step 63,
for what makefiles' recipes say.

`-r` never enters a mounted volume: it stops at a folder on another file
system (a stick mounted below the tree) with a message, so a stick is not
emptied by mistake, and a mount point itself (`/volumes/STICK`) is
refused. The other PATHs are still removed.

## Options

| Option | Meaning |
|---|---|
| `-r`, `-R` | also directories, with everything in them |
| `-f` | a PATH that is not there is no error and nothing is printed; `rm -f` with no PATH does nothing |
| `--` | the end of the options: what follows is a PATH even if it starts with `-` |

The options may be written together (`-rf`, `-fr`).

## Exit status and messages

- `0`: everything was removed.
- `1`: something was not (the others are still removed):
  `rm: PATH: No such file or directory`, `rm: PATH: Is a directory`
  (without `-r`), `rm: PATH: Permission denied`, `rm: PATH: Read-only
  file system`, `rm: PATH: a mounted volume (unmount it first)`.
- `2`: an unknown option, or no PATH without `-f`: `usage: rm [-rf]
  PATH...`.

## Examples

```
ritteros:/$ rm notes-old.txt
ritteros:/$ rm -r /home/root/build
ritteros:/$ rm -f *.o
ritteros:/$ rm /home
rm: /home: Is a directory
```

## See also

[`rmdir`](rmdir.md), [`mv`](mv.md), [`snapshot`](snapshot.md),
[`unmount`](unmount.md), [`ln`](ln.md)

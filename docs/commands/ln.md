# ln: make a link: a second name for a file, or with -s a symbolic link

```
ln [-s] [-f] TARGET [LINK] | ln [-s] [-f] TARGET... FOLDER
ln [-s] [-f] TARGET LINK
ln [-s] [-f] TARGET... FOLDER
ln [-s] [-f] TARGET
```

Without `-s`, LINK becomes a second name for the file TARGET (a hard
link): the same file under both names, so a change through one shows
through the other, and the file goes only when its last name is removed
(`ls -l` shows how many names it has). Hard links stay on one volume and
are not for folders. You must own the file, or be allowed to read and
write it.

With `-s`, LINK is a symbolic link: a small file that names TARGET, any
path, kept as written. Opening LINK opens TARGET; a relative TARGET is
taken from LINK's folder when the link is used (`ln -s ../lib/x
/test/a/b/x`), so a moved folder keeps its inner links. TARGET need not
exist (a link to nothing: `No such file or directory` until it does).
`rm LINK` removes the link, never TARGET. A path may pass through at most
40 links. `ls -l` shows a link as `name -> target` and its mode as
`lrwxrwxrwx`.

When the last name is a folder that exists, or more than two names are
given, each link is made inside that FOLDER under TARGET's last name. With
only TARGET, the link is made in the current folder. Links exist on
RitterFS only: FAT and exFAT volumes have none. `ln` has been in RitterOS
since step 57a.

## Options

| Option | Meaning |
|---|---|
| `-s` | make a symbolic link instead of a hard link |
| `-f` | remove a LINK that is there first (not a folder) |
| `--` | the end of the options: what follows is a name even if it starts with `-` |

Options may be combined: `-sf`.

## Exit status and messages

- `0`: every link was made.
- `1`: a link could not be made; the message names it: `ln: LINK: File
  exists` (without `-f`), `ln: LINK: Cross-device link` (a hard link to
  another volume), `ln: LINK: Operation not permitted` (a hard link to a
  folder, or a link on FAT or exFAT), `ln: LINK: No such file or
  directory` (a hard link's TARGET is missing), `ln: FOLDER: Not a
  directory` (several TARGETs and the last name is no folder). Into a
  folder the other links are still made.
- `2`: no name, or an unknown option: `usage: ln [-s] [-f] TARGET LINK`.

## Examples

```
ritteros:/$ ln -s /test/docs /home/root/docs
ritteros:/$ ls -l /home/root
lrwxrwxrwx   1     0     0         10  docs -> /test/docs
ritteros:/$ ln /home/root/notes.txt /home/root/notes-too.txt
ritteros:/$ ln -sf /test/new.txt /home/root/current.txt
ritteros:/$ ln -s /test/a.txt /test/b.txt /home/root
ritteros:/$ ln /home/root/notes.txt /volumes/STICK/notes.txt
ln: /volumes/STICK/notes.txt: Cross-device link
```

## See also

[`readlink`](readlink.md), [`ls`](ls.md), [`rm`](rm.md), [`cp`](cp.md),
[`mv`](mv.md)

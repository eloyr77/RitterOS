# chmod: set permissions (MODE in octal, e.g. 644)

```
chmod MODE PATH...
```

Gives each PATH the permissions MODE, an octal number: each of the three
groups (the owner, the group, everyone else) is r=4, w=2, x=1 added up, so
`644` is `rw-r--r--` (the owner reads and writes, the others read), `755`
is `rwxr-xr-x` and `600` is `rw-------`. `4000` added sets the setuid bit
(`4755`: the program runs as its owner for anyone, as `su` and `sudo`
do), `1000` the sticky bit (`1777` on a folder: everyone makes files in
it, but only their owners remove them, as in `/tmp`). Only a file's owner
or root may change its permissions. A symbolic link is followed: the file
it leads to changes. There are no letter forms (`u+x`); the whole mode is
always given.

Permissions came in step 20a, the setuid bit in 50b, the sticky bit in
57b. On FAT and exFAT volumes, which have no permissions, a mode without
the owner's `w` (`200`) sets the file's read-only attribute, and one with
it clears it.

## Exit status and messages

- `0`: every PATH was changed.
- `1`: a PATH could not be changed: `chmod: PATH: No such file or
  directory`, `chmod: PATH: Operation not permitted` (not your file); the
  others are still changed.
- `2`: no PATH, or MODE is not an octal number of the allowed bits (at
  most `777`, plus `4000` and `1000`; `2000`, setgid, does not exist):
  `usage: chmod MODE PATH... (MODE: octal, 0 to 777, plus 4000 for setuid
  and 1000 for the sticky bit; setgid, 2000, is not supported)`.

## Examples

```
ritteros:/$ chmod 600 /home/root/secret.txt
ritteros:/$ chmod 755 /home/root/bin/backup.sh
ritteros:/$ ls -l /home/root
-rw-------   1     0     0         42  secret.txt
drwxr-xr-x   1     0     0      <dir>  bin/
ritteros:/$ chmod 1777 /home/root/shared
ritteros:/$ chmod u+x notes.txt
usage: chmod MODE PATH... (MODE: octal, 0 to 777, plus 4000 for setuid and 1000 for the sticky bit; setgid, 2000, is not supported)
```

## See also

[`chown`](chown.md), [`ls`](ls.md) (`-l` shows the permissions),
[`id`](id.md), [`umask`](sh.md) (the shell's built-in), [`su`](su.md)

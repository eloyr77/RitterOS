# chown: give files to another user (root only)

```
chown UID[:GID] PATH...
```

Gives each PATH to the user numbered UID and, with `:GID`, to the group
GID. Users and groups are given by number, as `ls -l` shows them
(`chown 1000:1000 /home/anna/file`); `id` and `/system/config/users` tell
the numbers. Without `:GID` the group becomes the same number as the user
(`chown 1000 f` is `chown 1000:1000 f`), which is each user's own group
in RitterOS. Only root may give files away. A program given to another
owner loses its setuid bit, so a root-owned setuid program cannot be handed
to someone else and keep running with their rights. `chown` has been in
RitterOS since step 20a.

FAT and exFAT volumes have no owners: everything there stays root's.

## Exit status and messages

- `0`: every PATH was changed.
- `1`: a PATH could not be changed: `chown: PATH: REASON`
  (`Operation not permitted` for anyone but root, `No such file or
  directory`); the other PATHs are still changed.
- `2`: fewer than two arguments, or UID[:GID] is not one or two decimal
  numbers (of at most 32 bits): `usage: chown UID[:GID] PATH...`.

## Examples

```
ritteros:/$ useradd anna
useradd: anna: user ID 1000, home /home/anna; set a password with: passwd anna
ritteros:/$ cp /readme.txt /home/anna/
ritteros:/$ chown 1000:1000 /home/anna/readme.txt
ritteros:/$ ls -l /home/anna
-rw-r--r--   1  1000  1000       1325  readme.txt
ritteros:/$ chown anna /home/anna/readme.txt
usage: chown UID[:GID] PATH...
```

## See also

[`chmod`](chmod.md), [`id`](id.md), [`ls`](ls.md) (`-l`),
[`useradd`](useradd.md)

# useradd: add a user with a home directory (root only)

```
useradd NAME
```

Adds the user NAME. Only root may do it. The new user gets the next free
user ID from 1000 on, a group of its own with the same number, and the
home directory `/home/NAME`, which it owns (mode 0755; `/home` is made if
it is missing). The user is a line `NAME:UID:GID:/home/NAME` in
`/system/config/users`, which everyone may read. The new user has no
password yet and cannot log in until `passwd NAME` sets one. Names are
1-31 characters of `a-z`, `0-9`, `_` and `-`, not starting with a digit or
`-`. `useradd` has been in RitterOS since step 20b; since step 32 its file
is in `/system/config`.

## Exit status and messages

- `0`: the user was added: `useradd: NAME: user ID UID, home /home/NAME;
  set a password with: passwd NAME`.
- `1`:
  - `useradd: NAME: names are 1-31 of a-z 0-9 _ - (not starting with a
    digit or -)`
  - `useradd: only root may add users`
  - `useradd: NAME: the user exists`
  - `useradd: /home: REASON`, `useradd: /system/config/users: REASON` or
    `useradd: /home/NAME: REASON` when a file or folder could not be
    written.
- `2`: no NAME, or more than one: `usage: useradd NAME`.

## Examples

```
ritteros:/$ useradd anna
useradd: anna: user ID 1000, home /home/anna; set a password with: passwd anna
ritteros:/$ passwd anna
ritteros:/$ useradd anna
useradd: anna: the user exists
ritteros:/$ useradd 9lives
useradd: 9lives: names are 1-31 of a-z 0-9 _ - (not starting with a digit or -)
ritteros:/$ ls -l /home
drwx------   1     0     0      <dir>  root/
drwxr-xr-x   1  1000  1000      <dir>  anna/
```

## See also

[`passwd`](passwd.md), [`id`](id.md), [`su`](su.md), [`login`](login.md),
[`chown`](chown.md)

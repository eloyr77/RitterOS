# passwd: set a user's password: anyone's for root, else your own

```
passwd [-S] [NAME]
```

Sets the password of the user NAME, or without NAME your own. It asks for
the new password twice (`New password:`, `Again:`), without showing what
is typed. (69f, the user's) Root sets anyone's (`useradd` makes a user,
root then gives the password); any other user sets only their own, and is
asked for it first (`Current password:`): `passwd` is a setuid program,
as `su` and `sudo` (root's, mode 4755: `ls -l` shows `rws`), so it may
write the passwords file for them. A wrong present password is refused
after a pause of 2 seconds (guessing stays slow); every change and every
such refusal goes to the log `login`. A password is stored only as a
salted hash (PBKDF2-HMAC-SHA256, 100,000 rounds, our own code) in
`/system/config/passwords`, which only root can read; the users are in
`/system/config/users`. At most 127 characters count; an empty password is
refused. `passwd` has been in RitterOS since step 20b.

`-S` (69f, as `sudo -S`) reads the lines from standard input without
asking and without touching the terminal's echo: the present password (not
for root), then the new one twice. The settings app's Account and Users
pages use it.

**The keyring** (70b) follows your own new password: its key is sealed
again under it, by `keyringd` when it runs, else in `~/.config/keyring.db`
itself (as you). Root, who is not asked the present password, cannot do
that: when root sets a password, that user's keyring keeps the old one
until `keyring password`.

## Exit status and messages

- `0`: `passwd: password for NAME set`, then for the keyring: `passwd:
  the keyring follows the new password`; `passwd: the keyring did not open
  with the old password; it keeps its own`; `passwd: the keyring keeps the
  old password (not saved)` (or `(keyringd: not saved)`); for root:
  `passwd: NAME's keyring keeps its old password (keyring password changes
  it)`.
- `1`: `passwd: NAME: no such user`; `passwd: only root may set another
  user's password`; `passwd: wrong password; nothing changed`; `passwd: the
  two differ; nothing changed`; `passwd: an empty password is not
  allowed`; the input ended while it asked (Ctrl-D); `passwd:
  /system/config/passwords: ...` (the file could not be written); `passwd:
  it does not run as root (it must be root's, with mode 4755)`.
- `2`: more than one NAME: `usage: passwd [-S] [NAME]`.

## Examples

```
ritteros:/$ useradd anna
useradd: anna: user ID 1000, home /home/anna; set a password with: passwd anna
ritteros:/$ passwd anna
New password:
Again:
passwd: password for anna set
ritteros:/$ passwd bob
passwd: bob: no such user
```

anna, her own:

```
ritteros:/home/anna$ passwd
Current password:
New password:
Again:
passwd: password for anna set
```

## See also

[`useradd`](useradd.md), [`login`](login.md), [`su`](su.md),
[`sudo`](sudo.md), [`id`](id.md), [`Settings`](Settings.md)

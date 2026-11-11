# passwd: set a user's password (root only so far)

```
passwd [NAME]
```

Sets the password of the user NAME, or without NAME your own. It asks for
the new password twice (`New password:`, `Again:`), without showing what
is typed. Root only so far: `useradd` makes a user, root then gives the
password. A password is stored only as a salted hash (PBKDF2-HMAC-SHA256,
100,000 rounds, our own code) in `/system/config/passwords`, which only
root can read; the users are in `/system/config/users`. At most 127
characters count; an empty password is refused. `passwd` has been in
RitterOS since step 20b.

## Exit status and messages

- `0`: `passwd: password for NAME set`.
- `1`: `passwd: NAME: no such user`; `passwd: only root may set passwords
  so far`; `passwd: the two differ; nothing changed`; `passwd: an empty
  password is not allowed`; the input ended while it asked (Ctrl-D);
  `passwd: /system/config/passwords: ...` (the file could not be written).
- `2`: more than one NAME: `usage: passwd [NAME]`.

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

## See also

[`useradd`](useradd.md), [`login`](login.md), [`su`](su.md),
[`sudo`](sudo.md), [`id`](id.md)

# su: become another user (root if none) with their password

```
su [USER]
```

Asks USER's password (root's, when no USER is named) and starts a shell
(`sh`) as that user in the current folder; `exit` (or Ctrl-D) ends it and
you are yourself again. Root is not asked. The new shell is a session of
its own in the foreground, as `login` starts shells. A wrong password
gives `su: wrong password` after a pause of 2 seconds, so guessing stays
slow.

Every use is written to the log `login` (`/system/logs/login.log`): `su:
anna became bob`, or `su: anna to bob refused (a wrong password)`, with any
control characters in what was typed shown as `?`. `su` is a setuid
program: owned by root with the mode `4755`, it runs as root whoever starts
it. It has been in RitterOS since steps 50b-50c.

## Exit status and messages

- The shell's own exit status when it ends.
- `1`: `su: wrong password`; `su: no user NAME`; `su: it does not run as
  root (it must be root's, with mode 4755)` (its setuid bit was taken
  away, for example by a copy or `chown`); the input ended while it asked
  for the password; `su: /system/x86_64/bin/sh: ...` (the shell could not
  be started).
- `2`: more than one USER: `usage: su [USER]`.

## Examples

```
ritteros:/home/anna$ su bob
Password:
ritteros:/home/anna$ id
uid=1001(bob) gid=1001
ritteros:/home/anna$ exit
ritteros:/home/anna$ su
Password:
su: wrong password
ritteros:/home/anna$ su carol
su: no user carol
```

## See also

[`sudo`](sudo.md) (one command as root), [`login`](login.md),
[`passwd`](passwd.md), [`id`](id.md), [`chmod`](chmod.md) (the setuid
bit)

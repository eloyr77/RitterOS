# keyringd: the elf that keeps your keyring open (login starts it); -s the system's

```
keyringd [-s]
```

`keyringd` holds a user's keyring (`~/.config/keyring.db`, see `keyring`)
open, so programs reach their secrets through `libkeyring.so.1`
(`keyring.h`) without asking for a password. There is one per user,
running as that user and listening on the local socket `/run/keyring/UID`.
It came with step 70b (item 48).

`login` starts it at a login when it does not run, gives it the login
password and holds a session open with it while the shell runs; a second
login of the same user finds it running and does the same. `keyring
unlock` starts it when it does not run (over SSH, which asks no password,
or with `open_at_login = no`). `keyringd` starts locked. An unlock with a
password opens the keyring; when there is no file yet it makes a new,
empty keyring under that password, written only when something is stored
in it. It locks (forgets the keyring's key, the file stays) when the last
login session ends, at `keyring lock`, and after `lock_after` minutes
without use; locked it keeps running, for the next login.

(70c) It tells programs apart by the file each runs (the kernel's
`SYS_PEER_PROGRAM`: the program at the other end of a local socket,
taken when it connected). An entry names the programs that may use it,
the one that stored it first. When another program asks for it, replaces
or removes it, a child of keyringd opens the window `Keyring` (Allow,
Deny) through windowd (`/run/windowd`, or `$WINDOW_SOCKET` in keyringd's
environment), while keyringd goes on serving the others; one question at
a time (a request while one is open is refused). Allow adds the program
to the entry and saves it; Deny is kept until the keyring locks (asked
again then, the program is refused at once). Unanswered within 50
seconds, or without a window system, the request is refused. `keyring
allow PROGRAM SERVICE` adds a program to a service's entries, given the
keyring's password.

**`-s`** (70d) keeps the system keyring instead: `/system/config/keyring.db`
on `/run/keyring/system`, an elf (`/system/config/elves/keyringd`,
`command = keyringd -s`, made by login when missing). It runs as root and
opens the keyring at its start with the 32 bytes of
`/system/config/keyring.key`; with neither file it starts a new keyring
under a new random key and writes both (the key file first, mode 0600)
when the first entry is stored. Only root's programs may use it
(`keyring_system()` in libkeyring, `keyring -s`), each every entry: no
rule per program, no window, no lock, unlock, password or allow.

Only its own user may ask it for anything. Root (login, passwd) may only
open it, change its password, hold a session and ask whether it is open:
each needs the password or tells nothing. Since any program may bind any
local socket name, a client first checks that its own user is at the
other end, and refuses otherwise.

`keyringd` writes the keyring's file alone (as `keyring.db.new`, then
renamed over it); a change it cannot save is undone in memory and its
reason given to the program. It writes `~/.config/keyring` with its
defaults when the file is missing:

| Setting | Meaning |
|---|---|
| `open_at_login = yes` or `no` | login opens the keyring with your password (`yes` if not set); read by login |
| `lock_after = MINUTES` | lock after this many minutes without use; `0` (the default) keeps it open until your last login ends |

What it does goes to the log `keyring` (opened, locked and why, a password
change, an unlock that failed), never a password or a secret.

## Exit status and messages

- It runs until the machine stops (or a signal ends it).
- `1`: `keyringd: /run/keyring/UID: ` and the reason, when it cannot
  listen (another keyringd of the user runs, or another user holds the
  name); `keyringd: no home folder`.
- `1` with `-s`: `keyringd: -s: the system keyring is root's` (not run as
  root).
- `2`: `usage: keyringd [-s]`.

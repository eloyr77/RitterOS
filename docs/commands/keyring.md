# keyring: your passwords, kept encrypted (~/.config/keyring.db)

```
keyring [-s] add|get|remove SERVICE ACCOUNT | keyring [-s] list | keyring lock|unlock|password | keyring allow PROGRAM SERVICE
keyring add SERVICE ACCOUNT
keyring get SERVICE ACCOUNT
keyring remove SERVICE ACCOUNT
keyring list
keyring lock
keyring unlock
keyring password
keyring allow PROGRAM SERVICE
keyring -s add|get|remove SERVICE ACCOUNT
keyring -s list
```

The keyring keeps secrets (passwords, tokens) for you, each under a
service and an account: `keyring add mail.example.org anna` asks for the
secret and keeps it, `keyring get mail.example.org anna` prints it.
Everything is in one file, `~/.config/keyring.db`, encrypted as a whole
and readable only by you (mode 0600). The file's key is sealed under a key
made from the keyring's password, so without the password the file tells
nothing, not even the services' names. `docs/keyring.md` describes the
file. The keyring came with step 70a (item 48); keyringd with 70b.

The elf `keyringd` holds the keyring open, and `keyring`, like any program
(through `libkeyring.so.1`), asks it: no password is asked for add, get,
remove and list. `login` opens it with your login password when you log
in at the machine (unless `~/.config/keyring` says `open_at_login = no`),
and it locks when your last login ends. Your first login makes a new,
empty keyring under your login password (its file is written when you
first store something). Over SSH, which asks no password, `keyring unlock`
opens it. `passwd` changing your own password changes the keyring's with
it; when root sets your password, the keyring keeps the old one until
`keyring password`.

**Which program may use an entry** (70c). keyringd knows each program
that asks by the file it runs (`/system/x86_64/bin/keyring`,
`/system/x86_64/apps/Compass.program/Compass`...; a script by its
interpreter, `sh`). The program that stores an entry may use it; another
one that asks for it, replaces or removes it waits while keyringd asks
you in a window, `Keyring`: "The program PATH asks for SERVICE ACCOUNT
from your keyring", Allow or Deny. Allow is kept in the entry (that
program is not asked about it again); Deny is kept until the keyring
locks. Without the window system (at the console, over SSH) such a
request is refused, and `keyring allow PROGRAM SERVICE`, with the
keyring's password, lets a program use every entry of a service. `keyring`
itself is such a program: `keyring get` of an entry another program stored
asks too. `keyring list` shows every entry's names to any of your
programs (never a secret). Entries stored before 70c name no program:
every program may use them.

A program that drives the window system for you (`wsend` sends clicks)
could answer the window itself: the window keeps out programs that ask by
mistake or are only half trusted, not one of your own programs that sets
out to read the keyring.

**The system keyring** (70d, `-s`) keeps root's programs' secrets (Wi-Fi
passphrases, later) in `/system/config/keyring.db`, kept open by the elf
`keyringd -s` from the boot (`/system/config/elves/keyringd`, which login
makes). It has no password: its key is the 32 random bytes of
`/system/config/keyring.key` (root's, mode 0600), so copying the database
alone tells nothing, while anyone holding the whole disk can open it
(until the system disk is encrypted). Both files are written when the
first entry is stored. Only root may use it (`keyring -s get ...` as root),
and every root program every entry: root can read the key file anyway, so
there is no rule per program and no window. `-s` takes add, get, remove
and list.

A service and an account are 1 to 255 bytes without control characters
(spaces are allowed: quote them); a secret is one line of at most 4,096
bytes. On a terminal what is typed is not shown, and a new secret or
password is asked twice; when the input is a pipe, its lines are read in
the same order without prompts, each once (`printf 'SECRET\n' | keyring
add SERVICE ACCOUNT`).

## Options

| Command | Meaning |
|---|---|
| `add SERVICE ACCOUNT` | keep a secret: a new entry, or the entry's new secret |
| `get SERVICE ACCOUNT` | print the secret (and a line break) |
| `remove SERVICE ACCOUNT` | take the entry out |
| `list` | each entry's service and account, a tab between them (never a secret) |
| `lock` | keyringd forgets the keyring's key (the file stays) |
| `unlock` | open it with its password (`Keyring password:`), starting keyringd when it does not run |
| `password` | change the keyring's password: the present one, then the new one (the entries stay as they are) |
| `allow PROGRAM SERVICE` | let PROGRAM (a path, or a name in `/system/<arch>/bin` or `apps`) use SERVICE's entries; asks the keyring's password |

## Exit status and messages

- `0`: `keyring: SERVICE ACCOUNT kept`, the secret (`get`), the list
  (`keyring: no entries` when empty), `keyring: SERVICE ACCOUNT removed`,
  `keyring: locked` (`keyring: locked (keyringd does not run)`),
  `keyring: open`, `keyring: the password changed`, `keyring: PATH may
  use SERVICE (N entries)`.
- `1`: `keyring: SERVICE ACCOUNT: not in the keyring`; `keyring: SERVICE
  ACCOUNT: not allowed to this program (keyring allow PROGRAM SERVICE)`;
  `keyring: PROGRAM: no such program`; `keyring: no such entries (only
  entries a program stored name programs)`; `keyring: locked
  (keyring unlock opens it)`; `keyring: not open: keyringd does not run
  (keyring unlock starts it)`; `keyring: wrong password (or the file is
  damaged)` (the two cannot be told apart: the file's seal fails either
  way); `keyring: another user holds keyringd's name (/run/keyring/UID)`;
  `keyring: the two differ; nothing changed`; `keyring: an empty password
  is not allowed`; `keyring: a secret is at most 4096 bytes`; `keyring: a
  service or an account is 1-255 bytes without control characters`;
  `keyring: not saved: ` and the reason.
- `1` with `-s`: `keyring: the system keyring is root's (or it did not
  open: the log keyring says why)`; `keyring: the system keyring's
  keyringd does not run (elf start keyringd)`; `keyring: the system
  keyring has no password: -s takes add, get, remove and list`.
- `2`: `usage: keyring [-s] add|get|remove SERVICE ACCOUNT | keyring [-s]
  list | keyring lock|unlock|password | keyring allow PROGRAM SERVICE`.

## Notes

- A save writes `keyring.db.new` whole, commits it to the disk and
  renames it over the old file: a crash leaves the old keyring or the new
  one, never half of either.
- The password is stretched by 600,000 rounds of PBKDF2-HMAC-SHA256, so
  each guess at a copied file costs as much as opening it here: an unlock
  takes that time once.
- Programs use the keyring through `keyring.h` (`keyring_get`,
  `keyring_set`, `keyring_remove`, `keyring_list`; the SDK's guide).

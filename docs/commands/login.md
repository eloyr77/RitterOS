# login: log in: a user's name and password, then their shell

```
login
```

The program behind `login:`. The kernel starts it at the end of the boot;
you do not run it yourself. It asks for a user's name and password, then
starts that user's shell, and when the shell ends (`exit`, or Ctrl-D on
an empty line) it asks again. `login` came in step 20c.

**At its start**, before the first `login:`, it gets the system ready, in
this order:

1. Disks from before step 32 keep `users` and `passwords` in `/system`:
   they are moved to `/system/config` (`login: /system/users moved to
   /system/config/users`).
2. The elf files of `logd` (the logs' keeper) and `netnamesd` (names new
   network cards) are made in `/system/config/elves/` when they are
   missing (`login: /system/config/elves/logd made: the elf logd keeps the
   logs in /system/logs`).
3. If the machine has a network card, `net up` runs: the cards' settings
   from `/system/config/network/`, DHCP for a card without one.
4. If `/system/config/elves/` has files, `elvesd` starts (unless `net up`
   started it already), which starts the elves.
5. **First start:** on a new disk root has no password yet, and `login`
   asks for one, twice; it is never shown while typed. An empty password
   is not taken. The password is kept in `/system/config/passwords`
   (PBKDF2-HMAC-SHA256, 100,000 rounds).

**Logging in.** `login:` takes the user's name, `Password:` the password
(not shown). A wrong password and an unknown name both give `Login
incorrect.` and take the same time. Then `login` starts the user's shell
`sh -l` as that user, in their home directory (`/home/NAME`; root's is
made at its first use), as the leader of a session of its own, with
`HOME`, `USER`, `LOGNAME` and `SHELL` set. As a login shell, `sh -l` runs
`~/.config/login` first. The user's settings folder `~/.config` is made
when it is missing, with a `README` and the files `login` and `windows`,
every line a comment until the user changes it (a file the user deleted
comes back). Logins, refused logins and logouts are written to the log
`/system/logs/login.log`.

**Leaving.** Ctrl-D at `login:` ends `login`, and the kernel's rescue
console (`rescue>`) takes over. Ctrl-C at `login:` only asks
again.

`login` takes no options.

## Exit status and messages

- `0`: Ctrl-D at `login:` or `Password:` (the rescue console follows).
- `1`: the first start was not finished (Ctrl-D at its questions, or the
  password could not be stored).

Messages:

- `RitterOS: first start. Choose a password for root, the
  administrator.`, `New password for root: `, `Again: `, then `The
  password for root is set.`; `The two differ; once more.`, `An empty
  password is not allowed.`
- `[NAME logged in]`, `[NAME logged out]`, `Login incorrect.`
- `login: HOME: No such file or directory; starting in /` when the home
  directory is missing (the shell then starts in `/`).
- `login: PATH: WHY` when a file or folder could not be made or a program
  could not be started (`login: /system/config/passwords: Read-only file
  system`).

## Examples

A new disk's first start:

```
RitterOS: first start. Choose a password for root, the administrator.
New password for root:
Again:
The password for root is set.

login: root
Password:
[root logged in]
ritteros:/home/root$ exit
[root logged out]

login:
```

## See also

[`sh`](sh.md), [`passwd`](passwd.md), [`useradd`](useradd.md),
[`su`](su.md), [`elvesd`](elvesd.md), [`net`](net.md), [`logd`](logd.md)

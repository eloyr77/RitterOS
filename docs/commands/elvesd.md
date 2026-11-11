# elvesd: keeps the elves running (login starts it): /system/config/elves/

```
elvesd
```

Keeps the elves running. An elf is a program RitterOS starts at every
boot and starts again whenever it ends: a server like `sshd`, the logs'
keeper `logd`, the DHCP client `dhclient`. (Other systems call them
services or daemons.) Each elf has a file in `/system/config/elves/`,
named after it:

```
command = sshd -p 22
start = on
```

`command` is the program and its words (at most 16; a name without `/`
is a program in `/system/<arch>/bin`, next to `elvesd`); `start = on`
keeps it running, anything else leaves it stopped. Files whose names
begin with `.` are left out; at most 32 elves are kept.

Every 2 seconds `elvesd` reads the folder. An elf that is on and not
running is started: as root, in a process group and session of its own,
its output on the console and in its log (`/system/logs/NAME.log`,
written by `logd`). One that ended is started again after a wait: 2
seconds, doubled each time (up to 60) while it keeps ending within 10
seconds of its start; one that ran longer waits 2 seconds again. One that
is switched off, or whose file is gone, is stopped: SIGTERM, then SIGKILL
if it still runs 5 seconds later. `elvesd` itself writes no files: the
program `elf` changes them, and `elvesd` acts on the change within 2
seconds. Its own lines go to the console and to
`/system/logs/elvesd.log`. `elvesd` came in step 43b.

You do not run `elvesd` yourself: `login` starts it at the boot when the
folder has files (every system has `logd`'s, so in practice at every
boot), and `elf add` or `elf start` start it when it is not running.
Ctrl-C does not end it (it ignores SIGINT); it runs until the machine
stops.

`elvesd` takes no options; arguments are ignored.

## Exit status and messages

`elvesd` does not end by itself. Its lines:

- `elvesd: NAME started (process PID)`
- `elvesd: NAME ended (exit code N); starting it again in S s`, or
  `(signal N)` for an elf ended by a signal
- `elvesd: NAME stopped` (switched off or its file removed)
- `elvesd: NAME: PATH: WHY; again in S s`: the program could not be
  started (`No such file or directory` for a wrong command)
- `elvesd: NAME: no command = ... in its file`

## Examples

```
ritteros:/$ elf
elvesd: running (process 3)
dhclient         on   running (process 4)     dhclient
logd             on   running (process 5)     logd
sshd             on   running (process 6)     sshd -p 22
ritteros:/$ elf add crash false
elf: crash added, on (elvesd starts it within 2 s)
elvesd: crash started (process 9)
elvesd: crash ended (exit code 1); starting it again in 2 s
elvesd: crash started (process 10)
elvesd: crash ended (exit code 1); starting it again in 4 s
ritteros:/$ elf remove crash
elf: crash removed (elvesd stops it within 2 s)
ritteros:/$ cat /system/logs/elvesd.log
```

## See also

[`elf`](elf.md), [`login`](login.md), [`logd`](logd.md), [`sshd`](sshd.md),
[`dhclient`](dhclient.md), [`dnsd`](dnsd.md), [`ps`](ps.md)

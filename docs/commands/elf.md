# elf: the elves: programs kept running (/system/config/elves/; changes: root)

```
elf [start|stop|restart|remove NAME | add NAME COMMAND...]
elf
elf start|stop|restart|remove NAME
elf add NAME COMMAND...
```

An elf is a program RitterOS starts at every boot and starts again
whenever it ends: a server like `sshd`, the DHCP client `dhclient`, the
logs' keeper `logd`. (Other systems call them services or daemons.) Each
elf has a file in `/system/config/elves/`, named after it:

```
command = sshd -p 22
start = on
```

`command` is the program and its words (a name without `/` is a program of
the system, found like the shell finds it); `start = on` keeps it running,
`off` leaves it stopped. The elf `elvesd`, which `login` starts at the
boot, looks at the folder every 2 seconds: it starts an elf that is on and
not running, stops one that is off or whose file is gone (SIGTERM, then
SIGKILL after 5 seconds), and starts one that ended again after a wait (2
seconds, doubled up to 60 while it keeps ending within 10 seconds of its
start). The elves' output appears on the console and in their logs,
`/system/logs/NAME.log`. Elves came with step 43b.

`elf` alone lists the elves: first whether `elvesd` runs, then a line per
elf with its name, on or off, whether it is running (its process) and its
command. Everyone may look; only root may change them. Every change is for
the boots to come too, and `elvesd` acts on it within 2 seconds; `elf`
starts `elvesd` itself when an elf is switched on or added and `elvesd` is
not running.

An elf's process is found by its program's name among `elvesd`'s
children, so two elves of the same program show as one `running (2
processes)`, and `restart` then asks to stop and start instead.

## Options

| Command | Meaning |
|---|---|
| `start NAME` | switch it on: `start = on` in its file |
| `stop NAME` | switch it off: `start = off` (elvesd ends it) |
| `restart NAME` | end its process (SIGTERM); elvesd starts it again |
| `remove NAME` | delete its file; elvesd stops it |
| `add NAME COMMAND...` | make an elf, on: its file with `command = COMMAND...` |

NAME is 1-63 letters, digits, `-`, `_` and `.`, not starting with a dot.

## Exit status and messages

- `0`: listed, or changed, with a line saying what happens next: `elf: NAME
  on (elvesd starts it within 2 s)`, `elf: NAME off (elvesd stops it within
  2 s)`, `elf: NAME: process N ended (elvesd starts it again)`, `elf: NAME
  removed (elvesd stops it within 2 s)`, `elf: NAME added, on (elvesd
  starts it within 2 s)`; and `elf: elvesd started (process N)` when it
  started elvesd. A system without elves lists `no elves (root adds one:
  elf add NAME COMMAND...)`.
- `1`:
  - `elf: NAME: not a name for an elf (letters, digits, - _ .)`
  - `elf: start: only root may change the elves` (with the command given:
    `stop`, `add`, ...)
  - `elf: NAME: no such elf`
  - `elf: NAME: there is such an elf already` (add)
  - `elf: NAME: not running` (restart)
  - `elf: NAME: its program runs for several elves: stop and start it
    instead` (restart)
  - `elf: PATH: REASON`: the elf's file could not be read or written.
- `2`: an unknown command, or the wrong number of words: `usage: elf
  [start|stop|restart|remove NAME | add NAME COMMAND...]`.

## Examples

```
ritteros:/$ elf
elvesd: running (process 2)
dhclient         on   running (process 5)     dhclient
dnsd             on   running (process 6)     dnsd
logd             on   running (process 4)     logd
sshd             on   running (process 7)     sshd
ritteros:/$ elf stop sshd
elf: sshd off (elvesd stops it within 2 s)
ritteros:/$ elf start sshd
elf: sshd on (elvesd starts it within 2 s)
ritteros:/$ elf restart dhclient
elf: dhclient: process 5 ended (elvesd starts it again)
ritteros:/$ elf add soundd soundd
elf: soundd added, on (elvesd starts it within 2 s)
ritteros:/$ elf remove soundd
elf: soundd removed (elvesd stops it within 2 s)
```

## See also

[`elvesd`](elvesd.md), [`logd`](logd.md), [`dhclient`](dhclient.md),
[`wifid`](wifid.md), [`sshd`](sshd.md), [`ps`](ps.md), [`kill`](kill.md)

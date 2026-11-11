# guardd: the firewall's elf: loads /system/config/guard into the kernel

```
guardd
```

`guardd` is an elf: a program `elvesd` keeps running (other systems call
such programs services or daemons). The kernel checks every packet that
comes in for the machine or goes out of it on a network card against the
firewall's rules; `guardd` reads those rules from the file
`/system/config/guard` and loads them into the kernel, and loads them again
whenever the file changes (it looks every 2 seconds). It came in step 45d;
the rules themselves, and the command [`guard`](guard.md) that shows and
changes them, are described on `guard`'s page and in the manual's section
"The firewall".

At the boot the kernel lets nothing in (what goes out is let through, so
DHCP works) until `guardd` has loaded the rules. Its first start makes the
file when there is none: what comes in is dropped, what goes out is
allowed, and ping and the elves that are there to be reached are let in
(`sshd` on tcp 22, `dnsd` on udp 53, each only if its elf file exists):

```
# The firewall's rules (guardd made this file; see the manual, "guard").
# Read top to bottom: the first rule that matches a packet decides; the
# replies to the machine's own connections always pass.
in = drop
out = allow
allow in icmp                # ping
allow in tcp 22              # sshd
allow in udp 53              # dnsd
```

A file with a mistake is not loaded at all: the rules loaded before stay
(or, at the boot, nothing comes in until the file is right), and `guardd`
names the line, once for each version of the file. A file removed later
leaves the loaded rules as they are. `guardd` takes no options.

## Starting and stopping

Its elf file is `/system/config/elves/guardd`:

```
# The firewall, an elf (net up made this file)
command = guardd
start = on
```

`net up` (which `login` runs at every start on a machine with a network
card) makes it when it is missing. Then, as root:

| Command | Does |
|---|---|
| `elf stop guardd` | switches the firewall off: `guardd` tells the kernel to check nothing, also after the next boots (`start = off`; `net up` switches the checks off too) |
| `elf start guardd` | switches it on again: the rules are loaded |
| `elf restart guardd` | ends it and `elvesd` starts it again; the rules stay loaded meanwhile |

## Exit status and messages

- `guardd: /system/config/guard made: in = drop, out = allow; let in: icmp,
  tcp 22 (sshd), udp 53 (dnsd)`: its first start made the file.
- `guardd: 3 rule(s) loaded (in = drop, out = allow)`: the file was loaded.
- `guardd: /system/config/guard: line 10: 'sideways': in, out or forward is
  missing; the rules loaded before stay`: a mistake; the end of the line
  is `what comes in is dropped until it is right` at the boot, or `nothing
  is checked until it is right` while the firewall is off.
- `guardd: the kernel took no rules: REASON`.
- `guardd: /system/config/guard: REASON`: the file could not be made.
- `guardd: switched off: nothing is checked now`: after `elf stop guardd`.
- `0`: it ends only when stopped (it ignores SIGTERM and looks at its elf
  file instead: off or gone, it switches the checks off; on, as for a
  restart, it leaves the rules).

## Examples

```
ritteros:/$ net up
...
elvesd: guardd started (process 7)
guardd: /system/config/guard made: in = drop, out = allow; let in: icmp, tcp 22 (sshd), udp 53 (dnsd)
guardd: 3 rule(s) loaded (in = drop, out = allow)
ritteros:/$ guard add allow in tcp 8080 from 192.168.0.0/24
guard: rule 4: allow in tcp 8080 from 192.168.0.0/24; guardd loads it within 2 s
guardd: 4 rule(s) loaded (in = drop, out = allow)
ritteros:/$ elf stop guardd
guardd: switched off: nothing is checked now
```

## See also

[`guard`](guard.md), [`elf`](elf.md), [`elvesd`](elvesd.md),
[`net`](net.md), [`sshd`](sshd.md), [`dnsd`](dnsd.md)

# guard: the firewall's rules: show them, change them

```
guard [add RULE | insert N RULE | remove N | in|out|forward drop|allow]
```

`guard` alone shows the firewall as the kernel uses it: whether it is on,
the rules numbered, how many packets each decided since the rules were
last loaded, the three defaults, the packets of connections already let
through, and the last packets dropped (up to 20). Everyone may look.

Root changes the rules with it. Each change goes into the rules file
`/system/config/guard` (comments and the other lines stay as they are),
and the elf `guardd` loads the file into the kernel within 2 seconds. A
rule is checked before it is written, so a mistake never reaches the
file. `guard` has been in RitterOS since step 45e; the `forward` rules
came in step 49h.

**The rules.** Every packet that arrives for the machine on a network
card, or leaves it on one, is checked (`lo`, the machine talking to
itself, never is); with forwarding on (`net forward on`), so is every
packet passed between two cards. The file holds one rule per line, read
top to bottom: the first rule that matches decides. If none does, the
default of the packet's direction decides. Only the first packet of each
connection is checked: its further packets, both ways, pass, and so do the
answers to the machine's own connections (a web page, a DNS answer,
DHCP's offer).

**IPv6** (since 64e). The same file holds the rules of both families: a
rule that names no address counts for IPv4 and IPv6 alike (`allow in tcp
22`), one with an IPv4 address only for IPv4, one with an IPv6 address
(`from fe80::/10`, `to 2001:db8::5`) only for IPv6; a rule cannot name an
address of each. The kernel keeps a table per family, each in the file's
order, so "the first rule that matches decides" holds for both. The
defaults are shared. `icmp` covers ICMPv6 too. Neighbour discovery, router
advertisements, MLD and ICMPv6's error messages always pass: IPv6 does not
work without them. `guard` lists every rule once, numbered as in the file;
a rule without an address shows the packets of both families, and the
list of drops holds both, the newest first.

```
in = drop                    # what comes in when no rule matches (drop or allow)
out = allow                  # what goes out when none does
forward = drop               # what is forwarded between cards when none does
allow in icmp                # ping
allow in tcp 22              # sshd
allow in tcp 8000-8010 from 192.168.0.0/24 on eth0
limit in tcp 22 10/minute
drop in from 10.0.0.0/8
allow forward icmp from 192.168.1.0/24 in eth0 out eth1
```

A rule is `allow`, `drop` or `limit`, then the direction, then any of
the parts below, in this order for the protocol and port, the others in
any order:

| Part | Means |
|---|---|
| `allow`, `drop` | let the packet through, or drop it without an answer (the sender waits until it gives up) |
| `limit ... N/minute` | let at most N new connections a minute through, counted per other machine (one guessing passwords uses up its own allowance only); the rate is the rule's last word |
| `in`, `out`, `forward` | arriving for the machine, leaving it, or passing between two cards |
| `tcp`, `udp`, `icmp` | only this protocol (without: all) |
| `PORT`, `PORT-PORT` | after `tcp` or `udp`: coming in, the machine's port; going out or forwarded, the other side's (1-65535) |
| `from ADDRESS[/N]`, `to ADDRESS[/N]` | only from or to these addresses (`/24`: the first 24 bits must match); IPv4 or (64e) IPv6, `from 2001:db8::/32`: then the rule is that family's only |
| `on CARD` | an `in` or `out` rule: only on this card, `ethN`, `wlanN` (Wi-Fi cards since 50k), `tapN` or `brN` (virtual cards and bridges since 38b) |
| `in CARD`, `out CARD` | a `forward` rule: only entering on, or leaving on, this card (the same names) |
| `#` | a comment, to the end of the line |

A file holds at most 64 rules. A new system drops what comes in and
allows what goes out; `guardd`'s first start writes the file, letting in
ping and the elves that are there to be reached (`sshd` on TCP 22, `dnsd`
on UDP 53). Until `guardd` has loaded the rules at the boot, nothing
comes in; with the elf switched off (`elf stop guardd`), nothing is
checked.

## Options

| Command | Does |
|---|---|
| `guard` | show the rules, the defaults, their counters and the last packets dropped |
| `guard add RULE` | RULE becomes the last rule |
| `guard insert N RULE` | RULE goes before rule N (it is checked first); N may be one more than the number of rules (the end) |
| `guard remove N` | rule N goes |
| `guard in drop`, `guard in allow` | what comes in when no rule matches |
| `guard out drop`, `guard out allow` | what goes out when none does |
| `guard forward drop`, `guard forward allow` | what is forwarded between cards when none does |

RULE may be given as several words or as one quoted word. A default
changes its line in the file, or is added before the first rule.

## Exit status and messages

- `0`: shown, or changed:
  - `guard: rule N: RULE; guardd loads it within 2 s` (add, insert)
  - `guard: rule N removed; guardd loads the rules within 2 s`
  - `guard: in = drop; guardd loads it within 2 s` (a default)
- The state's first line when shown: `firewall: on; in = drop, out =
  allow, forward = drop`, `firewall: no rules loaded yet: nothing comes
  in`, or `firewall: off: nothing is checked (the elf guardd is off)`.
- `1`:
  - `guard: only root may change the firewall's rules`
  - `guard: N: no such rule (there are COUNT)`
  - `guard: 'RULE': not a rule (it begins with allow, drop or limit)`
  - what is wrong with RULE, such as `guard: 'sideways': in, out or
    forward is missing`, `guard: 'WORD': not allow, drop or limit`,
    `guard: 'WORD': not a port (1-65535) or ports (like 6000-6010)`,
    `guard: 'WORD': the ports go down`, `guard: 'WORD': a port needs tcp
    or udp`, `guard: 'WORD': what follows is missing`, `guard: 'WORD': not an
    address (like 192.168.0.0/24)`, `guard: 'WORD': not a card (like
    eth0, wlan0, tap0, br0)`, `guard: 'WORD': not understood here`, `guard: 'WORD': limit
    needs N/minute at the end (like limit in tcp 22 10/minute)`
  - `guard: /system/config/guard: REASON` (the file could not be read or
    written)
- `2`: `usage: guard add RULE, guard insert N RULE` (no RULE given), or
  `usage: guard [add RULE | insert N RULE | remove N |
  in|out|forward drop|allow]`.

## Examples

```
ritteros:/$ guard
firewall: on; in = drop, out = allow, forward = drop
  1  drop in tcp 7000 from 10.0.2.0/24                         5 packet(s)
  2  allow in icmp                                             0 packet(s)
  3  allow in tcp 22                                           0 packet(s)
     in = drop (no rule matched)                               0 packet(s)
     out = allow (no rule matched)                             0 packet(s)
     forward = drop (no rule matched)                          0 packet(s)
connections: 0 packet(s) of connections already let through (not checked again)
last dropped:
  2026-10-18 21:49:53  in tcp 7000 from 10.0.2.2 to 10.0.2.15 on eth0 (rule 1)
ritteros:/$ guard add allow in tcp 8080 from 192.168.0.0/24
guard: rule 4: allow in tcp 8080 from 192.168.0.0/24; guardd loads it within 2 s
ritteros:/$ guard insert 1 limit in tcp 22 10/minute
guard: rule 1: limit in tcp 22 10/minute; guardd loads it within 2 s
ritteros:/$ guard remove 2
guard: rule 2 removed; guardd loads the rules within 2 s
ritteros:/$ guard add allow sideways
guard: 'sideways': in, out or forward is missing
```

## See also

[`guardd`](guardd.md), [`net`](net.md) (forwarding, masquerade),
[`elf`](elf.md), [`sshd`](sshd.md), [`dnsd`](dnsd.md)

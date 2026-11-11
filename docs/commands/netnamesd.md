# netnamesd: an elf: records new network cards in /system/config/network/names

```
netnamesd
```

`netnamesd` is an elf: a program `elvesd` keeps running (other systems
call such programs services or daemons). It keeps the network cards' names
with the cards (since step 50f). RitterOS keeps a list of every card it has
found, by MAC address, in `/system/config/network/names`, and the kernel
names each card from it at the start and whenever a card is plugged in,
whatever order the cards are found in: so a card's settings file
(`/system/config/network/eth0`) always applies to the same card. A card not
on the list gets the lowest name of its kind (`ethN`, `wlanN` for Wi-Fi)
that no listed card holds, also one that is not plugged in now.

Once a second `netnamesd` compares the cards the kernel has (plugged in)
with the list, and adds each card that is not on it yet, under the name
the kernel gave it, with a comment saying what it is and when it was found.
The first card it records also makes the file, with a header:

```
# The network cards RitterOS has found, by MAC address: each keeps its
# name (and its settings file beside this one). The elf netnamesd adds
# new cards; `net forget NAME` frees a name (or delete the card's lines);
# a name changed here counts from the next start.
# eth0: PlutoSDR (ADALM-PLUTO) (usbnet at usb0 port 1), found 2026-10-26
eth0 = 02:aa:bb:cc:00:02
# eth1: Realtek RTL8168H, version 0x541 (rtl8168 at 03:00.0), found 2026-10-26
eth1 = 02:aa:bb:cc:00:01
```

The list is a settings file: change a name there (with `edit`, as root) to
rename a card; it counts from the next start. Rename the card's settings
file in `/system/config/network/` with it. `net names` shows the list, `net
forget NAME` takes a card that is not plugged in off it.

A card new to the machine also gets its settings file at once (since step
50h), the one `net up` would make; a file that is there is never changed:

```
# This card's settings (net up applies them; see the manual):
#   address = dhcp, or ADDRESS/PREFIX with gateway = ... and dns = ...
address = dhcp
```

and, if the elf `dhclient` has no file yet, `netnamesd` makes
`/system/config/elves/dhclient` too (`command = dhclient`, `start = on`).
So a card plugged in for the first time gets its address by itself,
without `net up`.

On a machine where `net up` never ran (since step 36d-3: there is no file
for the elf `guardd`), a new card also gets the other network elves' files
`net up` would make: `dnsd`, `sshd` and, last, the firewall's `guardd`
(each `command = NAME`, `start = on`, made only if missing). `login` runs
`net up` itself when the system starts, but only if a card is there by
then; a card that comes later (plugged in, or a Wi-Fi card such as Intel's,
whose firmware takes seconds to start) would otherwise leave the machine
with no firewall rules, so that nothing came in, not even ping or ssh.
With these files `guardd` makes its first rules: ping, ssh (tcp 22) and
DNS (udp 53) let in. `netnamesd` writes nothing else, takes no options and
prints only its errors.

## Starting and stopping

`netnamesd` runs on every machine, also one without a network card (a card
may be plugged in later). `login` makes its elf file when the system starts,
before anyone logs in, if it is missing, and `net up` does too:

```
# The network cards' names, an elf (login made this file)
command = netnamesd
start = on
```

Then, as root, `elf stop netnamesd` stops it (also for the next starts:
new cards are then named by the kernel but not recorded), `elf start
netnamesd` starts it again, `elf restart netnamesd` restarts it.

## Exit status and messages

- `login: /system/config/elves/netnamesd made: the elf netnamesd records new
  network cards`: `login` made its elf file.
- `netnamesd: PATH: REASON`: the list, its folder
  `/system/config/network` or a new card's settings file could not be
  written (`netnamesd: /system/config/network/names: Permission denied`).
  Each error is printed once, not every second.
- It never ends by itself.

## Examples

```
ritteros:/$ net names
eth0   02:aa:bb:cc:00:02  here     PlutoSDR (ADALM-PLUTO) (usbnet at usb0 port 1), found 2026-10-26
eth1   02:aa:bb:cc:00:01  here     Realtek RTL8168H, version 0x541 (rtl8168 at 03:00.0), found 2026-10-26
ritteros:/$ cat /system/config/network/names
ritteros:/$ elf
elvesd: running (process 3)
logd             on   running (process 4)     logd
netnamesd        on   running (process 5)     netnamesd
```

## See also

[`net`](net.md) (`net names`, `net forget`, `net up`), [`elf`](elf.md),
[`elvesd`](elvesd.md), [`dhclient`](dhclient.md), [`login`](login.md)

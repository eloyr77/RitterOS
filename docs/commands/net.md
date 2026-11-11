# net: the network cards; up: apply /system/config/network/ (root); names, forwarding, NAT, the default route, virtual cards, bridges

```
net [up | names | forget NAME | forward [on|off] | masquerade [CARD... | off] | default [CARD... | auto] | tap add [user NAME] | tap remove TAP | bridge add [BRIDGE] PORT... | bridge remove BRIDGE [PORT...]]
```

`net` alone shows the loopback `lo` first, then each network card: its
MAC address, the link and its speed, the card and where it sits, its
address, gateway and DNS server, its IPv6 addresses (since 64a), what it
received and sent; then the routes, forwarding, masquerade, the
neighbours whose MAC addresses are known (`arp:` for IPv4, `ndp:` for
IPv6) and TCP. It is the kernel's report, the same as the rescue
console's `net`; anyone may run it. Cards are `eth0`, `eth1`, ... (Wi-Fi
cards `wlan0`, ...), named by the list in `/system/config/network/names`.
Without a card, `net` says `no network cards` after `lo`. `net` has been
in RitterOS since step 33; `up` since step 34 (one file per card since
42d), forwarding since step 41, masquerade since 49i, `names` and `forget`
since 50f, `default` since 50g, `tap` since 38a, `bridge` since 38b.

```
lo     the loopback, 127.0.0.1/8
       received 12 packets (1008 bytes), sent 12 packets (1008 bytes), 0 not sent
       IPv6 ::1/128
eth0   52:54:00:12:34:56  link up, 1000 Mbit/s, Intel 82540EM (e1000 at 00:05.0)
       address 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3
       IPv6 fe80::5054:ff:fe12:3456/64 (link-local)
       received 5 frames (456 bytes), sent 8 frames (560 bytes), 0 not sent
route: 127.0.0.0/8 on lo
route: 10.0.2.0/24 on eth0
route: default via 10.0.2.2 on eth0
forwarding: off; 0 packets forwarded, not: 0 without a route, 0 out of time (TTL), 0 without an ARP answer
arp: 10.0.2.2        is 52:55:0a:00:02:02
ndp: fe80::ff:fe00:7102%eth0 is 02:00:00:00:71:02
tcp: 0 connection(s); segments 2120 sent, 1853 received; sent again: 0 after a timeout, 0 fast; 0 kept after a gap, 0 window probes; 0 with scaled windows, 0 with SACK, receive buffers doubled 0 times; 0 holes resent from SACK, send buffers doubled 0 times, 0 ACKs delayed, 0 reset with data unread
udp: 6 of 128 port(s) bound; 0 datagram(s) dropped by a full queue
```

The tcp line's last count (since 65e-15) is of the connections whose
windows were scaled (RFC 7323): a connection RitterOS opens offers it,
with up to 4 MiB in flight instead of 64 KiB (since 65e-17; 1 MiB before), and both sides use it when
the other end agrees (most servers do; QEMU's user network does not).
Since 65e-25 connections to RitterOS's own servers are scaled too (and use
SACK) when the other end's SYN offers it. Since 65e-18 the
buffer starts at 128 KiB and doubles (up to 4 MiB) while the sender keeps
reaching the window's edge and the program keeps up: the next count says
how often. The SACK count is of the connections that use selective
acknowledgements (65e-16). The last count (since 65e-29) is of the ACKs
held back: a single full-sized segment in a card's burst is acknowledged
with the next one, or after 40 ms (delayed ACKs, RFC 1122); short
segments, the first 16 of a connection, anything after a gap and anything
over `lo` are acknowledged at once. The very last count (since 36e-8) is
of the connections closed with a RST: a program that closes a connection
with received data still unread, or gets data after closing, ends it at
once (RFC 1122), so the other side's writes fail instead of waiting on a
closed reader.

Under each card (since 38d) a line says how often the system asked for
its link (several times for every packet it sends: routes, the send
itself) and how many of those asked the card's driver, which is often a
read of the chip: the driver is asked at most once every 10 ms (a timer
tick), the answer kept meanwhile. On the J1900 the reads had cost half
its sending speed.

Drivers may add lines of their own under their card, about the chip:
`rtl8168:` (the Realtek's counters and, since 38d, its PHY's registers:
what it can do, advertises and the other end offered), `mt7921:` (the MediaTek's counters
and airtime), `intelwifi:` (the Intel 8265's firmware, queues, bursts,
rates and restarts; the manual's `wifi` section shows them).

**IPv6** (since 64a, the first step of roadmap item 29). Every card gets
its link-local address when its link comes up: `fe80::` and the card's
MAC address made into 64 bits (`52:54:00:12:34:56` gives
`fe80::5054:ff:fe12:3456`), used only on the card's own network. It is
first checked for a second or two: `(link-local, being checked for
duplicates)`. Another machine with the same address makes it `(link-local,
used by another machine: not used)`, with a line when that is found:

```
eth1: IPv6 address fe80::5054:ff:fe00:7201 is used by another machine (02:00:00:00:72:02); not used
```

The address is dropped when the link goes and checked again when it
comes back. `lo` has `::1`.

Since 64b the card then asks the network's routers (a router
solicitation, at most three times, four seconds apart). A router's
advertisement names the prefixes on the link and those to make addresses
in: the card takes the prefix and the same 64 bits of its own as in its
link-local address (`2001:db8:1::/64` gives `2001:db8:1:0:5054:ff:fe12:3456`),
checks it for duplicates and uses it for as long as the router says
(`valid`; after `preferred` it is not chosen for new conversations). The
advertisement also names the default router, often a DNS server and the
link's MTU:

```
       IPv6 fe80::5054:ff:fe12:3456/64 (link-local)
       IPv6 2001:db8:1:0:5054:ff:fe12:3456/64 (from the router, valid 86393 s, preferred 14393 s)
       IPv6 router fe80::1 (1793 s), DNS 2001:db8:1::53 (1193 s), MTU 1400
route6: 2001:db8:1::/64 on eth0
route6: default via fe80::1 on eth0
```

`route6:` lines are IPv6's routes: a prefix on a card's link goes there,
anything else to the default route's router, on the first card in `net
default`'s order that has one (as for IPv4). A card with `address6 =
ADDRESS/N` shows `(fixed)` and `gateway`; with `address6 = off`, `IPv6
off`. `net up` prints a line for a card whose file says something about
IPv6 (`net: eth1: IPv6 2001:db8:72::1/64, gateway 2001:db8:72::2, DNS
2001:db8:72::53`, `net: eth1: IPv6 off`).

The Intel 82576, Realtek and MT7921 cards add lines with what the chip
itself counted, and Wi-Fi cards what the wireless layer counted; the
user's manual explains them.

**The cards' settings.** `net up` applies the files in the folder
`/system/config/network/` to the cards (root only): one file per card,
named after it (`eth0`, `wlan0`, ...). `login` does it by itself at every
start when there is a network card. A card without a file gets one that
asks the network (DHCP):

| Key | Means |
|---|---|
| `address = dhcp` | ask the network for the address, gateway and DNS server; the elf `dhclient` does it, and keeps asking |
| `address = ADDRESS/N` | a fixed address and how many leading bits name the network: `10.0.2.15/24` |
| `gateway = ADDRESS` | where packets for other networks go (with a fixed address) |
| `dns = ADDRESS` | the server that answers names; with `address = dhcp` it replaces the network's; `ADDRESS:PORT` asks on another port than 53 |
| `address6 = auto` | (64b) IPv6 addresses from the network's router (SLAAC); also without the line |
| `address6 = dhcp` | (64g) IPv6 addresses from a DHCPv6 server (the elf `dhclient`), none from the router's prefixes; its router, routes and MTU still from the router |
| `address6 = off` | no IPv6 on the card, not even its link-local address |
| `address6 = ADDRESS/N` | a fixed IPv6 address and its prefix length: `2001:db8::5/64`; the routers' advertisements are not taken then |
| `gateway6 = ADDRESS` | the IPv6 router for other networks (with a fixed `address6`; a link-local one is fine) |
| `dns6 = ADDRESS` | an IPv6 DNS server instead of the router's; `[ADDRESS]:PORT` for another port than 53 |

With a card, `net up` also makes the files of the network's elves in
`/system/config/elves/` that are missing: `dhclient` (when a card uses
DHCP), `dnsd`, `sshd`, `netnamesd` and `guardd`, and starts `elvesd`. With
`guardd` switched off, it switches the firewall's checks off. A disk from
before step 42d has all of it in one file, `/system/config/network`:
`net up` moves it into the folder and `/system/config/routing`. Change a
file with `edit`, then `net up`.

**Routing.** `/system/config/routing` holds what goes for all cards, also
applied by `net up`:

| Key | Means |
|---|---|
| `forward = on` | pass packets on between the cards (without the line, or `off`: not) |
| `masquerade = CARD...` | the private cards (space-separated): a packet forwarded from one out a card not listed gets that card's address as its source (source NAT, IPv4 only); without the line, none |
| `default = CARD...` | the cards whose gateway carries the default route, in order: the first with a link, an address and a gateway; when none can, the first other card with a gateway. Without the line: the first card with a gateway (`auto`) |

Each card with an address reaches its own network directly; everything
else goes to the default route's gateway; 127.x is `lo`. The most
specific route wins. A card without a link is passed over. A forwarded
packet is also checked against the firewall's `forward` rules, whose
default is `drop` (see `guard`).

**The cards' names** stay with the cards. `/system/config/network/names`
lists every card RitterOS has found, by MAC address, each under a comment
saying what it is; the elf `netnamesd` adds a new card within a second
(with the lowest free name of its kind) and makes its settings file:

```
# eth0: PlutoSDR (ADALM-PLUTO) (usbnet at usb0 port 1), found 2026-10-26
eth0 = 02:aa:bb:cc:00:02
# eth1: Realtek RTL8168H, version 0x541 (rtl8168 at 03:00.0), found 2026-10-26
eth1 = 02:aa:bb:cc:00:01
```

Change a name there (as root) to rename a card, from the next start; and
rename its settings file too.

**Virtual cards** (since 38a). `net tap add` (root) makes a card `tap0`
(then `tap1`, ..., the lowest number free, up to `tap63`) whose wire is a
program: the program opens `/dev/tap0` and every `read` gives it one
whole Ethernet frame the system sent through the card (without its
checksum), every `write` of one frame (14 to 1514 bytes) is a frame the
card received. It is a card like the others: its address, routes, the
firewall and `net`'s report work as for a real one. Its MAC address is
made up at random (locally administered) each time it is made. Its link
is up (10000 Mbit/s) while a program holds `/dev/tapN` and down when the
program closes it; frames not read by then are dropped. Only one program
holds it at a time (a second `open` fails: `Device or resource busy`),
and only root and the card's user may open it: `net tap add user NAME`
gives it to that user. A frame written for another card's MAC address is
dropped, as a card's own filter would (broadcast and multicast frames go
up); `net` counts them under the card. `net tap remove tap0` takes it away
again, though not while a program has it open. Its settings are a card's
as any other's: `net up` makes `/system/config/network/tap0` with
`address = dhcp` when it has none (since 38b-3), and applies what it says
(for example `address = 192.168.77.1/24`). A tap or a bridge is named
wherever a card is: the firewall's rules, `net default`, `net
masquerade`. Taps are made again at every start (38c, below).

```
tap0   2a:19:9f:10:58:69  no link, virtual card (tap at /dev/tap0)
       address 192.168.77.1/24, gateway none, DNS none
       received 4 frames (336 bytes), sent 7 frames (574 bytes), 0 not sent
       the program's side: /dev/tap0, user 0, not open; 0 bytes waiting, 1 frame(s) for other cards dropped
```

**Bridges** (since 38b). `net bridge add eth0 tap0` (root) makes a bridge
`br0` (then `br1`, ..., up to `br7`) with these cards as its ports: one
Ethernet network made of several, as a switch makes one. A frame that
comes in on a port goes out the port where its destination was last
heard from, or out every other port while that is not known (always for
broadcast and multicast frames); an address unheard for 300 seconds is
forgotten. The bridge is a card itself: the machine's address goes on
`br0` (its settings file, `address = dhcp` when `net up` makes it), and
`br0` takes the first port's MAC address, so a DHCP server gives it the
address that card had. The ports carry nothing of their own while they
are ports: their addresses go (IPv4 and IPv6), their settings files are
not used, and they take every frame (promiscuous mode). `br0`'s link is
up while a port's is (at its fastest port's speed). Wired cards and taps
can be ports, up to 16; a Wi-Fi card cannot (an 802.11 client carries
only frames of its own MAC address). `net bridge add br0 tap1` adds a
port, `net bridge remove br0 tap1` takes one out, `net bridge remove br0`
takes the bridge away (every port is a card of its own again: DHCP asks
for its address by itself). Bridges are made again at every start (38c,
below).

Two cautions. There is no spanning tree (as Linux's default): two
bridges joined by two paths pass broadcasts round for ever (`net` counts
frames of the bridge's own that came back, `come back (a loop)`). And the
firewall checks only what reaches `br0` itself: frames passed between
ports are not IP packets to the machine, and its rules do not see them.

```
br0    52:54:00:12:34:56  link up, 1000 Mbit/s, bridge (bridge at its ports)
       address 10.0.2.15/24 (DHCP, lease 86400 s), gateway 10.0.2.2, DNS 10.0.2.3
       received 9 frames (1756 bytes), sent 11 frames (1922 bytes), 0 not sent
       IPv6 fe80::5054:ff:fe12:3456/64 (link-local)
       bridge: ports eth0 tap0 tap1; 5 address(es) known; 14 frame(s) passed on, 8 sent to every port, 0 for their own port, 0 come back (a loop)
eth0   52:54:00:12:34:56  link up, 1000 Mbit/s, Intel 82540EM (e1000 at 00:05.0)
       a port of br0: its frames are the bridge's
       received 14 frames (3272 bytes), sent 26 frames (3994 bytes), 0 not sent
```

(The lines `its link asked ...` under each card, since 38d, are left out
here.)

**Kept for the next start** (since 38c). `/system/config/network/virtual`
holds the taps and bridges: `net tap add`, `net tap remove`, `net bridge
add` and `net bridge remove` change their own lines in it (the others and
the comments stay), and `net up` (login runs it at every start, also on a
machine whose only cards are these) makes what it says before the cards'
own settings: the taps first (each with its number and user), then the
bridges with their ports in order (the first port's MAC address is the
bridge's). A card the file names that is not there (a USB adapter not
plugged in) is said and passed over. `net up` writes the file with its
default text when it is missing:

```
# Virtual network cards and bridges, made again by net up at every start
# (taps first, then bridges; then every card's own settings). `net tap add`,
# `net tap remove`, `net bridge add` and `net bridge remove` change their
# lines. Remove a # to use a line:
#
# A virtual card tapN whose /dev/tapN the user (and root) may open.
# tap0 = user root
#
# A bridge brN of these cards (its MAC address the first one's there).
# br0 = eth0 tap0
```

After `net tap add`, `net bridge add eth0 tap0`, the file ends with:

```
tap0 = user root
br0 = eth0 tap0
```

and the next start makes them again (a tap gets a new MAC address each
time; the bridge keeps eth0's):

```
net: tap0: a virtual card; /dev/tap0 is root's
net: br0: a bridge (its MAC address eth0's)
net: br0: eth0 is its port
net: br0: tap0 is its port
net: tap0: /system/config/network/tap0 made (address = dhcp)
net: br0: /system/config/network/br0 made (address = dhcp)
net: eth0: a port of br0: its own settings are not used
net: tap0: a port of br0: its own settings are not used
net: br0: DHCP: the elf dhclient asks for its address
```

A bridge's port keeps its own settings file, unused while it is a port
(`net up` says so), and used again when it leaves the bridge.

The bridge's line: its ports, the MAC addresses heard on them now, the
frames passed on to one port, sent to every port (broadcasts, multicast,
destinations not known yet), not passed on because their destination is
on the port they came in on, and frames with the bridge's own MAC
address that came back in.

## Options

| Command | Does |
|---|---|
| `net` | the report: `lo`, the cards, routes, forwarding, masquerade, ARP, TCP |
| `net up` | apply `/system/config/network/` and `/system/config/routing` (root) |
| `net names` | the cards in `/system/config/network/names`: name, MAC address, `here` or `not here`, what it is |
| `net forget NAME` | a card that is not plugged in leaves the names list and its settings file is removed: its name is free for the next new card (root) |
| `net forward` | whether packets are forwarded between cards |
| `net forward on`, `net forward off` | switch forwarding at once (root) |
| `net masquerade` | the `masquerade:` line of the report: the private cards and the packets rewritten |
| `net masquerade CARD...` | these cards are the private ones, at once (root) |
| `net masquerade off` | no private cards (`none` too) (root) |
| `net default` | the default route's order and the route now |
| `net default CARD...` | the default route's cards in this order, at once (root); names may also be separated by commas |
| `net default auto` | no preference: the first card with a gateway (root) |
| `net tap add` | a new virtual card `tapN`; `/dev/tapN` is root's (root) |
| `net tap add user NAME` | the same, `/dev/tapN` that user's (and root's) (root) |
| `net tap remove TAP` | the virtual card goes; not while a program has it open (root) |
| `net bridge add PORT...` | a new bridge `brN` with these cards as its ports, its MAC address the first one's (root) |
| `net bridge add BRIDGE PORT...` | more ports for the bridge (root) |
| `net bridge remove BRIDGE PORT...` | the ports leave the bridge (root) |
| `net bridge remove BRIDGE` | the bridge goes, every port leaving it (root) |

`forward`, `masquerade` and `default` given on the command line last until
the next `net up` or start: keep them in `/system/config/routing`.

## Exit status and messages

- `0`, with lines such as:
  - `net: eth0: /system/config/network/eth0 made (address = dhcp)`
  - `net: eth0: DHCP: the elf dhclient asks for its address`
  - `net: eth0: 192.168.1.1/24, gateway none, DNS none`
  - `net: /system/config/elves/dnsd made: the elf dnsd answers their
    questions`
  - `net: the firewall checks nothing (the elf guardd is off)`
  - `net: /system/config/network: moved into one file per card in
    /system/config/network/ (forward: /system/config/routing)`
  - `net: forwarding between cards: on`
  - `net: masquerade (source NAT), private: eth0`
  - `net: the default route: eth1, then any card with a gateway; now via
    10.0.3.2 on eth1`, `net: the default route: eth0, then any card with
    a gateway`, `net: the default route: the first card with a gateway
    (auto)`
  - `net: no cards in /system/config/network/names yet`
  - `net: eth2 (MAC) forgotten: the name is free; removed
    /system/config/network/eth2`
  - `net: tap0: a virtual card; /dev/tap0 is root's`, `net: tap0 removed`
  - `net: br0: a bridge (its MAC address eth0's)`, `net: br0: eth0 is its
    port`, `net: br0: tap1 is no longer its port`, `net: br0 removed`
  - `net: eth0: a port of br0: its own settings are not used` (`net up`)
- `1`: `net: WHAT: REASON`, such as:
  - `net: /system/config/network/eth0: no address = ... in it`
  - `net: VALUE: not dhcp or an address with its prefix (like
    10.0.2.15/24)`, `net: VALUE: not an address (like 10.0.2.2)`, `net:
    VALUE: not an address (like 10.0.2.3 or 10.0.2.2:5300)`
  - `net: eth0: no such network card`
  - `net: VALUE: not on or off`
  - `net: forward: Operation not permitted` (and `masquerade:`,
    `default:`: not root)
  - `net: NAME: not a card's name (ethN, wlanN, tapN or brN)` (default)
  - `net: NAME: No such file or directory` (masquerade: no such card
    here; the cards that are there are still marked)
  - `net: NAME: not in /system/config/network/names`
  - `net: eth1 is plugged in (WHAT IT IS): unplug it first`
  - `net: NAME: no such user`, `net: tap: no room for another virtual
    card`, `net: tap: Operation not permitted` (not root)
  - `net: tap0: a program has it open`, `net: tap7: no such virtual card`,
    `net: NAME: not a virtual card (tap0, tap1, ...)`
  - `net: eth0: a bridge's port already`, `net: tap5: no such network
    card`, `net: br5: no such bridge`, `net: tap1: not a port of br0, or no
    such bridge`, `net: wlan0: a Wi-Fi card cannot be a port (it carries
    only its own MAC address)`, `net: eth3: its driver cannot take other
    machines' frames`, `net: br1: a bridge or the loopback cannot be a
    port`, `net: tap9: no room (br0 to br7, 16 ports each)`, `net: NAME:
    not a bridge (br0, br1, ...)`
  - `net up`: `net: eth3: no such network card (not made a port)`, `net:
    tap0: not user NAME (a user there)` (a line of
    `/system/config/network/virtual`)
- `1` from `net up` also when there is no network card at all (nothing
  is printed).
- `2`: a wrong command: `usage: net [up | names | forget NAME | forward
  [on|off] | masquerade [ethN ... | off] | default [CARD... | auto] | tap
  add [user NAME] | tap remove TAP | bridge add [BRIDGE] PORT... | bridge
  remove BRIDGE [PORT...]]`.

## Examples

```
ritteros:/$ net up
net: eth0: /system/config/network/eth0 made (address = dhcp)
net: eth0: DHCP: the elf dhclient asks for its address
net: /system/config/elves/dhclient made: the elf dhclient asks for the addresses
net: /system/config/elves/dnsd made: the elf dnsd answers their questions
net: /system/config/elves/sshd made: the elf sshd lets others log in (port 22)
ritteros:/$ net forward on
net: forwarding between cards: on
ritteros:/$ net masquerade eth0
net: masquerade (source NAT), private: eth0
ritteros:/$ net default
net: the default route: eth1, then any card with a gateway; now via 10.0.3.2 on eth1
ritteros:/$ net names
eth0   02:aa:bb:cc:00:02  here     PlutoSDR (ADALM-PLUTO) (usbnet at usb0 port 1), found 2026-10-26
eth1   02:aa:bb:cc:00:01  here     Realtek RTL8168H, version 0x541 (rtl8168 at 03:00.0), found 2026-10-26
eth2   52:54:00:12:34:59  not here QEMU USB NCM Network Device (usbnet at usb0 port 6), found 2026-10-20
ritteros:/$ net forget eth2
net: eth2 (52:54:00:12:34:59) forgotten: the name is free; removed /system/config/network/eth2
```

A router between two networks: `/system/config/network/eth0` holds
`address = 192.168.1.1/24`, `/system/config/network/eth1` holds `address
= 10.0.2.15/24` and `gateway = 10.0.2.2`, and `/system/config/routing`
holds `forward = on`:

```
ritteros:/$ net up
net: eth0: 192.168.1.1/24, gateway none, DNS none
net: eth1: 10.0.2.15/24, gateway 10.0.2.2, DNS none
net: forwarding between cards: on
```

A virtual card with an address, and a program on its wire (the test
program `taptest`, which answers ARP and pings as 192.168.77.2):

```
ritteros:/$ net tap add
tap0: a virtual card for user 0, MAC 2a:19:9f:10:58:69 (/dev/tap0)
net: tap0: a virtual card; /dev/tap0 is root's
ritteros:/$ net up
  tap0:          address 192.168.77.1/24, gateway none
net: tap0: 192.168.77.1/24, gateway none, DNS none
ritteros:/$ /system/aarch64/test/taptest tap0 192.168.77.2 6 & sleep 1; ping -c 3 192.168.77.2; sleep 6
[13]
taptest: tap0: 192.168.77.2 is 02:00:00:00:00:02
tap0: link up, 10000 Mbit/s
PING 192.168.77.2: 56 data bytes
64 bytes from 192.168.77.2: seq=1 time=0.06 ms
64 bytes from 192.168.77.2: seq=2 time=0.03 ms
64 bytes from 192.168.77.2: seq=3 time=0.11 ms
--- 192.168.77.2: 3 sent, 3 received, 0% lost; round trip min/avg/max 0.03/0.07/0.11 ms
taptest: tap0: 7 frame(s) read, 1 ARP answer(s), 3 echo answer(s)
tap0: link down
```

(`net up` also made the network elves' files here: lines left out.)

A bridge of the machine's card and two taps; `br0` gets eth0's address by
DHCP, and a program on tap0 (as 10.0.2.40, MAC 02:00:00:00:00:28) reaches
the gateway on eth0's network through the bridge:

```
ritteros:/$ net bridge add eth0 tap0 tap1
br0: a bridge, MAC 52:54:00:12:34:56 (eth0's)
br0: eth0 is its port now
net: br0: a bridge (its MAC address eth0's)
net: br0: eth0 is its port
br0: tap0 is its port now
net: br0: tap0 is its port
br0: tap1 is its port now
net: br0: tap1 is its port
ritteros:/$ net up
net: tap0: /system/config/network/tap0 made (address = dhcp)
net: tap1: /system/config/network/tap1 made (address = dhcp)
net: br0: /system/config/network/br0 made (address = dhcp)
net: eth0: a port of br0: its own settings are not used
net: tap0: a port of br0: its own settings are not used
net: tap1: a port of br0: its own settings are not used
net: br0: DHCP: the elf dhclient asks for its address
ritteros:/$
dhclient: br0: 10.0.2.15/24 from 10.0.2.2, gateway 10.0.2.2, DNS 10.0.2.3, lease 86400 s
ritteros:/$ /system/x86_64/test/taptest tap0 10.0.2.40 5 10.0.2.2
taptest: tap0: 10.0.2.40 is 02:00:00:00:00:28
taptest: tap0: 10.0.2.2 is 52:55:0a:00:02:02
taptest: tap0: echo seq=1 answered by 10.0.2.2
tap0: link up, 10000 Mbit/s
taptest: tap0: echo seq=2 answered by 10.0.2.2
taptest: tap0: echo seq=3 answered by 10.0.2.2
taptest: tap0: 4 frame(s) read, 0 ARP answer(s), 0 echo answer(s)
taptest: tap0: 10.0.2.2: 1 ARP question(s), 3 of 3 echo request(s) answered
ritteros:/$ net bridge remove br0
br0: tap0 is no longer its port
br0: eth0 is no longer its port
br0: removed
net: br0 removed
```


## See also

[`ping`](ping.md), [`guard`](guard.md), [`dhclient`](dhclient.md),
[`dnsd`](dnsd.md), [`netnamesd`](netnamesd.md), [`wifi`](wifi.md),
[`elf`](elf.md), [`fetch`](fetch.md)

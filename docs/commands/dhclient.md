# dhclient: the DHCP client, an elf: addresses for the cards set to dhcp (and DHCPv6)

```
dhclient
elf start dhclient
elf stop dhclient
```

`dhclient` is an elf: a program `elvesd` keeps running, not one you type.
It gets the address of every network card whose settings file says so:

```
address = dhcp
```

in `/system/config/network/eth0` (one file per card: `eth1`, `wlan0`,
...; every kind of card the kernel lists). For each such card it asks the
network (DHCP, RFC 2131: it broadcasts a DISCOVER, a server OFFERs an
address, it REQUESTs it, the server ACKs) and gives the card the address,
the network's mask, the gateway and the DNS server, for the lease's time.
A `dns = ...` line in the card's file wins over the network's server
(`dns = 10.0.2.3:5353` asks on another port). It talks through a UDP socket
bound to the card, port 68 to 67. DHCP was in the kernel until step 44b
made it this elf; it was called `dhcpd` until 44d (on Unix that is a DHCP
server's name).

Once a second it looks at each card:

- no address: it asks every 10 seconds, and at once when the link comes
  up. Each try sends three times, a second apart.
- an address: it asks again at half the lease (at least a minute), every
  minute while no server answers, and gives the address up when the lease
  ends.
- the link gone: the address goes too, until the link is back.
- (since 38d) the card gone (a virtual card or a bridge removed), or its
  address gone from the card (it became a bridge's port): the lease is
  forgotten, and the card is asked for again as a new one when it is back.
  A bridge's port shows no link, so nothing is asked for it meanwhile.

A lease without a time counts as a day (86,400 s), an answer without a mask
as `/24`. A card whose file changes to a fixed address is left alone from
then on. At most 64 cards (16 before 38a).

`net up` makes the elf's file, `/system/config/elves/dhclient`, when a card
uses DHCP (`net: /system/config/elves/dhclient made: the elf dhclient asks
for the addresses`), and so does `netnamesd` for a card plugged in for the
first time:

```
command = dhclient
start = on
```

`elf stop dhclient` switches it off (for the next boots too), `elf start
dhclient` on,
`elf restart dhclient` makes it ask anew. `net` shows each card's address
and the lease's time.

**DHCPv6** (since 64g, RFC 8415). For every card on IPv6, `dhclient` also
asks a DHCPv6 server when the card is set to `address6 = dhcp`, or when
it is on `address6 = auto` (the default) and the network's router says so
in its advertisements:

- "managed": an address (Solicit, a server Advertises, Request, it
  Replies) with its lifetimes and a DNS server; renewed at the server's
  T1 with the same server (Renew), with any server after T2 (Rebind), and
  given up at the end of its valid lifetime;
- "other" only: the DNS server alone (an Information-request), asked again
  every day.

It asks from the card's link-local address (port 546) to every server's
group `ff02::1:2` (port 547), as the card's DUID (made from its MAC). With
`address6 = dhcp` the card makes no address from the router's prefixes;
its router, routes and MTU still come from the router. `net` shows such
an address as `(from DHCPv6, valid N s, preferred N s)` and the server as
`DNS ADDRESS (DHCPv6, N s)`. `dhclient` runs while a card uses DHCP
(`address = dhcp` or `address6 = dhcp`); its lines:

```
dhclient: eth0: IPv6 2001:db8:71::1:1 from DHCPv6 (fe80::ff:fe00:7102), DNS 2001:db8:71::53, valid 3600 s
dhclient: eth0: IPv6 2001:db8:71::1:1 renewed, valid 3600 s
dhclient: eth0: the IPv6 lease ended: no IPv6 address from DHCPv6
dhclient: eth0: no answer from a DHCPv6 server yet; asking again every 10 s
dhclient: eth1: DNS 2001:db8:72::53 from DHCPv6
```

## Exit status and messages

`dhclient` does not end by itself. Its lines appear on the console and in
`/system/logs/dhclient.log`:

- `dhclient: eth0: 10.0.2.15/24 from 10.0.2.2, gateway 10.0.2.2, DNS
  10.0.2.3, lease 86400 s`: an address, from that server (`gateway none`,
  `DNS none` when the server gave none). Printed again only when a renewal
  brings another address.
- `dhclient: eth0: no answer from a DHCP server yet; asking again every 10
  s`: said once, until an answer comes.
- `dhclient: eth0: no link: no address`: the cable is out (or Wi-Fi left).
- `dhclient: eth0: the lease ended: no address`: no server renewed it.
- `dhclient: eth0: no socket for it: REASON`: tried again a second later.
- `dhclient: eth0: REASON`: the kernel did not take the settings.

## Examples

```
ritteros:/$ net up
net: eth0: /system/config/network/eth0 made (address = dhcp)
net: eth0: DHCP: the elf dhclient asks for its address
net: /system/config/elves/dhclient made: the elf dhclient asks for the addresses
elvesd: dhclient started (process 4)
dhclient: eth0: 10.0.2.15/24 from 10.0.2.2, gateway 10.0.2.2, DNS 10.0.2.3, lease 86400 s
ritteros:/$ cat /system/logs/dhclient.log
ritteros:/$ elf restart dhclient
elf: dhclient: process 4 ended (elvesd starts it again)
```

## See also

[`net`](net.md), [`elf`](elf.md), [`dnsd`](dnsd.md),
[`netnamesd`](netnamesd.md), [`wifi`](wifi.md), [`ping`](ping.md)

# ping: send ICMP echo requests and show the round trips

```
ping [-4|-6] [-c COUNT] HOST
```

Sends an ICMP echo request (56 bytes of data) to HOST once a second, 4
times or COUNT times, and shows how long each answer took, then a
summary. Ctrl-C stops it early; the summary still comes. Each request
waits up to 2 seconds for its answer. `ping` has been in RitterOS since
step 34.

HOST is an address (`10.0.2.2`) or a name (`ping example.com`), which
the elf `dnsd` turns into an address (`PING example.com (93.184.215.14):
...`); `localhost` is 127.0.0.1 without asking. Our own address and
127.0.0.1 always answer.

Since step 64a HOST may also be an IPv6 address (any form with a `:`:
`::1`, `fe80::1`, `2001:db8::5`). A link-local one (`fe80::...`) or a
group on the link (`ff02::1`, every machine there) exists on each card's
network separately, so it needs its card after a `%`: `ping
fe80::5054:ff:fe12:3456%eth0`. Every member of a group answers; the
first answer counts. A name (since 64d) is taken as every program takes
it: its IPv6 address first when the machine has an IPv6 address beyond
the link, then its IPv4 one (if the first cannot be reached at all, the
next is tried); `-6` or `-4` asks for one family only. An address written
out is pinged in its own family; `-4` with an IPv6 address, or `-6` with
an IPv4 one, is refused.

## Options

| Option | Meaning |
|---|---|
| `-c COUNT` | send COUNT requests instead of 4 (at least 1) |
| `-6` | (64d) a name's IPv6 address only (its AAAA record); an IPv4 address is refused |
| `-4` | a name's IPv4 address only; an IPv6 address is refused |

## Exit status and messages

- `0`: at least one answer came.
- `1`: none came.
- `2`: a wrong command line (`usage: ping [-4|-6] [-c COUNT] HOST`, also for a
  COUNT of 0 or not a number), or HOST could not be turned into an
  address: `ping: HOST: Name not found` (the DNS server knows no such
  name), `ping: HOST: Timed out` (it did not answer), `ping: HOST: Network
  is unreachable` (no DNS server); for IPv6 `ping: HOST: not an IPv6
  address`, `ping: HOST: a link-local address needs its card
  (ADDRESS%eth0)`, `ping: HOST: no such card`, `ping: HOST: not an IPv6
  address` (-6 with an IPv4 one), `ping: HOST: not an IPv4 address`.

A request without an answer gives a line of its own, `seq=N: WHY`:
`Timed out` (nothing came back in 2 seconds), `No route to host` (nobody
in the local network answered ARP, or for IPv6 neighbour discovery, for
that address), `Network is unreachable` (no address set, no link, or no
gateway for another network; for IPv6 also while the card's address is
still being checked, or is a duplicate). The firewall may also refuse a request: `Operation not
permitted`.

## Examples

```
ritteros:/$ ping 10.0.2.2
PING 10.0.2.2: 56 data bytes
64 bytes from 10.0.2.2: seq=1 time=10.54 ms
64 bytes from 10.0.2.2: seq=2 time=9.05 ms
64 bytes from 10.0.2.2: seq=3 time=14.98 ms
64 bytes from 10.0.2.2: seq=4 time=11.91 ms
--- 10.0.2.2: 4 sent, 4 received, 0% lost; round trip min/avg/max 9.05/11.62/14.98 ms
ritteros:/$ ping -c 1 192.168.1.250
PING 192.168.1.250: 56 data bytes
seq=1: No route to host
--- 192.168.1.250: 1 sent, 0 received, 100% lost
ritteros:/$ ping -c 2 fe80::ff:fe00:7102%eth0
PING fe80::ff:fe00:7102%eth0: 56 data bytes
64 bytes from fe80::ff:fe00:7102%eth0: seq=1 time=0.52 ms
64 bytes from fe80::ff:fe00:7102%eth0: seq=2 time=6.83 ms
--- fe80::ff:fe00:7102%eth0: 2 sent, 2 received, 0% lost; round trip min/avg/max 0.52/3.68/6.83 ms
ritteros:/$ ping -c 1 ::1
PING ::1: 56 data bytes
64 bytes from ::1: seq=1 time=0.30 ms
--- ::1: 1 sent, 1 received, 0% lost; round trip min/avg/max 0.30/0.30/0.30 ms
```

## See also

[`net`](net.md), [`fetch`](fetch.md), [`guard`](guard.md),
[`dnsd`](dnsd.md), [`dhclient`](dhclient.md)

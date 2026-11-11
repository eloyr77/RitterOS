# dnsd: names (DNS) for this machine and others, an elf: answers kept by their time to live

```
dnsd
```

`dnsd` is an elf: a program `elvesd` keeps running (other systems call
such programs services or daemons). It turns names such as `example.com`
into addresses for the machine's programs: `ping`, `fetch`, `ssh` and
every program that calls `ritteros_resolve` or `getaddrinfo` ask it.
DNS left the kernel for this elf in step 44c (the elf was chosen for its
cache).

It listens on UDP port 53 on all of the machine's addresses and answers
everyone who asks, the machine's own programs and other machines alike
(one that names this machine as its DNS server gets the same answers and
shares the cache); what may reach it from outside is the firewall's to
decide (`guardd` lets in udp 53 when `dnsd` is there). For a question it
does not have in its cache, it asks the DNS server of the first card that
has one, in the order `net` lists the cards (any kind of card, a card with
its link up before one without, so an unplugged cable does not stop names
over Wi-Fi) and passes the answer on. The server comes from DHCP, or from
`dns = ADDRESS` (or `dns = ADDRESS:PORT`) in the card's settings file
`/system/config/network/ethN`, which wins over the network's. Without
any, it asks an IPv6 DNS server (since 64d): the one a card's router
names, or `dns6 = ADDRESS` in its file. It asks twice, waiting 2 seconds
each time, and takes an answer only from the server it asked, for the
same question. Since 64q it does not wait for one answer before asking
the next question: up to 64 questions are on their way at once, each from
a port of its own chosen at random, and a question asked again while the
same one is on its way waits for that answer instead of going to the
server a second time (a page with many files, whose names are all looked
up at once, no longer waits for each answer in turn). Its socket is an
IPv6 one: askers over IPv6 and IPv4 are answered alike.

Answers to plain address questions (type A, and since 64d AAAA, IPv6's) are kept for their time to
live, at most an hour, 32 names at a time (when it is full, the one that
runs out first goes). Since 64k the empty answers are kept too: "no such
name", and "no record of that type" (a name with an IPv4 address but no
IPv6 one, which every lookup on a network with IPv6 asks about first), for
the time the name's zone gives (its SOA record), else a minute, at most
five minutes. A restart of `dnsd` forgets them all. `localhost` and addresses typed as numbers never reach
`dnsd`: the library answers them itself.

`dnsd` takes no options and writes nothing to the disk.

## Starting and stopping

Its settings are its elf file, `/system/config/elves/dnsd`:

```
# Names for the programs, an elf (net up made this file)
command = dnsd
start = on
```

`net up` (which `login` runs at every start on a machine with a network
card) makes this file when it is missing, so `dnsd` runs from the first
start (`net: /system/config/elves/dnsd made: the elf dnsd answers their
questions`). Then, as root:

| Command | Does |
|---|---|
| `elf stop dnsd` | stops it, also for the next starts (`start = off`) |
| `elf start dnsd` | starts it again (`start = on`) |
| `elf restart dnsd` | ends it; `elvesd` starts it again (the cache is emptied) |

Without `dnsd`, programs cannot look names up (addresses still work).

## Exit status and messages

- `elvesd: dnsd started (process 5)`: `elvesd`'s line when it starts it.
- `dnsd: port 53: Address already in use` (or another reason): it could not take
  its port, and ends with `1`; `elvesd` starts it again after a wait.
  Otherwise `dnsd` runs until it is stopped and prints nothing.

What the programs asking it see, for example `ping`'s or `fetch`'s
message:

- `Name not found`: the DNS server knows no such name.
- `Timed out`: the server did not answer (twice 2 seconds); `dnsd` answers
  "server failure".
- `Network is unreachable`: no card has a DNS server; `dnsd` answers
  "refused" at once, so nobody waits for nothing.

## Examples

```
ritteros:/$ elf
elvesd: running (process 3)
dhclient         on   running (process 4)     dhclient
dnsd             on   running (process 5)     dnsd
ritteros:/$ ping -c 1 example.com
PING example.com (93.184.215.14): 56 data bytes
...
ritteros:/$ elf restart dnsd
```

## See also

[`elf`](elf.md), [`elvesd`](elvesd.md), [`net`](net.md),
[`dhclient`](dhclient.md), [`ping`](ping.md), [`fetch`](fetch.md),
[`guard`](guard.md), [`guardd`](guardd.md)

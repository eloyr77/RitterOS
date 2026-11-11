# sshd: the SSH server: log in from other machines (root)

```
sshd [-p PORT]
```

The SSH server: it lets people log in on this machine from other computers
with `ssh`, run commands there and move files (since step 40c). It listens
on TCP port 22, or PORT, over IPv6 and IPv4 alike (since 64d: one IPv6
socket; an IPv4 client shows as `10.0.2.2`, an IPv6 one as `2001:db8::5`
or `fe80::1%eth0`), and handles every connection in a process of its
own (it starts itself again as `sshd -c ADDRESS` with the connection as its
input and output, as inetd would). Only root may start it.

**An elf.** `sshd` is meant to be kept running by `elvesd` (since 44h): on a
machine with a network card, `net up` at the first login makes its elf's
file `/system/config/elves/sshd`,

```
# The SSH server, an elf (net up made this file)
command = sshd
start = on
```

so it runs from then on, and again after every boot. `elf stop sshd`
switches it off (now and at the boots to come), `elf start sshd` on again,
`elf restart sshd` ends its process (elvesd starts it again), and `elf`
alone shows whether it runs. On a machine without a card, `elf add sshd
sshd` makes the elf; `sshd &` starts it once, as a background job. The
firewall's first rules (`/system/config/guard`) let TCP port 22 in.

**The host key.** The first start makes the machine's host key (Ed25519)
and keeps it secret in `/system/config/ssh_host_ed25519_key` (`seed =
HEX`, mode 0600); every start prints its fingerprint. The first time a
client connects it shows the same fingerprint and asks whether to trust it:
compare the two.

**Logging in** works with an Ed25519 key listed in
`HOME/.ssh/authorized_keys` (one per line as OpenSSH writes them:
`ssh-ed25519 AAAA... comment`), or with the user's password, as `login`
checks it. After 10 tries the connection is closed. Each login, refusal and
logout is reported on the console and written to the log `login`
(`/system/logs/login.log`).

**The session.** A client that asks for a terminal (an interactive login,
or `ssh -t`) gets a pseudo-terminal running the user's shell, with echo and
line editing, sized as the client's window (and resized with it); when the
client goes away, the terminal hangs up and its programs end. A client that
runs a command without a terminal (`ssh HOST COMMAND`, as scripts and tools
do) instead gets pipes: standard input, output and error pass bytes through
unchanged, so binary and file transfers are clean. The command runs under
`sh -c`, so `;`, pipes and redirections work in it, at any length; its exit
status becomes the `ssh` client's. Either way the connection's programs
are a session of their own (41h-8: since then also without a terminal),
so what one session holds, such as sudo's 5-minute memory, never reaches
another connection.

The connection uses curve25519-sha256 for the key exchange, ssh-ed25519 host
keys and chacha20-poly1305@openssh.com encryption, all written for RitterOS
from the RFCs (current OpenSSH has all three). Not there yet: other
algorithms, keys other than Ed25519, a new key exchange during a long
connection (OpenSSH asks for one after about a gigabyte), port forwarding,
`scp` and `sftp`.

## Options

| Option | Meaning |
|---|---|
| `-p PORT` | listen on PORT (1-65535) instead of 22; for an elf, as `command = sshd -p PORT` in its file |

`-c ADDRESS` is what `sshd` passes to its own copy for one connection; it
is not for use by hand.

## Exit status and messages

On the output, at the start and for each connection:

- `sshd: made a new host key SHA256:...` (the first start) or `sshd: the
  host key SHA256:...`
- `sshd: listening on port 22`
- `sshd: a connection from 10.0.2.2 (process 12)`

On the error output, from the connection's process:

- `sshd: root logged in from 10.0.2.2 (publickey)` (or `(password)`)
- `sshd: NAME from 10.0.2.2: password refused` (or `publickey refused`)
- `sshd: root from 10.0.2.2: the session ended`
- `sshd: 10.0.2.2: REASON`: the connection failed (the key exchange, a bad
  packet, the client went away).

Exit codes:

- `0`: stopped by Ctrl-C (or a signal while it waits).
- `1`: `sshd: sshd: only root may start it`, `sshd: listen: REASON` (the
  port is taken, for instance), or the host key cannot be read or made
  (`sshd: /system/config/ssh_host_ed25519_key: no seed = ... in it`).
- `2`: anything but `-p PORT`, or a port outside 1-65535: `usage: sshd [-p
  PORT]`.

## Examples

```
ritteros:/$ elf
ritteros:/$ elf stop sshd
ritteros:/$ elf start sshd
ritteros:/$ sshd -p 2200 &
[7]
sshd: the host key SHA256:Xl3C...
sshd: listening on port 2200
```

From another computer:

```
$ ssh root@ritteros
$ ssh root@ritteros 'cat > /system/x86_64/bin/game' < game      # copy a program in
$ ssh root@ritteros 'cat /system/logs/kernel.log' > kernel.log  # copy a file out
$ ssh root@ritteros 'mem; uptime'                               # run commands
$ ssh root@ritteros sh < script                                 # run a script
```

From the Mac with `make run` (QEMU passes the Mac's port 2222 to the
machine's port 22): `ssh -p 2222 root@localhost`.

## See also

[`ssh`](ssh.md), [`elf`](elf.md), [`elvesd`](elvesd.md), [`net`](net.md),
[`guard`](guard.md), [`login`](login.md), [`passwd`](passwd.md),
[`sh`](sh.md)

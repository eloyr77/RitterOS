# ssh: log in on another machine (SSH)

```
ssh [-p PORT] [USER@]HOST [COMMAND]...
```

Logs in on HOST as USER (else under your own name) and runs a shell there,
or COMMAND: the words after HOST, joined with spaces, as the other side's
shell reads them. The session runs on a terminal on the other side, with
yours in raw mode: every key goes there and what comes back is shown. The
exit code is the remote command's. `ssh` came in step 40d, with the
cryptography written for RitterOS from the RFCs: curve25519-sha256,
ssh-ed25519 host keys and chacha20-poly1305@openssh.com, which current
OpenSSH servers (and RitterOS's own [`sshd`](sshd.md)) all have.

HOST is a name or an address, IPv6 too (since 64d: `ssh root@2001:db8::5`,
`ssh root@[fe80::1%eth0]`); a name with both kinds of address is tried
IPv6 first on a network with IPv6, the next address if one does not
answer. The server's host key is checked against
`HOME/.ssh/known_hosts` (OpenSSH's format: `HOST ssh-ed25519 BASE64`, or
`[HOST]:PORT ...` for a port other than 22). A host seen the first time is
shown with its key's fingerprint; type `yes` to connect and keep the key
there (the folder `HOME/.ssh` is made if needed). A known host that shows a
different key is refused: someone may be in between; remove its line from
the file if the key was changed on purpose.

Logging in: with the key `HOME/.ssh/id_ed25519` if there is one (OpenSSH's
format, Ed25519, without a passphrase), else with the password, asked
without echo, up to three times.

During a session, `~.` at the start of a line (after Enter) ends the
connection; `~~` sends one `~` (so `~~.` reaches a `cu` or `ssh` running on
the other side as `~.`). A `~` followed by anything else sends both.

## Options

| Option | Meaning |
|---|---|
| `-p PORT` | connect to PORT (1-65535) instead of 22; it must come first, before HOST |

## Exit status and messages

- The remote command's exit code, or the shell's when it ends; `129` when
  it was ended by a signal.
- At the end: `ssh: the connection to HOST is closed`.
- `ssh: HOST is not known yet. Its host key: SHA256:...` and `ssh: Connect,
  and keep the key in known_hosts? (yes/no)`: anything but `yes` ends it
  (`255`).
- `ssh: WARNING: the host key of HOST is not the one in PATH (it is
  SHA256:... now).` followed by `ssh: Someone may be in between; if the key
  was changed on purpose, remove the line from that file.`: refused (`255`).
- `USER@HOST's password:`, after a wrong one `Permission denied, please try
  again.`; after three, `ssh: HOST: Permission denied` (`255`).
- `ssh: PATH has a passphrase: not supported yet`: the key is skipped, the
  password asked.
- `ssh: PATH: REASON (the key is not kept)`: `known_hosts` could not be
  written; the connection goes on.
- `ssh: HOST: REASON` (`255`): the name could not be looked up (`Name not
  found`), the connection could not be made (`Connection refused`, `Timed
  out`, `Network is unreachable`), or the server broke off.
- `ssh: session: the server refused it`, `the server did not start a
  shell`, `the server did not run the command` (`255`).
- `2`: no HOST, or a PORT out of range: `usage: ssh [-p PORT] [USER@]HOST
  [COMMAND]`.

## Examples

```
ritteros:/$ ssh root@10.0.2.2
ssh: 10.0.2.2 is not known yet. Its host key: SHA256:Xl3C...
ssh: Connect, and keep the key in known_hosts? (yes/no) yes
root@10.0.2.2's password:
...
ritteros:/$ ssh -p 2222 anna@192.168.1.20 ls /home/anna
ritteros:/$ ssh root@192.168.1.20 'uptime; mem'
```

## See also

[`sshd`](sshd.md), [`cu`](cu.md), [`net`](net.md), [`ping`](ping.md),
[`guard`](guard.md)

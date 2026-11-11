# logd: the logs' keeper, an elf: writes /system/logs/NAME.log every 10 s

```
logd
elf start logd
elf stop logd
```

`logd` is an elf: a program `elvesd` keeps running, not one you type. The
kernel collects the logs in memory: its own messages (from the boot's first
line on), the elves' output, and `login`'s lines about logins. Every 10
seconds `logd` takes what the kernel kept and adds it to
`/system/logs/NAME.log`, a plain text file per log, each line with its date
and time (UTC) in front: `kernel.log`, `login.log` (logins, refused logins
and logouts, on the console and over SSH; `su` and `sudo` too),
`elvesd.log`, and one per elf with what it printed (`dhclient.log`,
`sshd.log`, ...). It makes `/system/logs` when it is missing. A log that
would grow past 64 KiB becomes `NAME.log.old` (the one before it goes) and
a new one begins. Lines the kernel had to drop because its memory for logs
was full are noted where they were: `(N bytes lost here: the log was
full)`. `logd` came with step 45c.

Every system has the elf: `login` makes its file,
`/system/config/elves/logd`, at the first start when it is missing (`login:
/system/config/elves/logd made: the elf logd keeps the logs in
/system/logs`):

```
command = logd
start = on
```

Asked to stop (SIGTERM, which `elf stop logd` and `elf restart logd`
send), it writes what is left first, then ends. While it is off the kernel
keeps collecting in memory, a limited amount (what does not fit is noted as
lost), until it runs again. `poweroff` and `reboot` let it write its last
lines before the machine goes. A normal boot writes nothing else to the
disk; the logs are the agreed exception.

## Exit status and messages

- `0`: it was asked to stop and wrote what was left.
- `1`: `/system/logs` is missing and could not be made: `logd:
  /system/logs: REASON`.
- `logd: /system/logs/NAME.log: REASON`: a log could not be written (or
  renamed to `.old`); `logd` goes on with the next one.

## Examples

```
ritteros:/$ elf
elvesd: running (process 2)
logd             on   running (process 4)     logd
ritteros:/$ ls /system/logs
ritteros:/$ tail -f /system/logs/kernel.log
ritteros:/$ grep -ri 'error' /system/logs
ritteros:/$ elf stop logd
elf: logd off (elvesd stops it within 2 s)
```

From another machine:

```
mac$ ssh root@ritteros 'cat /system/logs/kernel.log' > kernel.log
```

## See also

[`elf`](elf.md), [`elvesd`](elvesd.md), [`login`](login.md),
[`tail`](tail.md), [`grep`](grep.md), [`reboot`](reboot.md),
[`poweroff`](poweroff.md)

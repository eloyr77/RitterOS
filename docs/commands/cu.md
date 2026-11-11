# cu: a terminal on a serial line (~. leaves)

```
cu [-l] DEVICE
```

Connects your terminal to a serial port: what you type goes to DEVICE as it
is, Ctrl-C included (it reaches the device, not `cu`), and what the device
sends comes to your screen. DEVICE is a path, or a name in `/dev`: `cu
usbserial0` is `cu /dev/usbserial0`. `-l DEVICE` is Unix's form and does
the same. `cu` has been in RitterOS since step 50e (the name is Unix's
"call up").

USB serial ports (CDC ACM) appear in `/dev` when they are plugged in, also
as one function of a device with several, such as the PlutoSDR's console.
The line is 115200 baud, 8 bits, no parity, with DTR and RTS on (a
device's own USB port ignores the speed). Bytes the device sent before
`cu` started are shown first: the port keeps up to 16 KiB.

While `cu` runs, your terminal is in raw mode: every key goes to the
device at once, and the device does its own echo and line editing. Two
escapes are read at the start of a line (after Enter):

| Typed | Does |
|---|---|
| `~.` | leaves `cu` (`Disconnected.`) |
| `~~` | sends one `~` |

A `~` followed by anything else sends both characters.

Inside an `ssh` session, the `ssh` you started from reads `~.` at the start
of a line too, and would end the whole connection. Type `~~.` instead: that
`ssh` passes one `~` on, and `cu` sees `~.`. With OpenSSH (on a Mac or a
Linux machine) you can also start the session as `ssh -e none ...`, which
switches its escape off; RitterOS's own [`ssh`](ssh.md) has no `-e`, so
from it use `~~.`.

`cu` also ends by itself when the device is unplugged, or when its input
ends (when it is not reading a terminal: `cu usbserial0 < commands.txt`).

## Options

| Option | Meaning |
|---|---|
| `-l` | Unix's form, `cu -l DEVICE`: the same as `cu DEVICE` (it must be the first word) |

## Exit status and messages

- At the start: `Connected to /dev/usbserial0 (~. leaves).`
- `0`: left with `~.` (`Disconnected.`), or its input ended
  (`Disconnected: the input ended.`).
- `1`: the device could not be opened (`cu: /dev/NAME: No such file or
  directory`: no such port; `cu: /dev/NAME: Permission denied`: only root
  and the user at the console may use the ports), or the connection ended
  otherwise: `Disconnected: the device is gone.` (unplugged),
  `Disconnected: the device refused what was typed.`, or a read error's
  text.
- `2`: no DEVICE, or more than one word: `usage: cu DEVICE`.

## Examples

```
ritteros:/$ cu usbserial0
Connected to /dev/usbserial0 (~. leaves).

Welcome to Tezuka
pluto login: root
...
~.
Disconnected.
ritteros:/$ cu -l /dev/usbserial0
ritteros:/$ cu usbserial3
cu: /dev/usbserial3: No such file or directory
```

## See also

[`ssh`](ssh.md), [`net`](net.md), [`ls`](ls.md) (`ls /dev` lists the
serial ports)

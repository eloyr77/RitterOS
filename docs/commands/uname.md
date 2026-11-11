# uname: the system's name, release, commit and machine

```
uname [-amnrsv]
```

Prints the system's names, as POSIX's `uname`. Without options it prints
the system's name, `RitterOS`. The options choose what is printed, always
in this order and separated by spaces:

- `-s`: the system's name, `RitterOS`.
- `-n`: the machine's name. RitterOS has no host names yet: `ritteros`.
- `-r`: the release, year.month (`2026.11`; a second release in the same
  month is `2026.11.1`). It comes from the file `VERSION` at the top of
  the source tree.
- `-v`: the commit the kernel was built from (`513e2a4`), with a `+` when
  the tree had changes not yet committed; `unknown` for a kernel built
  outside a git checkout.
- `-m`: the machine: `x86_64` or `aarch64`.
- `-a`: all of them.

Options may be given apart (`-r -m`) or together (`-rm`). The answers are
the running kernel's: a machine whose kernel was replaced shows the new
one only after its reboot. The kernel's first line at the boot says the
same: `RitterOS kernel started: 2026.11 (513e2a4), x86_64`. Programs ask
with `uname()` from `<sys/utsname.h>`. `uname` has been in RitterOS
since step 68i.

## Exit status and messages

- `0`: the names were printed.
- `2`: `uname: unknown option -X` or `uname: unexpected 'WORD'` (uname
  takes no operands), then `usage: uname [-amnrsv]`.

## Examples

```
ritteros:/$ uname
RitterOS
ritteros:/$ uname -a
RitterOS ritteros 2026.11 513e2a4 x86_64
ritteros:/$ uname -rm
2026.11 x86_64
```

From another computer, which release a machine runs:

```
ssh root@192.168.1.20 uname -r
```

## See also

[`uptime`](uptime.md), [`ssh`](ssh.md), [`sh`](sh.md)

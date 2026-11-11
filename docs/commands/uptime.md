# uptime: time since the kernel's timer started

```
uptime
```

Prints how long the machine has been running: the seconds (with two
decimals) since the kernel started its timer early in the boot, and the
timer ticks counted, at 100 ticks a second. `uptime` has been a program
since step 11d; the rescue console has an `uptime` command too.

`uptime` takes no options; arguments are ignored.

## Exit status and messages

- `0`: the time was printed: `up SECONDS s (TICKS timer ticks at 100 Hz)`.
- `1`: the kernel did not answer (nothing is printed).

## Examples

```
ritteros:/$ uptime
up 5.25 s (525 timer ticks at 100 Hz)
ritteros:/$ echo "today: $(uptime)"
today: up 312.40 s (31240 timer ticks at 100 Hz)
```

Over SSH from another machine:

```
mac$ ssh root@ritteros 'uptime; elf'
```

## See also

[`mem`](mem.md), [`date`](date.md), [`ps`](ps.md), [`threads`](threads.md)

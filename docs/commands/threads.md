# threads: list the kernel's threads

```
threads
```

Prints the kernel's list of threads, the same report as the rescue
console's `threads` command: every kernel thread and every thread of a
program, oldest first (all of them since 65d; until then at most 32). The
columns are the thread's number, its name, its state (`running` is one
running on a core now, `ready` one that could run and waits for its turn,
`waiting` one that waits for something: input, a disk, a timer, another
process; `ended` one not yet cleaned up), `cpu`, the processor core it runs
on or last ran on (65d; `-` if it has not run yet), the timer ticks it was
running at (the tick is 10 ms, so it is a rough measure of the time it
used) and how often the scheduler switched to it.

`main`, `idle` and `console` are always there, and an idle thread for
each other core (`idle1`, `idle2`, ...); `usb` with a USB
controller, a card's thread (`eth0`, `wlan0`) per network card; a
program's first thread has the program's name (`login`, `sh`), its other
threads the name and a number (`windowd:1`). Threads came in step 11c1.
It takes no arguments.

Since 64t a last line shows the processor's real clock while it ran and
how much of the time it ran (65d: with several cores, a line for each,
under a heading with the base clock), on processors that count both (x86's APERF
and MPERF: AMD and Intel processors have them, QEMU's do not, and on ARM
the line is left out):

```
  clock: 3539 MHz while running (base 3194 MHz), running 27.3% of the time in the 20 s since the last look
```

and with several cores:

```
  clock (base 3194 MHz), in the 20 s since the last look:
    core  0: 3539 MHz while running, running 27.3% of the time
    core  1: 2410 MHz while running, running 4.1% of the time
```

Each core reads its own counters when the list is made (asked by an
interrupt), so nothing is measured between lists.

The first list after the start counts from when the machine was switched
on, every later one from the list before. The clock is the average while
the processor worked (it does not count while it waits for an
interrupt), so it shows whether it boosts above its base clock whatever
the load; the base is the rate of its time stamp counter. Nothing is
measured between lists: the processor counts by itself, and a list reads
its counters. What the processor aims for is set in /system/config/cpu
(`clock`, read at the start; see the manual).

## Exit status and messages

- `0`: the list was printed.
- `1`: the kernel did not give the report (nothing is printed).

## Examples

```
ritteros:/$ threads
  id  name             state     cpu   ticks  switches
   0  main             waiting    0      42     15078
   1  idle             ready      0     468     15455
   4  idle1            running    1     455       912
   5  console          waiting    1       1       465
   6  usb              waiting    0       0         1
   7  login            waiting    0     148       361
   8  elvesd           waiting    1       0         3
   9  sh               waiting    0      16       187
  24  threads          running    1       0         1
```

## See also

[`ps`](ps.md) (the processes), [`uptime`](uptime.md), [`mem`](mem.md),
[`kill`](kill.md)

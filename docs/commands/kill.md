# kill: ask processes to end (SIGNAL: TERM, KILL, INT, HUP, USR1, USR2 or a number)

```
kill [-SIGNAL] PID...
```

Sends a signal to each process PID, numbered as `ps` lists them. Without
-SIGNAL it is TERM (15), which asks the process to end. A program ends at
the signal unless it ignores it or has a handler for it (since step 22r
programs may have handlers); KILL (9) cannot be ignored or handled. Root
may signal any process, other users only their own (the kernel checks).
`kill` came with the remote development loop, so that an agent working
over SSH can stop what it started; the shell reports a job that ended by
a signal (`[4] stopped by signal 15`).

## Options

| Option | Meaning |
|---|---|
| `-TERM` | the signal TERM (15): the default |
| `-KILL` | KILL (9): the process ends at once, whatever it does |
| `-INT` | INT (2), what Ctrl-C sends |
| `-HUP` | HUP (1), what a closed terminal sends |
| `-USR1`, `-USR2` | USR1 (10), USR2 (12): requests a program defines itself (66c-1: `windowd` prints its frame report on USR1) |
| `-N` | the signal numbered N, 1-31 |

The names may also be written with `SIG` in front (`-SIGKILL`). Only the
first argument is read as a signal; it must come before the PIDs.

## Exit status and messages

- `0`: every process got the signal.
- `1`: a PID was not right; the others still get the signal:
  - `kill: PID: not a process number`
  - `kill: PID: No such process`
  - `kill: PID: Operation not permitted` (another user's process)
- `2`:
  - `kill: SIGNAL: not a signal (TERM, KILL, INT, HUP, USR1, USR2 or 1-31)`
  - no PID given: `usage: kill [-SIGNAL] PID...`

## Examples

```
ritteros:/$ /system/x86_64/test/loop &
[4]
loop: running until Ctrl-C
ritteros:/$ ps
  pid  parent  group  state    name
    1       0      1  running  login
    2       1      2  running  elvesd
    3       1      3  running  sh
    4       3      4  running  loop
    5       3      5  running  ps
ritteros:/$ kill 4
[4] stopped by signal 15
ritteros:/$ kill -KILL 999
kill: 999: No such process
ritteros:/$ kill -STOP 4
kill: STOP: not a signal (TERM, KILL, INT, HUP, USR1, USR2 or 1-31)
```

## See also

[`ps`](ps.md), [`sh`](sh.md) (jobs, `&`, Ctrl-C), [`elf`](elf.md)
(stopping elves)

# kill: ask processes to end, stop or go on (SIGNAL: TERM, KILL, INT, HUP, USR1, USR2, STOP, CONT or a number)

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
a signal (`[1] ended by signal 15`).

## Options

| Option | Meaning |
|---|---|
| `-TERM` | the signal TERM (15): the default |
| `-KILL` | KILL (9): the process ends at once, whatever it does |
| `-INT` | INT (2), what Ctrl-C sends |
| `-HUP` | HUP (1), what a closed terminal sends |
| `-USR1`, `-USR2` | USR1 (10), USR2 (12): requests a program defines itself (66c-1: `windowd` prints its frame report on USR1) |
| `-STOP` | (44a) STOP (19): the process stops (it keeps its memory and files, and gets no processor time) until CONT; it cannot be ignored or handled. `ps` shows it `stopped` |
| `-TSTP`, `-TTIN`, `-TTOU` | TSTP (20), TTIN (21), TTOU (22): stop the process too, unless it handles or ignores them (TSTP is what a terminal's Ctrl-Z sends) |
| `-CONT` | CONT (18): a stopped process goes on; a call it was waiting in when it stopped goes on too |
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
  - `kill: SIGNAL: not a signal (TERM, KILL, INT, HUP, USR1, USR2, STOP, CONT or 1-31)`
  - no PID given: `usage: kill [-SIGNAL] PID...`

## Examples

```
ritteros:/$ /system/x86_64/test/loop &
[1] 4
loop: running until Ctrl-C
ritteros:/$ ps
  pid  parent  group  state    name
    1       0      1  running  login
    2       1      2  running  elvesd
    3       1      3  running  sh
    4       3      4  running  loop
    5       3      5  running  ps
ritteros:/$ kill 4
[1] ended by signal 15
ritteros:/$ kill -KILL 999
kill: 999: No such process
ritteros:/$ /system/x86_64/test/loop &
[1] 6
loop: running until Ctrl-C
ritteros:/$ kill -STOP 6
ritteros:/$ ps
  pid  parent  group  state    name
    1       0      1  running  login
    2       1      2  running  elvesd
    3       1      3  running  sh
    6       3      6  stopped  loop
    7       3      7  running  ps
ritteros:/$ kill -CONT 6
ritteros:/$ kill -FOO 6
kill: FOO: not a signal (TERM, KILL, INT, HUP, USR1, USR2, STOP, CONT or 1-31)
```

## See also

[`ps`](ps.md), [`sh`](sh.md) (jobs, `&`, Ctrl-C), [`elf`](elf.md)
(stopping elves)

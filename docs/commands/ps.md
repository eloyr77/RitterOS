# ps: list the processes

```
ps
```

Lists the processes that are running, oldest first, as the kernel reports
them (the rescue console's `ps` shows the same list). Each line gives the
process ID, its parent's ID, its process group, its state and its name:

- **pid**: the number [`kill`](kill.md) takes.
- **parent**: the process that started it (0 for `login`, which the kernel
  starts).
- **group**: the job it belongs to. The shell starts every command as a job
  of its own, whose group number is its first process's ID; the commands of
  a pipeline share one group.
- **state**: `new` (being started), `running`, or `ended` (finished but not
  yet waited for by its parent).
- **name**: the program's name.

At most 32 processes are listed. `ps` takes no options; arguments are
ignored. It lists itself too.

## Exit status and messages

- `0`: the list was shown.
- `1`: the kernel did not answer (nothing is printed).

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
```

## See also

[`kill`](kill.md), [`threads`](threads.md), [`elf`](elf.md),
[`mem`](mem.md), [`sh`](sh.md) (jobs and `&`)

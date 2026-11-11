# cpus: list the processor's cores

```
cpus
```

Prints the kernel's list of the processor's cores, the same report as the
rescue console's `cpus` command: every core the firmware names (ACPI's
MADT table), in its order. The columns are the kernel's number for the
core (`-` for one it did not start), its hardware ID (x86: the Local APIC
ID; ARM: the MPIDR's affinity fields) and its state:

- `running (the boot core), N ticks, M device interrupts, K threads taken
  from other cores`: the core the
  machine started with; M counts the devices' interrupts it handled
  (not the tick, nor other cores asking it something); K (65e-39: each
  core has its own queue of threads ready to run) the threads it took
  from another core's queue, idle or with a queue two shorter;
- `online, started in T, N ticks, M device interrupts, K threads taken
  from other cores`: started at the boot (every core is,
  unless the kernel command line's `cpus=N` asks for fewer); it set itself
  up (its interrupts, its tick)
  and runs threads like the boot core (since 65c-2), its idle thread
  (`idle1`, `idle2`, ... in `threads`) when none is ready. T is the
  time from the start signal to its report (on x86 mostly the 10 ms wait
  after INIT); N its timer interrupts so far, M the devices' (65d: a
  network card's interrupts go to the cores after the boot core, one card
  each in turn, as the boot log's `interrupts:` lines say);
- `left off (cpus=N)`: not started, because the kernel command line asked
  for at most N cores;
- `did not start`: it was asked to and did not report in;
- `cannot be started: ...`: the reason (for example an x86 APIC ID above
  254, which needs the x2APIC mode).

Since 65e a last part lists the locks the cores had to wait for since the
start, the longest waits first (at most ten), each by the lock's name
(every lock of one name together: each process's `mappings`, each
file system's `ritterfs`, each TCP connection's `tcp connection`, beside
the connections' table, `tcp table`). Since 65e-36 a file system's lock is a
readers-writer lock: its waits are `(read)` (a reader waited for a writer)
or `(write)`; its block cache has `ritterfs cache`, and its directory index
and snapshot views `ritterfs index`. Since 65e-38 a process's `mappings`
is a readers-writer lock too: page faults wait `(read)` for a change of
the mappings (mmap, munmap, mprotect, fork, a copy-on-write fault), the
changes `(write)`. `(spun)` is for a spinlock, whose waiter goes round
until it is free (time lost), and for a mutex's or (since 65e-45) a
readers-writer lock's waiter going round a moment before it sleeps;
`(slept)` for a mutex, whose waiter sleeps meanwhile (other threads run),
as `(read)` and `(write)` are a readers-writer lock's sleeps. A sleep's
time runs until the lock is taken, so it includes waiting for a free core
after the wake-up: with more busy threads than cores, much of it is that
(compare the idle threads' ticks in `threads`). It is where several cores get in each
other's way:

```
  waits for locks since the start, the longest first:
    mappings         (slept)      29992 times, 30780.702 ms in all
    heap             (spun)      691631 times, 25215.127 ms in all
    scheduler        (spun)       60120 times, 50.284 ms in all
```

Since 65e-31 the last list is the file systems' lock (`ritterfs`, one per
RitterFS: every operation takes it) by where it was taken: how often, how
long it was held in all and at most, how long others waited while it held
it, and how long it waited itself (the J1900's first 30 s, its SSD):

```
  the file systems' lock (ritterfs) by where it was taken, the longest held first:
    ritterfs_commit                 5 times, held 543.133 ms in all, at most 113.480 ms; others waited 592.762 ms, it waited 0.093 ms
    ritterfs_lookup              1128 times, held 31.372 ms in all, at most 4.513 ms; others waited 4.752 ms, it waited 482.172 ms
```

Since 65e-32 it ends with where the commits' time goes, phase by phase
(how often, in all, at most): a transaction begun, a bitmap block read in,
the new blocks written, the first flush, the superblock and its flush, the
freed blocks found and TRIMmed, the bitmap copied.

The boot log's `cores:` line sums it up. It takes no arguments.

## Exit status and messages

- `0`: the list was printed.
- `1`: the kernel did not give the report (nothing is printed).

## Examples

```
ritteros:/$ cpus
  core  id      state
     0  0x0     running (the boot core), 613 ticks, 1412 device interrupts, 210 threads taken from other cores
     1  0x1     online, started in 10.5 ms, 531 ticks, 2380 device interrupts, 35 threads taken from other cores
     2  0x2     online, started in 10.0 ms, 530 ticks, 96 device interrupts, 41 threads taken from other cores
     3  0x3     online, started in 10.0 ms, 530 ticks, 88 device interrupts, 29 threads taken from other cores
```

## See also

[`threads`](threads.md) (the kernel's threads), [`mem`](mem.md),
[`uptime`](uptime.md)

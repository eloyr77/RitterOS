# mem: free and usable memory

```
mem
```

Shows how much of the machine's memory is free, exact to the page, and what
the page cache holds. The first line gives the free and the usable memory
in MiB and in pages (4 KiB each); usable is the RAM the kernel can give
out, what the firmware keeps for itself not counted. The second line is the page
cache's: the files in it, their pages, how many of those pages are kept
although nothing maps them any more, and how many pages were read from
files since the start. The third line (since 35a) is swap's: `swap: off`,
or how much of the swap file holds pages and how many were written to it
and read from it since the start (see [`swap`](swap.md)).

Programs and libraries are loaded through the page cache: one copy in memory
serves every program running the same file. A file nothing maps any more
stays cached (up to 32 MiB, at most an eighth of the memory; the one unused
longest goes first), so a program started again is not read from the disk
again. `mem` gives those kept files back before it counts, so they show as
free memory and the count is exact (the rescue console's `mem` and the
program [`uptime`](uptime.md) do the same). The page cache line came with
step 22g.

`mem` takes no options; arguments are ignored.

## Exit status and messages

- `0`: the memory was shown.
- `1`: the kernel did not answer (nothing is printed).

## Examples

```
ritteros:/$ mem
memory: 4065 MiB free of 4086 MiB (1040746 of 1046188 pages of 4 KiB)
page cache: 6 file(s), 35 page(s) (0 kept unused), 35 page(s) read
swap: off
ritteros:/$ ssh root@ritteros 'mem; uptime'
```

## See also

[`ps`](ps.md), [`threads`](threads.md), [`uptime`](uptime.md),
[`ldd`](ldd.md), [`swap`](swap.md)

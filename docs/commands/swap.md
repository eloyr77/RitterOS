# swap: show swap; make the swap file /system/swap, turn swap on or off

```
swap [make SIZE | on | off]
```

Swap is where the kernel keeps pages of programs' memory while memory is
short. On RitterOS it is one file, `/system/swap`, on the system's RitterFS
(since step 35a; pages go there since 35b).

- `swap` alone shows whether swap is on, the file's size, how many of its
  pages (4 KiB each) hold something, and how many pages were written to it
  and read from it since the start. Anyone may ask.
- `swap make SIZE` makes `/system/swap` of SIZE (in MiB or GiB: `512M`,
  `4G`) and turns swap on. Making it writes zeros once, which gives the
  file its blocks on the disk; nothing else is written until then.
- `swap on` turns swap on with the file that is there, and keeps it on at
  every later start.
- `swap off` turns it off, for later starts too. Pages still in the file
  are read back first; if memory has no room for them, it is refused.

Changing swap is root's. Both `on` and `off` write
`/system/config/swap` (`on = yes` or `on = no`), which the kernel reads at
every start, right after the system's disk is mounted; without the file
swap stays off. Since 37e-1b the kernel writes the file with `on = no` and
comments when it is missing, and `on` and `off` change only that line (a
missing file starts as that text), so the comments stay.

**A fixed file.** RitterFS never writes a block in place, and keeps a
checksum of every block in the pointer to it. The swap file is the
exception, a *fixed* file (RitterFS's flag `fixed`, made so the first time
swap goes on): its blocks stay where they are, and only the kernel writes
them, in place, so their checksums are not kept (a check and a scrub look
at its blocks' places, not their contents). Programs cannot read, write,
copy or cut it (`Device or resource busy`), and while swap is on it cannot
be removed; a copy of a folder that holds it leaves it out. A file can
become the swap file only if it is made of whole blocks without holes, and
no snapshot or instant copy shares any of them (else it would change
under them): `swap make` makes such a file. A snapshot taken afterwards
holds the file's blocks like any other's (deleting the file then frees
them only with the snapshot), but reading the file through it is refused
too.

**Paging out** (since 35b). While swap is on and free memory falls below a
mark (a 64th of the memory, between 1 and 64 MiB), the kernel's thread
`pageout` goes through the programs, a few dozen pages of one at a time,
and writes the pages a program has not used since its last look to the
file; the page is then free. A page used meanwhile only gets its "used" mark
cleared (on x86 the processor's accessed bit, on ARM the access flag), so
pages in use stay in memory. When the program touches a page in swap, the
kernel reads it back (the program waits for that one read). A program that
needs a page when none is free waits for `pageout` instead of failing; only
when nothing can be written out (swap full, or every page in use) does it
run out of memory, as without swap. Pages go out of a program's own
memory and of memory programs share (`MAP_SHARED | MAP_ANONYMOUS`, since
35c-2: such a page is taken out of every program that maps it at once),
not of files they map. Since 35c that includes the pages a program and its forked
children hold together until one of them writes: such a page goes to swap
once, and each holder's entry names the same place in the file; the page
is free once the last holder's entry went. A child forked with pages in
swap names their places too, so forking needs no memory for them. A page
read back from a place others still name stays linked to it while it is
only read, so going out again it is not written a second time. The
kernel's own memory never goes to swap.

**Encrypted.** Every page is written encrypted (XTS-AES-256, as disk
encryption does, with the page's place in the file in the encryption) with
a key the kernel makes each time swap goes on and keeps only in memory.
What stays in the file after the machine is switched off cannot be read by
anyone, so passwords and keys that programs held in memory never reach the
disk readable.

## Exit status and messages

- `0`: done (or shown).
- `1`: it did not work, with the reason:
  - `swap: on: No such file or directory (no /system/swap: swap make SIZE
    makes one)`;
  - `swap: on: Invalid argument (/system/swap has holes, a part block or
    blocks a snapshot shares)`: that file cannot be the swap file (remove
    it, then `swap make`);
  - `swap: /system/swap: File exists (swap off, then rm it, to make
    another)`;
  - `swap: off: Cannot allocate memory`: the pages in the file do not fit
    in the free memory (end a program, then again);
  - `swap: changing swap is root's`.
- `2`: wrong arguments (a usage line, or `not a size in MiB or GiB`).

The kernel's own lines (the console and `/system/logs/kernel.log`) say when
swap goes on (the file's size, its pages, in how many runs of consecutive
blocks it lies) and off, with how many pages `swap off` read back.

## Examples

```
ritteros:/$ swap
swap: off; no /system/swap (swap make SIZE makes one)
ritteros:/$ swap make 4G
swap: /system/swap made: 4096 MiB
swap: on, /system/swap: 4096 MiB (1048576 pages), 0 in use; since the boot 0 pages written, 0 read
ritteros:/$ mem
memory: 4065 MiB free of 4086 MiB (1040746 of 1046188 pages of 4 KiB)
page cache: 6 file(s), 35 page(s) (0 kept unused), 35 page(s) read
swap: 0 of 4096 MiB used (0 of 1048576 pages), 0 pages written, 0 read
ritteros:/$ swap off
swap: off; /system/swap: 4096 MiB
```

## See also

[`mem`](mem.md), [`snapshot`](snapshot.md), [`disks`](disks.md)

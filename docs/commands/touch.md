# touch: make empty files, or give files the time now (or another)

```
touch [-acm] [-r REFERENCE | -t [[CC]YY]MMDDhhmm[.SS]] FILE...
```

Gives each FILE the current time as its modification time, the time `make`
and `ls -l` go by; folders too. A FILE that is missing is made, empty
(unless `-c`). `touch` came with step 19b (making files); since step 63m it
sets times as POSIX's touch does: `-t` gives another time, `-r` the time of
another file.

`-t` takes the time in the machine's local time (the zone of
`/system/config/time`): `touch -t 202001021530.44 f` is 2 January 2020,
15:30:44. With a two-digit year, 69-99 is 19YY and 00-68 20YY; without a
year it is this year's; without `.SS` the seconds are 0. `-m` sets the
modification time and `-a` the access time (both when neither is given);
RitterOS keeps no access time, so `-a` alone changes nothing. Setting the
time now needs the right to write the file (or owning it); any other time
owning it (or root). On FAT a time is kept in steps of 2 seconds, on exFAT
of 10 ms. A file that is made gets the mode 0666 less the umask (0644
usually).

## Options

| Option | Meaning |
|---|---|
| `-a` | set the access time (RitterOS keeps none) |
| `-c` | make no missing FILE: one that is not there stays missing, without a message |
| `-m` | set the modification time |
| `-r REFERENCE` | the time of the file REFERENCE instead of now |
| `-t [[CC]YY]MMDDhhmm[.SS]` | this time instead of now, in local time |
| `--` | the end of the options: what follows is a FILE even if it starts with `-` |

Letters may be combined (`-cm`), and the value of `-r` or `-t` may follow
it directly (`-t01011200`). `-r` and `-t` cannot both be given.

## Exit status and messages

- `0`: every FILE was made or given its time.
- `1`: a FILE could not be made or changed: `touch: FILE: REASON`
  (`Permission denied`, `Operation not permitted` for a time other than now
  on a file that is not yours, `No such file or directory` for a folder
  that is missing); the other FILEs are still done. Or REFERENCE is not
  there: `touch: REFERENCE: No such file or directory`.
- `2`: no FILE, an unknown option, `-r` or `-t` without a value, or both:
  `usage: touch [-acm] [-r REFERENCE | -t [[CC]YY]MMDDhhmm[.SS]] FILE...`;
  a `-t` value that is not a time: `touch: VALUE: not a time as
  [[CC]YY]MMDDhhmm[.SS]`.

## Examples

```
ritteros:/$ touch /home/root/empty.txt
ritteros:/$ touch -t 202001021530.44 /home/root/old.txt
ritteros:/$ touch -r /home/root/old.txt /home/root/notes.txt
ritteros:/$ touch -c /home/root/missing.txt
ritteros:/$ touch src/main.c && make
```

## See also

[`make`](make.md), [`ls`](ls.md), [`date`](date.md), [`tar`](tar.md),
[`sh`](sh.md) (`> FILE` also makes an empty file)

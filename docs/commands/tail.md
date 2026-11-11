# tail: the last lines (or bytes) of a file; -f: follow it

```
tail [-f] [-n [+]N | -N] [-c [+]N] [FILE]...
```

Prints the last 10 lines of FILE, or of its standard input without FILE.
`-n N` (or the traditional `-N`, since step 63c) prints the last N lines
instead, `-c N` the last N bytes. With `+N` it prints everything from the
Nth line or byte on: `tail -n +2` leaves out the first line. `tail -f`
then keeps following the file: it looks every second and prints what was
added to it, until Ctrl-C; a file that was cut shorter is followed from
its new end. `tail` has been in RitterOS since step 51e and follows
POSIX; `-c` is a common extra.

`tail` reads the whole input into memory first, then prints the part
asked for. With several FILEs (since 50k, as BSD's and GNU's `tail`) each
comes under a heading `==> FILE <==`, a blank line between them; a FILE
that cannot be read gets its message and the others are still shown.

## Options

| Option | Meaning |
|---|---|
| `-n N` | the last N lines (10 by default); `-n +N`: from line N on |
| `-N` | the same as `-n N` (`tail -20`) |
| `-c N` | the last N bytes; `-c +N`: from byte N on |
| `-f` | after the end, follow FILE and print what is added (only with a FILE, not the input) |
| `--` | the end of the options: what follows is FILE even if it starts with `-` |

Each option is a word of its own (`-f -n 5`, not `-fn5`); the number may
follow at once (`-n5`, `-c+100`). The last of `-n`, `-N` and `-c` counts.

## Exit status and messages

- `0`: printed (with `-f`, `tail` ends only by Ctrl-C).
- `1`: FILE could not be opened (`FILE: No such file or directory`, `FILE:
  Permission denied`), or `tail: out of memory`.
- `2`: an unknown option or one without its number: `usage: tail [-f] [-n
  [+]N | -N] [-c [+]N] [FILE]`.

## Examples

```
ritteros:/$ tail /system/logs/login.log
ritteros:/$ tail -n 3 notes.txt
ritteros:/$ tail -20 /system/logs/kernel.log
ritteros:/$ tail -n +2 table.csv
ritteros:/$ tail -c 100 data.bin
ritteros:/$ tail -f /system/logs/kernel.log
```

## See also

[`head`](head.md), [`cat`](cat.md), [`wc`](wc.md), [`logd`](logd.md)

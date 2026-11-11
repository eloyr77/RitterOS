# sort: the lines in order

```
sort [-bcfhnruV] [-t C] [-k F[.C][,G[.C]][bfhnrV]]... [-o FILE] [FILE]...
```

Prints the lines of the files (or of its standard input; `-` as a FILE is
the input too) in order: by default byte by byte over the whole line. All
lines are read first, so the result may go into one of the input files
with `-o`. Lines that are equal by their keys come in the order of the
whole line (as POSIX says), and lines equal in that too keep the order
they came in. `sort` has been in RitterOS since step 51e and follows
POSIX; `-h` and `-V` are common extras.

**Keys.** `-k F,G` sorts by fields F to G, counted from 1; `-k F` alone
from field F to the line's end. `F.C` starts at character C of field F,
`G.C` ends after character C of field G (`-k 1.2,1.3` is characters 2 to
3 of field 1). Fields are separated by the character C of `-t C`; without
`-t` each field begins with the blanks (spaces and tabs) before it, which
then count in the comparison unless `-b` is given. Letters after a key
(`-k 2,2nr`, `-k 3n`) are options for that key alone; a key without any
takes the options given outside it. Several `-k` (at most 16) are tried
in turn: the next decides when one finds two lines equal.

## Options

| Option | Meaning |
|---|---|
| `-n` | by the number at the key's start (blanks before it skipped; no number counts as 0) |
| `-h` | as `-n`, with K M G T P E after the number counted in powers of 1024 (`2K` before `1M`, small letters too) |
| `-V` | as versions: numbers inside the text compared as numbers, the rest byte by byte (`v1.9` before `v1.10`) |
| `-f` | small letters as capitals (`apple` and `Apple` alike) |
| `-b` | blanks before a key do not count |
| `-r` | the other way round |
| `-u` | lines with equal keys only once: the first of them |
| `-c` | only check whether the input is in order; nothing is printed when it is |
| `-t C` | fields are separated by the character C (`-t:`, `-t ,`) |
| `-k KEY` | sort by KEY (above); may be given several times |
| `-o FILE` | the result into FILE instead of the standard output |
| `--` | the end of the options: what follows is a FILE even if it starts with `-` |

The letters may be written together (`-nr`, `-rn`), and an option's value
may follow it at once (`-t:`, `-k2,2n`, `-oout.txt`).

## Exit status and messages

- `0`: sorted (with `-c`: the input is in order).
- `1`: with `-c`, the input is not in order: `sort: disorder: LINE`, the
  first line out of place (with `-u` also a line equal to the one before).
- `2`: an unknown option, an option without its value or a 17th `-k`:
  `usage: sort [-nruhVfbc] [-t C] [-k F,G] [-o FILE] [FILE]...`; a FILE
  that cannot be read: `FILE: No such file or directory` (the others are
  still sorted); `-o FILE` that cannot be written: `FILE: Permission
  denied`; `sort: out of memory`.

## Examples

```
ritteros:/$ sort words.txt
ritteros:/$ sort -n -r sizes.txt
ritteros:/$ sort -t: -k3,3n /etc/users
ritteros:/$ ls -l | sort -k5,5n
ritteros:/$ sort -h disk-sizes.txt
ritteros:/$ sort -V versions.txt
ritteros:/$ sort -u -o words.txt words.txt
ritteros:/$ sort -c words.txt
sort: disorder: apple
```

## See also

[`uniq`](uniq.md), [`cut`](cut.md), [`awk`](awk.md), [`ls`](ls.md),
[`tr`](tr.md)

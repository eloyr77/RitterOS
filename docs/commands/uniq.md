# uniq: runs of equal lines once

```
uniq [-c] [-d | -u] [-i] [-f N] [-s N] [INPUT [OUTPUT]]
```

Prints its input with each run of equal lines, one after the other, once.
Only neighbours are compared, so equal lines far apart are all kept: to see
each different line once, sort first (`sort words.txt | uniq`). INPUT is a
file (the input when it is missing or `-`); with OUTPUT the result goes into
that file (replaced, or made if it is missing) instead of the output.

`-f` and `-s` leave the start of each line out of the comparison: first N
fields (each field is blanks, then everything up to the next blank), then N
characters. The line printed for a run is its first one. `uniq` has been in
RitterOS since step 51e and follows POSIX; `-i` is a common extra.

## Options

| Option | Meaning |
|---|---|
| `-c` | put the run's length before each line, in 4 columns and a space |
| `-d` | print only lines that repeat (a run of two or more) |
| `-u` | print only lines that do not repeat (`-d` and `-u` together print nothing) |
| `-i` | small and capital letters alike (A-Z only) |
| `-f N` | leave the first N fields out of the comparison; also written `-fN` |
| `-s N` | leave N more characters out of the comparison; also written `-sN` |
| `--` | the end of the options: what follows is INPUT even if it starts with `-` |

Options may be put together: `uniq -ci`.

## Exit status and messages

- `0`: the input was read to its end.
- `1`: INPUT could not be read or OUTPUT could not be made (`NAME: No such
  file or directory`, `NAME: Permission denied`), or a wrong option or more
  than two files: `usage: uniq [-c] [-d|-u] [-i] [-f N] [-s N] [INPUT
  [OUTPUT]]`.

## Examples

```
ritteros:/$ cat fruit.txt
apple
apple
pear
apple
ritteros:/$ uniq fruit.txt
apple
pear
apple
ritteros:/$ sort fruit.txt | uniq -c
   3 apple
   1 pear
ritteros:/$ sort fruit.txt | uniq -d
apple
ritteros:/$ uniq -f 1 log.txt short.txt
```

## See also

[`sort`](sort.md) (`-u` also keeps equal lines once), [`cut`](cut.md),
[`wc`](wc.md), [`grep`](grep.md)

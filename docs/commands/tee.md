# tee: copy the input to the output and into files

```
tee [-a] [-i] [FILE]...
```

Copies its standard input to its standard output and into each FILE at
the same time, so a command's output can be watched and kept: `ps | tee
ps.txt` shows the processes and writes them to `ps.txt`. Each FILE is
replaced (made if it is missing) unless `-a` is given. `tee` has been in
RitterOS since step 51e and follows POSIX.

## Options

| Option | Meaning |
|---|---|
| `-a` | add to the end of each FILE instead of replacing it |
| `-i` | ignore Ctrl-C (SIGINT): `tee` goes on until its input ends |
| `--` | the end of the options: what follows is a FILE even if it starts with `-` |

At most 64 files are written; more are left out.

## Exit status and messages

- `0`: everything was copied.
- `1`: a FILE could not be opened (`FILE: Permission denied`, `FILE: No such
  file or directory`; the others are still written) or could not be
  written to the end.
- `2`: an unknown option: `usage: tee [-a] [-i] [FILE]...`.

## Examples

```
ritteros:/$ ps | tee /home/root/ps.txt
ritteros:/$ echo another line | tee -a /home/root/ps.txt
ritteros:/$ net | tee net-now.txt | grep route
```

## See also

[`cat`](cat.md), [`sh`](sh.md) (`>` and `>>` write a command's output to a
file), [`grep`](grep.md)

# head: the first lines (or bytes) of files

```
head [-n N | -N] [-c N] [FILE]...
```

Prints the first 10 lines of each FILE, or of its input when no FILE is
given: `ps | head -3` shows the heading and the first two processes. With
`-n N` it prints the first N lines instead, with `-c N` the first N bytes.
A file shorter than that is printed whole. With several files each one comes
under a `==> NAME <==` line, with an empty line before every heading but
the first.

`head` has been in RitterOS since step 51e and follows POSIX; `-c` and the
traditional `-N` (`head -5`, the same as `-n 5`, since 63d) are common
extras. When several of the options are given, the last one counts.

## Options

| Option | Meaning |
|---|---|
| `-n N` | the first N lines (10 without any option); also written `-nN` |
| `-N` | the same as `-n N` (`head -20`) |
| `-c N` | the first N bytes instead of lines; also written `-cN` |
| `--` | the end of the options: what follows is a FILE even if it starts with `-` |

N must not be negative; `-n 0` prints nothing.

## Exit status and messages

- `0`: everything was printed.
- `1`: a FILE could not be opened (`FILE: No such file or directory`,
  `FILE: Permission denied`; the others are still printed).
- `2`: an unknown option, or `-n`/`-c` without a number or with a negative
  one: `usage: head [-n N | -N] [-c N] [FILE]...`.

## Examples

```
ritteros:/$ head -n 2 /readme.txt
ritteros:/$ ps | head -3
  pid  parent  group  state    name
    1       0      1  running  login
    2       1      2  running  elvesd
ritteros:/$ head -1 a.txt b.txt
==> a.txt <==
first line of a

==> b.txt <==
first line of b
ritteros:/$ head -c 16 /system/logs/kernel.log
```

## See also

[`tail`](tail.md), [`cat`](cat.md), [`wc`](wc.md), [`sed`](sed.md)

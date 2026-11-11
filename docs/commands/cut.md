# cut: parts of each line

```
cut -b LIST [-n] | -c LIST | -f LIST [-d C] [-s] [FILE]...
cut -b LIST [-n] [FILE]...
cut -c LIST [FILE]...
cut -f LIST [-d C] [-s] [FILE]...
```

Prints parts of each line of the FILEs, or of its input when no FILE is
given (`-` is the input too): bytes (`-b`), characters (`-c`, the same, as a
character is a byte in RitterOS's "C" locale), or fields (`-f`). Fields are
separated by tabs, or by the character given with `-d`; the chosen fields
are printed with the same separator between them. LIST is numbers and
ranges counted from 1, separated by commas: `1,3`, `2-4`, `-3` (up to 3),
`5-` (from 5 to the end), at most 64 of them. The parts are printed in the
line's own order, whatever order LIST names them in.

With `-f`, a line without the separator is printed whole, or left out with
`-s`. A last line without a line break is still cut, and gets one. `cut`
has been in RitterOS since step 51e and follows POSIX.

## Options

| Option | Meaning |
|---|---|
| `-b LIST` | the bytes LIST names; also written `-bLIST` |
| `-c LIST` | the characters LIST names (the same as `-b`) |
| `-f LIST` | the fields LIST names |
| `-d C` | fields are separated by C (its first character; a tab without `-d`); also `-dC` |
| `-s` | with `-f`: leave out lines that have no separator |
| `-n` | taken and ignored (POSIX's "do not split characters" for `-b`: a character is a byte) |
| `--` | the end of the options: what follows is a FILE even if it starts with `-` |

Exactly one of `-b`, `-c` and `-f` must be given.

## Exit status and messages

- `0`: every FILE was read.
- `1`: a FILE could not be opened (`FILE: No such file or directory`; the
  others are still read), or no `-b`, `-c` or `-f`, two of them, an empty or
  wrong LIST (a 0, a range going down like `5-2`, other characters), or an
  unknown option: `usage: cut -b LIST | -c LIST | -f LIST [-d C] [-s]
  [FILE]...`.

## Examples

```
ritteros:/$ cut -d ' ' -f 1,3 data.txt
ritteros:/$ echo a:b:c:d | cut -d : -f 2-
b:c:d
ritteros:/$ echo abcdefgh | cut -c -3,6
abcf
ritteros:/$ ps | cut -c 1-5,30-
  pid  name
    1  login
```

## See also

[`awk`](awk.md) (fields split at any blanks), [`tr`](tr.md),
[`sort`](sort.md), [`uniq`](uniq.md)

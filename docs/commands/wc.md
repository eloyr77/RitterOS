# wc: count lines, words and bytes

```
wc [-l] [-w] [-c | -m] [FILE]...
```

Counts the lines, words and bytes of each FILE, or of its input when no FILE
is given, and prints them in that order, each in 8 columns (as BSD's `wc`),
followed by the file's name. With several files a last line gives the
`total`. A line is counted at each line break, so a last line without one
is not counted; a word is a run of characters between blanks (spaces, tabs,
line breaks, and the vertical tab, form feed and carriage return).

Options choose some of the counts; they are always printed in the order
lines, words, bytes, whatever order the options came in. `wc` has been in
RitterOS since step 51e and follows POSIX.

## Options

| Option | Meaning |
|---|---|
| `-l` | the lines |
| `-w` | the words |
| `-c` | the bytes |
| `-m` | the characters: the same as `-c`, as a character is a byte in RitterOS's "C" locale |
| `--` | the end of the options: what follows is a FILE even if it starts with `-` |

Without any of them all three are printed. Options may be put together:
`wc -lw`.

## Exit status and messages

- `0`: every FILE was counted.
- `1`: a FILE could not be opened (`FILE: No such file or directory`,
  `FILE: Permission denied`; the others are still counted and the total
  leaves it out).
- `2`: an unknown option: `usage: wc [-l] [-w] [-c] [FILE]...`.

## Examples

```
ritteros:/$ wc /readme.txt
      12      96     598 /readme.txt
ritteros:/$ ls /system/x86_64/bin | wc -l
     121
ritteros:/$ wc -lc a.txt b.txt
       3      40 a.txt
       5      77 b.txt
       8     117 total
```

## See also

[`head`](head.md), [`tail`](tail.md), [`grep`](grep.md) (`-c` counts
matching lines), [`sort`](sort.md)

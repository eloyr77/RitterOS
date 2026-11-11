# tr: change, delete or squeeze characters

```
tr [-c] [-d] [-s] SET1 [SET2]
```

Copies its standard input to its standard output with characters
changed: each character of SET1 becomes the one at the same place in
SET2 (SET2's last one again where SET2 is shorter, as BSD's `tr`). `-d`
deletes the characters of SET1 instead, `-s` squeezes each run of one
character of the last set given into one, and `-c` takes every byte that
is not in SET1 (in byte order) in place of SET1. A character is a byte.
`tr` has been in RitterOS since step 51e and follows POSIX.

Which sets go with which options:

| Command | Does |
|---|---|
| `tr SET1 SET2` | changes SET1's characters into SET2's |
| `tr -s SET1` | squeezes runs of SET1's characters |
| `tr -s SET1 SET2` | changes, then squeezes runs of SET2's characters |
| `tr -d SET1` | deletes SET1's characters |
| `tr -ds SET1 SET2` | deletes SET1's characters, then squeezes runs of SET2's |

A set is made of:

| In a set | Means |
|---|---|
| a character | itself |
| `a-z` | the characters from `a` to `z` |
| `\n` `\t` `\r` `\a` `\b` `\f` `\v` `\\` | escapes; `\NNN` a byte in octal (one to three digits) |
| `[:alpha:]` | a class: also `upper` `lower` `digit` `alnum` `xdigit` `space` `blank` `punct` `print` `graph` `cntrl` (ASCII only) |
| `[x*3]` | `x` three times (a number starting with 0 is octal) |
| `[x*]` | in SET2: `x` as many times as SET1 needs |

When a character comes twice in SET1, its later place wins (as BSD's).

## Options

| Option | Meaning |
|---|---|
| `-c`, `-C` | every byte not in SET1 instead of SET1 |
| `-d` | delete the characters of SET1 |
| `-s` | squeeze runs of one character of the last set into one |
| `--` | the end of the options: what follows is SET1 even if it starts with `-` |

The options may be written together (`-cd`, `-cs`).

## Exit status and messages

- `0`: the input was copied to its end.
- `1`: a wrong command line (`usage: tr [-c] [-d] [-s] SET1 [SET2]`: an
  unknown option, or the wrong number of sets for the options), `tr: range
  z-a in reverse order`, `tr: set too long` (more than 8192 characters),
  `tr: empty SET2`, or the output could not be written.

## Examples

```
ritteros:/$ tr a-z A-Z < notes.txt
ritteros:/$ echo hello | tr el ip
hippo
ritteros:/$ tr -d '\r' < dos.txt > unix.txt
ritteros:/$ tr -s ' ' < table.txt
ritteros:/$ tr -cs '[:alpha:]' '\n' < notes.txt     (one word a line)
ritteros:/$ tr -cd '[:digit:]\n' < phone.txt
```

## See also

[`sed`](sed.md), [`cut`](cut.md), [`sort`](sort.md), [`awk`](awk.md)

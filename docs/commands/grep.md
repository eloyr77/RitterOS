# grep: print the lines that match a pattern

```
grep [-EFcilnqrsvx] [-e PATTERN]... [-f FILE]... [PATTERN] [FILE...]
```

Prints the lines of the files (or of its standard input, `grep x <
FILE`) that match PATTERN. With several files, or with `-r`, each line
starts with its file's name and a colon. PATTERN is a regular expression:
quote it, so the shell leaves `*` and `$` alone. `grep` came in step 50a;
since step 58a its patterns are POSIX's, from the C library's `regex.h`:
of the matches that start first, the longest.

A basic regular expression (the default):

| In PATTERN | Matches |
|---|---|
| a character | itself |
| `.` | any one character |
| `[abc]`, `[a-z]` | one of the set |
| `[^...]` | one character not in the set |
| `[[:alpha:]]`, `[[:digit:]]`, ... | one of a class (`alnum` `blank` `cntrl` `graph` `lower` `print` `punct` `space` `upper` `xdigit` too) |
| `X*` | the item X any number of times, none too |
| `X\{2\}`, `X\{2,5\}`, `X\{2,\}` | X that many times |
| `\(...\)`, `\1`-`\9` | a group, and what the group took, again |
| `^`, `$` | the line's start, its end (first or last in PATTERN) |
| `\X` | X itself (`\.` a dot, `\*` a star) |

With `-E` PATTERN is an extended regular expression, as `awk` has them:
`A|B` (either), `X+` (once or more), `X?` (once or not), `X{2,5}`, and
groups in `( )` without backslashes. With `-F` it is plain text, every
character itself.

(68f) `-e PATTERN` and `-f FILE` give the patterns instead of the first
operand, as often as wanted: a line is selected when any of them matches.
A pattern with line breaks in it is one pattern per line, and so is each
line of FILE; an empty pattern matches every line, an empty FILE none.

A file with a NUL byte is binary: from the first line that holds one on,
its lines are not printed, and if any line matched, `grep: FILE: binary
file matches` is printed (on the standard output) at the file's end.

## Options

| Option | Meaning |
|---|---|
| `-E` | PATTERN is an extended regular expression |
| `-F` | PATTERN is plain text (68f) |
| `-e PATTERN` | a pattern (also `-ePATTERN`; for one starting with `-`) (68f) |
| `-f FILE` | the patterns in FILE, one per line (68f) |
| `-x` | only lines the pattern matches whole, from start to end (68f) |
| `-q` | nothing printed: the exit code says, and the search ends at the first selected line (68f) |
| `-s` | nothing said of files missing or unreadable (the exit code is still 2) (68f) |
| `-i` | small and capital letters alike |
| `-v` | the lines that do not match |
| `-n` | each line with its number (`12:text`) |
| `-c` | only how many lines were selected, per file |
| `-l` | only the names of the files with a selected line |
| `-r` | folders too, with everything below them; without FILE the current folder (names then start with `./`) |
| `--` | the end of the options: what follows is PATTERN even if it starts with `-` |

Options combine (`-in`, `-rl`).

## Exit status and messages

- `0`: a line was selected (printed or counted).
- `1`: no line was selected.
- `2`: an error, also when lines were selected elsewhere (but with `-q` a
  selected line is `0` at once: the files after it are not looked at):
  - `grep: FILE: No such file or directory`, `grep: FILE: Permission
    denied` (the other files are still searched);
  - `grep: FOLDER: is a folder (-r searches it)` without `-r`;
  - `grep: PATTERN: WHY` for a pattern the regular expressions cannot
    read (WHY is the C library's `regerror` text: `grep: [a: brackets ([ ])
    not balanced`);
  - `grep: unknown option -X` followed by `usage: grep [-EFcilnqrsvx] [-e PATTERN]... [-f FILE]... [PATTERN] [FILE...]`,
    or the usage line alone when PATTERN is missing;
  - `grep: -e needs an argument` (or `-f`), then the usage line;
  - `grep: FILE: No such file or directory` for `-f FILE` (also with `-s`);
  - `grep: FILE: a line too long`, `grep: FILE: out of memory`.

## Examples

```
ritteros:/$ grep -n hello /test/text/lorem.txt
ritteros:/$ grep -ri 'error' /system/logs
ritteros:/$ grep -c '^$' notes.txt
ritteros:/$ grep -E 'eth[0-9]|wlan[0-9]' /system/logs/kernel.log
ritteros:/$ ps | grep -v elvesd
ritteros:/$ grep -l TODO *.c
ritteros:/$ if id | grep -q '^uid=0('; then echo root; fi
ritteros:/$ grep -xF -f names.txt users.txt
ritteros:/$ grep -e -v -e -n options.txt
```

## See also

[`sed`](sed.md), [`awk`](awk.md), [`find`](find.md), [`wc`](wc.md),
[`sort`](sort.md)

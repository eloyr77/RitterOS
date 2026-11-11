# awk: scan and process text: patterns, fields, arrays (POSIX's awk)

```
awk [-F FS] [-v NAME=VALUE]... ['PROGRAM' | -f PROGFILE...] [FILE | NAME=VALUE]...
awk [-F FS] [-v NAME=VALUE]... [--] 'PROGRAM' [FILE | NAME=VALUE]...
awk [-F FS] [-v NAME=VALUE]... -f PROGFILE... [--] [FILE | NAME=VALUE]...
```

POSIX's awk: a small language for text. PROGRAM is a list of `PATTERN {
ACTION }`: for each line (record) of the files (or of the standard input)
that PATTERN selects, ACTION runs; `BEGIN { }` runs before the first
line, `END { }` after the last. A missing pattern selects every line, a
missing action prints it. Each line is split into fields `$1`, `$2`, ...
(`$0` the whole line, `NF` how many) at runs of blanks, or at FS (`-F:`
for colons, `-F '\t'` or `-F t` for tabs; more than one character: an
extended regular expression). `awk` has been in RitterOS since step 58c.

Values are numbers or strings as they are used; variables need no
declaration; arrays take any subscript (`count[$1]++`, `a[i, j]` joined
by SUBSEP, `for (k in a)`, `delete a[k]`, `delete a`, `if (k in a)`).
Patterns: an expression, `/RE/`, `A, B` (a range: from a line A selects
to the next line B selects). The language has POSIX's operators (`^` `*`
`/` `%` `+` `-`, concatenation, `<` `<=` `==` `!=` `>=` `>`, `~` `!~`,
`in`, `&&` `||` `!`, `?:`, `=` `+=` `-=` `*=` `/=` `%=` `^=`, `++`
`--`), `if`/`else`, `while`, `do`, `for`, `for (k in a)`, `break`,
`continue`, `next`, `nextfile`, `exit [CODE]`, and functions (`function
name(a, b,   local) { ... return x }`; `func` too; arrays are passed by
reference). Regular expressions are POSIX's extended ones (the C
library's `regex.h`).

| Built-in | |
|---|---|
| `length(s)`, `substr(s, m, n)`, `index(s, t)` | length (of an array too; `length` alone: of `$0`), part, position |
| `split(s, a, fs)`, `sub(re, t, v)`, `gsub(re, t, v)`, `match(s, re)` | split into a; replace the first or every match (`&`: the match); where it matches (RSTART, RLENGTH) |
| `sprintf(fmt, ...)`, `tolower(s)`, `toupper(s)` | formatting as printf's, case |
| `sin` `cos` `atan2` `exp` `log` `sqrt` `int` `rand` `srand` | numbers |
| `system(cmd)`, `close(name)`, `fflush()` | run a command, close a file or pipe, write out |

`print a, b` writes its items with OFS between and ORS after; `printf
fmt, ...` formats as C's (`%d %i %o %x %X %u %c %s %e %f %g %%`, widths,
`*`). Either may go to a file (`> "f"`, `>> "f"`) or a command (`|
"sort"`). `getline` reads the next line, in its six forms (`getline`,
`getline var`, `getline < "file"`, `getline var < "file"`, `"cmd" |
getline`, `"cmd" | getline var`). The variables NR, NF, FNR, FS, OFS,
ORS, RS (`RS = ""`: paragraphs), FILENAME, SUBSEP, RSTART, RLENGTH,
CONVFMT, OFMT, ENVIRON, ARGC and ARGV are POSIX's.

Among the files, `-` is the standard input, and a word `NAME=VALUE` (NAME
a variable's name) sets NAME when it is reached, before the next file is
read. Where the Mac's awk and POSIX disagree, RitterOS's follows POSIX
and gawk.

## Options

| Option | Meaning |
|---|---|
| `-F FS` | the field separator (`-F:`; `-F t` is a tab; escapes like `\t` are taken) |
| `-v NAME=VALUE` | set NAME before `BEGIN` runs (escapes in VALUE taken); may be given several times |
| `-f PROGFILE` | the program from PROGFILE (`-`: the standard input) instead of the command line; several `-f` are joined in order |
| `--` | the end of the options |

An option's value may follow at once (`-F:`, `-vn=3`, `-fprog.awk`).

## Exit status and messages

- `0`, or the CODE of `exit CODE`.
- `2`: an error, after what was printed so far; the message names the
  program's line when it has one:
  - `awk: a syntax error (line 1)`, `awk: a string without its end (line
    1)`, `awk: an unexpected character '@' (line 1)`;
  - `awk: can't open file NAME` (an input file or a `-f` program file);
  - `awk: NAME: No such file or directory` for an output file or command
    that cannot be opened;
  - `awk: division by zero`, `awk: calling undefined function NAME`,
    `awk: function NAME called with 3 arguments, it takes 2`;
  - `awk: unknown option -X`, `awk: -F needs an argument`, `awk: -v needs
    NAME=VALUE`;
  - without a program: `usage: awk [-F FS] [-v NAME=VALUE] 'PROGRAM'
    [FILE...]`.

## Examples

```
ritteros:/$ awk '{ print $1 }' file.txt                 # the first word of each line
ritteros:/$ awk -F: '$3 >= 1000 { print $1 }' /etc/users
ritteros:/$ awk '{ s += $2 } END { print s, s / NR }' data.txt
ritteros:/$ awk 'length > 72' notes.txt                # the long lines
ritteros:/$ awk '!seen[$0]++' list.txt                 # each line once
ritteros:/$ awk -v limit=5 'NR <= limit' notes.txt
ritteros:/$ awk '{ n[$1]++ } END { for (w in n) print n[w], w | "sort -rn" }' words.txt
ritteros:/$ awk -f report.awk data.txt
```

## See also

[`sed`](sed.md), [`grep`](grep.md), [`cut`](cut.md), [`sort`](sort.md),
[`printf`](printf.md)

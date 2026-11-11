# sed: edit a stream: replace, delete and insert lines by a script

```
sed [-nE] [-i[SUFFIX]] [-e SCRIPT | -f SCRIPTFILE]... [SCRIPT] [FILE]...
sed [-nE] [-i[SUFFIX]] SCRIPT [FILE]...
sed [-nE] [-i[SUFFIX]] [-e SCRIPT]... [-f SCRIPTFILE]... [FILE]...
```

Reads the files (or its standard input; a FILE `-` is the standard input
too) a line at a time into the pattern space, runs SCRIPT on each line and
prints the result: POSIX's sed, since step 58b, with a few extras of GNU's
and the BSDs'. With `-i` it changes each FILE itself instead of printing.

A command may have an address in front, which picks the lines it runs on:

| Address | Lines |
|---|---|
| (none) | every line |
| `N` | line N (counted over all the files; with `-i`, in each file) |
| `$` | the last line |
| `/RE/` | the lines that match RE; `\cREc` with another delimiter c (`\%/usr%`) |
| `A,B` | from a line A up to a line B (B a number, `$` or `/RE/`) |
| `A,+N` | line A and the N lines after it (as GNU and BSD) |
| `ADDRESS!` | the other lines |

| Command | Does |
|---|---|
| `s/RE/TEXT/FLAGS` | replace: `&` in TEXT is the match, `\1`-`\9` the groups, `\n` a line break; FLAGS `g` every match, `N` the Nth (`2g`: from the second on), `p` print it if replaced, `w FILE` write it to FILE if replaced |
| `d` | delete the line, start the next |
| `D` | delete up to the first line break, then run the script again on the rest |
| `p`, `P` | print the line, print up to the first line break |
| `n`, `N` | print the line (not with `-n`) and read the next one instead; `N` appends it after a line break |
| `a\ TEXT`, `i\ TEXT`, `c\ TEXT` | add a line after, insert one before, change the line (`a TEXT` on one line too, as GNU's; a line ending in `\` goes on) |
| `y/ABC/XYZ/` | change each A to X, B to Y, C to Z |
| `h`, `H`, `g`, `G`, `x` | the hold space: copy the line there, append to it, copy back, append back, exchange |
| `b LABEL`, `t LABEL`, `: LABEL` | jump to the label (to the end without one); `t` only if an `s` replaced something since the last line was read or the last `t` |
| `{ ... }` | a group of commands under one address |
| `=` | print the line's number |
| `l` | show the line with its invisible characters (`\t`, `$` at the end) |
| `q` | print the line and stop |
| `r FILE` | append FILE's contents after the line |
| `w FILE` | write the line to FILE (`/dev/stdout` is the output) |
| `#` | a comment, to the end of the line |

REs are basic, as `grep` takes them, or extended with `-E`; `\n` in one
matches a line break, and an empty one (`s//X/`) is the last one used.
Commands are separated by line breaks or `;` (not after `a`, `i`, `c`,
`r` and `w`, whose text or file name is the rest of the line; a label
ends at `;`, as in GNU's). Several `-e` and `-f` scripts are joined, each
as its own lines. At most 16 files are written by `w`.

`-i` writes the result back into each FILE once it has run over it (its
own line numbers and `$`): the file keeps its permissions and its other
names (hard links). `-iSUFFIX` (written together, as `-i.bak`) first keeps
the old content as FILE + SUFFIX.

## Options

| Option | Meaning |
|---|---|
| `-n` | print only what `p`, `P`, `l`, `=`, `s///p` and `w` ask for |
| `-E`, `-r` | extended REs (`+`, `?`, `|`, `( )`, `{ }` without backslashes) |
| `-e SCRIPT` | a script (also `-eSCRIPT`); with `-e` or `-f` no SCRIPT word is taken |
| `-f SCRIPTFILE` | a script from a file (also `-fSCRIPTFILE`) |
| `-i[SUFFIX]` | edit each FILE in place; SUFFIX: keep the old content as FILE + SUFFIX |
| `--` | the end of the options |

Letters may be combined (`-nE`); `-i` takes the rest of its word as the
SUFFIX, so it goes last or alone.

## Exit status and messages

- `0`: everything was read and done.
- `1`: a bad script or option, nothing run: `sed: an unknown command: k`,
  `sed: an unterminated s command`, `sed: no such label: top`, `sed: an
  unmatched '{'`, `sed: extra characters after a command: ...`, `sed: y:
  the two sets differ in length`, an RE's error from `regcomp`, `sed: an
  option needs an argument: -e`, `sed: -i needs a file`, `sed: FILE:
  REASON` (a `w` or `-f` file), or `usage: sed [-nE] [-i[SUFFIX]] SCRIPT
  [FILE...]` for an unknown option or no script. Also, while it runs, `sed:
  no previous regular expression` (an empty RE before any other was used).
- `2`: a FILE could not be read (`sed: FILE: No such file or directory`;
  it goes on with the others), or with `-i` could not be written back
  (`sed: FILE: a folder`, `sed: FILE: the new content could not be read
  back`, `sed: FILE.bak: REASON`).

## Examples

```
ritteros:/$ sed 's/colour/color/g' notes.txt
ritteros:/$ sed -n '/^error/p' /system/logs/kernel.log
ritteros:/$ sed -i '3d' list.txt
ritteros:/$ sed -i.bak -E 's/([0-9]+) apples/\1 pears/' list.txt
ritteros:/$ sed -n '2,+3p' list.txt
ritteros:/$ ps | sed 1d
ritteros:/$ sed -e '/^#/d' -e 's/ *$//' Makefile
```

## See also

[`grep`](grep.md), [`awk`](awk.md), [`tr`](tr.md), [`cut`](cut.md),
[`edit`](edit.md)

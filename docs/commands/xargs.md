# xargs: run a command with the input's words as arguments

```
xargs [-t] [-n N | -L N | -I REPL] [COMMAND [ARG]...]
```

Reads words from its standard input and runs COMMAND (`echo` if none is
given) with the ARGs and then those words as its arguments: `find /home
-name "*.txt" | xargs wc -l` counts the lines of every file found. As
many words go into one command as fit (120 words and 3,800 bytes with the
ARGs, its own limits: the kernel takes 4096 words and 128 KiB); more input runs the
command again. Words are separated by blanks and line breaks; `'...'`,
`"..."` and `\` quote. Empty input runs nothing. Each command is waited
for before the next one starts. A COMMAND named without a `/` is looked up
in `/system/<arch>/bin`, then in `/system/<arch>/apps` (42b: inside an app bundle, `NAME.program/NAME`); a file without
`#!` that is not a program runs as a shell script (since step 52g).
`xargs` has been in RitterOS since step 51e and follows POSIX.

## Options

| Option | Meaning |
|---|---|
| `-t` | print each command on the error output before it runs |
| `-n N` | at most N words of the input per command |
| `-L N` | the words of N input lines per command |
| `-I REPL` | run COMMAND once per input line, with every REPL in COMMAND and its ARGs replaced by the line (blanks at its ends removed; quotes are not taken out); the line is not added at the end |
| `--` | the end of the options: what follows is COMMAND even if it starts with `-` |

The values may also be written joined: `-n2`, `-I{}`. N must be 1 or
more. `-I` and `-n`/`-L` exclude each other: `-I` after them switches them
off, and with `-I` given they are ignored. `-t` must be written alone.

## Exit status and messages

- `0`: every command ran and ended with 0.
- `1`: the input or the options were not right: `xargs: unmatched quote`
  (a quote not closed before the line's end), `xargs: argument too long`
  (one word longer than a command can take), or `usage: xargs [-t] [-n N |
  -L N | -I REPL] [COMMAND [ARG]...]`.
- `123`: a command ended with an exit code of 1-125 (xargs goes on).
- `124`: a command ended with 255: `xargs: COMMAND exited with 255`, and
  xargs stops.
- `125`: a command was stopped by a signal; xargs stops.
- `126`: COMMAND could not be started: `xargs: COMMAND: REASON`.
- `127`: there is no such COMMAND: `xargs: COMMAND: command not found`.

## Examples

```
ritteros:/$ find /home -name "*.txt" | xargs wc -l
ritteros:/$ echo a b c d e | xargs -n 2
a b
c d
e
ritteros:/$ echo a b c | xargs -t echo got
echo got a b c
got a b c
ritteros:/$ ls /home/root | xargs -I NAME echo file: NAME
```

## See also

[`find`](find.md), [`sh`](sh.md), [`env`](env.md), [`echo`](echo.md)

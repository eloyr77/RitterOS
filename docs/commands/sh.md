# sh: the shell

```
sh [-l] | sh -c TEXT [NAME [ARG]...] | sh FILE [ARG]...
sh [-l]
sh -c TEXT [NAME [ARG]...]
sh FILE [ARG]...
```

The shell reads commands and runs them: programs, built-in commands, and
whole scripts with variables, pipes, loops and functions. `login` starts it
for you after you log in (as `sh -l` at the console), and `sshd` starts it
for every SSH session and runs `ssh HOST 'COMMAND'` through `sh -c`.
This page is about starting `sh` itself; the language is described in the
user's manual, [section 2, "The shell"](../manual.md#2-the-shell).

**At a terminal** (`sh`, `sh -l`) it shows the prompt `ritteros:DIR$ ` with
the current directory, reads a line with its own line editor (Left and
Right, Home and End, Up and Down through the last 50 commands, Ctrl-U,
Ctrl-L) and runs it. A command not finished at the line's end (an `if`
without its `fi`, an open quote, a `|` at the end) goes on after the prompt
`> `. A line whose last command failed is reported, as `grep: exit code
1`. Ctrl-C cancels the line being typed (the shell itself ignores it; a
program running in the foreground is stopped by it), Ctrl-D on an empty
line (the shell prints `exit`) or the built-in `exit` ends it.

**Without a terminal** (since 63c), as in `ssh HOST sh < script.sh` or
`cat script | sh`, it reads the commands line by line and runs them without
prompts, without `> ` and without the `exit` at the end. Such lines may
have up to 4,095 bytes; a longer one is not run (the next one is).

**`sh -l`**, a login shell, first runs `~/.config/login` as a script (since
15l): commands you want at every console login, such as `windowd`. An
`exit` in it ends the shell there.

**`sh -c TEXT`** runs TEXT, however long, as one script, and ends with its
exit code. With NAME, `$0` is NAME and the ARGs are `$1`, `$2`, ...;
without it `$0` is `sh`. It reports no failed commands and no background
jobs.

**`sh FILE`** runs the script FILE with `$0` = FILE and the ARGs as `$1`,
`$2`, .... A script whose first line is `#!/system/x86_64/bin/sh` can also
be started by its name once it may be run (`chmod 755 FILE`); since 52g one
without that line is run by a new `sh` too, as POSIX says.

## The language in short

- Words are split at blanks; `'single'` and `"double"` quotes keep spaces,
  `\` quotes one character. `#` begins a comment.
- `;` and line breaks separate commands, `|` makes a pipeline (one job,
  since 51a), `&&` and `||` run the next command only after a success or a
  failure, `!` inverts, `&` at the end runs in the background.
- Redirections: `< FILE`, `> FILE`, `>> FILE`, `2> FILE`, `2>&1`, `N>&M`,
  and here-documents `<<WORD` and `<<-WORD` (since 63i).
- Expansions (since 51b): `$NAME`, `${NAME}`, `$(COMMAND)`, `` `COMMAND`
  ``, `$((EXPRESSION))`, `$?`, `$$`, `$!`, `$0`...`$9`, `$#`, `$@`, `$*`,
  `~`, and names with `*`, `?`, `[...]` (their quoted parts plain text,
  the rest a pattern: `"$dir"/*.txt`, since 68h) (at most 4096 words a command,
  128 KiB of them).
- Control flow (since 51c): `if`, `while`, `until`, `for`, `case`,
  `{ ...; }`, `( ... )` (a subshell: a fork since 56c), functions
  `NAME() { ...; }`. A `case` pattern's quoted parts are plain text and
  the rest stays a pattern (`"$dir"*`, `'a?'*`; since 68g, before it a
  pattern with any quote was compared whole as text).
- Built-in commands: `cd`, `pwd`, `exit`, `help`, `export`, `unset`, `:`,
  `.`, `eval`, `exec`, `set` (`-e`, `-u`, `-x`, `-f`, `--`), `shift`,
  `read`, `break`, `continue`, `return`, `umask`. Everything else (`echo`,
  `test`, `printf`, `true`, ...) is a program, looked up in
  `/system/<arch>/bin`, then in `/system/<arch>/apps` (42b: inside an app bundle, `NAME.program/NAME`); a name with a `/` is
  a path.

## Options

| Option | Meaning |
|---|---|
| `-l` | a login shell: run `~/.config/login` before the first prompt (only as the one argument) |
| `-c TEXT` | run TEXT and end; the words after it become `$0`, `$1`, ... |

Any other first argument starting with `-` is ignored, and the shell reads
its commands as without one. `set` changes the options of a running shell
(`set -e`, `set -x`).

## Exit status and messages

- `sh -c TEXT` and `sh FILE` end with the exit code of the last command
  run, or the one `exit N` gives. A command ends with 0 for success,
  something else for failure: 127 when it is not found (`sh: NAME: command
  not found`), 126 when it cannot be started (`sh: NAME: Permission
  denied`, `sh: NAME: Exec format error`), 128 + N when signal N stopped
  it, 2 after a syntax error (`sh: syntax error near TOKEN`).
- `sh FILE`: `127` and `sh: FILE: No such file or directory` when FILE
  cannot be read.
- At a terminal: `0` at Ctrl-D or the input's end, or the code `exit N`
  gives.
- Other messages: `sh: the input ended inside a command` (an `if` never
  closed), `sh: the input ended inside a here-document (no line 'WORD')`,
  `sh: a line longer than 4095 bytes: not run`, `sh: PATTERN: too many
  names (at most 4096 words)`.

## Examples

```
ritteros:/$ sh -c 'cd /test; ls | wc -l'
ritteros:/$ sh -c 'echo $0 got $# arguments: $@' me a b
me got 2 arguments: a b
ritteros:/$ cat > /home/root/count.sh <<'END'
> for f; do echo "$f: $(wc -l < "$f") lines"; done
> END
ritteros:/$ sh /home/root/count.sh /readme.txt
/readme.txt:       12 lines
ritteros:/$ sh /nope.sh
sh: /nope.sh: No such file or directory
sh: exit code 127
```

From another computer:

```
$ ssh root@ritteros 'mem; uptime'
$ ssh root@ritteros sh < script.sh
```

## See also

The manual's [section 2](../manual.md#2-the-shell) (the whole language),
[`login`](login.md), [`sshd`](sshd.md), [`test`](test.md),
[`echo`](echo.md), [`printf`](printf.md), [`env`](env.md),
[`xargs`](xargs.md)

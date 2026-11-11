# env: show the environment variables, or run a command with some set

```
env [-i] [NAME=VALUE]... [COMMAND [ARG]...]
```

Without a COMMAND, prints the environment variables, one `NAME=value` per
line, after setting the NAME=VALUE pairs given. With a COMMAND, runs it
with those variables set (added to the environment, or replacing a
variable of the same name), waits for it and ends with its exit code.
With `-i` the command gets only the variables given, none of the others.
A COMMAND named without a `/` is looked up as the shell does: in
`/system/<arch>/bin`, then in `/system/<arch>/apps` (42b: inside an app bundle, `NAME.program/NAME`). A file that is not a
program (no ELF header) and has no `#!` line runs as a shell script, as
POSIX says (since step 52g). `env` has been in RitterOS since step 22t.

## Options

| Option | Meaning |
|---|---|
| `-i` | start from an empty environment: only the NAME=VALUE pairs given |

`-i` must be the first argument. Every following word with a `=` in it
(not at its start) is a NAME=VALUE; the first word without one is the
COMMAND.

## Exit status and messages

- `0`: the variables were printed.
- the COMMAND's own exit code, or 128 plus the signal's number if a
  signal ended it.
- `125`: a variable could not be set: `env: NAME=VALUE: REASON`.
- `127`: COMMAND could not be started: `env: COMMAND: command not found`
  (no such program), or `env: COMMAND: REASON` (such as `Permission
  denied`).

The messages go to the standard error.

## Examples

```
ritteros:/$ env
HOME=/home/root
USER=root
LOGNAME=root
SHELL=/system/x86_64/bin/sh
ritteros:/$ env -i GREETING=hello env
GREETING=hello
ritteros:/$ env TZ=UTC date
ritteros:/$ env nosuchprogram
env: nosuchprogram: command not found
```

## See also

[`sh`](sh.md) (`NAME=value COMMAND`, `export`), [`xargs`](xargs.md),
[`login`](login.md) (sets `HOME`, `USER`, `LOGNAME`, `SHELL`)

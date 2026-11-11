# false: do nothing, unsuccessfully (exit code 1)

```
false
```

Does nothing and fails: its exit code is always 1. It is for scripts and
the shell's `if`, `while`, `&&` and `||`, where a command that is sure to
fail is wanted (`while false; do ...; done` never runs its body). Any
arguments are ignored. Its partner [`true`](true.md) succeeds (0).

## Exit status and messages

- `1`: always. `false` prints nothing; typed at the prompt the shell
  reports `false: exit code 1` (not under `sh -c` or in a script).

## Examples

```
ritteros:/$ false
false: exit code 1
ritteros:/$ false || echo it failed
it failed
ritteros:/$ false; echo $?
1
```

## See also

[`true`](true.md), [`test`](test.md), [`sh`](sh.md)

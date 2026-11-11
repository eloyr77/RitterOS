# true: do nothing, successfully (exit code 0)

```
true
```

Does nothing and ends with exit code 0. It is useful where the shell wants
a command that succeeds: an endless loop (`while true; do ...; done`), or
after `||` to ignore a failure. Arguments are ignored. `true` has been in
RitterOS since step 51d, as on Unix; it is a program, not a shell
built-in (the user's choice).

## Exit status and messages

- `0`: always. It prints nothing.

## Examples

```
ritteros:/$ true
ritteros:/$ echo $?
0
ritteros:/$ rm /home/root/old.txt || true
ritteros:/$ while true; do date; sleep 60; done
```

## See also

[`false`](false.md), [`test`](test.md), [`sh`](sh.md)

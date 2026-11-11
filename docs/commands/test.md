# test: check files, strings and numbers (exit code 0: true); also [ ... ]

```
test EXPRESSION
[ EXPRESSION ]
```

Checks EXPRESSION and says the answer only by its exit code: 0 when it is
true, 1 when it is false, 2 when it cannot be read. It prints nothing
except for an error. It is meant for the shell's `if`, `while`, `&&` and
`||`: `if [ -d /test ]; then echo yes; fi`. The same program is installed
as `[`; called so, its last word must be `]`, which is not part of the
expression.

Each operator and each value is a word of its own, so spaces matter:
`[ "$n" -gt 3 ]`, not `["$n" -gt 3]`. Quote variables, or an empty one
leaves a word out. Without any expression `test` is false. With one to
three words POSIX's rules by the number of words come first (so `test -n`
is true: the string `-n` is not empty, and `[ "$a" = "$b" ]` works whatever
`$a` holds); longer expressions are read with `!` before `-a` before `-o`,
and parentheses group.

`test` is a program, not a shell built-in. It has been in RitterOS since
step 51d and follows POSIX with XSI's `-a`, `-o` and parentheses; `-h`/`-L`
came with links (57a), `-k` with the sticky bit (57b); `<`, `>`, `==`,
`-nt` and `-ot` are common extras.

## Options

`test` has no options of its own: these are the parts an EXPRESSION is made
of. FILE operators follow symbolic links (but `-h` and `-L`), and are false
for a FILE that is not there.

| Expression | True when |
|---|---|
| `STRING`, `-n STRING` | the string is not empty |
| `-z STRING` | the string is empty |
| `A = B`, `A == B` | the strings are equal |
| `A != B` | the strings are not equal |
| `A < B`, `A > B` | A comes before, after B in byte order (quote `'<'` and `'>'` in the shell, or they redirect) |
| `A -eq B`, `-ne`, `-lt`, `-le`, `-gt`, `-ge` | the whole numbers A and B are equal, not equal, less, less or equal, greater, greater or equal (blanks around a number are allowed) |
| `-e FILE` | FILE exists |
| `-f FILE` | it is a regular file |
| `-d FILE` | it is a directory |
| `-s FILE` | its size is more than 0 |
| `-r FILE`, `-w FILE`, `-x FILE` | you may read, write, run it (by its owner, group and other bits; root may read and write anything, and run what has any x bit, or a directory) |
| `-b FILE` | it is a disk or partition (`/dev/nvme0n1`) |
| `-c FILE` | it is a character device (`/dev/null`, `/dev/tty`, a terminal) |
| `-p FILE` | it is a pipe |
| `-S FILE` | it is a socket |
| `-h FILE`, `-L FILE` | it is a symbolic link (the link itself, not followed) |
| `-u FILE` | its setuid bit is set |
| `-k FILE` | its sticky bit is set (`/tmp`) |
| `-g FILE` | always false (RitterOS has no setgid bit) |
| `A -nt B` | file A is newer than B (changed later), or A exists and B does not |
| `A -ot B` | file A is older than B, or B exists and A does not |
| `-t FD` | descriptor FD (a number) is a terminal |
| `! E` | E is false |
| `E1 -a E2` | both are true |
| `E1 -o E2` | at least one is true |
| `( E )` | E (for grouping; quote `'('` and `')'` in the shell) |

Times are compared in whole seconds.

## Exit status and messages

- `0`: EXPRESSION is true.
- `1`: it is false, or there is none.
- `2`: it cannot be read. The message names the problem (`test:` also when
  called as `[`), only the first one:
  - `test: abc: not an integer`: a number operator got something else.
  - `test: an argument is missing`: an operator at the end (`test 1 -a`).
  - `test: a ')' is missing`
  - `test: WORD: too many arguments`: words left over after the
    expression; WORD is the first of them.
  - `[: a ']' is missing`: called as `[` without `]` at the end.

## Examples

```
ritteros:/$ test -f /readme.txt && echo a file
a file
ritteros:/$ if [ -d /test ]; then echo yes; else echo no; fi
yes
ritteros:/$ n=5
ritteros:/$ [ "$n" -gt 3 ] && echo big
big
ritteros:/$ [ "$n" -gt x ]
test: x: not an integer
[: exit code 2
ritteros:/$ [ -d /tmp -a ! -h /tmp ] && [ -k /tmp ] && echo a sticky folder
a sticky folder
ritteros:/$ [ -h /bin ] && echo /bin is a link
/bin is a link
ritteros:/$ while [ ! -s /home/root/out.txt ]; do sleep 1; done
```

## See also

[`sh`](sh.md) (`if`, `while`, `&&`, `||`), [`true`](true.md),
[`false`](false.md), [`ls`](ls.md), [`readlink`](readlink.md)

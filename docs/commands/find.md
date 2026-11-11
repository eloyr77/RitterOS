# find: search for files by name and attributes

```
find [DIR] [-name PATTERN] [-attr KEY[=VALUE]]...
```

Searches DIR (by default the current directory) and everything below it,
and prints the path of each file and folder that meets all the conditions
given, one per line, in the order the folders hold them (DIR itself is not
printed). Without conditions it lists everything. The paths start with DIR
as it was typed: `find /test` prints `/test/docs/a.txt`, `find` alone
prints `./docs/a.txt`. Symbolic links are listed but not followed into.
`find` came with attributes in step 18f.

## Options

| Condition | Matches |
|---|---|
| `-name PATTERN` | names (the last part of the path) matching PATTERN: `*` any characters, `?` one character, the rest itself; quote it, or the shell expands it first: `-name '*.txt'` |
| `-attr KEY` | files and folders with the attribute KEY (see [`attr`](attr.md)) |
| `-attr KEY=VALUE` | those whose attribute KEY is exactly VALUE |

Conditions may be repeated (all must hold), up to 8 of each kind. DIR, if
given, must come first.

## Exit status and messages

- `0`: the whole tree was searched (whether anything matched or not).
- `1`: a folder could not be read (`find: /home/anna: Permission denied`;
  the rest is still searched) or a path grew longer than 511 bytes (`find:
  PATH/NAME: path too long`).
- `2`: a wrong command line (an unknown condition, a condition without its
  value, DIR not first, more than 8 of a kind): `usage: find [DIR] [-name
  PATTERN] [-attr KEY[=VALUE]]...`.

## Examples

```
ritteros:/$ find /test -name '*.txt'
/test/hello.txt
/test/docs/readme.txt
ritteros:/$ find / -attr project=ritteros
/home/root/notes.txt
ritteros:/$ cd /home/root
ritteros:/home/root$ find -name 'note?.txt'
./notes.txt
ritteros:/home/root$ find /system -name kernel.elf
/system/x86_64/kernel.elf
```

## See also

[`attr`](attr.md), [`ls`](ls.md), [`grep`](grep.md) (searches inside
files), [`xargs`](xargs.md)

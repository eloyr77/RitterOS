# readlink: print what a symbolic link names (-f: the full path)

```
readlink [-f] PATH...
```

Prints what the symbolic link PATH names, exactly as it was written when
the link was made (a relative target stays relative). With `-f` it prints
instead the full path that PATH leads to, from `/`, with every link on the
way followed (at most 40) and every `.` and `..` worked out; PATH itself
need not be a link, and its last part need not exist (the folder it is in
must). One line per PATH. `readlink` has been in RitterOS since step 57a.

## Options

| Option | Meaning |
|---|---|
| `-f` | the full path PATH leads to, every link followed |

Options come before the names; `--` ends them.

## Exit status and messages

- `0`: every PATH was printed.
- `1`: one could not be (the others still are): `readlink: PATH: Invalid
  argument` (PATH is no link; without `-f`), `readlink: PATH: No such file
  or directory`, `readlink: PATH: Too many levels of symbolic links` (with
  `-f`, more than 40), `readlink: PATH: File name too long`.
- `2`: no PATH: `usage: readlink [-f] PATH...`.

## Examples

```
ritteros:/$ readlink /bin
/system/x86_64/bin
ritteros:/$ ln -s ../docs /home/root/projects/docs
ritteros:/$ readlink /home/root/projects/docs
../docs
ritteros:/$ readlink -f /home/root/projects/docs
/home/root/docs
ritteros:/$ readlink -f /etc/users
/system/config/users
ritteros:/$ readlink /readme.txt
readlink: /readme.txt: Invalid argument
```

## See also

[`ln`](ln.md), [`ls`](ls.md) (`ls -l` shows `name -> target`),
[`basename`](basename.md), [`dirname`](dirname.md)

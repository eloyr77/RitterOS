# id: the user and group you are

```
id
```

Prints the user and the group the program runs as, by number, with the
user's name when `/system/config/users` knows it (user 0 is always
`root`): `uid=1000(anna) gid=1000`. Run inside a setuid program (a
root-owned one with mode `4755`, such as a copy of `id` itself) it also
shows the user whose rights it has when that differs from who started it:
`uid=1000(anna) euid=0(root) gid=1000` (the real user and the effective
one). `id` came with step 20a, the names with 20b, `euid=` with 50b.

`id` takes no options; arguments are ignored.

## Exit status and messages

- `0`: always.

## Examples

```
ritteros:/$ id
uid=0(root) gid=0
ritteros:/$ su anna
ritteros:/$ id
uid=1000(anna) gid=1000
ritteros:/$ sudo id
Password for anna:
uid=0(root) gid=0
```

## See also

[`su`](su.md), [`sudo`](sudo.md), [`useradd`](useradd.md),
[`chown`](chown.md), [`login`](login.md)

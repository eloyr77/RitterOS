# attr: list, show, set or remove a file's attributes

```
attr PATH [KEY | KEY=VALUE | -KEY]
```

Attributes are `key=value` pairs that RitterFS keeps on files and
directories, beside their content: a project name, a tag, a note. Keys are
up to 63 bytes, values up to 1 KiB. With PATH alone `attr` lists every
attribute, one `key=value` per line; with KEY it prints that one; with
`KEY=VALUE` it sets it (a key that is there gets the new value); with
`-KEY` it removes it. A change is written to the disk at once. A value
that is not plain text (it holds control characters) is shown by its size
instead: `KEY: 16 bytes`. `find -attr` searches by attributes. `attr` has
been in RitterOS since step 18f.

Reading attributes needs the right to read PATH, changing them the right to
write it. In a snapshot (`/.snapshots/NAME/...`) the attributes are as they
were then, and read only. Only RitterFS has attributes: on FAT and exFAT
volumes `attr` fails with `Operation not supported`.

## Options

| Option | Meaning |
|---|---|
| `KEY` | print the attribute KEY as `KEY=VALUE` |
| `KEY=VALUE` | set KEY to VALUE (the text after the first `=`; it may be empty) |
| `-KEY` | remove the attribute KEY |

## Exit status and messages

- `0`: done.
- `1`: PATH or the attribute could not be read or changed:
  `attr: PATH: REASON` (`No such file or directory`, `Permission denied`,
  `Operation not supported` on FAT and exFAT), or with the key named:
  `attr: PATH: KEY: No such attribute`, `attr: PATH: KEY: Read-only file
  system` (in a snapshot), `attr: PATH: KEY: Invalid argument` (a key
  longer than 63 bytes or a value longer than 1 KiB).
- `2`: no PATH, or more than two arguments: `usage: attr PATH [KEY |
  KEY=VALUE | -KEY]`.

## Examples

```
ritteros:/$ attr /home/root/notes.txt project=ritteros
ritteros:/$ attr /home/root/notes.txt status=draft
ritteros:/$ attr /home/root/notes.txt
project=ritteros
status=draft
ritteros:/$ attr /home/root/notes.txt status
status=draft
ritteros:/$ attr /home/root/notes.txt -status
ritteros:/$ attr /home/root/notes.txt status
attr: /home/root/notes.txt: status: No such attribute
ritteros:/$ find /home -attr project=ritteros
```

## See also

[`find`](find.md) (`-attr`), [`snapshot`](snapshot.md),
[`versions`](versions.md), [`ls`](ls.md)

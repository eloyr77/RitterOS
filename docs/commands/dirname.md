# dirname: a path without its last part

```
dirname STRING
```

Prints STRING without its last part: the directory a path is in.
`dirname /a/b/c.txt` is `/a/b`. Slashes at the end are left out first
(`dirname /a/b/` is `/a`), and so are the slashes before the last part. A
name without a directory gives `.`, one in the root gives `/`. `dirname`
only looks at the text: the path need not exist. It has been in RitterOS
since step 51d and follows POSIX.

## Exit status and messages

- `0`: the directory part was printed.
- `2`: not exactly one STRING: `usage: dirname STRING`.

## Examples

```
ritteros:/$ dirname /home/root/notes.txt
/home/root
ritteros:/$ dirname notes.txt
.
ritteros:/$ dirname /readme.txt
/
ritteros:/$ cd $(dirname /system/x86_64/bin/sh)
```

## See also

[`basename`](basename.md), [`readlink`](readlink.md), [`sh`](sh.md)

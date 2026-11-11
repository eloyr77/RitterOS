# basename: a path's last part (without SUFFIX)

```
basename STRING [SUFFIX]
```

Prints the last part of the path STRING, without the directories before
it: `basename /a/b/c.txt` is `c.txt`. Slashes at the end do not count
(`basename /a/b/` is `b`), and `/` stays `/`. With SUFFIX, that ending is
also taken off, unless it is all that is left: `basename /a/b/c.txt .txt`
is `c`, but `basename .txt .txt` is `.txt`. STRING is only text: the path
does not have to exist. `basename` has been in RitterOS since step 51d and
follows POSIX.

## Exit status and messages

- `0`: the name was printed.
- `2`: no STRING, or more than two arguments: `usage: basename STRING
  [SUFFIX]`.

## Examples

```
ritteros:/$ basename /home/root/notes.txt
notes.txt
ritteros:/$ basename /home/root/notes.txt .txt
notes
ritteros:/$ basename /system/x86_64/bin/
bin
ritteros:/$ for f in /home/root/*.txt; do echo $(basename $f .txt); done
```

## See also

[`dirname`](dirname.md), [`sh`](sh.md) (`$(...)` takes its output into a
command), [`ls`](ls.md)

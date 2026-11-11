# ar: make and read archives of object files (static libraries)

```
ar rcs ARCHIVE FILE... | ar t ARCHIVE | ar x ARCHIVE [MEMBER...]
ar rcs ARCHIVE FILE...
ar t ARCHIVE
ar x ARCHIVE [MEMBER...]
```

Static libraries such as `libritteros.a` are archives: object files kept in
one file, with an index of the symbols they define, which [`ld`](ld.md)
searches for anything still undefined. `ar rcs` adds each FILE to ARCHIVE
(making it if it is not there), replacing a member of the same name, and
puts new ones at the end; a member is named by its file's last path part
(`ar rcs lib.a build/x.o` adds `x.o`). The index is always made again. `ar
t` lists the members, one name a line, in their order. `ar x` writes the
members into the current folder under their names (all of them, or only the
MEMBERs named), replacing files of those names.

The format is GNU's, written as llvm-ar writes it by default: the symbol
index (`/`), a table for names of 16 characters or more (`//`), then the
members, each padded to an even size; no dates, owners or modes are kept
(all 0, mode 644), so the same objects always make the same archive. `ar`
came with step 63, so that RitterOS can build its own libraries.

## Options

The first word gives the operation, as letters (a `-` before them is
allowed: `ar -t lib.a`). Exactly one of `r`, `t` and `x` must be among
them.

| Letter | Meaning |
|---|---|
| `r` | add or replace the FILEs |
| `t` | list the members |
| `x` | extract members into the current folder |
| `c` | taken: the archive is always made quietly when it is missing |
| `s` | taken: the symbol index is always written |
| `D` | taken: the archive is always deterministic |
| `u` | taken and ignored: every FILE named is put in, newer or not |

## Exit status and messages

- `0`: done. An ARCHIVE that is not there is empty to `t` and `x` (nothing
  is listed or written).
- `1`: `ar: cannot read FILE`, `ar: cannot write PATH`, `ar: ARCHIVE: not an
  archive`, `ar: ARCHIVE: a member past the archive's end` (a damaged
  archive), `ar: out of memory reading FILE`.
- `2`: fewer than two words, an unknown letter, or not exactly one of `r`,
  `t` and `x`:

  ```
  usage: ar rcs ARCHIVE FILE...
         ar t ARCHIVE
         ar x ARCHIVE [MEMBER...]
  ```

## Examples

```
ritteros:/home/root/parts$ cc -c part1.c part2.c
ritteros:/home/root/parts$ ar rcs libparts.a part1.o part2.o
ritteros:/home/root/parts$ ar t libparts.a
part1.o
part2.o
ritteros:/home/root/parts$ cc -o prog main.c libparts.a
ritteros:/home/root/parts$ cd /tmp
ritteros:/tmp$ ar x /system/x86_64/lib/libritteros.a file.o
```

## See also

[`cc`](cc.md), [`ld`](ld.md), [`as`](as.md), [`make`](make.md),
[`ldd`](ldd.md), [`tar`](tar.md)

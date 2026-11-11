# ldd: the shared libraries a program needs, and whether they are all there

```
ldd PROGRAM...
```

Shows, for each PROGRAM, the shared libraries it needs and where they are:
the ones it names itself and the ones those name, each once, in the order
`ld.so` loads them (breadth first), from `/system/<arch>/lib`. Then it
checks that every function the program and its libraries call is defined
by one of them, searched as `ld.so` searches (the program first, then the
libraries in load order), and names every one that is missing. A program
built without shared libraries (`ld.so` itself, programs made with the
SDK's static link) is said to be "statically linked".

`ldd` reads the files (it maps them into memory); it never runs the program,
so it is safe for any file. PROGRAM is a path, or a bare name looked up as
the shell does: in `/system/<arch>/bin`, then in `/system/<arch>/apps` (42b: inside an app bundle, `NAME.program/NAME`).
`ldd` came with step 22d. Weak references (functions a program may do
without) are not checked.

## Exit status and messages

- `0`: every library and every function each PROGRAM needs is there.
- `1`: something is missing for at least one PROGRAM (the others are still
  listed):
  - `    NAME => not found`: a library that is not in `/system/<arch>/lib`.
  - `    OBJECT needs FUNCTION, which none of its libraries has`.
  - `ldd: PATH: No such file or directory`: no such program.
  - `ldd: PATH: Exec format error`: not an ELF program for this processor
    (a script, a text file, a program for the other architecture).
  - `    (more than 15 libraries)`: the list stops there.
- `2`: no PROGRAM: `usage: ldd PROGRAM...`.

Its reports, also the error lines, go to the output.

## Examples

```
ritteros:/$ ldd imageview
/system/x86_64/apps/imageview.program/imageview:
    libritteros.so.1 => /system/x86_64/lib/libritteros.so.1
    libwindow.so.1 => /system/x86_64/lib/libwindow.so.1
    libimage.so.1 => /system/x86_64/lib/libimage.so.1
ritteros:/$ ldd /system/x86_64/test/ldtest-static
/system/x86_64/test/ldtest-static:
    statically linked: it needs no libraries
ritteros:/$ ldd /readme.txt
ldd: /readme.txt: Exec format error
ldd: exit code 1
```

A library removed from `/system/x86_64/lib` would show so:

```
ritteros:/$ ldd imageview
/system/x86_64/apps/imageview.program/imageview:
    libritteros.so.1 => /system/x86_64/lib/libritteros.so.1
    libwindow.so.1 => /system/x86_64/lib/libwindow.so.1
    libimage.so.1 => not found
    imageview needs image_load, which none of its libraries has
```

## See also

[`ld`](ld.md), [`cc`](cc.md), [`ar`](ar.md), [`mem`](mem.md) (the page
cache that shares the libraries), [`sh`](sh.md)

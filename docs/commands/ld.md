# ld: link programs

```
ld [-o FILE] [OPTION]... FILE...
ld [-o FILE] [-L DIR]... [-lNAME]... [--as-needed] [-dynamic-linker PATH]
   [-static | -shared [-soname NAME] [--version-script FILE] | --no-dynamic-linker | --efi]
   [-T SCRIPT] [-z KEYWORD]... FILE...
```

RitterOS's linker (since step 60), the one `cc` runs: it links ELF
objects, archives of them (`.a`, made by `ar`) and shared libraries into a
program like the system's, written to FILE (`a.out` without `-o`, mode
0755 less the umask). By default the program is position-independent,
started by the loader `/system/<arch>/lib/ld.so`, with the layout of
`user/user.ld` built in (code, read-only data, the part that is read-only
after the relocations, and the data, each on pages of their own), every
library function bound when the program starts (the PLT entries only jump
through their GOT slots) and no copy relocations. The program starts at
`_start`, the start code (`start.o`), which must be among the inputs.

Inputs are read in the order given. Like LLVM's lld, an archive is searched
for every symbol still undefined, wherever it stands on the line, so the
order of archives does not matter. `-lNAME` looks in each `-L` folder, in
turn, for `libNAME.so`, `libNAME.so.1`, then `libNAME.a`. A shared library
named without `--as-needed` is recorded as needed; after `--as-needed`
only if the program uses it. An undefined symbol is always an error (a
weak one is 0). Symbols marked hidden (`__attribute__((visibility
("hidden")))` in C, `.hidden` in assembly) are never exported and are
reached without the GOT.

Since step 62d it also makes what the rest of the system is built as, with
lld's option names: static programs, shared libraries, the loader itself,
and (62f) the kernel from a linker script, (62g) the bootloader as a UEFI
application.

The linker scripts it reads (what `kernel/linker.ld` uses): `ENTRY(_start)`,
`SECTIONS { ... }` with `. = EXPR;`, `NAME = EXPR;`, output sections `NAME
[ADDRESS] [(NOLOAD)] : [ALIGN(N)] { ... }` holding `*(PATTERN ...)` (also
inside `KEEP(...)`), `*(COMMON)`, assignments and `. += N`, and
`/DISCARD/`. Expressions take numbers (with `K` and `M`), `.`, symbols,
`ALIGN(N)`, `ALIGN(EXPR, N)`, `+ - * / % & | << >> ~` and parentheses.
Each input section goes to the first rule that names it; an allocated
section that no rule names is an error. The result is a static executable
with one segment for each run of sections with the same permissions, and a
symbol table.

## Options

| Option | Meaning |
|---|---|
| `-o FILE` | write FILE instead of `a.out` |
| `-L DIR`, `-LDIR` | look for `-l` libraries in DIR (up to 64 folders) |
| `-lNAME` | the library `libNAME.so`, `libNAME.so.1` or `libNAME.a` from the `-L` folders |
| `--as-needed`, `--no-as-needed` | the shared libraries named after it are recorded only if used (or always again) |
| `-dynamic-linker PATH` | the loader programs name (default `/system/<arch>/lib/ld.so`) |
| `-static` | a program without the loader or shared libraries (ET_EXEC): every address filled in when linking |
| `-shared` | a shared library (linked at 0) |
| `-soname NAME`, `-soname=NAME` | the name programs record for the library |
| `--version-script FILE`, `--version-script=FILE` | the functions a library exports (`{ global: a; b; local: *; };`); the rest stay inside |
| `--no-dynamic-linker` | a position-independent file that loads itself (ld.so); per-thread data is refused |
| `--efi` | a UEFI application (PE32+): the objects laid out as lld-link does (ImageBase 0x140000000; .text, .rdata, .data, .reloc), the entry point `efi_main`, a base relocation for every 64-bit address |
| `-T SCRIPT`, `-TSCRIPT` | `user.ld` and `ldso.ld` are the built-in layouts; any other file is read as a linker script (with `-static` only) |
| `-z norelro` | no part made read-only after the relocations |
| `-z now`, `-z relro`, `-z nocopyreloc`, `-z defs`, `-z max-page-size=N` | taken: what it always does |
| `-pie`, `-Bstatic`, `-Bdynamic`, `-nostdlib`, `--hash-style=sysv` | taken: what it always does (`-pie` with `-static` keeps the program position-independent) |

## Exit status and messages

- `0`: the file was written.
- `1`, with `ld: ` and the reason:
  - `ld: undefined symbol: NAME (referenced by FILE.o)`,
    `ld: duplicate symbol: NAME (in A.o and B.o)`;
  - `ld: cannot find -lNAME`, `ld: cannot read FILE`, `ld: FILE: not a
    64-bit little-endian ELF file`, `ld: FILE: built for another
    processor`, `ld: FILE: an archive without a symbol index (make it with
    ar s)`;
  - `ld: no input files`, `ld: no _start (the start code is missing)`,
    `ld: --efi: no efi_main (the UEFI application's entry point)`;
  - `ld: unknown option -x`, `ld: unsupported -z KEYWORD`, `ld: SCRIPT: a
    linker script's layout is for -static links`, `ld: the version script
    names NAME, which is not defined`;
  - `ld: FILE: relocation type N is not supported`, `ld: FILE: a branch out
    of range` and others about a relocation; `ld: cannot write FILE`.

## Examples

```
ritteros:/$ cc -c hello.c
ritteros:/$ ld -o hello /system/x86_64/lib/start.o hello.o -L /system/x86_64/lib --as-needed -lritteros
ritteros:/$ ld -static -o tool /system/x86_64/lib/start.o tool.o /system/x86_64/lib/libritteros.a
ritteros:/$ ld -shared -soname libdemo.so.1 --version-script demo.map -o libdemo.so.1 demo.o
ritteros:/$ ld -o hello hello.o
ld: no _start (the start code is missing)
```

## See also

[`cc`](cc.md) (it runs `ld` for you), [`as`](as.md), [`ar`](ar.md),
[`ldd`](ldd.md), [`make`](make.md)

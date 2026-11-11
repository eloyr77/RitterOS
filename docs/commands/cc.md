# cc: compile C programs

```
cc [-c|-S|-E] [-o FILE] [OPTION]... FILE...
cc [-c | -S | -E | -fsyntax-only] [-o FILE] [-I DIR] [-isystem DIR] [-D NAME[=VALUE]] [-U NAME]
   [-L DIR] [-lNAME] [-B DIR] [-target ARCH] [-v] [-MD | -MMD] [-MP] [-MF FILE] [-MT TARGET]
   [-nostdinc] [-nostdlib] [-O0] [-fno-pic] [-mgeneral-regs-only] FILE...
```

RitterOS's own C compiler (since step 60, written for RitterOS, not a
port), for the machine it runs on: x86-64 or AArch64. It takes C11 with the
GNU additions the system's own sources use (`__attribute__`, statement
expressions, `typeof`, case ranges, the `__builtin_` and `__atomic_`
functions, inline assembly; since 65e-52 `_Atomic` objects and
`<stdatomic.h>`; since 32d on x86-64 the vector types and
`<immintrin.h>`), and makes programs like the system's:
position-independent, linked with `libritteros.so.1`. RitterOS builds
itself with it.

`cc` works as on Unix. Each C file is compiled to assembly (in a process of
its own: `cc` runs itself), assembled by [`as`](as.md), and everything is
linked by [`ld`](ld.md) with the start code `/system/<arch>/lib/start.o`,
the files and `-l` libraries in the order given, `libritteros.so.1` and the
static library `/system/<arch>/lib/libritteros.a` (the SSH, cipher, disk
and window code a program may use without its shared library). `.s` files
go straight to `as`, `.S` files are preprocessed first; any other file
(`.o`, `.a`, `.so`) goes to `ld`. The program is `a.out` unless `-o` names
it. The headers are in `/system/include`: the C library's and cc's own
(`stddef.h`, `stdint.h`, `stdarg.h`, `stdbool.h`, `limits.h`, `float.h`,
`stdalign.h`, `stdnoreturn.h`, `iso646.h`, `stdatomic.h`, `immintrin.h`). Temporary files
go to `/tmp` and are removed at the end.

On x86-64 (since step 32d) `<immintrin.h>` has the intrinsics of SSE to
SSE4.1, AVX, AVX2 and FMA under Intel's names (`_mm256_add_epi32`,
`_mm256_fmadd_ps`, `_mm_shuffle_epi8`, ...; 655 of them), on the types
`__m128`, `__m128i`, `__m128d`, `__m256`, `__m256i` and `__m256d`. A vector
type is GCC's `typedef T NAME __attribute__((vector_size(16 or 32)))`:
copied, passed and returned whole, in an xmm or ymm register as clang
passes them (a function compiled by clang and one compiled by cc call each
other with vectors), without operators, subscripts or brace initializers
(the header's functions do that work). Since 32h a vector is a value in
an xmm/ymm register (xmm8-xmm15; on the stack across a call), and each
intrinsic is its instruction as inline assembly with `"x"` operands, which
cc inlines and gives registers like any instruction: a FIR filter's AVX2
loop runs at 3.6 ns a sample on the laptop (clang's: 1.4; before 32h: 323).
A function marked `__attribute__((target("avx2")))` (or `"avx"`, `"fma"`)
is AVX code: written in VEX encodings throughout, with `vzeroupper` before
its calls and returns, and never inlined into a function without the mark
(so a check of the processor and its SSE fallback stay SSE). AVX's and AVX2's need a processor that has them: ask CPUID
and XGETBV first (`sdk/CLAUDE.md` shows how). `-mavx`, `-mavx2` and `-mfma`
define `__AVX__`, `__AVX2__` and `__FMA__` (cc writes no vector code of its
own, so the intrinsics are there with or without them), and
`__attribute__((target("avx2")))` marks AVX code (see above). Not there: SSE4.2's string
instructions, AVX2's gathers, AVX-512, and the separate headers
`<emmintrin.h>`, `<xmmintrin.h>` and the like (include `<immintrin.h>`).

Inline assembly with `"x"` operands (vector registers, GCC's constraint;
`%x0` names an operand's xmm half, `%t0` its ymm register) is given
registers like an instruction when it has at most one output (`"=x"` or
`"=r"`), only `"x"`, `"r"` and immediate inputs, no memory operand and no
`"memory"` clobber, and clobbers only xmm0-xmm7; its template may give
`{SSE form|VEX form}`, the second taken in AVX code. Other inline assembly
keeps every value in memory around it, as before.

An `_Atomic` object is read and written with atomic instructions, and its
`++`, `--` and `op=` are one atomic update (threads running at once on
several cores lose nothing); every atomic operation is sequentially
consistent. Atomic objects are scalars of 1, 2, 4 or 8 bytes: an atomic
structure or long double is refused (clang calls a library for them).

`-c` and `-S` write `NAME.o` and `NAME.s` in the current folder (the
input's name with the new suffix), or the file `-o` names when there is
one input. Projects made with RitterOS's SDK build on the machine with their
own Makefile and `make` (since 63f: the SDK is in `/system/sdk`, its guide
`/system/sdk/CLAUDE.md`).

The code is optimized by default (since 61: values in registers, constants
folded into instructions, small static functions inlined); `-O0` keeps
every value on the stack. Not yet: `asm goto`, `switch` on an `__int128`,
conversions between `__int128` and long double; long double constants hold
a double's precision. The manual's section on `cc` has the details of what
each step added.

## Options

| Option | Meaning |
|---|---|
| `-c` | compile and assemble, do not link (`NAME.o`, or `-o`) |
| `-S` | compile only (`NAME.s`, or `-o`) |
| `-E` | preprocess only, to the output (or `-o`) |
| `-fsyntax-only` | only check the files: preprocess and parse, write nothing |
| `-o FILE` | the result's name (`a.out` for a program); with `-c`, `-S` or `-E` only for one input file |
| `-I DIR` | look for headers in DIR too (`#include "..."` first in the file's own folder); also `-IDIR` |
| `-isystem DIR` | look for headers in DIR as a system folder (left out by `-MMD`) |
| `-D NAME[=VALUE]` | define a macro (VALUE 1 without one); also `-DNAME` |
| `-U NAME` | undefine a macro; also `-UNAME` |
| `-L DIR` | look for libraries in DIR too; also `-LDIR` |
| `-lNAME` | link `libNAME.so`, `libNAME.so.1` or `libNAME.a`, at its place among the files (no space after `-l`) |
| `-B DIR` | take `as`, `ld` and cc itself from DIR, not `/system/<arch>/bin`; also `-BDIR` |
| `-mavx`, `-mavx2`, `-mfma` | (x86-64, since 32d) define `__AVX__`; `__AVX__` and `__AVX2__`; `__FMA__` |
| `-target ARCH` | make code for ARCH: `x86_64` or `aarch64`, or a triple starting with one (`aarch64-unknown-none-elf`); the tools come from `/system/ARCH/bin` unless `-B` says otherwise |
| `-v` | show each command it runs, on the error output |
| `-MD` | also write a make rule of what each object was built from (since 63g): `OBJECT: SOURCE HEADERS...` in `OBJECT.d` beside the object |
| `-MMD` | the same without the system's headers (those of `/system/include` and `-isystem` folders) |
| `-MP` | with them: an empty rule for each header too, so a header that is gone does not stop `make` |
| `-MF FILE` | the rule's file, instead of `OBJECT.d` |
| `-MT TARGET`, `-MQ TARGET` | the rule's target, instead of OBJECT |
| `-nostdinc` | no `/system/include` |
| `-nostdlib` | link without the start code and the C library |
| `-O0` | no optimization: every value in its own place on the stack |
| `-fno-pic`, `-fno-pie` (also `-fno-PIC`, `-fno-PIE`) | every symbol reached directly, never through the GOT (since 62f: for code whose addresses are all known when linking, as the kernel's) |
| `-fPIC`, `-fpic` | position-independent code again (the default); `-fPIE`, `-fpie` are taken |
| `-mgeneral-regs-only` | no floating-point or SIMD register anywhere (since 62f: the kernel's code); floating point is an error |
| `-mcmodel=kernel`, `-mcmodel=small`, `-mno-red-zone`, `-mno-stack-arg-probe` | taken: the code is already right for them |
| `-O...`, `-W...`, `-f...`, `-g`, `-std=...`, `-pipe`, `-pedantic` | taken and ignored |

`-cc1` and `-asm` are what cc passes to itself for one compilation and for
a `.S` file; they are not for use by hand.

## Exit status and messages

- `0`: everything was built.
- `1`: an error. Errors in a source say where: `FILE:LINE:COLUMN: error:
  ...` (inside a macro, the place where the macro is used); warnings the
  same with `warning:`, and the build goes on. Others:
  - `cc: unknown option '-x'`
  - `cc: cannot read 'FILE'`, `cc: cannot write 'FILE'`
  - `cc: FILE: not a C or assembly file`: an object or library given to
    `-c`, `-S` or `-E`.
  - `cc: -o with -c, -S or -E takes one input file`
  - `cc: unknown target 'NAME' (x86_64 or aarch64)`
  - `cc: cannot run /system/x86_64/bin/as: REASON`
  - The messages of `as` and `ld`; cc ends with `ld`'s exit code.
- `2`: no input file, or `-I`, `-D`, `-U`, `-L` or `-B` without its
  argument: `usage: cc [-c|-S|-E] [-o FILE] [-I DIR] [-D NAME[=VALUE]] [-U
  NAME] [-L DIR] [-lNAME] FILE...`.

## Examples

```
ritteros:/home/root$ cc -o hello hello.c
ritteros:/home/root$ ./hello
hello, world
ritteros:/home/root$ cc -c part.c
ritteros:/home/root$ cc -o prog main.c part.o -lwindow
ritteros:/home/root$ cc -E -DDEBUG=2 config.h | tail -3
ritteros:/home/root$ cc -c -MMD -MP -o build/main.o main.c
ritteros:/home/root$ cat build/main.d
build/main.o: main.c util.h

util.h:
ritteros:/home/root$ cc -o bad bad.c
bad.c:4:5: error: use of undeclared identifier 'cout'
cc: exit code 1
```

## See also

[`as`](as.md), [`ld`](ld.md), [`ar`](ar.md), [`make`](make.md),
[`ldd`](ldd.md), the SDK's guide `/system/sdk/CLAUDE.md`

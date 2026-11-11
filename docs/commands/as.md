# as: assemble

```
as [-target ARCH] [-o FILE] FILE
as [-target ARCH] [-o FILE] -
```

The assembler `cc` uses: it reads GNU assembler syntax (AT&T syntax on
x86-64) and writes an ELF object file, `a.out` unless `-o` names another.
`-` as FILE reads the text from standard input. It assembles for the
machine it runs on; `-target` picks the other one (`x86_64` or `aarch64`,
or a triple starting with either, such as `aarch64-unknown-none-elf`).
`as` has been in RitterOS since step 60.

It knows the instructions `cc` writes and the C library's own assembly, and
the directives `.text .data .bss .section .previous .globl .global .weak
.hidden .local .type .size .p2align .balign .align .byte .short .hword
.value .2byte .word .long .int .4byte .quad .dword .xword .8byte .zero
.skip .space .ascii .asciz .string .set .equ`. Directives that only carry
information for debuggers (`.file`, `.loc`, `.cfi_...`, `.ident`,
`.addrsig`, `.arch`, `.code64`) are taken and ignored. Short jumps are used
where they reach, and the byte sequences are the ones LLVM's assembler
chooses, so an object is byte for byte what clang makes from the same text.

Since step 62e it also takes what the system's own `.S` files use: macros
(`.macro NAME PARAM[=DEFAULT], ...` to `.endm`, called as `NAME ARGS`, with
`\PARAM`, `\()` and `\@` in the body), repetitions (`.rept COUNT`, `.irp
NAME, VALUES`, `.irpc NAME, CHARS`, each to `.endr`), numeric labels (`1:`,
referred to as `1b`, the last one before, and `1f`, the next), and the
kernel's instructions (x86-64: `cli`, `sti`, `iretq`, `swapgs`, `lgdt`,
`ltr`, `in`/`out`, `fxsave64`, moves to and from segment and control
registers; AArch64: `wfe`, `sev`, `smc`, `tlbi`, `ic`, `dc`, system
registers by name). `cc` preprocesses a `.S` file before it reaches `as`.

Since step 32c it also knows, on x86-64, the vector instructions: SSE's (to
SSE4.1: moves, integer and floating-point arithmetic, logic, compares,
shuffles, packing, conversions, `pmovmskb`, `pshufb`, `ptest`, ...), each
also as AVX (`v` and the name, with a second source register, and
`%ymm0`-`%ymm15` for 256 bits: `vpaddd %ymm2, %ymm1, %ymm0`), AVX's and
AVX2's own (`vbroadcastss`, `vpbroadcastb`, `vinserti128`, `vextracti128`,
`vperm2i128`, `vpermq`, `vpermd`, `vpblendd`, `vpblendvb`, `vpsllvd`,
`vpmaskmovd`, `vzeroupper`, ...), FMA (`vfmadd231ps` and the other 131,
213 and 231 forms) and the compares by predicate name (`cmpeqps`,
`vcmpneq_oqpd`). Every one gives clang's bytes (the test `avxas_host_test`
compares about 15,000 of them). Not there: AVX2's gathers (`vpgatherdd`),
SSE4.2's string instructions, F16C and AVX-512.

Since step 32f it knows AArch64's vector (NEON) instructions on integers
too: registers as `v0.16b`, `v1.4s`, lanes as `v2.s[1]`, lists as
`{v0.16b, v1.16b}` or `{v0.16b-v3.16b}`; the operations on three
registers of one arrangement (`add`, `sub`, `cmeq`, `cmhi`, `umax`,
`addp`, `uminp`, `mul`, `and`, `orr`, `eor`, `bic`, `bsl`, ...), on two
(`cnt`, `not`, `rev64`, `abs`, `neg`, the compares with `#0`, `xtn`),
across lanes (`addv`, `umaxv`, `uminv`, `uaddlv`), shifts by an immediate
(`shl`, `ushr`, `sri`, `shrn`, `ushll`, `uxtl`), `dup`, `ins`/`mov` of
lanes, `umov`, `smov`, `movi`, `mvni`, `ext`, `tbl`, `tbx`, `zip1` and the
other permutes, `ld1`-`ld4` and `st1`-`st4` of whole registers (also with
a step after), `ld1r`-`ld4r`, and `fmov` of a register's upper half; all
as clang encodes them (the test `neonas_host_test` compares about 4,700).
Not there: the floating-point vector operations and single lanes to
memory.

## Options

| Option | Meaning |
|---|---|
| `-o FILE` | write the object to FILE instead of `a.out` |
| `-target ARCH` | assemble for ARCH: `x86_64` or `aarch64` (also `x86_64-...` and `aarch64-...` triples); by default the machine's own |

Only one FILE is assembled: with several, the last one counts.

## Exit status and messages

- `0`: the object was written.
- `1`: the input could not be read (`as: cannot read 'FILE'`), the object
  could not be written (`as: cannot write 'FILE'`), or the text has an
  error, reported with its file and line and the first error ending the
  run: `FILE:LINE: error: unknown instruction 'movz %eax'`, `... error:
  symbol 'x' is already defined`, `... error: unknown directive '.foo'`,
  `... error: Intel syntax is not supported`, `... error: the file ends
  inside a .macro`.
- `2`: a wrong command line: `usage: as [-target ARCH] [-o FILE] FILE`,
  `as: unknown option '-x'`, `as: unknown target 'riscv64'`.

## Examples

```
ritteros:/$ cd /home/root
ritteros:/home/root$ as -o start.o start.s
ritteros:/home/root$ cc -S -o hello.s hello.c
ritteros:/home/root$ as -o hello.o hello.s
ritteros:/home/root$ as -target aarch64 -o arm.o arm.s
ritteros:/home/root$ as -o x.o x.s
x.s:12: error: unknown register in '%rxx'
```

## See also

[`cc`](cc.md), [`ld`](ld.md), [`ar`](ar.md), [`make`](make.md)

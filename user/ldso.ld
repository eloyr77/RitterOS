/*
 * ld.so's memory layout: a static program like the others (user/user.ld),
 * but position-independent since 22l (linked at 0, put at a random place by
 * the kernel; it applies its own relocations first, ldso.c relocate_self).
 * Its segments are named, so that each starts on a page of its own (the
 * kernel's loader refuses segments sharing a page) and the headers are in
 * memory (__ehdr_start: where it was put). ld.so has no per-thread data (22k:
 * it has a plain errno, since dlopen runs it on the program's threads): a
 * .tdata or .tbss reaching it fails the link here.
 */
ENTRY(_start)

PHDRS
{
    text PT_LOAD FILEHDR PHDRS FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
    dynamic PT_DYNAMIC;
}

SECTIONS
{
    . = SIZEOF_HEADERS;
    .text : {
        *(.text.start)                  /* ld.so's _start (user/arch/<arch>/ldso.S) first */
        *(.text .text.*)
    } :text
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) } :rodata
    .dynsym : { *(.dynsym) } :rodata
    .dynstr : { *(.dynstr) } :rodata
    .hash : { *(.hash) } :rodata
    .gnu.hash : { *(.gnu.hash) } :rodata
    .rela.dyn : { *(.rela.dyn) } :rodata  /* its own relocations */
    . = ALIGN(4096);
    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) } :data
    .dynamic : { *(.dynamic) } :data :dynamic
    .got : { *(.got) } :data
    .got.plt : { *(.got.plt) } :data
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame*) *(.ritteros.help) *(.interp) }
}

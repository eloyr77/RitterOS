/*
 * Program memory layout: linked at 0x400000 in the lower half. Each kind of
 * section starts on a new page, so that the kernel can give code, read-only
 * data and writable data their own permissions (kernel/process.c). A program
 * that uses shared libraries (22b) also has the dynamic linking sections,
 * each with the others of its kind: the PLT is code, the symbol and
 * relocation tables read-only, the GOT and the dynamic section writable
 * while ld.so fills them in and read-only afterwards: they come first in
 * the writable part, on pages of their own (PT_GNU_RELRO, 22g). A static
 * program has none of them.
 */
ENTRY(_start)

SECTIONS
{
    /* The headers in memory, with the dynamic linking's read-only tables in
     * their page (22l: ld.so finds a position-independent program's place
     * from PT_PHDR; a static program has none of these, nor the headers). */
    . = 0x400000 + SIZEOF_HEADERS;
    .interp : { *(.interp) }            /* the loader's path: /system/<arch>/lib/ld.so */
    .hash : { *(.hash) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .rela.dyn : { *(.rela.dyn) }
    .rela.plt : { *(.rela.plt) }
    /* Statements, not section attributes: they also hold when a section
     * is empty (a program without .data still needs .bss on its own page). */
    . = ALIGN(4096);
    .text : {
        *(.text.start)                  /* _start first */
        *(.text .text.*)
    }
    .plt : { *(.plt) *(.iplt) }         /* calls into shared libraries */
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    .tdata : { *(.tdata .tdata.*) }     /* per-thread data's template (22i): read-only too */
    .tbss : { *(.tbss .tbss.*) }
    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) }
    .dynamic : { *(.dynamic) }
    .got : { *(.got) }
    .got.plt : {
        *(.got.plt)
        . = ALIGN(4096);                /* (so that PT_GNU_RELRO ends on a page boundary) */
    }
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.*) *(COMMON) }
    /* The program's help text (PROGRAM_HELP): in the file, not in memory. */
    .ritteros.help 0 (INFO) : { KEEP(*(.ritteros.help)) }
    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame*) }
}

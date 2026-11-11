/*
 * Kernel memory layout. The kernel is linked at KERNEL_VIRT_BASE in the top
 * 2 GiB of the address space (common/vmlayout.h); the bootloader places it
 * anywhere in physical memory and maps it here.
 *
 * Sections are page-aligned so that the page tables can give each its own
 * permissions (kernel/paging.c):
 *   .text            read + execute
 *   .rodata          read only
 *   .data, .bss      read + write
 *   stack guard      not mapped: a stack overflow faults instead of
 *                    silently overwriting .bss
 *   .stack           read + write (the boot stack from start.S)
 */
ENTRY(_start)

SECTIONS
{
    . = 0xFFFFFFFF80000000;     /* KERNEL_VIRT_BASE */
    __kernel_start = .;

    .text : {
        __text_start = .;
        *(.text.start)          /* _start first, at the very beginning */
        *(.text .text.*)
        __text_end = .;
    }

    .rodata : ALIGN(4096) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    }

    .data : ALIGN(4096) {
        __data_start = .;
        *(.data .data.*)
    }

    .bss : {
        *(COMMON)
        *(.bss .bss.*)
        __data_end = .;
    }

    .stack (NOLOAD) : ALIGN(4096) {
        __stack_guard = .;
        . += 4096;
        __stack_bottom = .;
        *(.stack)
        __stack_top = .;
    }

    . = ALIGN(4096);
    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}

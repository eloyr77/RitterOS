/* Text output helpers on top of the UEFI console (ConOut). */
#ifndef RITTEROS_CONSOLE_H
#define RITTEROS_CONSOLE_H

#include "efi.h"

void console_init(EFI_SYSTEM_TABLE *st);
void set_color(UINTN attribute);
EFI_INPUT_KEY wait_key(void);

/* While on, print() pauses with a "more" prompt whenever the screen is full. */
void set_pager(BOOLEAN on);

void print(const CHAR16 *s);
void print_ascii(const char *s);                  /* 8-bit text, e.g. file contents */
void print_dec(UINT64 value, int width);          /* right-aligned in width */
void print_hex(UINT64 value, int digits);         /* "0x" + zero-padded digits */
void print_size(UINT64 bytes, int width);         /* "640 KiB", "2047 MiB" */

#endif

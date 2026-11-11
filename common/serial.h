/*
 * Direct output to the first serial port, without any firmware help, so it
 * keeps working once the firmware's console is gone. On x86 we set up the
 * line ourselves (115200 8N1); ARM's firmware has done it. The port itself is architecture-specific: serial_putc and
 * serial_present are implemented in arch/<arch>/uart.c. Without a working
 * port (many PCs have none), output is dropped instead of waiting forever.
 */
#ifndef RITTEROS_SERIAL_H
#define RITTEROS_SERIAL_H

#include <stdbool.h>
#include <stdint.h>

/* Is there a serial port that answers (architecture-specific)? */
bool serial_present(void);

/* Send one byte (architecture-specific); dropped without a port. */
void serial_putc(char c);

/* Text, with "\n" sent as "\r\n". */
void serial_write(const char *s);
void serial_write_dec(uint64_t value);
void serial_write_hex(uint64_t value);                 /* 0x + 16 digits */
void serial_write_hexn(uint64_t value, int digits);    /* 0x + given digits */

#endif

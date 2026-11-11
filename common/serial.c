#include "serial.h"

void serial_write(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n')
            serial_putc('\r');
        serial_putc(*s);
    }
}

void serial_write_dec(uint64_t value)
{
    char buf[21];
    int i = 20;
    buf[i] = 0;
    do {
        buf[--i] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    serial_write(&buf[i]);
}

void serial_write_hexn(uint64_t value, int digits)
{
    char buf[19];
    if (digits < 1)
        digits = 1;
    if (digits > 16)
        digits = 16;
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < digits; i++) {
        uint64_t nibble = (value >> ((digits - 1 - i) * 4)) & 0xF;
        buf[2 + i] = (char)(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
    }
    buf[2 + digits] = 0;
    serial_write(buf);
}

void serial_write_hex(uint64_t value)
{
    serial_write_hexn(value, 16);
}

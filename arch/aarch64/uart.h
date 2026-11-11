/* The PL011 registers' address: physical until the kernel's own translation
 * tables are active, then their mapping in the direct map. */
#ifndef RITTEROS_UART_H
#define RITTEROS_UART_H

#include <stdint.h>

#define PL011_PHYS 0x09000000UL

extern volatile uint8_t *pl011_regs;

#endif

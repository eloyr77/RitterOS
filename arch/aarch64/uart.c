/*
 * The first serial port of QEMU's virt machine: an Arm PL011 UART, reached
 * through memory-mapped registers (ARM has no I/O ports). The firmware has
 * already set it up. On real hardware the address would come from the ACPI
 * SPCR table.
 */
#include "mmio.h"
#include "serial.h"
#include "uart.h"

#define REG_DR      0x00        /* data register */
#define REG_FR      0x18        /* flag register */
#define FR_TXFF     (1u << 5)   /* transmit FIFO full */

volatile uint8_t *pl011_regs = (volatile uint8_t *)PL011_PHYS;

/* QEMU's virt machine always has its PL011. */
bool serial_present(void)
{
    return true;
}

void serial_putc(char c)
{
    while (mmio_read32(pl011_regs + REG_FR) & FR_TXFF)
        ;
    mmio_write32(pl011_regs + REG_DR, (uint8_t)c);
}

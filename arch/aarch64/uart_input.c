/*
 * Receiving on the PL011 UART (uart.c). Its interrupt is a shared
 * peripheral interrupt of the GIC, whose number the ACPI SPCR table (serial
 * port console redirection) gives. The UART interrupts when its receive FIFO
 * reaches a fill level, or when bytes have waited a while without reaching it
 * (the receive timeout), so that single key presses arrive at once. The
 * timer tick also looks at the FIFO, a safety net (as for xHCI): input once
 * stopped under load with bytes waiting and no interrupt coming.
 */
#include "arch.h"
#include "acpi.h"
#include "gic.h"
#include "kprintf.h"
#include "string.h"
#include "timer.h"
#include "uart.h"

#define REG_DR      0x000       /* data register */
#define REG_FR      0x018       /* flag register */
#define REG_IMSC    0x038       /* interrupt mask set/clear: 1 = enabled */
#define REG_ICR     0x044       /* interrupt clear */
#define FR_RXFE     (1u << 4)   /* receive FIFO empty */
#define INT_RX      (1u << 4)   /* receive FIFO level reached */
#define INT_RT      (1u << 6)   /* receive timeout */

#define SPCR_GIC    (1u << 3)   /* interrupt type: GIC interrupt (GSIV field) */
#define DEFAULT_INTID 33        /* QEMU virt: SPI 1 */

static void (*deliver)(uint8_t byte);

/* Clear the interrupt first, then empty the FIFO: a byte that arrives
 * after the last look at the FIFO raises a new interrupt. The other way
 * round, clearing it afterwards also cleared that byte's interrupt; QEMU
 * raises one only when the FIFO goes from empty to one byte, so the input
 * stopped for good (seen under load: typed lines cut off). */
static void pl011_interrupt(void *context)
{
    (void)context;
    mmio_write32(pl011_regs + REG_ICR, INT_RX | INT_RT);
    while (!(mmio_read32(pl011_regs + REG_FR) & FR_RXFE))
        deliver((uint8_t)mmio_read32(pl011_regs + REG_DR));
}

/* The timer tick (interrupt context; never at the same time as the
 * handler): data waiting means an interrupt was missed. */
static void tick(void *context)
{
    if (!(mmio_read32(pl011_regs + REG_FR) & FR_RXFE))
        pl011_interrupt(context);
}

bool arch_serial_input_init(void (*receive)(uint8_t byte))
{
    deliver = receive;
    /* SPCR: interrupt type at offset 52, global system interrupt at 54. */
    uint32_t intid = DEFAULT_INTID;
    const char *source = "default";
    const uint8_t *spcr = (const uint8_t *)acpi_find_table("SPCR");
    if (spcr && ((const struct acpi_header *)spcr)->length >= 58 && (spcr[52] & SPCR_GIC)) {
        memcpy(&intid, spcr + 54, 4);
        source = "from SPCR";
    }
    if (!gic_route_spi(intid, false, pl011_interrupt, 0)) {
        kprintf("  serial0:       PL011 receive interrupt %u cannot be routed\n", intid);
        return false;
    }
    while (!(mmio_read32(pl011_regs + REG_FR) & FR_RXFE))      /* drop what came before */
        (void)mmio_read32(pl011_regs + REG_DR);
    mmio_write32(pl011_regs + REG_ICR, 0x7FF);
    mmio_write32(pl011_regs + REG_IMSC, mmio_read32(pl011_regs + REG_IMSC) | INT_RX | INT_RT);
    timer_add_tick_handler(tick, 0);
    kprintf("  serial0:       PL011 UART at %p, INTID %u (%s, level-triggered): receiving\n",
            (void *)(uint64_t)PL011_PHYS, intid, source);
    return true;
}

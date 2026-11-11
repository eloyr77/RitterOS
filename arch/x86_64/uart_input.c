/*
 * Receiving on COM1 (the 16550 UART in uart.c): the UART raises ISA IRQ 4
 * when a byte has arrived, which the IO APIC delivers as an interrupt. On a
 * PC the UART's interrupt line only reaches the interrupt controller when
 * the "OUT2" bit of the modem control register is set. The IO APIC takes
 * the ISA interrupt by its edge: data left in the UART when the handler
 * returns raises no new one, and the input would stop for good (seen with
 * fast typing under load). So the handler reads until the UART is empty,
 * and the timer tick also looks (a safety net, as for xHCI).
 */
#include "apic.h"
#include "arch.h"
#include "io.h"
#include "kprintf.h"
#include "serial.h"
#include "timer.h"

#define COM1        0x3F8
#define COM1_IRQ    4
#define REG_DATA    0       /* receive buffer (read) */
#define REG_IER     1       /* interrupt enable */
#define REG_FCR     2       /* FIFO control (write) */
#define REG_MCR     4       /* modem control */
#define REG_LSR     5       /* line status */
#define IER_RECEIVED 0x01   /* interrupt when data has arrived */
#define FCR_FIFO_ON  0x07   /* enable and clear both FIFOs, interrupt per byte */
#define MCR_DTR_RTS_OUT2 0x0B
#define LSR_DATA_READY   0x01
#define DRAIN_MAX        1024

/* Read what has arrived, until the UART is empty; at most DRAIN_MAX bytes,
 * so that a port that vanished (reads 0xFF: "data ready") cannot keep us
 * here. */
static void drain(void (*receive)(uint8_t byte))
{
    for (int i = 0; i < DRAIN_MAX && (inb(COM1 + REG_LSR) & LSR_DATA_READY); i++) {
        uint8_t byte = inb(COM1 + REG_DATA);
        if (receive)
            receive(byte);
    }
}

static void (*deliver)(uint8_t byte);

static void com1_interrupt(void *context)
{
    (void)context;
    drain(deliver);
}

/* The timer tick (interrupt context, like the handler; they never run at
 * the same time): data waiting means an interrupt was missed. */
static void tick(void *context)
{
    (void)context;
    if (inb(COM1 + REG_LSR) & LSR_DATA_READY)
        drain(deliver);
}

static bool receiving;                  /* (37g-1) the interrupt is on */

/* (37g-1) After a sleep, which reset the UART: its line as at the boot
 * (uart.c), then receiving again. */
void x86_serial_wake(void)
{
    if (!serial_present())
        return;
    x86_serial_setup_again();
    if (receiving) {
        outb(COM1 + REG_FCR, FCR_FIFO_ON);
        outb(COM1 + REG_MCR, MCR_DTR_RTS_OUT2);
        outb(COM1 + REG_IER, IER_RECEIVED);
    }
}

bool arch_serial_input_init(void (*receive)(uint8_t byte))
{
    deliver = receive;
    if (!serial_present()) {
        kprintf("  serial0:       no UART at COM1 (I/O 0x%x)\n", COM1);
        return false;
    }
    uint32_t gsi;
    unsigned vector;
    if (!apic_route_isa_irq(COM1_IRQ, com1_interrupt, 0, &gsi, &vector)) {
        kprintf("  serial0:       COM1 receive interrupt cannot be routed\n");
        return false;
    }
    outb(COM1 + REG_FCR, FCR_FIFO_ON);
    drain(0);                                       /* drop what came before */
    outb(COM1 + REG_MCR, MCR_DTR_RTS_OUT2);
    outb(COM1 + REG_IER, IER_RECEIVED);
    receiving = true;
    timer_add_tick_handler(tick, 0);
    kprintf("  serial0:       16550 UART COM1 at I/O 0x%x, IRQ %u -> GSI %u (IO APIC), "
            "vector 0x%02x: receiving\n", COM1, COM1_IRQ, gsi, vector);
    return true;
}

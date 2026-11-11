/* The PC's COM1 serial port: a 16550 UART reached through x86 I/O ports. */
#include "serial.h"
#include "io.h"

#define COM1        0x3F8
#define REG_DATA    0       /* transmit holding register (write); divisor low with DLAB */
#define REG_IER     1       /* interrupt enable; divisor high with DLAB */
#define REG_FCR     2       /* FIFO control (write) */
#define REG_LCR     3       /* line control */
#define REG_MCR     4       /* modem control */
#define REG_LSR     5       /* line status register */
#define REG_SCRATCH 7       /* scratch register: stores a byte, nothing else */
#define LSR_THRE    0x20    /* transmit holding register empty */
#define LCR_DLAB    0x80    /* registers 0 and 1 become the baud rate divisor */
#define LCR_8N1     0x03    /* 8 data bits, no parity, 1 stop bit */
#define FCR_FIFO_ON 0x07    /* enable and clear both FIFOs */
#define MCR_DTR_RTS 0x03
#define DIVISOR_115200 1    /* the UART clock 1.8432 MHz / 16 / 115200 */

/* A character takes 87 us at 115200 baud; a port read about 1 us. Waiting
 * much longer than that means the port is there but does not work. */
#define SEND_POLLS  100000

static int state;           /* 0 not checked yet, 1 present, -1 absent */

/*
 * Set the line up ourselves: 115200 baud, 8N1, FIFOs on, no interrupts.
 * Firmware does not always do it; a real PC left the baud rate divisor at a
 * random value and sent about one character every 5 seconds.
 */
static void setup(void)
{
    outb(COM1 + REG_IER, 0);
    outb(COM1 + REG_LCR, LCR_DLAB);
    outb(COM1 + REG_DATA, DIVISOR_115200 & 0xFF);
    outb(COM1 + REG_IER, DIVISOR_115200 >> 8);
    outb(COM1 + REG_LCR, LCR_8N1);
    outb(COM1 + REG_FCR, FCR_FIFO_ON);
    outb(COM1 + REG_MCR, MCR_DTR_RTS);
}

/*
 * Without a UART, reads of its ports return 0xFF: "ready to send" and "data
 * received" would always be set. The scratch register tells: a real 16550
 * gives back what was written to it.
 */
bool serial_present(void)
{
    if (state == 0) {
        outb(COM1 + REG_SCRATCH, 0xA5);
        uint8_t first = inb(COM1 + REG_SCRATCH);
        outb(COM1 + REG_SCRATCH, 0x5A);
        uint8_t second = inb(COM1 + REG_SCRATCH);
        state = first == 0xA5 && second == 0x5A ? 1 : -1;
        if (state > 0)
            setup();
    }
    return state > 0;
}

/* (37g-1) After a sleep, which reset the UART: its line set up again. */
void x86_serial_setup_again(void)
{
    if (state > 0)
        setup();
}

void serial_putc(char c)
{
    if (!serial_present())
        return;
    for (unsigned polls = 0; (inb(COM1 + REG_LSR) & LSR_THRE) == 0; polls++) {
        if (polls == SEND_POLLS) {
            state = -1;     /* stuck: give up on the port for good */
            return;
        }
    }
    outb(COM1 + REG_DATA, (uint8_t)c);
}

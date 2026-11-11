/* CPU exception handling: crash reports for fatal exceptions. */
#ifndef RITTEROS_EXCEPTIONS_H
#define RITTEROS_EXCEPTIONS_H

#include "idt.h"

/*
 * Called by the stubs in isr.S for every exception and interrupt.
 * Interrupts (vectors 32-255) go to apic_interrupt. Returns only for those
 * and for exceptions the kernel can continue from (the breakpoint).
 * Fatal exceptions print a report to the console and the serial port, turn
 * the status strip red and halt.
 */
void exception_handler(struct interrupt_frame *frame);

#endif

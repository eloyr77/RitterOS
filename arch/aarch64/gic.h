/* AArch64 interrupts: the GICv3 interrupt controller and the timer. */
#ifndef RITTEROS_GIC_H
#define RITTEROS_GIC_H

#include "arch.h"

/* Called by exception_handler for an IRQ taken while the kernel ran. */
void gic_handle_irq(void);

/* Deliver shared peripheral interrupt intid (32-1019) to this core and call
 * handler(context) for it. Wired device interrupts are usually level-
 * triggered (edge = false), messages edge-triggered. */
bool gic_route_spi(uint32_t intid, bool edge, irq_handler_fn handler, void *context);

/* (65c) A started core's redistributor, CPU interface and tick. */
bool gic_init_core(void);

#endif

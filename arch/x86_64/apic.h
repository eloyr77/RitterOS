/* x86-64 interrupts: the Local APIC and its timer. */
#ifndef RITTEROS_APIC_H
#define RITTEROS_APIC_H

#include <stdint.h>
#include "arch.h"

#define VECTOR_TIMER    0x20    /* first vector after the 32 CPU exceptions */
#define VECTOR_DEVICE_FIRST 0x30   /* device interrupts: MSI-X and IO APIC */
#define VECTOR_DEVICE_LAST  0xEF
#define VECTOR_RESCHEDULE   0xF0   /* (65c) another core: look at the run queue */
#define VECTOR_TLB          0xF1   /* (65c-2) another core: forget a page (tlb_page) */
#define VECTOR_SLEEP        0xF2   /* (37g-1) another core: keep your place, the machine sleeps */
#define VECTOR_SPURIOUS 0xFF

/* Called by exception_handler for vectors 32-255. */
void apic_interrupt(uint64_t vector);

/* Deliver ISA interrupt line irq (e.g. 4 for COM1) through the IO APIC to a
 * new vector that calls handler(context). Applies the MADT's interrupt
 * source overrides; sets *gsi and *vector for the log. */
bool apic_route_isa_irq(uint8_t irq, irq_handler_fn handler, void *context, uint32_t *gsi,
                        unsigned *vector);

/* (65b) This core's Local APIC ID (the boot core's: the others never ask). */
uint32_t apic_boot_id(void);

/* (65b) INIT, then STARTUP at page vector * 4 KiB, to the core with Local
 * APIC ID id; false if the APIC did not take the message. */
bool apic_send_init(uint32_t id);
bool apic_send_startup(uint32_t id, uint8_t vector);

/* (65c) A started core's own Local APIC: on, and ticking as the boot core's. */
void apic_init_core(void);

/* (65c) An interrupt to one core (by APIC ID), and an NMI to all others. */
bool apic_send_ipi(uint32_t id, uint8_t vector);
void apic_nmi_others(void);

/* (37g-1) Around a sleep: the IO APIC's entries kept, and after it the
 * 8259s masked again, the IO APIC's entries back. */
void apic_sleep_save(void);
void apic_wake(void);

/* (37g-5d) Before a reset, as Linux's: every IO APIC input masked
 * (clear_IO_APIC), and this core's Local APIC quiet and off
 * (disable_local_APIC). */
void apic_mask_all(void);
void apic_shutdown_core(void);

#endif

/*
 * The ACPI MADT (signature "APIC"): the machine's interrupt controllers. After
 * a short header it holds variable-length entries, each starting with a type
 * byte and a length byte:
 *   x86:  0 Local APIC (one per core), 1 IO APIC, 2 interrupt source override
 *   ARM:  0x0B GIC CPU interface (one per core), 0x0C GIC distributor,
 *         0x0D GIC MSI frame, 0x0E GIC redistributor range
 */
#ifndef RITTEROS_MADT_H
#define RITTEROS_MADT_H

#include <stdint.h>
#include "acpi.h"

#define MADT_LOCAL_APIC         0x00
#define MADT_IO_APIC            0x01
#define MADT_SOURCE_OVERRIDE    0x02
#define MADT_GIC_CPU            0x0B
#define MADT_GIC_DISTRIBUTOR    0x0C
#define MADT_GIC_MSI_FRAME      0x0D
#define MADT_GIC_REDISTRIBUTOR  0x0E

struct madt {
    struct acpi_header header;
    uint32_t local_apic_address;   /* x86: physical address of the Local APICs */
    uint32_t flags;                /* bit 0: the PC's 8259 PICs are present too */
} __attribute__((packed));

/* The MADT, or 0 if the firmware has none. */
const struct madt *madt_get(void);

/* The index-th entry of the given type, or 0. Entries are packed structures:
 * read their fields with memcpy or packed struct types. */
const uint8_t *madt_entry(uint8_t type, int index);

/* How many entries of the given type there are. */
int madt_count(uint8_t type);

#endif

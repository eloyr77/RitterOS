/*
 * SMBIOS (System Management BIOS): the firmware's inventory of the machine:
 * manufacturer and model, firmware version, processors, memory modules.
 */
#ifndef RITTEROS_SMBIOS_H
#define RITTEROS_SMBIOS_H

#include <stdint.h>

/* Parse the SMBIOS tables at the entry point (physical address) and log a
 * summary. */
void smbios_report(uint64_t entry_point);

#endif

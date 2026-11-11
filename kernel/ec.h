/*
 * The ACPI embedded controller (37b-5, roadmap item 37): the laptop's small
 * computer for the battery, the lid, the keyboard's hot keys, the fans,
 * which the firmware's code reads and writes through EmbeddedControl
 * regions. ACPI 6.5 chapter 12: a data port and a command/status port,
 * 256 bytes of its memory read (0x80) and written (0x81) one at a time.
 * Found from the ECDT when the firmware has one, else as the device
 * PNP0C09 (its _CRS gives the ports); after it is set up the firmware's
 * _REG(3, 1) says it may be used (as Linux).
 */
#ifndef RITTEROS_EC_H
#define RITTEROS_EC_H

#include <stdbool.h>
#include <stdint.h>

struct aml_node;

/* Before the boot's _INI: the ECDT's controller, if the firmware has one
 * (so its _INI can use it already). */
void ec_start_early(void);

/* After the boot's _INI: the PNP0C09 device if no ECDT gave one, then
 * _REG for the controller's space. Called with the interpreter held. */
void ec_start(void);

/* The interpreter's EmbeddedControl region access: width bits (8 per
 * byte, low byte first) at address of the controller's memory. */
bool ec_access(uint64_t address, int width, uint64_t *value, bool write);

/* (37c-2c) Its GPE (the device's _GPE, else the ECDT's), -1 when it has
 * none or does not answer; its events (queries) while it says one waits
 * (SCI_EVT), each one's _Qxx run: how many. With the interpreter held. */
int ec_gpe(void);
int ec_run_queries(void);

/* power's line about it. */
void ec_print(void);

#endif

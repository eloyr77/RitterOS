/*
 * The firmware's events (37c-2, roadmap item 37): ACPI's interrupt, the
 * SCI (System Control Interrupt), and what it says: on x86 the fixed
 * events of the PM1 registers (the power button) and the general-purpose
 * events (GPEs) of the FADT's GPE blocks. The interrupt handler only
 * takes note; the kernel thread `acpi` acts (the firmware's methods need
 * the interpreter's mutex, a program start the scheduler).
 *
 * The power button does what /system/config/power's `power_button` says
 * (the user's choice, 2026-11-05): `off` (the default) starts
 * /system/<arch>/bin/poweroff, so a press switches off exactly as typing
 * poweroff does (Linux's orderly_poweroff); `nothing` only logs it;
 * `sleep` comes with 37g. Holding the button switches off in the chipset
 * whatever the kernel does.
 *
 * (37c-2b) The GPEs' methods (\_GPE._Lxx, _Exx) run on the thread, as do
 * ARM's events (the GED's _EVT); a power button device's Notify(0x80)
 * presses the button too.
 */
#ifndef RITTEROS_ACPI_EVENTS_H
#define RITTEROS_ACPI_EVENTS_H

#include <stdbool.h>
#include <stdint.h>

/* After acpi_aml_load: ACPI mode on, every fixed event and GPE off and
 * cleared, the SCI routed, the power button's event on, the thread
 * started; the GPEs with methods (not for waking only) on; the GED's
 * lines routed. Logs what it set up. */
void acpi_events_start(void);

/* (37g-1) Before a sleep (going): the events off but the RTC's alarm (and
 * the fixed power button) as wake-ups; after it: everything as before, and
 * *wake_status what PM1's status said (bit 15 woke, 10 the RTC's alarm, 8
 * the power button). */
/* (37g-1) Around a sleep; (37g-7) alarm: the RTC's enabled to wake. */
void acpi_events_sleep(bool going, bool alarm, uint32_t *wake_status);

/* (37g-7) The sleep's end: presses acted on again. */
void acpi_events_awake(void);

/* (37g-7) sleep_after's minutes passed without use: the thread sleeps. */
void acpi_events_idle(void);

/* (37c-2b) A power button device's Notify(0x80): from the interpreter. */
void acpi_events_button(void);

/* (37c-2c) A lid's, an AC adapter's or a battery's Notify: from the
 * interpreter; the thread looks at the new state. */
enum { ACPI_EVENT_LID, ACPI_EVENT_AC, ACPI_EVENT_BATTERY };
void acpi_events_notify(int what);

/* After the root mount: /system/config/power's power_button and (37c-2c)
 * lid (`nothing`, the default, or `sleep`: from 37g). Logs them. */
void acpi_events_settings(void);

/* power's line about the events. */
void acpi_events_print(void);

#endif

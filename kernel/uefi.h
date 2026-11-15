/*
 * UEFI runtime services: the part of the firmware that stays usable after
 * ExitBootServices (clock, reset/power-off, firmware variables). They are
 * called with the Microsoft x64 calling convention (EFIAPI in efi.h) and need
 * the firmware's runtime memory mapped at its physical address.
 */
#ifndef RITTEROS_UEFI_H
#define RITTEROS_UEFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Remember the runtime services table (physical address, may be 0). */
void uefi_init(uint64_t runtime_services);

/* Read the firmware clock and log it; returns false if unavailable. */
bool uefi_report_time(void);

/* The firmware clock in nanoseconds since 1970-01-01 UTC; 0 if unavailable. */
uint64_t uefi_time_ns(void);

/* Start the clock: the firmware's time now, carried on by the CPU's
 * counter (needs its rate: after timer_init). (68p) Also makes the clock
 * page programs read it from (struct sys_clock). */
void uefi_clock_start(void);

/* (68p) The clock page's physical address (0: none), which every program
 * maps read-only; uefi_clock_publish writes it again after anything it
 * holds changed (the clock set; x86: the counter carried over a sleep). */
uint64_t uefi_clock_page(void);
void uefi_clock_publish(void);

/* (68u) The counter's rate measured better: the clock goes on at hz from
 * now, without a jump (its base moved; the page published with it). */
void uefi_clock_rate(uint64_t hz);

/* (68p) Nanoseconds since the clock started: CLOCK_MONOTONIC, which date -s
 * does not move (the date's clock where there is no counter). */
uint64_t uefi_clock_monotonic_ns(void);

/* Nanoseconds since 1970-01-01 UTC from that clock, without calling the
 * firmware (before uefi_clock_start: the firmware's); 0 if unknown. */
uint64_t uefi_clock_ns(void);

/* The same once the clock runs (uefi_clock_start), else 0: never calls
 * the firmware (the logs stamp every line, also the boot's first ones). */
uint64_t uefi_clock_ns_running(void);

/* (68l) Set the clock to ns (since 1970-01-01 UTC): the running clock at
 * once, and the firmware's (SetTime, in its own zone) so the next start
 * reads it. True when the firmware's clock read the new time back (within
 * 3 s); false when it could not be set (no runtime services, SetTime
 * refused). Called from a system call (the FP/SIMD registers saved). */
bool uefi_clock_set(uint64_t ns);

/* "YYYY-MM-DD HH:MM:SS" (UTC) of ns since 1970, for messages. */
void uefi_ns_text(uint64_t ns, char *buf, size_t size);

/* A firmware variable (name in ASCII, guid as the firmware keeps it), for
 * the system call efivar: get its attributes and data into buf (*size: the
 * room, then the variable's bytes), or set it (size 0 deletes it). 0, or
 * -ERR_NOENT, -ERR_2BIG (*size set), -ERR_NOSYS (no runtime services),
 * -ERR_IO. Called from a system call: the program's FP/SIMD registers are
 * saved around the firmware call (firmware code may use them). */
int uefi_variable_get(const char *name, const uint8_t *guid, uint32_t *attributes, void *buf,
                      uint64_t *size);
int uefi_variable_set(const char *name, const uint8_t *guid, uint32_t attributes,
                      const void *data, uint64_t size);

/* The firmware clock as "YYYY-MM-DD hh:mm:ss"; false if unavailable. */
bool uefi_time_text(char *buf, size_t size);

#endif

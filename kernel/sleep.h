/*
 * (37g, roadmap item 37) Sleep: ACPI S3, "suspend to RAM", woken by the
 * RTC's alarm. See sleep.c.
 */
#ifndef RITTEROS_SLEEP_H
#define RITTEROS_SLEEP_H

#include <stdbool.h>
#include <stddef.h>

/* Sleep for seconds (1 to a day) and wake by the RTC's alarm: 0 once
 * awake again, -1 if it did not sleep (why in the log). A thread's call.
 * Logs both ends. */
int sleep_for(unsigned seconds);

/* (37g-2) A kernel driver's device, for sleep: suspend (its work stopped,
 * what the device must keep in memory; false: the sleep is called off)
 * before, the last registered first, and resume (as before) after, in
 * the order registered. Without them (0, 0) the machine does not sleep
 * while the device is there: name says which. */
void sleep_register(const char *name, bool (*suspend)(void *ctx), bool (*resume)(void *ctx),
                    void *ctx);

#endif

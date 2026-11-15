/*
 * (43b-4) Event counters and timers as descriptors, as Linux's eventfd and
 * timerfd (FILE_EVENT, FILE_TIMER): a program waits for either with read
 * or poll beside its other descriptors. Calls on behalf of a program wait
 * unless its descriptor is O_NONBLOCK (thread->dont_wait: -ERR_AGAIN) and
 * end on a stop request (-ERR_INTR).
 */
#ifndef RITTEROS_EVENTFD_H
#define RITTEROS_EVENTFD_H

#include <stdbool.h>
#include <stdint.h>

/* An event counter starting at initial; semaphore: each read takes 1
 * instead of all. 0 without memory. */
struct eventfd;
struct eventfd *eventfd_new(uint64_t initial, bool semaphore);
void eventfd_free(struct eventfd *e);

/* Read: waits while the count is 0, then takes it (or 1) into *value.
 * Write: adds add (not 2^64 - 1: -ERR_INVAL), waiting while the sum would
 * pass 2^64 - 2. Poll: POLLIN while above 0, POLLOUT while 1 more fits. */
int64_t eventfd_read(struct eventfd *e, uint64_t *value);
int64_t eventfd_write(struct eventfd *e, uint64_t add);
int eventfd_poll(struct eventfd *e);

/* A timer on the system's clock (uefi_clock_ns, CLOCK_REALTIME; 68p:
 * CLOCK_MONOTONIC's uefi_clock_monotonic_ns, as the library reads it). Set: first expiry `value` nanoseconds from
 * now (absolute: at that time), then every `interval` (0: once); value 0
 * disarms it; *old_value and *old_interval what it was (the time left).
 * Read: waits until it has expired, then the expirations since the last
 * read. Poll: POLLIN once expired; *wake the timer tick by which a poll
 * should look again (lowered, never raised: 0 is none). */
struct timerfd;
struct timerfd *timerfd_new(bool monotonic);  /* (68p) monotonic: CLOCK_MONOTONIC/_BOOTTIME */
void timerfd_free(struct timerfd *t);
void timerfd_set(struct timerfd *t, uint64_t value, uint64_t interval, bool absolute,
                 uint64_t *old_value, uint64_t *old_interval);
void timerfd_get(struct timerfd *t, uint64_t *value, uint64_t *interval);
int64_t timerfd_read(struct timerfd *t, uint64_t *expirations);
int timerfd_poll(struct timerfd *t, uint64_t *wake);

#endif

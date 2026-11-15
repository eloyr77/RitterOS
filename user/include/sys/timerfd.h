/*
 * sys/timerfd.h (43b-4): timers as descriptors, as Linux's. read waits
 * until the timer has expired and gives how many times since the last
 * read (8 bytes, a uint64_t; EAGAIN with TFD_NONBLOCK); poll: POLLIN once
 * expired. timerfd_settime's it_value is the first expiry (from now, or
 * the clock's time with TFD_TIMER_ABSTIME; 0 disarms it), it_interval the
 * time between later ones (0: once); timerfd_gettime gives the time left
 * and the interval. CLOCK_REALTIME, CLOCK_MONOTONIC and CLOCK_BOOTTIME
 * are one clock here. Expiries are seen at the system's tick (10 ms).
 */
#ifndef RITTEROS_SYS_TIMERFD_H
#define RITTEROS_SYS_TIMERFD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <time.h>
#include "../ritteros.h"

#define TFD_CLOEXEC         OPEN_CLOEXEC
#define TFD_NONBLOCK        OPEN_NONBLOCK
#define TFD_TIMER_ABSTIME   1
#define TFD_TIMER_CANCEL_ON_SET 2

int timerfd_create(int clock, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec *value, struct itimerspec *old);
int timerfd_gettime(int fd, struct itimerspec *value);

#ifdef __cplusplus
}
#endif

#endif

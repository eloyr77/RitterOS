/*
 * Cancellation points (54c, pthread_cancel): the library's waiting calls
 * act on a pending deferred cancel when they begin and when their wait
 * was interrupted (ERR_INTR). Weak, so a program that never uses pthreads
 * (ld.so, static programs) does not take pthread.c in. Internal to the
 * library.
 */
#ifndef RITTEROS_CANCEL_H
#define RITTEROS_CANCEL_H

#include <stdint.h>
#include "ritteros.h"

__attribute__((weak)) void __ritteros_cancel_point(void);

static inline void cancel_point(void)
{
    if (__ritteros_cancel_point)
        __ritteros_cancel_point();
}

/* A system call's result, after acting on a cancel that interrupted it. */
static inline int64_t cancel_after(int64_t r)
{
    if (r == -ERR_INTR)
        cancel_point();
    return r;
}

#endif

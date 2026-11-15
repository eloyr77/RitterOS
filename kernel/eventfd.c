/*
 * (43b-4) Event counters and timers as descriptors (see eventfd.h). Each
 * has a spinlock over its state and one pollable queue for readers and
 * writers alike; a waiter joins the queue under the lock and sleeps after
 * giving it back (as the mutex does), so a change between cannot pass it
 * by. A timer counts its expirations from the clock when it is looked at
 * (nothing runs when it expires): a waiting read sleeps until the tick of
 * the next expiry, and a poll lowers its own deadline to it.
 */
#include "eventfd.h"
#include "heap.h"
#include "sched.h"
#include "sync.h"
#include "syscalls.h"
#include "timer.h"
#include "uefi.h"

#define EVENT_MAX 0xFFFFFFFFFFFFFFFEULL     /* the most a counter holds (Linux's) */

struct eventfd {
    struct spinlock lock;
    uint64_t count;
    bool semaphore;
    struct wait_queue queue;
};

/* One sleep on q, joined under lock (taken, state its interrupts) and
 * given back before sleeping until deadline (0: none): 0, or -ERR_AGAIN
 * for a call that must not wait, -ERR_INTR on a stop request. */
static int64_t sleep_once(struct wait_queue *q, struct spinlock *lock, uint64_t state,
                          uint64_t at)
{
    struct thread *self = thread_current();
    if (self->dont_wait) {
        spin_unlock(lock, state);
        return -ERR_AGAIN;
    }
    if (self->stop_requested) {
        spin_unlock(lock, state);
        return -ERR_INTR;
    }
    wait_prepare_at(q, at);                 /* (45a: a precise deadline, 0: none) */
    spin_unlock(lock, state);
    wait_commit_at(at);
    return 0;
}

struct eventfd *eventfd_new(uint64_t initial, bool semaphore)
{
    struct eventfd *e = kzalloc(sizeof(*e));
    if (!e)
        return 0;
    struct spinlock init = SPINLOCK_INIT("eventfd");
    e->lock = init;
    e->count = initial;
    e->semaphore = semaphore;
    e->queue.pollable = true;
    return e;
}

void eventfd_free(struct eventfd *e)
{
    wait_queue_detach(&e->queue);               /* (pollers keep pointers to it) */
    kfree(e);
}

int64_t eventfd_read(struct eventfd *e, uint64_t *value)
{
    for (;;) {
        uint64_t state = spin_lock(&e->lock);
        if (e->count) {
            *value = e->semaphore ? 1 : e->count;
            e->count -= *value;
            spin_unlock(&e->lock, state);
            wait_queue_wake_all(&e->queue);     /* (writers waiting for room) */
            return 0;
        }
        int64_t r = sleep_once(&e->queue, &e->lock, state, 0);
        if (r)
            return r;
    }
}

int64_t eventfd_write(struct eventfd *e, uint64_t add)
{
    if (add == ~0ULL)
        return -ERR_INVAL;
    for (;;) {
        uint64_t state = spin_lock(&e->lock);
        if (add <= EVENT_MAX - e->count) {
            e->count += add;
            spin_unlock(&e->lock, state);
            if (add)
                wait_queue_wake_all(&e->queue);
            return 0;
        }
        int64_t r = sleep_once(&e->queue, &e->lock, state, 0);
        if (r)
            return r;
    }
}

int eventfd_poll(struct eventfd *e)
{
    poll_watch(&e->queue);                      /* (65e) before the state is read */
    uint64_t state = spin_lock(&e->lock);
    int r = (e->count ? POLLIN : 0) | (e->count < EVENT_MAX ? POLLOUT : 0);
    spin_unlock(&e->lock, state);
    return r;
}

/* --- timers --- */

struct timerfd {
    struct spinlock lock;
    bool monotonic;                         /* (68p) its clock: since the start, else the date's */
    uint64_t next;                          /* the next expiry (clock ns), 0: disarmed */
    uint64_t interval;                      /* ns between expiries, 0: once */
    uint64_t expired;                       /* expirations not read yet */
    struct wait_queue queue;
};

struct timerfd *timerfd_new(bool monotonic)
{
    struct timerfd *t = kzalloc(sizeof(*t));
    if (!t)
        return 0;
    struct spinlock init = SPINLOCK_INIT("timerfd");
    t->lock = init;
    t->monotonic = monotonic;
    t->queue.pollable = true;
    return t;
}

/* (68p) Its clock now: CLOCK_MONOTONIC's as the library reads it (the
 * clock page), or the date's. */
static uint64_t clock_now(struct timerfd *t)
{
    return t->monotonic ? uefi_clock_monotonic_ns() : uefi_clock_ns();
}

void timerfd_free(struct timerfd *t)
{
    wait_queue_detach(&t->queue);
    kfree(t);
}

/* The expirations up to now counted (t->lock held). */
static void settle(struct timerfd *t, uint64_t now)
{
    if (!t->next || now < t->next)
        return;
    if (!t->interval) {
        t->expired++;
        t->next = 0;
        return;
    }
    uint64_t n = (now - t->next) / t->interval + 1;
    t->expired += n;
    t->next += n * t->interval;
}

/* (45a) The arch_counter() value at which the clock reaches at (a
 * precise deadline; until 45a the tick after it). */
static uint64_t counter_at(uint64_t at, uint64_t now)
{
    uint64_t c = sched_counter_after_ns(at > now ? at - now : 0);
    return c ? c : 1;
}

void timerfd_set(struct timerfd *t, uint64_t value, uint64_t interval, bool absolute,
                 uint64_t *old_value, uint64_t *old_interval)
{
    uint64_t now = clock_now(t);
    uint64_t state = spin_lock(&t->lock);
    settle(t, now);
    *old_value = t->next ? t->next - now : 0;
    *old_interval = t->interval;
    t->expired = 0;                         /* (Linux's: a new setting starts afresh) */
    t->interval = value ? interval : 0;
    t->next = !value ? 0 : absolute ? (value > now ? value : now) : now + value;
    spin_unlock(&t->lock, state);
    wait_queue_wake_all(&t->queue);         /* (a waiting read looks at the new time) */
}

void timerfd_get(struct timerfd *t, uint64_t *value, uint64_t *interval)
{
    uint64_t now = clock_now(t);
    uint64_t state = spin_lock(&t->lock);
    settle(t, now);
    *value = t->next ? t->next - now : 0;
    *interval = t->interval;
    spin_unlock(&t->lock, state);
}

int64_t timerfd_read(struct timerfd *t, uint64_t *expirations)
{
    for (;;) {
        uint64_t now = clock_now(t);
        uint64_t state = spin_lock(&t->lock);
        settle(t, now);
        if (t->expired) {
            *expirations = t->expired;
            t->expired = 0;
            spin_unlock(&t->lock, state);
            return 0;
        }
        int64_t r = sleep_once(&t->queue, &t->lock, state, t->next ? counter_at(t->next, now) : 0);
        if (r)
            return r;
    }
}

int timerfd_poll(struct timerfd *t, uint64_t *wake)
{
    poll_watch(&t->queue);
    uint64_t now = clock_now(t);
    uint64_t state = spin_lock(&t->lock);
    settle(t, now);
    int r = t->expired ? POLLIN : 0;
    if (!r && t->next) {
        uint64_t at = counter_at(t->next, now);
        if (!*wake || at < *wake)
            *wake = at;
    }
    spin_unlock(&t->lock, state);
    return r;
}

/*
 * Futexes (see futex.h). A waiter puts itself on its bucket's list first
 * and reads the word afterwards (outside the lock: reading may bring the
 * page in): a waker changes the word before it wakes, so either the waiter
 * reads the new value (and does not sleep) or the waker finds it listed.
 * Waking marks waiters, then wakes the bucket's queue; each sleeper checks
 * its own mark, so exactly the ones counted go on.
 */
#include "futex.h"
#include "process.h"
#include "sched.h"
#include "sync.h"
#include "syscalls.h"
#include "timer.h"

#define BUCKETS 32

struct waiter {
    const void *program;
    uint64_t addr;
    bool woken;
    struct waiter *next;
};

static struct bucket {
    struct waiter *list;                /* in arrival order */
    struct wait_queue queue;
} buckets[BUCKETS];
static struct spinlock lock = SPINLOCK_INIT("futex");

static struct bucket *bucket_of(const void *program, uint64_t addr)
{
    return &buckets[(((uintptr_t)program >> 4) ^ (addr >> 2)) % BUCKETS];
}

int64_t futex_wait(const void *program, uint64_t addr, uint32_t value, uint64_t timeout_ns)
{
    struct bucket *b = bucket_of(program, addr);
    struct waiter w = { program, addr, false, 0 };
    uint64_t state = spin_lock(&lock);
    struct waiter **end = &b->list;
    while (*end)
        end = &(*end)->next;
    *end = &w;
    spin_unlock(&lock, state);

    uint32_t now;
    int64_t result = 0;
    if (!process_copy_from_user(&now, addr, sizeof(now))) {
        result = -ERR_FAULT;
    } else if (now != value) {
        result = -ERR_AGAIN;
    } else {
        const uint64_t per_tick = 1000000000 / TIMER_HZ;
        uint64_t deadline = timeout_ns ? timer_ticks() + (timeout_ns + per_tick - 1) / per_tick + 1 : 0;
        struct thread *self = thread_current();
        state = arch_irq_save();
        while (!w.woken) {
            if (self->stop_requested) {
                result = -ERR_INTR;
                break;
            }
            /* (65a-3) On the queue before the look at w.woken: a wake that
             * sets it afterwards finds us there. */
            wait_prepare(&b->queue, deadline);
            if (w.woken || self->stop_requested) {
                wait_cancel();
                continue;
            }
            if (!wait_commit(deadline)) {
                result = -ERR_TIMEDOUT;
                break;
            }
        }
        arch_irq_restore(state);
    }

    state = spin_lock(&lock);
    for (struct waiter **at = &b->list; *at; at = &(*at)->next)
        if (*at == &w) {
            *at = w.next;
            break;
        }
    bool woken = w.woken;
    spin_unlock(&lock, state);
    return woken ? 0 : result;                  /* (a wake counted for us is not lost) */
}

int64_t futex_wake(const void *program, uint64_t addr, int64_t n)
{
    struct bucket *b = bucket_of(program, addr);
    int64_t count = 0;
    uint64_t state = spin_lock(&lock);
    for (struct waiter *w = b->list; w && count < n; w = w->next)
        if (w->program == program && w->addr == addr && !w->woken) {
            w->woken = true;
            count++;
        }
    spin_unlock(&lock, state);
    if (count)
        wait_queue_wake_all(&b->queue);
    return count;
}

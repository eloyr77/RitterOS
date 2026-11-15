/*
 * (43a-4) Unnamed semaphores on the futex: the count is the word; a taker
 * takes 1 by compare-and-swap while it is above 0, else counts itself in
 * `waiters` and sleeps while the word is 0; sem_post adds 1 and wakes one
 * sleeper if there are any. (43b-3) Shared between programs too (pshared,
 * sem_open): in shared memory the kernel keys the futex by the memory.
 */
#include <errno.h>
#include <limits.h>
#include <semaphore.h>
#include <stdbool.h>
#include <time.h>
#include "cancel.h"
#include "ritteros.h"

int sem_init(sem_t *s, int pshared, unsigned int value)
{
    (void)pshared;                              /* (43b-3: the same either way) */
    if (value > SEM_VALUE_MAX) {
        errno = EINVAL;
        return -1;
    }
    s->value = (int)value;
    s->waiters = 0;
    return 0;
}

int sem_destroy(sem_t *s)
{
    (void)s;
    return 0;
}

static bool take(sem_t *s)
{
    int v = __atomic_load_n(&s->value, __ATOMIC_RELAXED);
    while (v > 0)
        if (__atomic_compare_exchange_n(&s->value, &v, v - 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return true;
    return false;
}

int sem_trywait(sem_t *s)
{
    if (take(s))
        return 0;
    errno = EAGAIN;
    return -1;
}

/* Wait until one is taken; until (CLOCK_REALTIME) 0: no limit. */
static int wait_for(sem_t *s, const struct timespec *until)
{
    cancel_point();
    for (;;) {
        if (take(s))
            return 0;
        int64_t ns = 0;
        if (until) {
            if (until->tv_nsec < 0 || until->tv_nsec >= 1000000000) {
                errno = EINVAL;
                return -1;
            }
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            ns = (int64_t)(until->tv_sec - now.tv_sec) * 1000000000 + (until->tv_nsec - now.tv_nsec);
            if (ns <= 0) {
                errno = ETIMEDOUT;
                return -1;
            }
        }
        __atomic_fetch_add(&s->waiters, 1, __ATOMIC_SEQ_CST);
        int64_t r = syscall6(SYS_FUTEX, (int64_t)&s->value, FUTEX_WAIT, 0, ns, 0, 0);
        __atomic_fetch_sub(&s->waiters, 1, __ATOMIC_SEQ_CST);
        if (r == -ERR_INTR) {
            cancel_point();
            errno = EINTR;
            return -1;
        }
    }
}

int sem_wait(sem_t *s)
{
    return wait_for(s, 0);
}

int sem_timedwait(sem_t *s, const struct timespec *until)
{
    return wait_for(s, until);
}

int sem_post(sem_t *s)
{
    int v = __atomic_load_n(&s->value, __ATOMIC_RELAXED);
    do {
        if (v == SEM_VALUE_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
    } while (!__atomic_compare_exchange_n(&s->value, &v, v + 1, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    if (__atomic_load_n(&s->waiters, __ATOMIC_SEQ_CST))
        syscall6(SYS_FUTEX, (int64_t)&s->value, FUTEX_WAKE, 1, 0, 0, 0);
    return 0;
}

int sem_getvalue(sem_t *s, int *value)
{
    *value = __atomic_load_n(&s->value, __ATOMIC_RELAXED);
    return 0;
}

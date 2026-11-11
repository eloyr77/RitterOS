/*
 * threads.h (step 22y) over thread.h's threads and the kernel's futex.
 * The mutex is Drepper's ("Futexes Are Tricky": 0 free, 1 taken, 2 taken
 * and maybe waited for, so an unlock without waiters makes no system
 * call); a condition variable is a sequence number that waiters sleep on;
 * thrd_detach is the library's: a detached thread marks itself done as it
 * ends, and thrd_create joins those first, which frees their numbers.
 */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <thread.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>
#include "ritteros.h"
#include "threadsize.h"

#define LOAD(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define SWAP(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)

static int cas(int *p, int expected, int desired)
{
    __atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;                            /* (the value found) */
}

static int64_t futex_wait(void *word, unsigned value, uint64_t ns)
{
    return syscall6(SYS_FUTEX, (int64_t)word, FUTEX_WAIT, value, (int64_t)ns, 0, 0);
}

static void futex_wake(void *word, int n)
{
    syscall6(SYS_FUTEX, (int64_t)word, FUTEX_WAKE, n, 0, 0, 0);
}

/* The time left until `until` (TIME_UTC) in ns, at least 1; 0 if passed. */
static uint64_t left_until(const struct timespec *until)
{
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    int64_t ns = (int64_t)(until->tv_sec - now.tv_sec) * 1000000000 + (until->tv_nsec - now.tv_nsec);
    return ns > 0 ? (uint64_t)ns : 0;
}

/* --- thread-specific storage --- */

#define KEYS 64
static tss_dtor_t dtors[KEYS];
static int used[KEYS];
static _Thread_local void *values[KEYS];

int tss_create(tss_t *key, tss_dtor_t dtor)
{
    for (tss_t k = 0; k < KEYS; k++)
        if (cas(&used[k], 0, 1) == 0) {
            dtors[k] = dtor;
            values[k] = 0;
            *key = k;
            return thrd_success;
        }
    return thrd_error;
}

void *tss_get(tss_t key)
{
    return key < KEYS ? values[key] : 0;
}

int tss_set(tss_t key, void *value)
{
    if (key >= KEYS || !LOAD(&used[key]))
        return thrd_error;
    values[key] = value;
    return thrd_success;
}

void tss_delete(tss_t key)
{
    if (key < KEYS)
        STORE(&used[key], 0);                   /* (no destructors run, as C says) */
}

static void run_destructors(void)
{
    for (int round = 0; round < TSS_DTOR_ITERATIONS; round++) {
        int any = 0;
        for (tss_t k = 0; k < KEYS; k++)
            if (LOAD(&used[k]) && dtors[k] && values[k]) {
                void *v = values[k];
                values[k] = 0;
                dtors[k](v);
                any = 1;
            }
        if (!any)
            return;
    }
}

/* --- threads --- */

static int detached[PROCESS_THREADS], done[PROCESS_THREADS];

struct start {
    thrd_start_t func;
    void *arg;
};

static void finish(void)
{
    run_destructors();
    int self = thread_self();
    if (self > 0 && self < PROCESS_THREADS)
        STORE(&done[self], 1);
}

static int begin(void *data)
{
    struct start s = *(struct start *)data;
    free(data);
    int result = s.func(s.arg);
    finish();
    return result;
}

/* Join the detached threads that have ended: their numbers are free again. */
static void reap(void)
{
    for (int t = 1; t < PROCESS_THREADS; t++)
        if (LOAD(&detached[t]) && LOAD(&done[t])) {
            thread_join(t, 0);
            STORE(&detached[t], 0);
            STORE(&done[t], 0);
        }
}

int thrd_create(thrd_t *thr, thrd_start_t func, void *arg)
{
    return __ritteros_thrd_create_sized(thr, func, arg, 0);
}

int __ritteros_thrd_create_sized(thrd_t *thr, thrd_start_t func, void *arg, size_t stack_size)
{
    struct start *s = malloc(sizeof(*s));
    if (!s)
        return thrd_nomem;
    s->func = func;
    s->arg = arg;
    reap();
    int number = __ritteros_thread_create_sized(begin, s, stack_size);
    if (number < 0) {
        free(s);
        return errno == ENOMEM ? thrd_nomem : thrd_error;
    }
    *thr = number;
    return thrd_success;
}

int thrd_join(thrd_t thr, int *result)
{
    if (thread_join(thr, result) < 0)
        return thrd_error;
    if (thr > 0 && thr < PROCESS_THREADS)
        STORE(&done[thr], 0);
    return thrd_success;
}

int thrd_detach(thrd_t thr)
{
    if (thr <= 0 || thr >= PROCESS_THREADS)
        return thrd_error;
    STORE(&detached[thr], 1);
    reap();
    return thrd_success;
}

thrd_t thrd_current(void)
{
    return thread_self();
}

int thrd_equal(thrd_t a, thrd_t b)
{
    return a == b;
}

void thrd_exit(int result)
{
    finish();
    thread_exit(result);
}

int thrd_sleep(const struct timespec *duration, struct timespec *remaining)
{
    if (nanosleep(duration, remaining) == 0)
        return 0;
    return errno == EINTR ? -1 : -2;
}

void thrd_yield(void)
{
    syscall6(SYS_SLEEP, 0, 0, 0, 0, 0, 0);      /* (0 ms: the kernel gives the processor away) */
}

/* --- mutexes --- */

int mtx_init(mtx_t *m, int type)
{
    if (type & ~(mtx_recursive | mtx_timed))
        return thrd_error;
    m->state = 0;
    m->type = type;
    m->owner = 0;
    m->count = 0;
    return thrd_success;
}

/* Take m's word: 0 or, with a deadline, thrd_timedout. */
static int take(mtx_t *m, const struct timespec *until)
{
    int c = cas(&m->state, 0, 1);
    if (c == 0)
        return thrd_success;
    if (c != 2)
        c = SWAP(&m->state, 2);
    while (c != 0) {
        uint64_t ns = 0;
        if (until && !(ns = left_until(until)))
            return thrd_timedout;               /* (state 2 stays: one wake too many is harmless) */
        futex_wait(&m->state, 2, ns);
        c = SWAP(&m->state, 2);
    }
    return thrd_success;
}

static int lock(mtx_t *m, const struct timespec *until)
{
    int self = thread_self() + 1;
    if ((m->type & mtx_recursive) && LOAD(&m->owner) == self) {
        m->count++;
        return thrd_success;
    }
    int r = take(m, until);
    if (r != thrd_success)
        return r;
    STORE(&m->owner, self);
    m->count = 1;
    return thrd_success;
}

int mtx_lock(mtx_t *m)
{
    return lock(m, 0);
}

int mtx_timedlock(mtx_t *m, const struct timespec *until)
{
    return lock(m, until);
}

int mtx_trylock(mtx_t *m)
{
    int self = thread_self() + 1;
    if ((m->type & mtx_recursive) && LOAD(&m->owner) == self) {
        m->count++;
        return thrd_success;
    }
    if (cas(&m->state, 0, 1) != 0)
        return thrd_busy;
    STORE(&m->owner, self);
    m->count = 1;
    return thrd_success;
}

int mtx_unlock(mtx_t *m)
{
    if ((m->type & mtx_recursive) && --m->count > 0)
        return thrd_success;
    STORE(&m->owner, 0);
    if (__atomic_fetch_sub(&m->state, 1, __ATOMIC_SEQ_CST) != 1) {
        STORE(&m->state, 0);                    /* (it was 2: someone may wait) */
        futex_wake(&m->state, 1);
    }
    return thrd_success;
}

void mtx_destroy(mtx_t *m)
{
    (void)m;
}

/* --- condition variables --- */

int cnd_init(cnd_t *c)
{
    c->sequence = 0;
    return thrd_success;
}

int cnd_signal(cnd_t *c)
{
    __atomic_fetch_add(&c->sequence, 1, __ATOMIC_SEQ_CST);
    futex_wake(&c->sequence, 1);
    return thrd_success;
}

int cnd_broadcast(cnd_t *c)
{
    __atomic_fetch_add(&c->sequence, 1, __ATOMIC_SEQ_CST);
    futex_wake(&c->sequence, INT_MAX);
    return thrd_success;
}

static int cond_wait(cnd_t *c, mtx_t *m, const struct timespec *until)
{
    unsigned seen = LOAD(&c->sequence);
    unsigned held = m->count;                   /* (a recursive mutex: let go of it all) */
    m->count = 1;
    mtx_unlock(m);
    int64_t r = 0;
    uint64_t ns = 0;
    if (until && !(ns = left_until(until)))
        r = -ERR_TIMEDOUT;
    else
        r = futex_wait(&c->sequence, seen, ns);  /* (a signal since `seen`: back at once) */
    lock(m, 0);
    m->count = held ? held : 1;
    return r == -ERR_TIMEDOUT ? thrd_timedout : thrd_success;
}

int cnd_wait(cnd_t *c, mtx_t *m)
{
    return cond_wait(c, m, 0);
}

int cnd_timedwait(cnd_t *c, mtx_t *m, const struct timespec *until)
{
    return cond_wait(c, m, until);
}

void cnd_destroy(cnd_t *c)
{
    (void)c;
}

/* --- once --- */

void call_once(once_flag *flag, void (*func)(void))
{
    if (LOAD(&flag->state) == 2)
        return;
    if (cas(&flag->state, 0, 1) == 0) {
        func();
        STORE(&flag->state, 2);
        futex_wake(&flag->state, INT_MAX);
        return;
    }
    while (LOAD(&flag->state) == 1)
        futex_wait(&flag->state, 1, 0);
}

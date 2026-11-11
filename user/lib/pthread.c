/*
 * POSIX threads (54a) over threads.c's C11 threads: a thread's void *
 * result is kept here by its number; keys are a table of their own (with
 * POSIX's destructor rounds), run before threads.c's tss destructors.
 * Errors are returned, never put in errno.
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <thread.h>
#include <threads.h>
#include <time.h>
#include "cancel.h"
#include "ritteros.h"
#include "threadsize.h"
#include "../../common/vmlayout.h"   /* (54d: the stack sizes) */

#define LOAD(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)

static void *results[PROCESS_THREADS];

/* (54c) Cancellation, by thread number: a request, the state and type. */
static int cancel_pending[PROCESS_THREADS], cancel_off[PROCESS_THREADS], cancel_async[PROCESS_THREADS];
static _Thread_local int me = -1;               /* this thread's number (thread_self once) */
static _Thread_local struct __pthread_cleanup *cleanups;

static int self_number(void)
{
    if (me < 0)
        me = thread_self();
    return me >= 0 && me < PROCESS_THREADS ? me : 0;
}

/* --- keys --- */

static void (*destructors[PTHREAD_KEYS_MAX])(void *);
static int key_used[PTHREAD_KEYS_MAX];
static _Thread_local void *key_values[PTHREAD_KEYS_MAX];

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *value))
{
    for (pthread_key_t k = 0; k < PTHREAD_KEYS_MAX; k++) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&key_used[k], &expected, 1, false, __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST)) {
            destructors[k] = destructor;
            key_values[k] = 0;
            *key = k;
            return 0;
        }
    }
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key)
{
    if (key >= PTHREAD_KEYS_MAX || !LOAD(&key_used[key]))
        return EINVAL;
    STORE(&key_used[key], 0);                   /* (no destructors run, as POSIX says) */
    return 0;
}

void *pthread_getspecific(pthread_key_t key)
{
    return key < PTHREAD_KEYS_MAX ? key_values[key] : 0;
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
    if (key >= PTHREAD_KEYS_MAX || !LOAD(&key_used[key]))
        return EINVAL;
    key_values[key] = (void *)value;
    return 0;
}

static void run_key_destructors(void)
{
    for (int round = 0; round < PTHREAD_DESTRUCTOR_ITERATIONS; round++) {
        bool any = false;
        for (pthread_key_t k = 0; k < PTHREAD_KEYS_MAX; k++)
            if (LOAD(&key_used[k]) && destructors[k] && key_values[k]) {
                void *v = key_values[k];
                key_values[k] = 0;
                destructors[k](v);
                any = true;
            }
        if (!any)
            return;
    }
}

/* --- threads --- */

struct start {
    void *(*start)(void *);
    void *arg;
};

static int begin(void *data)
{
    struct start s = *(struct start *)data;
    free(data);
    int n = self_number();
    STORE(&cancel_pending[n], 0);               /* (a number used before: fresh) */
    cancel_off[n] = cancel_async[n] = 0;
    void *value = s.start(s.arg);
    int self = thread_self();
    if (self >= 0 && self < PROCESS_THREADS)
        results[self] = value;
    run_key_destructors();
    return 0;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
    struct start *s = malloc(sizeof(*s));
    if (!s)
        return EAGAIN;
    s->start = start;
    s->arg = arg;
    thrd_t t;
    int r = __ritteros_thrd_create_sized(&t, begin, s, attr ? attr->stack_size : 0);
    if (r != thrd_success) {
        free(s);
        return EAGAIN;                          /* (all 256 taken, or no memory) */
    }
    *thread = (pthread_t)t;
    if (attr && attr->detached == PTHREAD_CREATE_DETACHED)
        thrd_detach(t);
    return 0;
}

int pthread_join(pthread_t thread, void **value)
{
    if (thread == pthread_self())
        return EDEADLK;
    if (thread >= PROCESS_THREADS)
        return ESRCH;
    int ignored;
    __ritteros_cancel_point();                  /* (54c: a cancellation point) */
    while (thrd_join((thrd_t)thread, &ignored) != thrd_success) {
        if (errno != EINTR)
            return ESRCH;
        __ritteros_cancel_point();              /* (interrupted: by a cancel, or a signal) */
    }
    if (value)
        *value = results[thread];
    results[thread] = 0;
    return 0;
}

int pthread_detach(pthread_t thread)
{
    return thread < PROCESS_THREADS && thrd_detach((thrd_t)thread) == thrd_success ? 0 : ESRCH;
}

pthread_t pthread_self(void)
{
    return (pthread_t)thrd_current();
}

int pthread_equal(pthread_t a, pthread_t b)
{
    return a == b;
}

static void run_cleanups(void)
{
    while (cleanups) {
        struct __pthread_cleanup *c = cleanups;
        cleanups = c->next;
        c->fn(c->arg);
    }
}

void pthread_exit(void *value)
{
    run_cleanups();
    results[self_number()] = value;
    run_key_destructors();
    thrd_exit(0);
}

/* --- cancellation (54c) --- */

__attribute__((noreturn)) static void cancel_now(void)
{
    int n = self_number();
    cancel_off[n] = 1;                          /* (no second cancel in the handlers) */
    STORE(&cancel_pending[n], 0);
    pthread_exit(PTHREAD_CANCELED);
}

/* The library's cancellation points call it (cancel.h). */
void __ritteros_cancel_point(void)
{
    int n = self_number();
    if (LOAD(&cancel_pending[n]) && !cancel_off[n])
        cancel_now();
}

/* Where the kernel sends an asynchronously cancelled thread (it enters as
 * a signal handler would, on the thread's stack, all signals blocked). */
static void cancel_entry(int unused)
{
    (void)unused;
    cancel_now();
}

int pthread_cancel(pthread_t thread)
{
    if (thread >= PROCESS_THREADS)
        return ESRCH;
    STORE(&cancel_pending[thread], 1);
    int how = cancel_async[thread] && !cancel_off[thread] ? THREAD_INTERRUPT_ENTER
                                                          : THREAD_INTERRUPT_WAKE;
    int64_t r = syscall6(SYS_THREAD_INTERRUPT, (int64_t)thread, how, (int64_t)(uintptr_t)cancel_entry,
                         0, 0, 0);
    if (r == -ERR_SRCH) {
        STORE(&cancel_pending[thread], 0);
        return ESRCH;
    }
    return 0;
}

int pthread_setcancelstate(int state, int *old)
{
    if (state != PTHREAD_CANCEL_ENABLE && state != PTHREAD_CANCEL_DISABLE)
        return EINVAL;
    int n = self_number();
    if (old)
        *old = cancel_off[n] ? PTHREAD_CANCEL_DISABLE : PTHREAD_CANCEL_ENABLE;
    cancel_off[n] = state == PTHREAD_CANCEL_DISABLE;
    if (!cancel_off[n] && cancel_async[n] && LOAD(&cancel_pending[n]))
        cancel_now();                           /* (asynchronous: at once) */
    return 0;
}

int pthread_setcanceltype(int type, int *old)
{
    if (type != PTHREAD_CANCEL_DEFERRED && type != PTHREAD_CANCEL_ASYNCHRONOUS)
        return EINVAL;
    int n = self_number();
    if (old)
        *old = cancel_async[n] ? PTHREAD_CANCEL_ASYNCHRONOUS : PTHREAD_CANCEL_DEFERRED;
    cancel_async[n] = type == PTHREAD_CANCEL_ASYNCHRONOUS;
    if (cancel_async[n] && !cancel_off[n] && LOAD(&cancel_pending[n]))
        cancel_now();
    return 0;
}

void pthread_testcancel(void)
{
    __ritteros_cancel_point();
}

void __pthread_cleanup_push(struct __pthread_cleanup *c, void (*fn)(void *), void *arg)
{
    c->fn = fn;
    c->arg = arg;
    c->next = cleanups;
    cleanups = c;
}

void __pthread_cleanup_pop(struct __pthread_cleanup *c, int execute)
{
    cleanups = c->next;
    if (execute)
        c->fn(c->arg);
}

int pthread_attr_init(pthread_attr_t *attr)
{
    attr->detached = PTHREAD_CREATE_JOINABLE;
    attr->stack_size = USER_STACK_SIZE;         /* (54d: the default, 8 MiB) */
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr)
{
    (void)attr;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int state)
{
    if (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED)
        return EINVAL;
    attr->detached = state;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *state)
{
    *state = attr->detached;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size)
{
    if (size < PTHREAD_STACK_MIN || size > USER_STACK_MAX)
        return EINVAL;
    attr->stack_size = size;                    /* (54d: the thread's stack is that big) */
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *size)
{
    *size = attr->stack_size;
    return 0;
}

/* --- mutexes --- */

static int owner_is_me(pthread_mutex_t *m)
{
    return LOAD(&m->m.owner) == thread_self() + 1;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)
{
    int kind = attr ? attr->kind : PTHREAD_MUTEX_NORMAL;
    mtx_init(&m->m, mtx_timed | (kind == PTHREAD_MUTEX_RECURSIVE ? mtx_recursive : 0));
    m->kind = kind;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    return LOAD(&m->m.state) ? EBUSY : 0;
}

static int locked(pthread_mutex_t *m, const struct timespec *until)
{
    if (m->kind == PTHREAD_MUTEX_ERRORCHECK && owner_is_me(m))
        return EDEADLK;
    int r = until ? mtx_timedlock(&m->m, until) : mtx_lock(&m->m);
    return r == thrd_success ? 0 : r == thrd_timedout ? ETIMEDOUT : EINVAL;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    return locked(m, 0);
}

int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *until)
{
    if (until->tv_nsec < 0 || until->tv_nsec >= 1000000000)
        return EINVAL;
    return locked(m, until);
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
    if (m->kind == PTHREAD_MUTEX_ERRORCHECK && owner_is_me(m))
        return EBUSY;
    return mtx_trylock(&m->m) == thrd_success ? 0 : EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if (m->kind != PTHREAD_MUTEX_NORMAL && !owner_is_me(m))
        return EPERM;                           /* (errorcheck, recursive: not its holder) */
    mtx_unlock(&m->m);
    return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr)
{
    attr->kind = PTHREAD_MUTEX_DEFAULT;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr)
{
    (void)attr;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int kind)
{
    if (kind != PTHREAD_MUTEX_NORMAL && kind != PTHREAD_MUTEX_RECURSIVE &&
        kind != PTHREAD_MUTEX_ERRORCHECK)
        return EINVAL;
    attr->kind = kind;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *kind)
{
    *kind = attr->kind;
    return 0;
}

int pthread_mutexattr_setpshared(pthread_mutexattr_t *attr, int shared)
{
    (void)attr;
    return shared == PTHREAD_PROCESS_PRIVATE ? 0 : shared == PTHREAD_PROCESS_SHARED ? ENOTSUP : EINVAL;
}

/* --- condition variables --- */

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr)
{
    cnd_init(&c->c);
    c->clock = attr ? attr->clock : CLOCK_REALTIME;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c)
{
    (void)c;
    return 0;
}

int pthread_cond_signal(pthread_cond_t *c)
{
    cnd_signal(&c->c);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c)
{
    cnd_broadcast(&c->c);
    return 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    if (m->kind != PTHREAD_MUTEX_NORMAL && !owner_is_me(m))
        return EPERM;
    __ritteros_cancel_point();
    cnd_wait(&c->c, &m->m);
    __ritteros_cancel_point();                  /* (54c: with m locked again, as POSIX says) */
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *until)
{
    if (until->tv_nsec < 0 || until->tv_nsec >= 1000000000)
        return EINVAL;
    if (m->kind != PTHREAD_MUTEX_NORMAL && !owner_is_me(m))
        return EPERM;
    struct timespec real = *until;
    if (c->clock == CLOCK_MONOTONIC) {          /* (cnd_timedwait counts in TIME_UTC) */
        struct timespec mono, now;
        clock_gettime(CLOCK_MONOTONIC, &mono);
        clock_gettime(CLOCK_REALTIME, &now);
        long long ns = (long long)(until->tv_sec - mono.tv_sec) * 1000000000 +
                       (until->tv_nsec - mono.tv_nsec) + now.tv_nsec;
        real.tv_sec = now.tv_sec + (time_t)(ns / 1000000000);
        real.tv_nsec = (long)(ns % 1000000000);
        if (real.tv_nsec < 0)
            real.tv_nsec += 1000000000, real.tv_sec--;
    }
    __ritteros_cancel_point();
    int r = cnd_timedwait(&c->c, &m->m, &real) == thrd_timedout ? ETIMEDOUT : 0;
    __ritteros_cancel_point();
    return r;
}

int pthread_condattr_init(pthread_condattr_t *attr)
{
    attr->clock = CLOCK_REALTIME;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attr)
{
    (void)attr;
    return 0;
}

int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock)
{
    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC)
        return EINVAL;
    attr->clock = clock;
    return 0;
}

int pthread_condattr_getclock(const pthread_condattr_t *attr, clockid_t *clock)
{
    *clock = attr->clock;
    return 0;
}

/* --- once, signals, yield --- */

_Static_assert(sizeof(pthread_once_t) == sizeof(once_flag), "call_once's flag");

int pthread_once(pthread_once_t *once, void (*init)(void))
{
    call_once((once_flag *)once, init);
    return 0;
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *old)
{
    return sigprocmask(how, set, old) < 0 ? errno : 0;
}

int sched_yield(void)
{
    thrd_yield();
    return 0;
}

/* --- read-write locks (54b): a mutex and two condition variables --- */

int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *attr)
{
    if (attr && attr->shared)
        return ENOTSUP;
    pthread_mutex_init(&l->m, 0);
    pthread_cond_init(&l->readers, 0);
    pthread_cond_init(&l->writers, 0);
    l->reading = l->writer = l->waiting_writers = 0;
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *l)
{
    return l->reading || l->writer ? EBUSY : 0;
}

static int read_lock(pthread_rwlock_t *l, bool try, const struct timespec *until)
{
    int self = thread_self() + 1, r = 0;
    pthread_mutex_lock(&l->m);
    if (l->writer == self)
        r = EDEADLK;
    while (!r && (l->writer || l->waiting_writers)) {
        if (try)
            r = EBUSY;
        else if (until)
            r = pthread_cond_timedwait(&l->readers, &l->m, until);
        else
            pthread_cond_wait(&l->readers, &l->m);
    }
    if (!r)
        l->reading++;
    pthread_mutex_unlock(&l->m);
    return r;
}

static int write_lock(pthread_rwlock_t *l, bool try, const struct timespec *until)
{
    int self = thread_self() + 1, r = 0;
    pthread_mutex_lock(&l->m);
    if (l->writer == self)
        r = EDEADLK;
    l->waiting_writers++;
    while (!r && (l->writer || l->reading)) {
        if (try)
            r = EBUSY;
        else if (until)
            r = pthread_cond_timedwait(&l->writers, &l->m, until);
        else
            pthread_cond_wait(&l->writers, &l->m);
    }
    l->waiting_writers--;
    if (!r)
        l->writer = self;
    else if (!l->waiting_writers && !l->writer)
        pthread_cond_broadcast(&l->readers);    /* (readers held back for it go on) */
    pthread_mutex_unlock(&l->m);
    return r;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *l)
{
    return read_lock(l, false, 0);
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *l)
{
    return read_lock(l, true, 0);
}

int pthread_rwlock_timedrdlock(pthread_rwlock_t *l, const struct timespec *until)
{
    return read_lock(l, false, until);
}

int pthread_rwlock_wrlock(pthread_rwlock_t *l)
{
    return write_lock(l, false, 0);
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *l)
{
    return write_lock(l, true, 0);
}

int pthread_rwlock_timedwrlock(pthread_rwlock_t *l, const struct timespec *until)
{
    return write_lock(l, false, until);
}

int pthread_rwlock_unlock(pthread_rwlock_t *l)
{
    int self = thread_self() + 1, r = 0;
    pthread_mutex_lock(&l->m);
    if (l->writer == self)
        l->writer = 0;
    else if (l->reading > 0)
        l->reading--;
    else
        r = EPERM;
    if (!r && !l->writer && !l->reading) {
        if (l->waiting_writers)
            pthread_cond_signal(&l->writers);
        else
            pthread_cond_broadcast(&l->readers);
    }
    pthread_mutex_unlock(&l->m);
    return r;
}

int pthread_rwlockattr_init(pthread_rwlockattr_t *attr)
{
    attr->shared = 0;
    return 0;
}

int pthread_rwlockattr_destroy(pthread_rwlockattr_t *attr)
{
    (void)attr;
    return 0;
}

/* --- barriers (54b) --- */

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *attr, unsigned count)
{
    if (count == 0)
        return EINVAL;
    if (attr && attr->shared)
        return ENOTSUP;
    pthread_mutex_init(&b->m, 0);
    pthread_cond_init(&b->c, 0);
    b->count = count;
    b->arrived = 0;
    b->round = 0;
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *b)
{
    return b->arrived ? EBUSY : 0;
}

int pthread_barrier_wait(pthread_barrier_t *b)
{
    pthread_mutex_lock(&b->m);
    unsigned round = b->round;
    if (++b->arrived == b->count) {
        b->arrived = 0;
        b->round++;
        pthread_cond_broadcast(&b->c);
        pthread_mutex_unlock(&b->m);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    while (round == b->round)
        pthread_cond_wait(&b->c, &b->m);
    pthread_mutex_unlock(&b->m);
    return 0;
}

int pthread_barrierattr_init(pthread_barrierattr_t *attr)
{
    attr->shared = 0;
    return 0;
}

int pthread_barrierattr_destroy(pthread_barrierattr_t *attr)
{
    (void)attr;
    return 0;
}

/* --- spin locks (54b) --- */

int pthread_spin_init(pthread_spinlock_t *s, int shared)
{
    if (shared == PTHREAD_PROCESS_SHARED)
        return ENOTSUP;
    *s = 0;
    return 0;
}

int pthread_spin_destroy(pthread_spinlock_t *s)
{
    return LOAD(s) ? EBUSY : 0;
}

int pthread_spin_lock(pthread_spinlock_t *s)
{
    while (__atomic_exchange_n(s, 1, __ATOMIC_ACQUIRE))
        thrd_yield();                           /* (the holder may not be running: let it) */
    return 0;
}

int pthread_spin_trylock(pthread_spinlock_t *s)
{
    return __atomic_exchange_n(s, 1, __ATOMIC_ACQUIRE) ? EBUSY : 0;
}

int pthread_spin_unlock(pthread_spinlock_t *s)
{
    __atomic_store_n(s, 0, __ATOMIC_RELEASE);
    return 0;
}

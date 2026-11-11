/*
 * POSIX threads (54a), over threads.h's C11 threads and the kernel's
 * futex: a waiting thread uses no processor time. Up to 256 threads a
 * program, the main one included (EAGAIN beyond); each has a stack of
 * 8 MiB, or the size its attributes ask (PTHREAD_STACK_MIN to 63 MiB),
 * whose pages are made only when the thread first touches them (54d). Mutexes, condition variables and
 * once controls are a program's own (PTHREAD_PROCESS_PRIVATE). Calls
 * return 0 or an error number (they do not set errno).
 */
#ifndef RITTEROS_PTHREAD_H
#define RITTEROS_PTHREAD_H

#include <signal.h>
#include <stddef.h>
#include <threads.h>
#include <time.h>

typedef unsigned long pthread_t;        /* the thread's number (thread.h) */
typedef unsigned int pthread_key_t;

typedef struct {
    int detached;                       /* PTHREAD_CREATE_* */
    size_t stack_size;                  /* the thread's stack (54d) */
} pthread_attr_t;

typedef struct {
    mtx_t m;
    int kind;                           /* PTHREAD_MUTEX_* */
} pthread_mutex_t;

typedef struct {
    int kind;
} pthread_mutexattr_t;

typedef struct {
    cnd_t c;
    int clock;                          /* the clock of timedwait's time */
} pthread_cond_t;

typedef struct {
    int clock;
} pthread_condattr_t;

typedef struct {
    int state;                          /* 0 not yet, 1 running, 2 done */
} pthread_once_t;

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL

#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED  1       /* (not offered: ENOTSUP) */

#define PTHREAD_MUTEX_INITIALIZER { { 0, 0, 0, 0 }, PTHREAD_MUTEX_NORMAL }
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP { { 0, mtx_recursive, 0, 0 }, PTHREAD_MUTEX_RECURSIVE }
#define PTHREAD_COND_INITIALIZER  { { 0 }, CLOCK_REALTIME }
#define PTHREAD_ONCE_INIT         { 0 }

#define PTHREAD_KEYS_MAX              64
#define PTHREAD_DESTRUCTOR_ITERATIONS 4
#define PTHREAD_THREADS_MAX           256
#define PTHREAD_STACK_MIN             65536

/* Threads: start(arg) in a new thread; its result for pthread_join (a
 * detached one is nobody's to join). pthread_exit ends the calling thread
 * with value (keys' destructors run first, as when start returns); the
 * program ends when its last thread has. */
int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg);
int pthread_join(pthread_t thread, void **value);
int pthread_detach(pthread_t thread);
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
__attribute__((noreturn)) void pthread_exit(void *value);

int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_destroy(pthread_attr_t *attr);
int pthread_attr_setdetachstate(pthread_attr_t *attr, int state);
int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *state);
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size);
int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *size);

/* Mutexes. NORMAL: locking it again deadlocks; RECURSIVE: its holder may
 * lock it again (free once unlocked as often); ERRORCHECK: locking it
 * again is EDEADLK, unlocking another's EPERM. trylock: EBUSY;
 * timedlock: ETIMEDOUT past an absolute CLOCK_REALTIME time. */
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *until);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_mutexattr_init(pthread_mutexattr_t *attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int kind);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *kind);
int pthread_mutexattr_setpshared(pthread_mutexattr_t *attr, int shared);

/* Condition variables: wait unlocks m, waits for a signal and locks m
 * again (it may return without one: check in a loop); timedwait returns
 * ETIMEDOUT past an absolute time of the condition's clock
 * (CLOCK_REALTIME, or CLOCK_MONOTONIC by pthread_condattr_setclock). */
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *until);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_condattr_init(pthread_condattr_t *attr);
int pthread_condattr_destroy(pthread_condattr_t *attr);
int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock);
int pthread_condattr_getclock(const pthread_condattr_t *attr, clockid_t *clock);

/* init runs once for the control, whichever threads call. */
int pthread_once(pthread_once_t *once, void (*init)(void));

/* Keys: a value per thread (0 at first); a thread's non-zero values go to
 * the key's destructor when it ends (up to 4 rounds). */
int pthread_key_create(pthread_key_t *key, void (*destructor)(void *value));
int pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);

/* (56a) Handlers fork runs: prepare before it (the last registered first),
 * parent and child after it, in their process (in the order registered);
 * any may be 0. At most 32 sets (ENOMEM). */
int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void));

/* The calling thread's blocked signals (sigprocmask's, as an error number). */
int pthread_sigmask(int how, const sigset_t *set, sigset_t *old);

/* (54c) Cancellation: pthread_cancel asks a thread to end. A deferred one
 * (the default) ends at its next cancellation point -- read, write, open,
 * close, poll, select, sleep, usleep, nanosleep, wait, waitpid,
 * sigsuspend, accept, connect, send, recv and the like, pthread_join,
 * pthread_cond_wait and _timedwait, pthread_testcancel -- also when it is
 * waiting in one; an asynchronous one (PTHREAD_CANCEL_ASYNCHRONOUS) at
 * once, wherever it is (only for code that holds nothing, as POSIX says).
 * It runs the thread's cleanup handlers (last pushed first), then its
 * keys' destructors; pthread_join then gives PTHREAD_CANCELED. A thread
 * with cancellation disabled (pthread_setcancelstate) keeps the request
 * until it enables it. */
#define PTHREAD_CANCELED        ((void *)-1)
#define PTHREAD_CANCEL_ENABLE   0
#define PTHREAD_CANCEL_DISABLE  1
#define PTHREAD_CANCEL_DEFERRED     0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1

int pthread_cancel(pthread_t thread);
int pthread_setcancelstate(int state, int *old);
int pthread_setcanceltype(int type, int *old);
void pthread_testcancel(void);

/* Cleanup handlers: push and pop pair up in one block (they are macros);
 * pop(1) runs the handler, pop(0) only drops it; a cancel or pthread_exit
 * runs those still pushed. */
struct __pthread_cleanup {
    void (*fn)(void *);
    void *arg;
    struct __pthread_cleanup *next;
};
void __pthread_cleanup_push(struct __pthread_cleanup *c, void (*fn)(void *), void *arg);
void __pthread_cleanup_pop(struct __pthread_cleanup *c, int execute);
#define pthread_cleanup_push(fn, arg) \
    { struct __pthread_cleanup cleanup_; __pthread_cleanup_push(&cleanup_, (fn), (arg));
#define pthread_cleanup_pop(execute) \
    __pthread_cleanup_pop(&cleanup_, (execute)); }

/* (54b) Read-write locks: many readers or one writer; a waiting writer
 * goes before new readers (no writer waits for ever). A writer locking it
 * again, or a reader taking it for writing, is EDEADLK; try: EBUSY;
 * timed: ETIMEDOUT past an absolute CLOCK_REALTIME time. */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t readers, writers;
    int reading;                        /* readers holding it */
    int writer;                         /* the writing thread's number + 1, or 0 */
    int waiting_writers;
} pthread_rwlock_t;

typedef struct {
    int shared;
} pthread_rwlockattr_t;

#define PTHREAD_RWLOCK_INITIALIZER \
    { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0 }

int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *attr);
int pthread_rwlock_destroy(pthread_rwlock_t *l);
int pthread_rwlock_rdlock(pthread_rwlock_t *l);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *l);
int pthread_rwlock_timedrdlock(pthread_rwlock_t *l, const struct timespec *until);
int pthread_rwlock_wrlock(pthread_rwlock_t *l);
int pthread_rwlock_trywrlock(pthread_rwlock_t *l);
int pthread_rwlock_timedwrlock(pthread_rwlock_t *l, const struct timespec *until);
int pthread_rwlock_unlock(pthread_rwlock_t *l);
int pthread_rwlockattr_init(pthread_rwlockattr_t *attr);
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *attr);

/* (54b) Barriers: count threads wait in pthread_barrier_wait until the
 * last arrives; one of them gets PTHREAD_BARRIER_SERIAL_THREAD, the others
 * 0, and the barrier is ready for the next round. */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    unsigned count, arrived, round;
} pthread_barrier_t;

typedef struct {
    int shared;
} pthread_barrierattr_t;

#define PTHREAD_BARRIER_SERIAL_THREAD (-1)

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *attr, unsigned count);
int pthread_barrier_destroy(pthread_barrier_t *b);
int pthread_barrier_wait(pthread_barrier_t *b);
int pthread_barrierattr_init(pthread_barrierattr_t *attr);
int pthread_barrierattr_destroy(pthread_barrierattr_t *attr);

/* (54b) Spin locks: for very short holds. One processor: a waiter gives
 * the processor away instead of spinning. trylock: EBUSY. */
typedef int pthread_spinlock_t;

int pthread_spin_init(pthread_spinlock_t *s, int shared);
int pthread_spin_destroy(pthread_spinlock_t *s);
int pthread_spin_lock(pthread_spinlock_t *s);
int pthread_spin_trylock(pthread_spinlock_t *s);
int pthread_spin_unlock(pthread_spinlock_t *s);

#endif

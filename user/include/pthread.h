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

#ifdef __cplusplus
extern "C" {
#endif

#include <signal.h>
#include <stddef.h>
#include <threads.h>
#include <time.h>

typedef unsigned long pthread_t;        /* the thread's number (thread.h) */
typedef unsigned int pthread_key_t;

/* (43a-4) The scheduling and stack attributes too. Programs built before
 * had only detached and stack_size, which keep their places, so the calls
 * that touch only them serve both; pthread_attr_init and pthread_create
 * are new symbols (the __asm__ names below), the old ones kept for those
 * programs. */
typedef struct {
    int detached;                       /* PTHREAD_CREATE_* */
    int inherit;                        /* PTHREAD_INHERIT_SCHED or _EXPLICIT_SCHED */
    size_t stack_size;                  /* the thread's stack (54d) */
    void *stack_addr;                   /* pthread_attr_setstack's (the size is used) */
    size_t guard_size;
    int policy, priority, scope;        /* SCHED_OTHER, 0, PTHREAD_SCOPE_SYSTEM */
    int reserved;
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
int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
    __asm__("__ritteros_pthread_create2");
int pthread_join(pthread_t thread, void **value);
int pthread_detach(pthread_t thread);
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
__attribute__((noreturn)) void pthread_exit(void *value);

int pthread_attr_init(pthread_attr_t *attr) __asm__("__ritteros_pthread_attr_init2");
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
/* (43b-5) A signal to one thread of the program: its handler runs in that
 * thread (si_code SI_TKILL); without one, its default is the program's
 * (it ends). 0, ESRCH, EINVAL; signal 0 only asks. */
int pthread_kill(pthread_t thread, int signal);

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

/* (43a-4) The rest of POSIX's thread attributes and GNU's _np calls:
 * - pthread_attr_setstack/getstack: a stack's address and size (the
 *   size is used; the kernel places each thread's stack itself, so the
 *   address is only remembered); guard size, scope (PTHREAD_SCOPE_SYSTEM
 *   only), scheduling (SCHED_OTHER with priority 0 only: see sched.h);
 * - pthread_getattr_np: a running thread's attributes, its stack's place
 *   and size (from its first frame: the stack a garbage collector scans);
 *   pthread_setattr_default_np/getattr_default_np;
 * - pthread_getschedparam/setschedparam/setschedprio, the affinity
 *   (every core), pthread_getcpuclockid ((43b-6) the calling thread's:
 *   CLOCK_THREAD_CPUTIME_ID; another's ENOENT),
 *   the concurrency level (remembered);
 * - the mutex attributes' protocol (PTHREAD_PRIO_NONE), priority ceiling
 *   (0) and robustness (PTHREAD_MUTEX_STALLED), and the condition
 *   variables', read-write locks' and barriers' pshared (PRIVATE only):
 *   other values are ENOTSUP. */
#include <sched.h>
#define PTHREAD_INHERIT_SCHED   0
#define PTHREAD_EXPLICIT_SCHED  1
#define PTHREAD_SCOPE_SYSTEM    0
#define PTHREAD_SCOPE_PROCESS   1
#define PTHREAD_PRIO_NONE       0
#define PTHREAD_PRIO_INHERIT    1
#define PTHREAD_PRIO_PROTECT    2
#define PTHREAD_MUTEX_STALLED   0
#define PTHREAD_MUTEX_ROBUST    1
int pthread_attr_setstack(pthread_attr_t *attr, void *addr, size_t size);
int pthread_attr_getstack(const pthread_attr_t *attr, void **addr, size_t *size);
int pthread_attr_setguardsize(pthread_attr_t *attr, size_t size);
int pthread_attr_getguardsize(const pthread_attr_t *attr, size_t *size);
int pthread_attr_setscope(pthread_attr_t *attr, int scope);
int pthread_attr_getscope(const pthread_attr_t *attr, int *scope);
int pthread_attr_setinheritsched(pthread_attr_t *attr, int inherit);
int pthread_attr_getinheritsched(const pthread_attr_t *attr, int *inherit);
int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy);
int pthread_attr_getschedpolicy(const pthread_attr_t *attr, int *policy);
int pthread_attr_setschedparam(pthread_attr_t *attr, const struct sched_param *param);
int pthread_attr_getschedparam(const pthread_attr_t *attr, struct sched_param *param);
int pthread_getattr_np(pthread_t thread, pthread_attr_t *attr);
int pthread_setattr_default_np(const pthread_attr_t *attr);
int pthread_getattr_default_np(pthread_attr_t *attr);
int pthread_getschedparam(pthread_t thread, int *policy, struct sched_param *param);
int pthread_setschedparam(pthread_t thread, int policy, const struct sched_param *param);
int pthread_setschedprio(pthread_t thread, int priority);
int pthread_getaffinity_np(pthread_t thread, size_t size, cpu_set_t *set);
int pthread_setaffinity_np(pthread_t thread, size_t size, const cpu_set_t *set);
int pthread_getcpuclockid(pthread_t thread, clockid_t *clock);
/* (43b-6) A thread's name, as `threads` lists it (the program's name at
 * first): at most 15 bytes (ERANGE), and read into size bytes (ERANGE if
 * they cannot hold it). */
int pthread_setname_np(pthread_t thread, const char *name);
int pthread_getname_np(pthread_t thread, char *name, size_t size);
int pthread_getconcurrency(void);
int pthread_setconcurrency(int level);
int pthread_mutexattr_setprotocol(pthread_mutexattr_t *attr, int protocol);
int pthread_mutexattr_getprotocol(const pthread_mutexattr_t *attr, int *protocol);
int pthread_mutexattr_setprioceiling(pthread_mutexattr_t *attr, int ceiling);
int pthread_mutexattr_getprioceiling(const pthread_mutexattr_t *attr, int *ceiling);
int pthread_mutexattr_setrobust(pthread_mutexattr_t *attr, int robust);
int pthread_mutexattr_getrobust(const pthread_mutexattr_t *attr, int *robust);
int pthread_mutexattr_getpshared(const pthread_mutexattr_t *attr, int *shared);
int pthread_condattr_setpshared(pthread_condattr_t *attr, int shared);
int pthread_condattr_getpshared(const pthread_condattr_t *attr, int *shared);
int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *attr, int shared);
int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *attr, int *shared);

#ifdef __cplusplus
}
#endif

#endif

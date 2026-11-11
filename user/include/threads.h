/*
 * threads.h (step 22y): C11's threads, mutexes, condition variables,
 * call_once and thread-specific storage. Threads are thread.h's (up to 256
 * per program, the main one included; thrd_t is the thread's number);
 * waiting goes through the kernel's futex (SYS_FUTEX), so a blocked thread
 * uses no processor time. Timed calls take an absolute TIME_UTC time
 * (timespec_get), measured in the kernel's 10 ms ticks. Mutexes, condition
 * variables and once flags are a program's own (not shared between
 * programs). thread_local is C's _Thread_local (per-thread data, 22i).
 */
#ifndef RITTEROS_THREADS_H
#define RITTEROS_THREADS_H

#include <time.h>

#define thread_local _Thread_local
#define ONCE_FLAG_INIT { 0 }
#define TSS_DTOR_ITERATIONS 4

typedef int thrd_t;
typedef int (*thrd_start_t)(void *arg);

typedef struct {
    int state;                          /* 0 free, 1 taken, 2 taken with waiters */
    int type;                           /* mtx_plain, mtx_timed, | mtx_recursive */
    int owner;                          /* recursive: the holder's number + 1 */
    unsigned int count;                 /* recursive: times it is held */
} mtx_t;

typedef struct {
    unsigned int sequence;              /* counts signals; waiters sleep on it */
} cnd_t;

typedef struct {
    int state;                          /* 0 not yet, 1 running, 2 done */
} once_flag;

typedef unsigned int tss_t;
typedef void (*tss_dtor_t)(void *value);

enum { thrd_success = 0, thrd_busy = 1, thrd_error = 2, thrd_nomem = 3, thrd_timedout = 4 };
enum { mtx_plain = 0, mtx_recursive = 1, mtx_timed = 2 };

/* Threads. thrd_create: func(arg) in a new thread (thrd_error when all 16
 * are taken: a detached thread's number is free again once it ended).
 * thrd_join: wait for it, its result into *result. thrd_detach: nobody
 * will join it. thrd_exit: end the calling thread (thread-specific values'
 * destructors run first, also when func returns). thrd_sleep: 0, -1 if a
 * signal ended it early (the rest into *remaining). */
int thrd_create(thrd_t *thr, thrd_start_t func, void *arg);
int thrd_join(thrd_t thr, int *result);
int thrd_detach(thrd_t thr);
thrd_t thrd_current(void);
int thrd_equal(thrd_t a, thrd_t b);
__attribute__((noreturn)) void thrd_exit(int result);
int thrd_sleep(const struct timespec *duration, struct timespec *remaining);
void thrd_yield(void);

/* Mutexes: a recursive one may be locked again by its holder (and is free
 * once unlocked as often). */
int mtx_init(mtx_t *m, int type);
int mtx_lock(mtx_t *m);
int mtx_timedlock(mtx_t *m, const struct timespec *until);
int mtx_trylock(mtx_t *m);
int mtx_unlock(mtx_t *m);
void mtx_destroy(mtx_t *m);

/* Condition variables: cnd_wait unlocks m, waits for a signal and locks m
 * again (it may also return without one: check the condition in a loop). */
int cnd_init(cnd_t *c);
int cnd_signal(cnd_t *c);
int cnd_broadcast(cnd_t *c);
int cnd_wait(cnd_t *c, mtx_t *m);
int cnd_timedwait(cnd_t *c, mtx_t *m, const struct timespec *until);
void cnd_destroy(cnd_t *c);

/* func runs once for flag, whichever threads call; the others wait for it. */
void call_once(once_flag *flag, void (*func)(void));

/* Thread-specific storage: a value per thread for each key (64 keys); a
 * destructor runs on a thread's non-null value when the thread ends. */
int tss_create(tss_t *key, tss_dtor_t dtor);
void *tss_get(tss_t key);
int tss_set(tss_t key, void *value);
void tss_delete(tss_t key);

#endif

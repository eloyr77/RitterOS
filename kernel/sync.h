/*
 * Locks for a preemptible kernel on one CPU.
 *
 * Spinlock: for short critical sections, also those shared with interrupt
 * handlers. It disables interrupts while held, so neither an interrupt
 * handler nor another thread runs on this core (thread switches happen only
 * at the end of an interrupt), and (65a-2) it is a ticket lock, so other
 * cores wait their turn in the order they came. Taking one this core holds
 * already is a deadlock and panics.
 *
 * Mutex: for long sections (console output, the file system, a disk
 * command). A thread that finds it taken sleeps until it is released, so
 * other threads keep running. The owner may lock it again (recursive), which
 * file system callbacks need. Never in interrupt handlers.
 *
 * (65e-36) Readers-writer lock: a sleeping lock that many threads may hold
 * to read at once, or one to write. A writer waiting keeps new readers out
 * (writers are never starved), except a thread that reads it already: its
 * nested read goes on (it would wait for itself). The writer may read and
 * write again (deeper); a reader asking to write panics (it would wait for
 * itself). Each thread keeps the ones it reads (struct thread's reading[]).
 * (65e-45) A waiter goes round a while before it sleeps, as on a mutex.
 * Never in interrupt handlers.
 *
 * (65a-5) Each core keeps the spinlocks it holds (struct cpu's `held`), so
 * a sleep or a mutex taken while holding one panics with its name (the
 * holder would keep it through the sleep, and every other taker would spin
 * for good). With "lockcheck" on the kernel command line, the order locks
 * are taken in is checked too: taking B while holding A records "A before
 * B" for their names, and an order that closes a circle (B before A, or
 * through others) panics before it can deadlock two cores.
 */
#ifndef RITTEROS_SYNC_H
#define RITTEROS_SYNC_H

#include <stdbool.h>
#include <stdint.h>
#include "arch.h"
#include "driver.h"                 /* (67b) struct spinlock, struct mutex: the drivers' too */
#include "percpu.h"
#include "sched.h"

__attribute__((noreturn)) void spinlock_deadlock(struct spinlock *lock);

extern bool lock_checking;                  /* "lockcheck" on the command line (65a-5) */
void lockcheck_order(struct cpu *c, struct spinlock *lock);
void lock_given_back(struct cpu *c, struct spinlock *lock);
__attribute__((noreturn)) void lock_held_while(const char *what);

/* (65e) A lock was found taken: count the wait, by the lock's name (a
 * mutex: slept), for the report of the most-waited-for locks (`cpus`).
 * Called only when a taker had to wait, so a free lock costs nothing. */
void lock_waited(const char *name, uint64_t counts, bool slept);
/* (65e-36) A wait of a kind: WAIT_READ and WAIT_WRITE are a readers-writer
 * lock's, listed as (read) and (write). */
enum { WAIT_SPUN, WAIT_SLEPT, WAIT_READ, WAIT_WRITE };
void lock_waited_as(const char *name, uint64_t counts, int kind);
void lock_wait_report(void);
void lockcheck_report(void);                 /* a line for `threads`, with lockcheck */

/* Before anything that may sleep: panics if this core holds more spinlocks
 * than `allowed` (the scheduler's own, inside the scheduler). */
static inline void locks_none_held(int allowed, const char *what)
{
    uint64_t state = arch_irq_save();       /* (65c-3: a torn read could see another core's) */
    int held = arch_this_cpu()->locks_held;
    arch_irq_restore(state);
    if (held > allowed)
        lock_held_while(what);
}

static inline uint64_t spin_lock(struct spinlock *lock)
{
    uint64_t state = arch_irq_save();
    struct cpu *c = arch_this_cpu();
    int me = c->index + 1;
    if (lock->holder == me)
        spinlock_deadlock(lock);                /* this core holds it already */
    if (lock_checking)
        lockcheck_order(c, lock);               /* before waiting: a circle panics, not hangs */
    uint32_t ticket = __atomic_fetch_add(&lock->next, 1, __ATOMIC_RELAXED);
    if (__atomic_load_n(&lock->serving, __ATOMIC_ACQUIRE) != ticket) {
        uint64_t began = arch_counter();        /* (65e) taken: how long it waits */
        while (__atomic_load_n(&lock->serving, __ATOMIC_ACQUIRE) != ticket)
            arch_cpu_relax();
        lock_waited(lock->name, arch_counter() - began, false);
    }
    lock->holder = me;
    if (c->locks_held < CPU_LOCKS_KEPT)
        c->held[c->locks_held] = lock;
    c->locks_held++;
    return state;
}

/* Give the lock back, leaving interrupts as they are (off): for code that
 * took it in another place (the scheduler's, handed over a switch). */
static inline void spin_unlock_only(struct spinlock *lock)
{
    struct cpu *c = arch_this_cpu();
    int n = c->locks_held;
    if (n > 0 && n <= CPU_LOCKS_KEPT && c->held[n - 1] == lock)
        c->locks_held = n - 1;                  /* the last taken: the usual case */
    else
        lock_given_back(c, lock);
    lock->holder = 0;
    __atomic_store_n(&lock->serving, lock->serving + 1, __ATOMIC_RELEASE);
}

static inline void spin_unlock(struct spinlock *lock, uint64_t state)
{
    spin_unlock_only(lock);
    arch_irq_restore(state);
}

void mutex_lock(struct mutex *m);
bool mutex_try_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);

/* (65e-45) The lock's state is one word, changed only by atomic operations:
 * coming in and leaving are one each while nobody waits (until 65e-45 a
 * spinlock guarded it: 8 threads faulting pages on the laptop spun 18 s of
 * 80 on it, taking it twice per fault). */
#define RW_READERS 0xFFFFFFFFull                /* read holds of every thread (nested ones too) */
#define RW_WRITER (1ull << 32)                  /* held to write */
#define RW_WAITING_ONE (1ull << 33)             /* writers waiting, counted from here */
#define RW_WAITING (0x3FFFFFFFull << 33)
#define RW_SLEEPERS (1ull << 63)                /* someone may sleep on waiters */

struct rwlock {
    uint64_t state;
    struct thread *writer;          /* holding it to write, write_depth times */
    unsigned write_depth;
    struct wait_queue waiters;
    const char *name;
};

#define RWLOCK_INIT(n) { 0, 0, 0, { 0 }, (n), }

/* Hold to read: this thread's read depth after it (1: its outermost), or 0
 * when it is the writer (then it holds deeper as the writer). */
int rwlock_read(struct rwlock *l);
/* Hold to write. */
void rwlock_write(struct rwlock *l);
/* Give one hold back, whichever it was. */
void rwlock_unlock(struct rwlock *l);
/* (65e-48) The writer's only hold becomes a read hold, never free between:
 * what it changed stays as it left it, and readers come in. */
void rwlock_downgrade(struct rwlock *l);

#endif

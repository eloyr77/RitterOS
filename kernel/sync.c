#include "sync.h"
#include "arch.h"
#include "cpus.h"
#include "sched.h"
#include "kprintf.h"
#include "panic.h"
#include "string.h"

void spinlock_deadlock(struct spinlock *lock)
{
    lock->holder = 0;                       /* given back, so that the report itself can run */
    __atomic_store_n(&lock->serving, lock->serving + 1, __ATOMIC_RELEASE);
    panic("spinlock '%s' taken twice (deadlock)", lock->name);
}

/* --- what each core holds (65a-5) ----------------------------------------- */

/* A lock given back that is not the last one taken: taken out of the middle
 * of the list, or, if this core never took it, a fault (with lockcheck). */
void lock_given_back(struct cpu *c, struct spinlock *lock)
{
    int n = c->locks_held;
    int kept = n < CPU_LOCKS_KEPT ? n : CPU_LOCKS_KEPT;
    for (int i = kept - 1; i >= 0; i--) {
        if (c->held[i] == lock) {
            for (int j = i; j + 1 < kept; j++)
                c->held[j] = c->held[j + 1];
            c->locks_held = n - 1;
            return;
        }
    }
    if (n > CPU_LOCKS_KEPT) {
        c->locks_held = n - 1;                  /* one of those only counted */
        return;
    }
    if (lock_checking) {
        lock_checking = false;                  /* (the report takes locks) */
        panic("spinlock '%s' given back by a core that does not hold it", lock->name);
    }
}

void lock_held_while(const char *what)
{
    struct cpu *c = arch_this_cpu();
    int n = c->locks_held;
    /* The first one taken: inside the scheduler the last is its own. */
    const char *name = n > 0 ? c->held[0]->name : "?";
    struct thread *t = thread_current();
    lock_checking = false;
    panic("%s while holding spinlock '%s' (%d held, thread '%s')", what,
          name ? name : "(unnamed)", n, t ? t->name : "?");
}

/* --- the order of locks ("lockcheck", 65a-5) ------------------------------ */

/* Every lock name seen gets a number; `before[a]` has bit b when a lock
 * named b was taken while one named a was held. Taking b while holding a
 * is refused when b already leads to a (b before a, or b before c before
 * a ...): two cores going round that circle would wait for each other. Its
 * own state is guarded by a plain flag, not a struct spinlock, which would
 * check itself. */
#define CHECK_CLASSES 192
#define CLASS_WORDS ((CHECK_CLASSES + 63) / 64)

bool lock_checking;
static volatile int check_busy;
static const char *class_names[CHECK_CLASSES];
static uint64_t before[CHECK_CLASSES][CLASS_WORDS];
static int classes, orders, unnamed, same_name, too_many;

static int class_of(struct spinlock *lock)
{
    if (lock->check_class)
        return lock->check_class - 1;
    if (!lock->name) {
        unnamed++;
        return -1;
    }
    int k = 0;
    while (k < classes && strcmp(class_names[k], lock->name) != 0)
        k++;
    if (k == classes) {
        if (classes == CHECK_CLASSES) {
            too_many++;
            return -1;
        }
        class_names[classes++] = lock->name;
    }
    lock->check_class = k + 1;
    return k;
}

static bool has(int a, int b)
{
    return before[a][b / 64] >> (b % 64) & 1;
}

/* Does `from` lead to `to` through the orders seen? (A walk over at most
 * CHECK_CLASSES names, only when a new order appears.) */
static bool leads_to(int from, int to, int *via)
{
    static uint64_t seen[CLASS_WORDS];      /* (static: under check_busy, off the stack) */
    static int stack[CHECK_CLASSES];
    memset(seen, 0, sizeof(seen));
    int top = 0;
    stack[top++] = from;
    seen[from / 64] |= 1ULL << (from % 64);
    while (top) {
        int a = stack[--top];
        for (int b = 0; b < classes; b++) {
            if (!has(a, b) || (seen[b / 64] >> (b % 64) & 1))
                continue;
            if (b == to) {
                *via = a;
                return true;
            }
            seen[b / 64] |= 1ULL << (b % 64);
            stack[top++] = b;
        }
    }
    return false;
}

void lockcheck_order(struct cpu *c, struct spinlock *lock)
{
    int kept = c->locks_held < CPU_LOCKS_KEPT ? c->locks_held : CPU_LOCKS_KEPT;
    if (!kept)
        return;
    while (__atomic_exchange_n(&check_busy, 1, __ATOMIC_ACQUIRE))
        arch_cpu_relax();
    int b = class_of(lock);
    for (int i = 0; b >= 0 && i < kept; i++) {
        int a = class_of(c->held[i]);
        if (a < 0)
            continue;
        if (a == b) {
            same_name++;                        /* two of a kind: the name cannot tell them apart */
            continue;
        }
        if (has(a, b))
            continue;
        int via = a;
        if (leads_to(b, a, &via)) {
            __atomic_store_n(&check_busy, 0, __ATOMIC_RELEASE);
            lock_checking = false;
            if (via == b)
                panic("lock order: '%s' taken while holding '%s', but '%s' was taken while "
                      "holding '%s' before", class_names[b], class_names[a],
                      class_names[a], class_names[b]);
            panic("lock order: '%s' taken while holding '%s', but earlier orders lead from "
                  "'%s' to '%s' (last through '%s')", class_names[b], class_names[a],
                  class_names[b], class_names[a], class_names[via]);
        }
        before[a][b / 64] |= 1ULL << (b % 64);
        orders++;
    }
    __atomic_store_n(&check_busy, 0, __ATOMIC_RELEASE);
}

void lockcheck_report(void)
{
    if (!lock_checking)
        return;
    kprintf("  lockcheck:     %d lock names, %d orders seen, no circle", classes, orders);
    if (unnamed || same_name || too_many)
        kprintf(" (%d takings of unnamed locks, %d of two of a kind, %d past %d names)", unnamed,
                same_name, too_many, CHECK_CLASSES);
    kprintf("\n");
}

/* --- waits for locks (65e) ------------------------------------------------- */

/* By name: every lock of one name counts together (the many processes'
 * "mappings" mutexes, the TCP connections'). Its own state is guarded by a
 * plain flag, as lockcheck's: a struct spinlock would count itself. */
#define WAIT_NAMES 48
static struct {
    const char *name;
    int kind;                               /* WAIT_SPUN, ... */
    uint64_t times, counts;
} waits[WAIT_NAMES];
static volatile int waits_busy;
static int wait_names, waits_lost;

void lock_waited(const char *name, uint64_t counts, bool slept)
{
    lock_waited_as(name, counts, slept ? WAIT_SLEPT : WAIT_SPUN);
}

void lock_waited_as(const char *name, uint64_t counts, int kind)
{
    if (!name)
        name = "(unnamed)";
    /* Interrupts off while the flag is held: a mutex's wait comes here with
     * them on, and an interrupt that found a lock taken would come again on
     * this core and wait for the flag for good (seen: a hang in a wake-up). */
    uint64_t irq = arch_irq_save();
    while (__atomic_exchange_n(&waits_busy, 1, __ATOMIC_ACQUIRE))
        arch_cpu_relax();
    int i = 0;
    while (i < wait_names && (waits[i].name != name || waits[i].kind != kind))
        i++;
    if (i == wait_names && wait_names < WAIT_NAMES) {
        waits[i].name = name;
        waits[i].kind = kind;
        wait_names++;
    }
    if (i < wait_names) {
        waits[i].times++;
        waits[i].counts += counts;
    } else {
        waits_lost++;
    }
    __atomic_store_n(&waits_busy, 0, __ATOMIC_RELEASE);
    arch_irq_restore(irq);
}

void lock_wait_report(void)
{
    uint64_t hz = arch_counter_hz();
    int n = wait_names;
    if (!n || !hz)
        return;
    /* The longest waits first (a copy: the counts go on meanwhile). */
    int order[WAIT_NAMES];
    for (int i = 0; i < n; i++)
        order[i] = i;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && waits[order[j]].counts > waits[order[j - 1]].counts; j--) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    kprintf("  waits for locks since the start, the longest first:\n");
    static const char *const kinds[] = { "(spun)", "(slept)", "(read)", "(write)" };
    for (int k = 0; k < n && k < 10; k++) {
        int i = order[k];
        uint64_t us = waits[i].counts / (hz / 1000000 ? hz / 1000000 : 1);
        kprintf("    %-16s %-8s %9lu times, %lu.%03lu ms in all\n", waits[i].name,
                kinds[waits[i].kind], waits[i].times, us / 1000, us % 1000);
    }
}

/* --- mutexes ---------------------------------------------------------------- */

/* (65e) Before sleeping on a mutex held by a thread running on another
 * core: go round while it runs, up to 20 us, as most holds are shorter (the
 * laptop: TCP's lock met 4,300 times a second, free again after 2-3 us,
 * each wait a sleep and a wake-up through the scheduler). True if the owner
 * let go or changed: look again. */
#define SPIN_ROUNDS 4                       /* looks that may spin, then sleep */

static bool spin_while_running(struct mutex *m, struct thread *owner)
{
    uint64_t start = arch_counter(), limit = arch_counter_hz() / 50000;
    bool let_go = false;
    for (;;) {
        if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) != owner) {
            let_go = true;
            break;
        }
        /* (A thread that ended meanwhile: its block is heap memory still
         * mapped, so the look is harmless; it only ends the spin.) */
        if (!__atomic_load_n(&owner->on_cpu, __ATOMIC_RELAXED) || arch_counter() - start > limit)
            break;
        arch_cpu_relax();
    }
    lock_waited(m->name, arch_counter() - start, false);
    return let_go;
}

void mutex_lock(struct mutex *m)
{
    if (sched_in_interrupt())
        panic("mutex '%s' used in an interrupt handler", m->name);
    uint64_t irq = arch_irq_save();         /* (65c-3: this core's count, not a torn read) */
    int held = arch_this_cpu()->locks_held;
    arch_irq_restore(irq);
    if (held) {
        static char what[80];               /* (only once: the panic ends everything) */
        ksnprintf(what, sizeof(what), "mutex '%s' taken", m->name ? m->name : "(unnamed)");
        lock_held_while(what);
    }
    struct thread *self = thread_current();
    int rounds = 0;
    for (;;) {
        uint64_t state = spin_lock(&m->lock);
        if (!m->owner || m->owner == self) {
            m->owner = self;
            m->depth++;
            spin_unlock(&m->lock, state);
            return;
        }
        struct thread *owner = m->owner;
        /* Not past a thread already asleep on it: a woken sleeper must
         * first run, and spinners on a core would win every time (fork,
         * waiting for its program's mappings, lost two thirds of its turns
         * to smpstress's faulting threads). */
        if (rounds < SPIN_ROUNDS && owner->on_cpu && !m->waiters.head && cpus_online() > 1) {
            spin_unlock(&m->lock, state);
            rounds = spin_while_running(m, owner) ? rounds + 1 : SPIN_ROUNDS;
            continue;
        }
        /* (65a-3) On the queue while still holding the lock, so the owner's
         * unlock (under the same lock) cannot pass us by. */
        wait_prepare(&m->waiters, 0);
        spin_unlock(&m->lock, state);
        uint64_t began = arch_counter();
        wait_commit(0);
        lock_waited(m->name, arch_counter() - began, true);     /* (65e) */
    }
}

bool mutex_try_lock(struct mutex *m)
{
    uint64_t state = spin_lock(&m->lock);
    struct thread *self = thread_current();
    bool ok = !m->owner || m->owner == self;
    if (ok) {
        m->owner = self;
        m->depth++;
    }
    spin_unlock(&m->lock, state);
    return ok;
}

void mutex_unlock(struct mutex *m)
{
    uint64_t state = spin_lock(&m->lock);
    if (m->owner != thread_current()) {
        spin_unlock(&m->lock, state);
        panic("mutex '%s' unlocked by a thread that does not hold it", m->name);
    }
    if (--m->depth == 0) {
        m->owner = 0;
        wait_queue_wake_one(&m->waiters);
    }
    spin_unlock(&m->lock, state);
}

/* --- (65e-36) readers-writer locks ------------------------------------------ */

/* Neither in an interrupt handler nor with a spinlock held (it sleeps). */
static void rwlock_may_sleep(struct rwlock *l)
{
    if (sched_in_interrupt())
        panic("rwlock '%s' used in an interrupt handler", l->name);
    uint64_t irq = arch_irq_save();
    int held = arch_this_cpu()->locks_held;
    arch_irq_restore(irq);
    if (held) {
        static char what[80];               /* (only once: the panic ends everything) */
        ksnprintf(what, sizeof(what), "rwlock '%s' taken", l->name ? l->name : "(unnamed)");
        lock_held_while(what);
    }
}

/* Where this thread keeps its read holds of l, or -1. */
static int reading_slot(const struct thread *self, const struct rwlock *l)
{
    for (int i = 0; i < RW_READS; i++)
        if (self->reading[i] == l)
            return i;
    return -1;
}

/* (65e-45) Before sleeping, a waiter goes round while the busy bits stay:
 * up to 20 us a look, as on a mutex, but not while the writer holding it is
 * off its core. Readers' holds cannot be seen running: a writer waits for
 * them the whole 20 us (a fault holds it to read for about a microsecond).
 * True when the busy bits went: try again. */
static bool rw_spin(struct rwlock *l, uint64_t busy)
{
    uint64_t start = arch_counter(), limit = arch_counter_hz() / 50000;
    bool free = false;
    for (;;) {
        if (!(__atomic_load_n(&l->state, __ATOMIC_RELAXED) & busy)) {
            free = true;
            break;
        }
        /* (A writer that ended meanwhile: as for a mutex's owner, harmless.) */
        struct thread *w = __atomic_load_n(&l->writer, __ATOMIC_RELAXED);
        if ((w && !__atomic_load_n(&w->on_cpu, __ATOMIC_RELAXED)) || arch_counter() - start > limit)
            break;
        arch_cpu_relax();
    }
    lock_waited(l->name, arch_counter() - start, false);
    return free;
}

/* Sleep until someone lets go, unless the busy bits went meanwhile. On the
 * queue first, then the mark: whoever lets go after the mark sees it and
 * wakes the queue. (65e-56) Interrupts off from the queue to the sleep, as
 * wait_event has them: a thread switched out on the queue before its mark
 * (it counts as asleep there) was woken by nobody, and a writer counted
 * as waiting kept every reader asleep behind it for good (4 cores, 8
 * threads faulting: smpstress hung once woken threads came at once). */
static void rw_sleep(struct rwlock *l, uint64_t busy, uint64_t *began)
{
    uint64_t state = arch_irq_save();
    wait_prepare(&l->waiters, 0);
    if (!(__atomic_fetch_or(&l->state, RW_SLEEPERS, __ATOMIC_SEQ_CST) & busy)) {
        wait_cancel();
        arch_irq_restore(state);
        return;
    }
    if (!*began)
        *began = arch_counter();
    wait_commit(0);
    arch_irq_restore(state);
}

/* After letting go (now: the state it left): the sleepers woken, if the
 * mark says there may be any. Woken, each looks again (and marks again). */
static void rw_wake(struct rwlock *l, uint64_t now)
{
    if ((now & RW_SLEEPERS) &&
        (__atomic_fetch_and(&l->state, ~RW_SLEEPERS, __ATOMIC_SEQ_CST) & RW_SLEEPERS))
        wait_queue_wake_all(&l->waiters);
}

int rwlock_read(struct rwlock *l)
{
    rwlock_may_sleep(l);
    struct thread *self = thread_current();
    if (__atomic_load_n(&l->writer, __ATOMIC_RELAXED) == self) {
        l->write_depth++;                   /* the writer reads: one more hold as the writer */
        return 0;
    }
    int i = reading_slot(self, l);
    if (i >= 0) {
        /* A nested read goes on even past a waiting writer (who waits for
         * this thread's outer hold); a first one does not (writers first). */
        __atomic_add_fetch(&l->state, 1, __ATOMIC_ACQUIRE);
        return ++self->reading_depth[i];
    }
    i = reading_slot(self, 0);
    if (i < 0)
        panic("rwlock '%s': a thread reads more than %d rwlocks", l->name, RW_READS);
    uint64_t busy = RW_WRITER | RW_WAITING, began = 0;
    int rounds = 0;
    for (;;) {
        uint64_t s = __atomic_load_n(&l->state, __ATOMIC_RELAXED);
        if (!(s & busy)) {
            if (__atomic_compare_exchange_n(&l->state, &s, s + 1, false, __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED))
                break;
            continue;
        }
        if (rounds < SPIN_ROUNDS && cpus_online() > 1)
            rounds = rw_spin(l, busy) ? rounds + 1 : SPIN_ROUNDS;
        else
            rw_sleep(l, busy, &began);
    }
    self->reading[i] = l;
    self->reading_depth[i] = 1;
    if (began)
        lock_waited_as(l->name, arch_counter() - began, WAIT_READ);
    return 1;
}

void rwlock_write(struct rwlock *l)
{
    rwlock_may_sleep(l);
    struct thread *self = thread_current();
    if (__atomic_load_n(&l->writer, __ATOMIC_RELAXED) == self) {
        l->write_depth++;
        return;
    }
    if (reading_slot(self, l) >= 0)
        panic("rwlock '%s': a thread that reads it asks to write", l->name);
    uint64_t busy = RW_READERS | RW_WRITER, began = 0, counted = 0;
    int rounds = 0;
    for (;;) {
        uint64_t s = __atomic_load_n(&l->state, __ATOMIC_RELAXED);
        if (!(s & busy)) {
            if (__atomic_compare_exchange_n(&l->state, &s, (s | RW_WRITER) - counted, false,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                break;
            continue;
        }
        if (!counted) {
            counted = RW_WAITING_ONE;       /* (new readers wait from now on) */
            __atomic_add_fetch(&l->state, counted, __ATOMIC_RELAXED);
        } else if (rounds < SPIN_ROUNDS && cpus_online() > 1) {
            rounds = rw_spin(l, busy) ? rounds + 1 : SPIN_ROUNDS;
        } else {
            rw_sleep(l, busy, &began);
        }
    }
    __atomic_store_n(&l->writer, self, __ATOMIC_RELAXED);
    l->write_depth = 1;
    if (began)
        lock_waited_as(l->name, arch_counter() - began, WAIT_WRITE);
}

void rwlock_downgrade(struct rwlock *l)
{
    struct thread *self = thread_current();
    if (__atomic_load_n(&l->writer, __ATOMIC_RELAXED) != self || l->write_depth != 1)
        panic("rwlock '%s': downgraded by a thread not holding it to write once", l->name);
    int i = reading_slot(self, 0);
    if (i < 0)
        panic("rwlock '%s': a thread reads more than %d rwlocks", l->name, RW_READS);
    self->reading[i] = l;
    self->reading_depth[i] = 1;
    l->write_depth = 0;
    __atomic_store_n(&l->writer, (struct thread *)0, __ATOMIC_RELAXED);
    /* Writer bit off and one read hold on in one step. Readers asleep may
     * come in (writers still wait for this hold). */
    rw_wake(l, __atomic_add_fetch(&l->state, 1 - RW_WRITER, __ATOMIC_RELEASE));
}

void rwlock_unlock(struct rwlock *l)
{
    struct thread *self = thread_current();
    if (__atomic_load_n(&l->writer, __ATOMIC_RELAXED) == self) {
        if (--l->write_depth == 0) {
            __atomic_store_n(&l->writer, (struct thread *)0, __ATOMIC_RELAXED);
            rw_wake(l, __atomic_and_fetch(&l->state, ~RW_WRITER, __ATOMIC_RELEASE));
        }
        return;
    }
    int i = reading_slot(self, l);
    if (i < 0)
        panic("rwlock '%s' given back by a thread that does not hold it", l->name);
    if (--self->reading_depth[i] == 0)
        self->reading[i] = 0;
    uint64_t now = __atomic_sub_fetch(&l->state, 1, __ATOMIC_RELEASE);
    if (!(now & RW_READERS))
        rw_wake(l, now);
}

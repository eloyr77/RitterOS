#include "sched.h"
#include "cpus.h"
#include "heap.h"
#include "kprintf.h"
#include "memory.h"
#include "panic.h"
#include "paging.h"
#include "percpu.h"
#include "pmm.h"
#include "string.h"
#include "sync.h"
#include "timer.h"

/* 32 KiB kernel stack per thread (62f, the user's choice): our own cc's
 * ARM kernel used 14.8 KiB of 16 on its deepest path (clang's: 9.8 KiB),
 * and an overflow damages its neighbours silently (no guard page) */
#define STACK_PAGES 8
#define SLICE_TICKS 2                   /* 20 ms at 100 Hz */
#define BALANCE_TICKS 10                /* (65e-39) a busy core looks at the others' queues */

static struct thread boot_thread = { .state = THREAD_RUNNING, .name = "main", .on_cpu = true };

/* (65a) The boot core's block (kernel/percpu.h): the code running now is
 * its thread "main". */
struct cpu boot_cpu = { .self = &boot_cpu, .index = 0, .current = &boot_thread,
                        .slice_left = SLICE_TICKS, .run_lock = SPINLOCK_INIT("run queue"),
                        .page_lock = SPINLOCK_INIT("pmm cache") };

void percpu_init_boot(void)
{
    arch_set_this_cpu(&boot_cpu);
}
static struct thread *all_threads = &boot_thread;
static bool started;
static int next_id = 1;
/* (65c) Whether cores other than the boot core take threads from the run
 * queues. Until 65c-2 they only tick and idle. */
static bool cores_share;
/* (65e-39) Threads that ended and wait for reap (it looks only then). */
static int dead_waiting;

/* (65a) What each core keeps for itself (kernel/percpu.h): the running
 * thread, its slice, the interrupt depth, the address space in place. */
#define current         (arch_this_cpu()->current)
#define idle            (arch_this_cpu()->idle)
#define need_resched    (arch_this_cpu()->need_resched)
#define slice_left      (arch_this_cpu()->slice_left)
#define irq_depth       (arch_this_cpu()->irq_depth)
#define active_space    (arch_this_cpu()->active_space)
#define switched_from   (arch_this_cpu()->switched_from)

/* (65a-3) The scheduler's lock: every wait queue's list and its watchers, a
 * waiting thread's queue and deadline, and the list of all threads. Taken
 * before a run queue's lock, never inside one; other locks may be taken
 * inside it (the heap's, the page allocator's: reap), never it inside
 * theirs, but for the wait primitives below, which callers use under their
 * own.
 *
 * (65e-39) Each core has its own run queue (struct cpu) under its own lock,
 * "run queue". Until 65e-39 one queue under this lock served every core:
 * on the laptop 8 pairs of threads waking each other (smpstress wakeups)
 * spun on it 106 s in 10 s. schedule() is called with this core's
 * run-queue lock held and keeps it across the switch: the thread that runs
 * next gives it back (after its own switch in schedule(), or in
 * sched_thread_start when it is new), so a thread is on a core exactly
 * while its on_cpu says so, read under that core's lock. A thread's state
 * changes under the lock of the core it runs on or last ran on (t->cpu),
 * or by the thread itself with interrupts off: a waker takes that lock
 * (lock_core_of), so its look and the sleeper's switch never pass each
 * other. A woken thread goes to the waker's core when nothing else waits
 * there, else back where it ran last (its caches) if that core is idle,
 * else to an idle core; an idle core takes threads from the longest queue
 * of another, and a busy one evens out a queue two longer than its own at
 * its tick. */
static struct spinlock sched_lock = SPINLOCK_INIT("scheduler");

static void wake(struct thread *t);

/* This core's run-queue lock, with interrupts off first (else the thread
 * could move between finding its core and locking it). */
static uint64_t lock_here(void)
{
    uint64_t state = arch_irq_save();
    spin_lock(&arch_this_cpu()->run_lock);
    return state;
}

/* The lock of the core it runs on now (after a switch: another core's). */
static void unlock_here(uint64_t state)
{
    spin_unlock_only(&arch_this_cpu()->run_lock);
    arch_irq_restore(state);
}

/* The lock of the core t runs on or last ran on (the boot core's if it
 * never ran). t moves only while queued or running: looked at again once
 * held. */
static struct cpu *lock_core_of(struct thread *t, uint64_t *state)
{
    for (;;) {
        int index = __atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE);
        struct cpu *c = cpu_of(index);
        *state = spin_lock(&c->run_lock);
        if (t->cpu == index)
            return c;
        spin_unlock(&c->run_lock, *state);
    }
}

/* Everything below up to the wait queues runs with c's run lock held. */

/* (65c-3) A thread on a run queue twice would run on two cores at once. */
static void not_queued(struct thread *t)
{
    if (t->queued)
        panic("scheduler: thread '%s' queued while it is queued already", t->name);
    t->queued = true;
}

static void count(struct cpu *c, int more)
{
    __atomic_store_n(&c->run_count, c->run_count + more, __ATOMIC_RELAXED);
}

static void make_ready(struct cpu *c, struct thread *t)
{
    not_queued(t);
    t->state = THREAD_READY;
    t->next = 0;
    if (c->run_tail)
        c->run_tail->next = t;
    else
        c->run_head = t;
    c->run_tail = t;
    count(c, 1);
}

/* (64l) A thread interrupted before its slice ended (by a woken one): it
 * goes on first among the others, so the busy threads keep their order. */
static void make_ready_first(struct cpu *c, struct thread *t)
{
    not_queued(t);
    t->state = THREAD_READY;
    t->next = c->run_head;
    c->run_head = t;
    if (!c->run_tail)
        c->run_tail = t;
    count(c, 1);
}

static void make_woken(struct cpu *c, struct thread *t)
{
    not_queued(t);
    t->state = THREAD_READY;
    t->next = 0;
    if (c->woken_tail)
        c->woken_tail->next = t;
    else
        c->woken_head = t;
    c->woken_tail = t;
    count(c, 1);
}

/* The next thread of c's queue (*woken: it was a woken one), or 0: a
 * woken one first when the core takes its own, the one waiting longest
 * among the others first when another core takes it (65e-39: the woken
 * ones run soon where they are). */
static struct thread *take_ready(struct cpu *c, bool woken_first, bool *woken)
{
    *woken = c->woken_head && (woken_first || !c->run_head);
    struct thread *t = *woken ? c->woken_head : c->run_head;
    if (!t)
        return 0;
    if (*woken) {
        c->woken_head = t->next;
        if (!c->woken_head)
            c->woken_tail = 0;
    } else {
        c->run_head = t->next;
        if (!c->run_head)
            c->run_tail = 0;
    }
    t->next = 0;
    t->queued = false;
    count(c, -1);
    return t;
}

/* (65e-39) Where a thread that may run now goes (interrupts off, no run
 * lock held): a woken one to this core when nothing else waits here (the
 * waker mostly sleeps next, waiting for its answer: a pipe, a lock); else
 * the core it last ran on when that one is idle, else an idle core, else
 * this one (it runs soon: need_resched). Sent to an idle core every time,
 * each wake-up cost that core an interrupt and its way out of the halt: on
 * the laptop 8 pairs of threads passing a token made 170,000 round trips a
 * second, here 251,000 (one shared queue: 198,000). Before the cores share
 * (65c-2) the boot core takes them all. */
static struct cpu *place(struct thread *t, bool woken)
{
    struct cpu *me = arch_this_cpu();
    if (!cores_share)
        return cpu_of(0);
    if (woken && !__atomic_load_n(&me->run_count, __ATOMIC_RELAXED))
        return me;
    struct cpu *home = t->cpu >= 0 ? cpu_of(t->cpu) : me;
    if (home != me && cpus_core_idle(home))
        return home;
    struct cpu *other = cpus_find_idle(me);
    return other ? other : me;
}

/* t (ready, off every core and queue) onto the queue place() chooses, and
 * that core told: another is woken; this one switches soon when t was
 * woken (it waited for this), a new thread waits its turn. */
static void enqueue(struct thread *t, bool woken)
{
    struct cpu *c = place(t, woken);
    spin_lock(&c->run_lock);
    if (woken)
        make_woken(c, t);
    else
        make_ready(c, t);
    spin_unlock_only(&c->run_lock);
    if (c != arch_this_cpu())
        cpus_kick(c->index);
    else if (woken)
        need_resched = true;
}

/* (65e-39) Take a thread from another core's queue when its queue is at
 * least `more` longer than this one's (interrupts off, no run lock held):
 * true if one is on this core's queue now. */
static bool take_from_others(struct cpu *me, int more)
{
    struct cpu *from = 0;
    int most = __atomic_load_n(&me->run_count, __ATOMIC_RELAXED) + more - 1;
    int n = cpus_online();
    for (int i = 0; i < n; i++) {
        struct cpu *c = cpu_online_at(i);
        int queued = __atomic_load_n(&c->run_count, __ATOMIC_RELAXED);
        if (c != me && queued > most) {
            most = queued;
            from = c;
        }
    }
    if (!from)
        return false;
    bool woken;
    spin_lock(&from->run_lock);
    struct thread *t = take_ready(from, false, &woken);
    spin_unlock_only(&from->run_lock);
    if (!t)
        return false;
    spin_lock(&me->run_lock);
    if (woken)
        make_woken(me, t);
    else
        make_ready(me, t);
    me->taken++;
    spin_unlock_only(&me->run_lock);
    return true;
}

/* Free the stacks of threads that have ended and left their cores (no
 * lock held; the scheduler's taken here). */
static void reap(void)
{
    struct thread *gone = 0;
    uint64_t state = spin_lock(&sched_lock);
    struct thread **link = &all_threads;
    while (*link) {
        struct thread *t = *link;
        if (t->state == THREAD_DEAD && !__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) {
            *link = t->all_next;
            t->all_next = gone;
            gone = t;
            __atomic_sub_fetch(&dead_waiting, 1, __ATOMIC_RELAXED);
        } else {
            link = &t->all_next;
        }
    }
    spin_unlock(&sched_lock, state);
    while (gone) {
        struct thread *t = gone;
        gone = t->all_next;
        pmm_free_pages(t->stack, STACK_PAGES);
        kfree(t->fpu_block);
        kfree(t);
    }
}

/* (65e-55) A program's sysinfo (and `mem`) counts free pages right after a
 * child ended: its thread's stack (8 pages) was free or not by whether an
 * idle loop had run since (ARM's fork test missed by 9-10 pages, 1 run in
 * 6). A thread that ended is off its core within its last switch. */
void sched_reap(void)
{
    uint64_t began = arch_counter(), limit = arch_counter_hz() / 1000;
    while (__atomic_load_n(&dead_waiting, __ATOMIC_RELAXED)) {
        reap();
        if (arch_counter() - began > limit)
            break;
        arch_cpu_relax();
    }
}

/* After a switch, on the thread that now runs: the one switched away from
 * is off the core (its stack is no longer in use). */
static void finish_switch(void)
{
    if (switched_from) {
        __atomic_store_n(&switched_from->on_cpu, false, __ATOMIC_RELEASE);
        switched_from = 0;
    }
}

/* Give the CPU to the next ready thread of this core (or the idle thread):
 * a woken one first (64l). The current one, if it can still run, goes to
 * the end of the queue when its slice is used up (or it yields), else back
 * to its front. */
static void schedule(void)
{
    locks_none_held(1, "thread switch");    /* (65a-5) only this core's run lock */
    struct cpu *me = arch_this_cpu();
    struct thread *prev = current;
    if (prev->state == THREAD_RUNNING && prev != idle) {
        if (slice_left > 0 && me->woken_head && !prev->yielding) {
            make_ready_first(me, prev);
            prev->slice_rest = slice_left;  /* (65e-39) */
        } else {
            make_ready(me, prev);
        }
    }
    bool woken;
    struct thread *next = take_ready(me, true, &woken);
    if (!next)
        next = prev->state == THREAD_RUNNING ? prev : idle;
    need_resched = false;
    /* (65e-39) A thread put first by a woken one goes on with the rest of
     * its slice, then to the queue's end: with a fresh slice at each turn
     * (until 65e-39) one woken all the time (smpstress wakeups' pairs) kept
     * the thread it interrupted first for ever, and the threads behind it
     * on that core never ran (one shared queue hid it: other cores took
     * them). */
    slice_left = next->slice_rest > 0 ? next->slice_rest : SLICE_TICKS;
    next->slice_rest = 0;
    if (next == prev) {
        prev->state = THREAD_RUNNING;
        return;
    }
    if (next->on_cpu)                       /* (65c-3) */
        panic("scheduler: thread '%s' chosen while it runs on a core", next->name);
    if (prev == idle && prev->state == THREAD_RUNNING)
        prev->state = THREAD_READY;         /* idle is never queued */
    next->state = THREAD_RUNNING;
    next->switches++;
    next->on_cpu = true;
    __atomic_store_n(&next->cpu, me->index, __ATOMIC_RELEASE);     /* (65d) `threads`' cpu column */
    current = next;
    /* The next thread's program (or none) in the lower half, and its kernel
     * stack for interrupts and system calls from user mode. */
    if (next->space != active_space) {
        paging_user_activate(next->space);
        active_space = next->space;
        /* (65e) Seen by a core that changes these tables before this one
         * reads them: it clears an entry, then looks at active_space (to ask
         * this core to forget the page); this core stores active_space, then
         * walks the tables. Without a fence each could pass the other (x86
         * lets a load overtake an earlier store): seen as stale TLB entries
         * under smpstress (generation 838 read after 839 was announced). */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }
    if (next->stack)
        arch_set_kernel_stack(thread_stack_top(next));
    /* Programs' FP/SIMD registers: the kernel never uses them, so they only
     * change hands between programs. */
    if (prev->process && prev->fpu)
        arch_fpu_save(prev->fpu);
    if (next->process && next->fpu)
        arch_fpu_restore(next->fpu);
    /* And their thread pointers (22i; ARM programs may set theirs directly). */
    if (prev->process) {
        prev->tp = arch_thread_pointer();
        prev->segments = arch_user_segments();
    }
    if (next->process) {
        arch_set_thread_pointer(next->tp);
        arch_set_user_segments(next->segments);
    }
    uint64_t now = arch_counter();          /* (each thread's CPU time, 22o) */
    prev->cpu_counts += now - prev->ran_since;
    next->ran_since = now;
    switched_from = prev;
    arch_context_switch(&prev->sp, next->sp);
    /* Running again (a run lock held, handed over by the thread before; it
     * may be another core's): prev is the current thread once more, and
     * the one that ran before it here is off this core. */
    finish_switch();
}

/* Does this core take threads from the run queues? (65c) */
static bool takes_threads(void)
{
    return arch_this_cpu()->index == 0 || cores_share;
}

static void idle_loop(void *arg)
{
    (void)arg;
    for (;;) {
        if (__atomic_load_n(&dead_waiting, __ATOMIC_RELAXED))
            reap();
        uint64_t state = lock_here();
        struct cpu *me = arch_this_cpu();   /* (idle never moves) */
        me->kicked = false;                 /* (65c-2) it looks now */
        if ((me->run_head || me->woken_head) && takes_threads()) {
            schedule();
            unlock_here(state);
            continue;
        }
        spin_unlock_only(&me->run_lock);
        /* (65e-39) Nothing here: another core's queue, if one waits. Then
         * halt with interrupts still off: a wake-up meanwhile comes with an
         * interrupt, which ends the halt (on x86 STI takes effect only
         * after HLT has begun). */
        if (cores_share && take_from_others(me, 1)) {
            arch_irq_restore(state);
            continue;
        }
        arch_idle();                        /* (37d-2) enables interrupts and sleeps */
    }
}

/* A new thread's first code, with its core's run lock held from the switch
 * to it. */
void sched_thread_start(void)
{
    finish_switch();
    spin_unlock_only(&arch_this_cpu()->run_lock);
    arch_interrupts_enable();
}

/* (65c) A started core's own code (kernel/cpus.c) becomes its idle thread,
 * "idleN", on the stack it started on (STACK_PAGES pages at stack). It
 * never returns. */
void sched_core_enter(struct cpu *c, uint64_t stack)
{
    struct thread *t = kzalloc(sizeof(*t));
    if (!t)
        for (;;)
            arch_wait_for_interrupt();
    ksnprintf(t->name, sizeof(t->name), "idle%d", c->index);
    t->cpu = c->index;
    t->stack = stack;
    t->state = THREAD_RUNNING;
    t->on_cpu = true;
    slice_left = SLICE_TICKS;
    uint64_t state = spin_lock(&sched_lock);
    current = t;
    __atomic_store_n(&idle, t, __ATOMIC_RELEASE);     /* (cpus_core_idle reads them) */
    t->id = next_id++;
    t->all_next = all_threads;
    all_threads = t;
    t->ran_since = arch_counter();
    spin_unlock(&sched_lock, state);
    arch_interrupts_enable();
    idle_loop(0);
    __builtin_unreachable();
}

uint64_t thread_stack_top(const struct thread *t)
{
    return (uint64_t)phys_to_virt(t->stack) + STACK_PAGES * PMM_PAGE_SIZE;
}

void sched_set_space(uint64_t space)
{
    uint64_t state = arch_irq_save();       /* (this core's: no switch meanwhile) */
    current->space = space;
    if (space != active_space) {
        paging_user_activate(space);
        active_space = space;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);    /* (65e: as in schedule) */
    }
    arch_irq_restore(state);
}

void sched_request_stop(struct thread *t)
{
    uint64_t state = arch_irq_save();
    t->stop_requested = true;
    /* (65e-44) Before the look at its state: the waiter looks at the
     * request after joining its queue (wait_prepare's fence). Made runnable
     * only: it takes itself off its queue (wait_commit). */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (t->state == THREAD_BLOCKED) {
        wake(t);
    } else if (t->on_cpu && t->cpu != arch_this_cpu()->index) {
        /* (65d) Running on another core, perhaps in its program: interrupt
         * that core, so the thread sees the request at once (on the way
         * back to its program), not at that core's next tick. */
        cpus_kick(t->cpu);
    }
    arch_irq_restore(state);
}

struct thread *thread_create(const char *name, void (*fn)(void *arg), void *arg)
{
    return thread_create_for(name, fn, arg, 0, 0);
}

struct thread *thread_create_for(const char *name, void (*fn)(void *arg), void *arg,
                                 struct process *process, uint64_t space)
{
    if (__atomic_load_n(&dead_waiting, __ATOMIC_RELAXED) && !sched_in_interrupt())
        reap();
    struct thread *t = kzalloc(sizeof(*t));
    uint64_t stack = t ? pmm_alloc_pages(STACK_PAGES) : 0;
    if (t && process)
        t->fpu_block = kmalloc(ARCH_FPU_STATE_SIZE + 63);
    if (!stack || (process && !t->fpu_block)) {
        if (stack)
            pmm_free_pages(stack, STACK_PAGES);
        if (t)
            kfree(t->fpu_block);
        kfree(t);
        return 0;
    }
    if (process) {
        t->fpu = (void *)(((uint64_t)t->fpu_block + 63) & ~63ULL);
        arch_fpu_clean(t->fpu);
    }
    size_t n = strlen(name) < sizeof(t->name) - 1 ? strlen(name) : sizeof(t->name) - 1;
    memcpy(t->name, name, n);
    t->stack = stack;
    t->process = process;
    t->space = space;
    t->cpu = -1;                            /* (65d) not run yet */
    t->sp = arch_thread_init(thread_stack_top(t), fn, arg);
    t->state = THREAD_READY;
    uint64_t state = spin_lock(&sched_lock);
    t->id = next_id++;
    t->all_next = all_threads;
    all_threads = t;
    enqueue(t, false);
    spin_unlock(&sched_lock, state);
    return t;
}

void sched_init(void)
{
    idle = thread_create("idle", idle_loop, 0);
    if (!idle)
        panic("scheduler: no memory for the idle thread");
    uint64_t state = lock_here();
    bool woken;
    take_ready(arch_this_cpu(), true, &woken);  /* idle runs only when nothing else can */
    idle->state = THREAD_READY;
    need_resched = false;
    started = true;
    unlock_here(state);
    kprintf("  scheduler:     threads 'main' (this code) and 'idle', time slice %u ms, "
            "preemptible kernel\n", SLICE_TICKS * 1000 / TIMER_HZ);
}

bool sched_running(void)
{
    return started;
}

uint64_t sched_cpu_counts(struct thread *t)
{
    uint64_t state;
    struct cpu *c = lock_core_of(t, &state);
    uint64_t counts = t->cpu_counts + (t->on_cpu ? arch_counter() - t->ran_since : 0);
    spin_unlock(&c->run_lock, state);
    return counts;
}

/* (65c-3) A field of this core's block is two instructions away (the
 * block, then the field): a thread moved to another core in between reads
 * the old core's. With interrupts off it cannot move. (The fork child once
 * read core 0's `current`, another program's thread, after moving.) */
struct thread *thread_current(void)
{
    uint64_t state = arch_irq_save();
    struct thread *t = current;
    arch_irq_restore(state);
    return t;
}

void thread_exit(void)
{
    locks_none_held(0, "thread end");
    lock_here();
    current->state = THREAD_DEAD;
    __atomic_add_fetch(&dead_waiting, 1, __ATOMIC_RELAXED);
    schedule();
    panic("scheduler: a dead thread ran again");
}

void thread_yield(void)
{
    locks_none_held(0, "yield");
    uint64_t state = lock_here();
    current->yielding = true;               /* (64l: to the end, whatever its slice) */
    schedule();
    current->yielding = false;
    unlock_here(state);
}

void sched_sleep_until(uint64_t tick)
{
    locks_none_held(0, "sleep");
    uint64_t state = arch_irq_save();
    while (timer_ticks() < tick) {
        if (!started) {
            arch_wait_for_interrupt();
            arch_interrupts_disable();
            continue;
        }
        /* (65e-44) On no queue: only the tick wakes it (its deadline). */
        current->waiting_on = 0;
        current->wake_tick = tick;
        current->state = THREAD_BLOCKED;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);    /* (the tick's look) */
        spin_lock(&arch_this_cpu()->run_lock);
        if (timer_ticks() >= tick && current->state == THREAD_BLOCKED)
            current->state = THREAD_RUNNING;
        schedule();
        spin_unlock_only(&arch_this_cpu()->run_lock);
        current->wake_tick = 0;
    }
    arch_irq_restore(state);
}

/* --- wait queues -------------------------------------------------------------- */

/* (65e-44) Each queue has its own lock (struct wait_queue, "wait queue",
 * named at its first use: queues are zeroed everywhere): its list and the
 * waiting_on of the threads on it. Until 65e-44 the scheduler's lock
 * guarded every queue: on the laptop 8 pairs of threads passing tokens
 * through pipes (2.1 million round trips a second) spun on it 57 s in 10 s.
 * A thread's state changes under its core's run lock (lock_core_of). A
 * waker takes a thread off the queue and makes it runnable; a deadline (the
 * tick) or a stop request only makes it runnable: the thread takes itself
 * off in wait_commit, where the object it waits on is still kept by its own
 * call (the tick could not know the queue still exists). Order: a queue's
 * lock, then a run queue's; "poll" (below), then a poll table's queue. */
static uint64_t lock_queue(struct wait_queue *q)
{
    if (!__atomic_load_n(&q->lock.name, __ATOMIC_RELAXED))
        __atomic_store_n(&q->lock.name, "wait queue", __ATOMIC_RELAXED);
    return spin_lock(&q->lock);
}

static void unlink(struct wait_queue *q, struct thread *t)
{
    for (struct thread **link = &q->head; *link; link = &(*link)->wait_next) {
        if (*link == t) {
            *link = t->wait_next;
            t->wait_next = 0;
            return;
        }
    }
}

/* The calling thread off its queue, if it is still on one: true if it took
 * itself off (no waker did). */
static bool leave_queue(struct thread *t)
{
    for (;;) {
        struct wait_queue *q = __atomic_load_n(&t->waiting_on, __ATOMIC_ACQUIRE);
        if (!q)
            return false;
        uint64_t state = lock_queue(q);
        bool there = t->waiting_on == q;
        if (there) {
            unlink(q, t);
            __atomic_store_n(&t->waiting_on, 0, __ATOMIC_RELEASE);
        }
        spin_unlock(&q->lock, state);
        if (there)
            return true;
    }
}

/* Running again, if it was waiting (under its core's lock; with interrupts
 * off). */
static void stop_waiting(struct thread *t)
{
    spin_lock(&arch_this_cpu()->run_lock);
    if (t->state == THREAD_BLOCKED)
        t->state = THREAD_RUNNING;
    spin_unlock_only(&arch_this_cpu()->run_lock);
}

/* (65a-3) A wait in three steps, so that no wake-up is lost between the
 * look at the condition and the sleep (another core, or an interrupt, may
 * make it true then): the thread goes on the queue first, then looks, then
 * sleeps only if nothing woke it meanwhile (a wake-up of a thread still on
 * its core only marks it running again). */
void wait_prepare(struct wait_queue *q, uint64_t deadline)
{
    if (!started)
        return;                             /* (before the scheduler: wait_commit halts) */
    uint64_t state = lock_queue(q);
    if (irq_depth)                          /* (65c-3: read with interrupts off) */
        panic("scheduler: sleeping in an interrupt handler");
    struct thread *t = current;
    struct thread **link = &q->head;        /* at the end: first come, first woken */
    while (*link)
        link = &(*link)->wait_next;
    *link = t;
    t->wait_next = 0;
    __atomic_store_n(&t->waiting_on, q, __ATOMIC_RELEASE);
    t->wake_tick = deadline;
    t->timed_out = false;
    t->state = THREAD_BLOCKED;              /* (its own, interrupts off: no switch here) */
    spin_unlock(&q->lock, state);
    /* (65e) Before the caller looks at its condition: a waker looks at the
     * queue without the lock (nobody_waits), so one of the two sees the
     * other (the waker's fence matches this one). */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void wait_cancel(void)
{
    if (!started)
        return;
    uint64_t state = arch_irq_save();
    struct thread *t = current;
    leave_queue(t);
    t->wake_tick = 0;
    stop_waiting(t);
    arch_irq_restore(state);
}

bool wait_commit(uint64_t deadline)
{
    locks_none_held(0, "wait");             /* (65a-5) even when it would not sleep */
    if (!started) {
        arch_wait_for_interrupt();
        arch_interrupts_disable();
        return !deadline || timer_ticks() < deadline;
    }
    uint64_t state = arch_irq_save();
    struct thread *t = current;
    if (!deadline || timer_ticks() < deadline) {
        /* (65e-39) Under this core's run lock: a waker meanwhile made it
         * running again (schedule then keeps it), or it sleeps until one
         * comes (the waker takes this lock too). */
        spin_lock(&arch_this_cpu()->run_lock);
        schedule();
        spin_unlock_only(&arch_this_cpu()->run_lock);
    }
    /* (65e-44) Still on the queue: its deadline (the tick), a stop request,
     * or the deadline passed before it slept. The caller looks again. */
    if (leave_queue(t))
        t->timed_out = deadline && timer_ticks() >= deadline;
    t->wake_tick = 0;
    stop_waiting(t);
    bool ok = !t->timed_out;
    arch_irq_restore(state);
    return ok;
}

/* (65e-39) t made runnable, decided under the lock of the core t is on or
 * was last on: still there (between wait_prepare and its sleep, or
 * switching away, which holds that lock until it is off), it only becomes
 * running again; off it, it is asleep and nothing else moves it, so it is
 * queued (64l: among the woken, ahead of the busy threads) where place()
 * says, and that core told. Nothing if it is not waiting. */
static void wake(struct thread *t)
{
    uint64_t state;
    struct cpu *c = lock_core_of(t, &state);
    if (t->state != THREAD_BLOCKED) {
        spin_unlock(&c->run_lock, state);
        return;
    }
    if (t->on_cpu) {
        t->state = THREAD_RUNNING;          /* (65a-3) between wait_prepare and its sleep */
        spin_unlock(&c->run_lock, state);
        return;
    }
    t->state = THREAD_READY;
    spin_unlock(&c->run_lock, state);
    enqueue(t, true);
}

/* With q's lock held: every thread off it and woken. */
static void wake_list(struct wait_queue *q)
{
    while (q->head) {
        struct thread *t = q->head;
        q->head = t->wait_next;
        t->wait_next = 0;
        __atomic_store_n(&t->waiting_on, 0, __ATOMIC_RELEASE);
        wake(t);
    }
}

/* (65e) Poll tables: what one call of poll() watches. */
struct poll_entry {
    struct poll_table *table;
    struct wait_queue *q;           /* 0 once the queue was detached */
    struct poll_entry *next;        /* the queue's next watcher */
};

struct poll_table {
    struct wait_queue self;         /* its thread, while it sleeps */
    bool fired;                     /* a watched queue was woken since the reset */
    int used, size;
    struct poll_entry entries[];
};

/* (65e-44) The poll bookkeeping (every queue's watchers, the entries) has
 * its own lock, taken by a wake-up only when someone watches the queue: a
 * queue's owner detaches it while a poll table's owner takes its entries
 * off, so one lock must cover both. Inside it a poll table's own queue. */
static struct spinlock poll_lock = SPINLOCK_INIT("poll");

/* With poll_lock held: the watchers of q learn of a wake-up. */
static void fire_watchers(struct wait_queue *q)
{
    for (struct poll_entry *e = q->watchers; e; e = e->next) {
        __atomic_store_n(&e->table->fired, true, __ATOMIC_RELEASE);
        uint64_t state = lock_queue(&e->table->self);
        wake_list(&e->table->self);
        spin_unlock(&e->table->self.lock, state);
    }
}

/* (65e) No thread on q and no poller watching it: nothing to wake. Until
 * 65e every wake-up took the scheduler's lock: under smpstress on 4 cores
 * 23 million of its 31 million takes in 20 s were wake-ups of empty queues
 * (every mutex_unlock, every pipe and socket change), the spinning on it
 * 1.7 s. Looked at after a fence, as the waiter fences after joining
 * (wait_prepare, poll_watch): the waker has made the condition true before,
 * the waiter looks at it after, so a waiter it misses sees the condition. */
static bool nobody_waits(struct wait_queue *q)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return !__atomic_load_n(&q->head, __ATOMIC_RELAXED) &&
           !(q->pollable && __atomic_load_n(&q->watchers, __ATOMIC_RELAXED));
}

static void wake_watchers(struct wait_queue *q)
{
    if (!q->pollable || !__atomic_load_n(&q->watchers, __ATOMIC_ACQUIRE))
        return;
    uint64_t state = spin_lock(&poll_lock);
    fire_watchers(q);
    spin_unlock(&poll_lock, state);
}

void wait_queue_wake_all(struct wait_queue *q)
{
    if (nobody_waits(q))
        return;
    uint64_t state = lock_queue(q);
    wake_list(q);
    spin_unlock(&q->lock, state);
    wake_watchers(q);
}

void wait_queue_wake_one(struct wait_queue *q)
{
    if (nobody_waits(q))
        return;
    uint64_t state = lock_queue(q);
    struct thread *t = q->head;
    if (t) {
        q->head = t->wait_next;
        t->wait_next = 0;
        __atomic_store_n(&t->waiting_on, 0, __ATOMIC_RELEASE);
        wake(t);
    }
    spin_unlock(&q->lock, state);
    wake_watchers(q);
}

void wait_queue_detach(struct wait_queue *q)
{
    uint64_t state = spin_lock(&poll_lock);
    fire_watchers(q);
    for (struct poll_entry *e = q->watchers; e; e = e->next)
        e->q = 0;
    __atomic_store_n(&q->watchers, 0, __ATOMIC_RELEASE);
    spin_unlock(&poll_lock, state);
}

struct poll_table *poll_table_new(int entries)
{
    struct poll_table *pt = kzalloc(sizeof(*pt) + (size_t)entries * sizeof(pt->entries[0]));
    if (pt)
        pt->size = entries;
    return pt;
}

void poll_table_reset(struct poll_table *pt)
{
    uint64_t state = spin_lock(&poll_lock);
    for (int i = 0; i < pt->used; i++) {
        struct poll_entry *e = &pt->entries[i];
        if (!e->q)
            continue;
        for (struct poll_entry **link = &e->q->watchers; *link; link = &(*link)->next)
            if (*link == e) {
                __atomic_store_n(link, e->next, __ATOMIC_RELEASE);
                break;
            }
        e->q = 0;
    }
    pt->used = 0;
    __atomic_store_n(&pt->fired, false, __ATOMIC_RELAXED);
    spin_unlock(&poll_lock, state);
}

void poll_table_free(struct poll_table *pt)
{
    if (!pt)
        return;
    poll_table_reset(pt);
    kfree(pt);
}

void poll_watch(struct wait_queue *q)
{
    struct thread *t = thread_current();
    struct poll_table *pt = t->poll_table;
    if (!pt || !q)
        return;
    uint64_t state = spin_lock(&poll_lock);
    if (pt->used < pt->size) {
        struct poll_entry *e = &pt->entries[pt->used++];
        e->table = pt;
        e->q = q;
        e->next = q->watchers;
        __atomic_store_n(&q->watchers, e, __ATOMIC_RELEASE);
    } else {
        __atomic_store_n(&pt->fired, true, __ATOMIC_RELAXED);  /* (no room: look again) */
    }
    spin_unlock(&poll_lock, state);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);    /* (65e: as in wait_prepare) */
}

bool poll_table_wait(struct poll_table *pt, uint64_t deadline)
{
    struct thread *self = thread_current();
    return wait_event_until(&pt->self, __atomic_load_n(&pt->fired, __ATOMIC_ACQUIRE) ||
                                       self->stop_requested, deadline) &&
           !self->stop_requested;
}

/* --- called from interrupts ------------------------------------------------------- */

void sched_tick(void)
{
    if (!started)
        return;
    struct cpu *me = arch_this_cpu();       /* (an interrupt: it cannot move) */
    current->ticks++;
    /* Deadlines are the boot core's to watch (65c: one walk per tick). */
    if (me->index == 0) {
        uint64_t state = spin_lock(&sched_lock);
        uint64_t now = timer_ticks();
        for (struct thread *t = all_threads; t; t = t->all_next) {
            /* (65e-44) Made runnable only: it takes itself off its queue
             * and sees the deadline (a look at an older wait's deadline
             * wakes it early: it looks again and sleeps on). */
            uint64_t at = __atomic_load_n(&t->wake_tick, __ATOMIC_RELAXED);
            if (t->state == THREAD_BLOCKED && at && now >= at)
                wake(t);
        }
        spin_unlock(&sched_lock, state);
    }
    /* (65e-39) A busy core evens out a queue two longer than its own now
     * and then (an idle one takes threads in its loop). */
    if (cores_share && current != idle && me->ticks % BALANCE_TICKS == 0)
        take_from_others(me, 2);
    if (current == idle ? __atomic_load_n(&me->run_count, __ATOMIC_RELAXED) && takes_threads()
                        : --slice_left <= 0)
        need_resched = true;
}

void sched_irq_enter(void)
{
    irq_depth++;
}

void sched_irq_exit(void)
{
    irq_depth--;
}

void sched_share_cores(void)
{
    uint64_t state = spin_lock(&sched_lock);
    cores_share = true;
    spin_unlock(&sched_lock, state);
}

void sched_reschedule_interrupt(void)
{
    cpus_answer();                          /* (65d) a core asked this one something */
    if (takes_threads())
        need_resched = true;
}

bool sched_in_interrupt(void)
{
    uint64_t state = arch_irq_save();       /* (65c-3: as thread_current) */
    bool in = irq_depth > 0;
    arch_irq_restore(state);
    return in;
}

void sched_preempt_point(bool interrupts_were_on)
{
    if (!started || !need_resched || !interrupts_were_on || irq_depth)
        return;
    uint64_t state = lock_here();
    if (need_resched)
        schedule();
    unlock_here(state);
}

/* --- self-test ------------------------------------------------------------------- */

static volatile int last_runner;
static volatile uint64_t turns;
static volatile int finished;
static uint64_t test_end;
static struct wait_queue test_done;

static void spinner(void *arg)
{
    int me = (int)(uint64_t)arg;
    while (timer_ticks() < test_end) {
        if (last_runner != me) {            /* the other one ran since we last looked */
            last_runner = me;
            turns++;
        }
    }
    uint64_t state = arch_irq_save();
    finished++;
    arch_irq_restore(state);
    wait_queue_wake_all(&test_done);
}

void sched_self_test(void)
{
    const uint64_t ms = 300;
    last_runner = 0;
    turns = 0;
    finished = 0;
    test_end = timer_ticks() + ms * TIMER_HZ / 1000;
    if (!thread_create("spin-1", spinner, (void *)1) ||
        !thread_create("spin-2", spinner, (void *)2)) {
        kprintf("  scheduler:     self-test: out of memory\n");
        return;
    }
    wait_event(&test_done, finished == 2);
    kprintf("  scheduler:     2 threads computing in the kernel for %lu ms took %lu turns "
            "(preemption)\n", ms, turns);
}

/* --- information -------------------------------------------------------------- */

void sched_print_threads(void)
{
    static const char *const states[] = { "ready", "running", "waiting", "ended" };
    /* (65d) Every thread (until then at most 32: a machine with 16 cores
     * has 16 idle threads), copied under the lock into a list of its own. */
    struct row {
        int id, state, cpu;
        char name[16];
        uint64_t ticks, switches;
    };
    int room = 0;
    uint64_t state = spin_lock(&sched_lock);
    for (struct thread *t = all_threads; t; t = t->all_next)
        room++;
    spin_unlock(&sched_lock, state);
    room += 16;                             /* (some may come meanwhile) */
    struct row *rows = kmalloc(sizeof(*rows) * (size_t)room);
    if (!rows)
        return;
    int n = 0;
    state = spin_lock(&sched_lock);
    for (struct thread *t = all_threads; t && n < room; t = t->all_next) {
        struct row *r = &rows[n++];
        r->id = t->id;
        r->state = t->state;
        r->cpu = t->cpu;
        memcpy(r->name, t->name, sizeof(r->name));
        r->ticks = t->ticks;
        r->switches = t->switches;
    }
    spin_unlock(&sched_lock, state);
    kprintf("  id  name             state     cpu   ticks  switches\n");
    for (int i = n - 1; i >= 0; i--) {              /* oldest first */
        char cpu[8] = "  -";
        if (rows[i].cpu >= 0)
            ksnprintf(cpu, sizeof(cpu), "%3d", rows[i].cpu);
        kprintf("  %2d  %-16.16s %-8s %s  %6lu  %8lu\n", rows[i].id, rows[i].name,
                states[rows[i].state], cpu, rows[i].ticks, rows[i].switches);
    }
    kfree(rows);
    cpus_print_clock();
    lockcheck_report();
}

/*
 * Threads and the scheduler.
 *
 * A thread is a line of execution with its own kernel stack. The scheduler
 * gives the CPU to the ready threads in turn (round robin): the timer takes
 * it away after a time slice, at the end of an interrupt, whether the thread
 * ran a program or kernel code (a preemptible kernel). Only code that has
 * interrupts disabled cannot be interrupted, and so cannot be switched away
 * from: that is what spinlocks are (kernel/sync.h).
 *
 * A thread that waits (for input, a disk, a lock, a time) sleeps on a wait
 * queue and is woken by whoever makes its condition true, often an
 * interrupt handler. With nothing to do, the idle thread halts the CPU.
 */
#ifndef RITTEROS_SCHED_H
#define RITTEROS_SCHED_H

#include <stdbool.h>
#include <stdint.h>
#include "driver.h"
#include "arch.h"

enum thread_state { THREAD_READY, THREAD_RUNNING, THREAD_BLOCKED, THREAD_DEAD };

struct wait_queue;
struct kprintf_capture;

struct thread {
    uint64_t sp;                    /* saved stack pointer (arch_context_switch) */
    enum thread_state state;
    int id;
    char name[16];
    uint64_t stack;                 /* physical address of its kernel stack; 0: the boot stack */
    struct thread *next;            /* in a run queue */
    struct thread *wait_next;       /* (65e-44) in a wait queue: a thread made runnable by
                                     * its deadline or a stop is on both until it leaves */
    struct thread *all_next;        /* in the list of all threads */
    struct wait_queue *waiting_on;  /* while blocked on a queue */
    uint64_t wake_tick;             /* while blocked: wake at this tick (0: never) */
    bool timed_out;
    bool yielding;              /* (64l) in thread_yield: to the queue's end */
    int slice_rest;                 /* (65e-39) put first by a woken one: its slice's rest */
    bool queued;                    /* (65c-3) on a run queue (the check against twice) */
    int cpu;                        /* (65d) the core it runs on or last ran on; -1: none yet */
    struct poll_table *poll_table;  /* (65e) while in poll(): the queues it watches */
    bool on_cpu;                    /* (65a-3) running on a core (or being switched away
                                     * from it): its stack is in use */
    uint64_t ticks;                 /* timer ticks it was running for */
    uint64_t switches;              /* times it got the CPU */
    uint64_t cpu_counts;            /* CPU time it ran, in arch_counter counts (22o) */
    uint64_t ran_since;             /* arch_counter when it last got the CPU */
    uint64_t sig_blocked;           /* signals held back while its handlers run (22r) */
    uint64_t suspend_mask;          /* (52i) sigsuspend: the set to have again afterwards */
    bool suspended;                 /* (52i) ... until the handler it waited for returns */
    uint64_t enter_at;              /* (54c) run this program address on the way back (0:
                                     * none): pthread_cancel's asynchronous kind */
    bool dont_wait;                 /* (53b) this call: O_NONBLOCK or MSG_DONTWAIT -- a wait
                                     * becomes ERR_AGAIN (what was done so far, if any) */
    struct process *process;        /* the program it runs, or 0 (a kernel thread) */
    void *fpu;                      /* its program's FP/SIMD registers (64-byte aligned) */
    void *fpu_block;                /* the allocation that holds them */
    uint64_t space;                 /* the address space it needs (0: the kernel's only) */
    uint64_t tp;                    /* its program's thread pointer (22i: per-thread data) */
    uint64_t segments;              /* (65c-3) arch_user_segments: x86 its GS selector */
    volatile bool stop_requested;   /* its process is to end (Ctrl-C, kill) */
    bool net_rx;                    /* delivering a received frame: do not block on ARP (49c) */
    int net_batch;                  /* (65e) in a card's burst of frames (net_batch_begin) */
    int net_batch_frames;           /* (65e) frames in it since its ACKs last went out */
    /* (65e-36) The readers-writer locks it holds to read, and how deep. */
#define RW_READS 4
    struct rwlock *reading[RW_READS];
    uint16_t reading_depth[RW_READS];
    struct kprintf_capture *capture; /* kprintf goes here instead (kernel/kprintf.h) */
    uint8_t log_quiet;              /* its kprintf is not the kernel's message (kernel/log.h):
                                     * a program's output, the terminal's echo */
};

/* struct wait_queue (driver.h, 67b: the drivers' too): sleeping threads in
 * arrival order; pollable (52d): a wake-up there also wakes poll()'s
 * callers; watchers (65e): those that watch the queue. */

/* The boot code becomes thread 0 ("main"), an idle thread is made, and
 * scheduling starts. Needs the heap and the timer tick. */
void sched_init(void);
bool sched_running(void);
/* Free the stacks of threads that have ended now (else the idle loop or
 * the next thread made does): for counts of free memory that must be
 * exact. Waits up to 1 ms for one still leaving its core. */
void sched_reap(void);

/* A new kernel thread that runs fn(arg) and ends when fn returns. */
struct thread *thread_create(const char *name, void (*fn)(void *arg), void *arg);

/* The same for a program's thread: it runs in process's address space. */
struct process;
struct thread *thread_create_for(const char *name, void (*fn)(void *arg), void *arg,
                                 struct process *process, uint64_t space);

/* The top of a thread's kernel stack. */
uint64_t thread_stack_top(const struct thread *t);

/* Change the running thread's address space (0: none, e.g. when its
 * program has ended). */
void sched_set_space(uint64_t space);

/* Ask a thread to stop: set stop_requested and wake it if it sleeps, so
 * that an interruptible wait returns. Interrupt context is fine. */
void sched_request_stop(struct thread *t);
struct thread *thread_current(void);
/* The CPU time thread t has run (22o), in arch_counter counts: what it ran
 * before, and the running thread's time since it got the CPU. */
uint64_t sched_cpu_counts(struct thread *t);
__attribute__((noreturn)) void thread_exit(void);
void thread_yield(void);

/* Sleep until the timer reaches tick. */
void sched_sleep_until(uint64_t tick);

/*
 * (65a-3) A wait in three steps, so that a wake-up between the look at a
 * condition and the sleep is never lost (another core or an interrupt may
 * make it true then): wait_prepare puts the thread on q (until the timer
 * reaches deadline, 0: no limit), the caller then looks at its condition,
 * and either wait_cancel takes it off again or wait_commit sleeps, unless
 * it was woken meanwhile; false after the deadline. A caller whose
 * condition is guarded by its own spinlock prepares while holding it and
 * commits after giving it back (the mutex, the futex). Before scheduling
 * starts, wait_commit just waits for the next interrupt: callers look again
 * in a loop. The wait_event macros below are the usual way.
 */
void wait_prepare(struct wait_queue *q, uint64_t deadline);
void wait_cancel(void);
bool wait_commit(uint64_t deadline);

/* Wake every thread sleeping on q (from interrupt handlers too). */
void wait_queue_wake_all(struct wait_queue *q);
/* Wake the thread that has slept longest. */
void wait_queue_wake_one(struct wait_queue *q);

/*
 * (65e) poll(): while it looks at its descriptors, the calling thread's
 * poll table records every queue they would be woken on (poll_watch, from
 * each kind's readiness function, before it reads the state); a wake-up of
 * such a queue marks the table fired and wakes that thread alone (until
 * 65e every wake-up of any pollable queue woke every thread in poll()).
 * poll_table_reset forgets the queues (each look starts afresh), and
 * poll_table_wait sleeps until fired, the deadline or a stop request
 * (false then). A pollable queue about to be freed is detached first
 * (wait_queue_detach): its watchers are fired and forgotten.
 */
struct poll_table;
struct poll_table *poll_table_new(int entries);
void poll_table_free(struct poll_table *pt);
void poll_table_reset(struct poll_table *pt);
bool poll_table_wait(struct poll_table *pt, uint64_t deadline);
void poll_watch(struct wait_queue *q);
void wait_queue_detach(struct wait_queue *q);

/* Sleep on q until cond is true (looked at with interrupts disabled, and
 * again after joining q, so a wake-up between the look and the sleep cannot
 * be missed). */
#define wait_event(q, cond) do {                                            \
        uint64_t state_ = arch_irq_save();                                  \
        while (!(cond)) {                                                   \
            wait_prepare((q), 0);                                           \
            if (cond) {                                                     \
                wait_cancel();                                              \
                break;                                                      \
            }                                                               \
            wait_commit(0);                                                 \
        }                                                                   \
        arch_irq_restore(state_);                                           \
    } while (0)

/* Sleep on q until cond is true or the thread is asked to stop (a program
 * being ended); true if cond became true. For waits on behalf of a program:
 * reading input, waiting for a child, sleeping. Once cond is true it is not
 * looked at again (65e: it may take what it found, as waitpid's takes the
 * ended child: until 65e a true cond after joining q was looked at a second
 * time, found nothing left, and waitpid said ECHILD for a child that had
 * just ended on another core). */
#define wait_event_interruptible(q, cond) ({                                \
        uint64_t state_ = arch_irq_save();                                  \
        bool ok_ = true;                                                    \
        while (!(cond)) {                                                   \
            if (thread_current()->stop_requested) {                         \
                ok_ = false;                                                \
                break;                                                      \
            }                                                               \
            wait_prepare((q), 0);                                           \
            if (cond) {                                                     \
                wait_cancel();                                              \
                break;                                                      \
            }                                                               \
            if (thread_current()->stop_requested) {                         \
                wait_cancel();                                              \
                continue;                                                   \
            }                                                               \
            wait_commit(0);                                                 \
        }                                                                   \
        arch_irq_restore(state_);                                           \
        ok_;                                                                \
    })

/* The same with a deadline (timer tick); true if cond became true. */
#define wait_event_until(q, cond, deadline) ({                              \
        uint64_t state_ = arch_irq_save();                                  \
        bool ok_ = true;                                                    \
        while (!(cond)) {                                                   \
            wait_prepare((q), (deadline));                                  \
            if (cond) {                                                     \
                wait_cancel();                                              \
                break;                                                      \
            }                                                               \
            if (!wait_commit(deadline)) {                                   \
                ok_ = (cond);                                               \
                break;                                                      \
            }                                                               \
        }                                                                   \
        arch_irq_restore(state_);                                           \
        ok_;                                                                \
    })

/* --- called by the architecture and timer code -------------------------- */

/* Every timer tick (interrupt context): time slices, sleepers' deadlines. */
void sched_tick(void);

/* (65c) A started core becomes its idle thread there (never returns). */
struct cpu;
__attribute__((noreturn)) void sched_core_enter(struct cpu *c, uint64_t stack);

/* (65c) Another core asked this one to look at the run queue (an
 * interrupt's handler: the switch comes at its end). */
void sched_reschedule_interrupt(void);

/* (65c-2) Every online core takes threads from the run queues from now on. */
void sched_share_cores(void);

/* Around the handling of a device or timer interrupt. */
void sched_irq_enter(void);
void sched_irq_exit(void);
bool sched_in_interrupt(void);

/* At the end of an interrupt or system call, interrupts disabled: switch
 * threads if one is due and the interrupted code had interrupts enabled. */
void sched_preempt_point(bool interrupts_were_on);

/* A new thread's first code (thread_start in switch.S). */
void sched_thread_start(void);

/* Two kernel threads compute without ever waiting; they must take turns,
 * which only preemption can make them do. Logs the result. */
void sched_self_test(void);

/* One line per thread (the terminal's threads command). */
void sched_print_threads(void);

#endif

/*
 * Each processor core's own state (65a, roadmap item 31): what the
 * scheduler and the entry code keep per core. A core finds its block
 * through a register only the kernel uses (arch_this_cpu: x86's GS base,
 * swapped with the program's on every entry from and exit to user mode;
 * ARM's TPIDR_EL1), so no core ever reads another's by mistake. The boot
 * core's is boot_cpu; the others' are made by kernel/cpus.c (65b).
 */
#ifndef RITTEROS_PERCPU_H
#define RITTEROS_PERCPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver.h"                 /* (65e-39) struct spinlock */

struct thread;

/* Spinlocks a core remembers holding (65a-5); more are only counted. */
#define CPU_LOCKS_KEPT 16
#define IDLE_STATES 4                   /* (37d-2) C1 and up to three deeper */

/* (65e) The heap's size classes (16 to 1024 bytes) and the free blocks a
 * core keeps of each. */
#define HEAP_CLASSES 7
#define HEAP_CACHE   32

/* (65e-47) The free pages a core keeps (kernel/pmm.c). */
#define PMM_CACHE 64
/* (35e) The pages from the top of memory a core keeps for programs. */
#define PMM_HIGH 32

struct cpu {
    /* The first four are read by the entry code at fixed offsets
     * (arch/x86_64/user.S, arch/aarch64/vectors.S): keep them first and in
     * this order. */
    struct cpu *self;                   /* 0: what %gs:0 gives */
    uint64_t user_rsp;                  /* 8: the program's RSP during syscall_entry */
    uint64_t syscall_stack;             /* 16: the running thread's kernel stack top */
    uint64_t exception_stack;           /* 24: (65c-2) ARM's SP_EL1 for exceptions, its top */
    int index;                          /* 0 for the boot core */
    struct thread *current;             /* the thread running here */
    struct thread *idle;                /* runs when nothing else can */
    int irq_depth;                      /* interrupt handlers running (they do not nest) */
    int slice_left;                     /* timer ticks left of the current thread's slice */
    volatile bool need_resched;         /* switch at the next chance (interrupt's end) */
    uint64_t active_space;              /* the address space in the lower half now */
    struct thread *switched_from;       /* (65a-3) the thread this core just left */
    int locks_held;                     /* (65a-5) spinlocks this core holds now */
    struct spinlock *held[CPU_LOCKS_KEPT];  /* the first of them, in the order taken */
    /* (65b) Starting the core (kernel/cpus.c). */
    uint64_t hw_id;                     /* x86: Local APIC ID; ARM: MPIDR's affinity */
    volatile int state;                 /* CPU_* */
    uint64_t stack_top;                 /* its first stack (a parked core's) */
    uint64_t start_counts;              /* arch_counter() counts until it reported in */
    uint64_t ticks;                     /* (65c) timer interrupts on this core */
    uint64_t interrupts;                /* (65d) devices' interrupts handled here */
    /* (65d) The clock: counters read when asked (clock_asked), and what the
     * last list of them saw. */
    volatile int clock_asked;
    volatile int heap_asked;            /* (65e) give the heap's blocks back (heap_trim) */
    bool clock_valid;
    uint64_t clock_actual, clock_base, clock_at;
    uint64_t clock_last_actual, clock_last_base, clock_last_at;
    bool kicked;                        /* (65c-2) sent a reschedule interrupt, not yet looked */
    volatile int tlb_pending;           /* (65c-2) x86: a page to forget (tlb_page) */
    uint64_t tlb_page;
    /* (65e) Free small heap blocks of each size class kept by this core, so
     * that most kmalloc and kfree calls take no lock (kernel/heap.c); the
     * last one is given out first. Others only read them (with acquire). */
    void *heap_cache[HEAP_CLASSES][HEAP_CACHE];
    int heap_cached[HEAP_CLASSES];
    /* (65e-39) Its run queue (kernel/sched.c), under run_lock ("run
     * queue"): threads just woken first (64l), then the others in order.
     * run_count is read by other cores without the lock (where to put a
     * thread, where to take one from). */
    struct spinlock run_lock;
    struct thread *woken_head, *woken_tail, *run_head, *run_tail;
    int run_count;
    uint64_t taken;                     /* threads it took from other cores' queues */
    /* (65e-42) Looks at a program's descriptor slot without its lock under
     * way here (interrupts off; kernel/process.c's fd_use_peek). */
    int fd_peeks;
    /* (65e-47) Free pages kept by this core (kernel/pmm.c), under
     * page_lock ("pmm cache"): most pmm_alloc_page and pmm_free_page calls
     * take only it; another core takes it only to empty the cache when
     * memory runs short. */
    struct spinlock page_lock;
    uint64_t pages[PMM_CACHE];
    int pages_kept;
    /* (35e) With swap on, programs' pages from the top of memory
     * (pmm_alloc_page_high), PMM_HIGH at a time, also under page_lock. */
    uint64_t high[PMM_HIGH];
    int high_kept;
    /* (37d-2) The idle thread's sleep (arch_idle): its running average of
     * idle periods (counter ticks), and per state how often and how long. */
    uint64_t idle_predicted;
    uint64_t idle_entries[IDLE_STATES], idle_ticks[IDLE_STATES];
};

enum {
    CPU_RUNNING,                        /* the boot core */
    CPU_STARTING,                       /* asked to start, not reported in yet */
    CPU_ONLINE,                         /* started, taking interrupts (65c) */
    CPU_FAILED,                         /* did not report in */
};

_Static_assert(offsetof(struct cpu, self) == 0, "user.S reads %gs:0");
_Static_assert(offsetof(struct cpu, user_rsp) == 8, "user.S keeps the program's RSP at %gs:8");
_Static_assert(offsetof(struct cpu, syscall_stack) == 16, "user.S loads its stack from %gs:16");
_Static_assert(offsetof(struct cpu, exception_stack) == 24, "vectors.S reads it at 24");

extern struct cpu boot_cpu;

/* Make boot_cpu this core's block: the first thing kernel_main does, as
 * kprintf already asks for the current thread. */
void percpu_init_boot(void);

#endif

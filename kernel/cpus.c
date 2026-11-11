/*
 * The processor's cores (65b-65c): which there are, starting them, the
 * list. A started core runs its architecture's start-up code on a stack of
 * its own, takes its block (struct cpu), sets up its interrupts and timer
 * and reports in; its code then becomes its idle thread, idleN, and it
 * takes threads like the boot core. Every core is started (the user's
 * choice after 65c, once the J1900 and the laptop ran on all of theirs);
 * cpus=N starts at most N (cpus=1: the boot core alone).
 */
#include "cpus.h"
#include "ritterfs.h"
#include "arch.h"
#include "cmdline.h"
#include "heap.h"
#include "kprintf.h"
#include "memory.h"
#include "pci.h"
#include "pmm.h"
#include "sched.h"
#include "sync.h"

#define CORE_STACK_PAGES 8                  /* 32 KiB: sched.c's STACK_PAGES, as it becomes a thread's */

static struct cpu_entry entries[MAX_CPUS];
static struct cpu *cpus[MAX_CPUS];          /* by entry; 0 where not started */
static int entry_count, limit;
static bool limit_given;
/* (65c-2) The cores taking interrupts, the boot core first: the scheduler
 * looks for an idle one among them. Only added to, the count last. */
static struct cpu *online_cores[MAX_CPUS] = { &boot_cpu };
static int online_count = 1;
static struct cpu *by_index[MAX_CPUS] = { &boot_cpu };  /* (65e-39) */

void cpu_online(struct cpu *c)
{
    c->start_counts = arch_counter() - c->start_counts;
    __atomic_store_n(&c->state, CPU_ONLINE, __ATOMIC_RELEASE);
    /* Cores start one at a time: no other core adds itself meanwhile. */
    by_index[c->index] = c;
    online_cores[online_count] = c;
    __atomic_store_n(&online_count, online_count + 1, __ATOMIC_RELEASE);
    sched_core_enter(c, virt_to_phys((const void *)(c->stack_top -
                                                    CORE_STACK_PAGES * PMM_PAGE_SIZE)));
}

int cpus_online(void)
{
    return __atomic_load_n(&online_count, __ATOMIC_ACQUIRE);
}

struct cpu *cpu_online_at(int i)
{
    return online_cores[i];
}

void cpus_kick_idle(void)
{
    struct cpu *me = arch_this_cpu();
    int n = cpus_online();
    for (int i = 0; i < n; i++) {
        struct cpu *c = online_cores[i];
        if (c != me && c->current == c->idle && !c->kicked) {
            c->kicked = true;
            arch_wake_core(c);
            return;
        }
    }
}

struct cpu *cpu_of(int index)
{
    struct cpu *c = index >= 0 && index < MAX_CPUS ? by_index[index] : 0;
    return c ? c : &boot_cpu;
}

bool cpus_core_idle(const struct cpu *c)
{
    struct thread *idle = __atomic_load_n(&c->idle, __ATOMIC_ACQUIRE);
    return idle && __atomic_load_n(&c->current, __ATOMIC_RELAXED) == idle &&
           !__atomic_load_n(&c->run_count, __ATOMIC_RELAXED);
}

struct cpu *cpus_find_idle(const struct cpu *me)
{
    int n = cpus_online();
    for (int i = 0; i < n; i++)
        if (online_cores[i] != me && cpus_core_idle(online_cores[i]))
            return online_cores[i];
    return 0;
}

void cpus_wait_fd_peeks(void)
{
    int n = cpus_online();
    for (int i = 0; i < n; i++)
        while (__atomic_load_n(&online_cores[i]->fd_peeks, __ATOMIC_ACQUIRE))
            arch_cpu_relax();               /* (a look is a few instructions) */
}

void cpus_kick(int index)
{
    int n = cpus_online();
    for (int i = 0; i < n; i++)
        if (online_cores[i]->index == index) {
            arch_wake_core(online_cores[i]);
            return;
        }
}

void cpus_wait_space_unused(uint64_t space)
{
    /* (The caller has these tables out of place itself: every core may be
     * looked at, also the one it moves to meanwhile.) */
    int n = cpus_online();
    for (int i = 0; i < n; i++) {
        struct cpu *c = online_cores[i];
        while (__atomic_load_n(&c->active_space, __ATOMIC_ACQUIRE) == space)
            arch_cpu_relax();
    }
}

void cpus_answer(void)
{
    struct cpu *me = arch_this_cpu();
    if (__atomic_load_n(&me->heap_asked, __ATOMIC_ACQUIRE)) {
        heap_cache_flush();
        __atomic_store_n(&me->heap_asked, 0, __ATOMIC_RELEASE);
    }
    if (__atomic_load_n(&me->clock_asked, __ATOMIC_ACQUIRE)) {
        me->clock_valid = arch_clock_counts(&me->clock_actual, &me->clock_base);
        me->clock_at = arch_counter();
        __atomic_store_n(&me->clock_asked, 0, __ATOMIC_RELEASE);
    }
}

/* The clock and the share of time running from the counters' growth (da
 * real cycles, db cycles at the counter's rate hz, over dt counts). */
static void clock_of(uint64_t da, uint64_t db, uint64_t dt, uint64_t hz, uint64_t *mhz,
                     uint64_t *running)
{
    uint64_t ran = db;                      /* shifted copies, so the products fit */
    while (dt >> 50)
        dt >>= 1, ran >>= 1;
    *running = dt ? ran * 1000 / dt : 0;
    if (*running > 1000)
        *running = 1000;
    while (da >> 40 || db >> 40)
        da >>= 1, db >>= 1;
    *mhz = db ? hz / 1000 * da / db / 1000 : 0;
}

void cpus_flush_heap_caches(void)
{
    /* Interrupts off while this core flushes its own and asks the others;
     * on again for the wait (this thread may move: it only reads flags). */
    uint64_t irq = arch_irq_save();
    struct cpu *me = arch_this_cpu();
    heap_cache_flush();
    int n = cpus_online();
    for (int i = 0; i < n; i++)
        if (online_cores[i] != me) {
            __atomic_store_n(&online_cores[i]->heap_asked, 1, __ATOMIC_RELEASE);
            arch_wake_core(online_cores[i]);
        }
    arch_irq_restore(irq);
    uint64_t hz = arch_counter_hz(), start = arch_counter();
    for (int i = 0; i < n; i++)
        while (__atomic_load_n(&online_cores[i]->heap_asked, __ATOMIC_ACQUIRE) &&
               hz && arch_counter() - start < hz / 10)          /* 100 ms at most */
            arch_cpu_relax();
}

void cpus_print_clock(void)
{
    uint64_t hz = arch_counter_hz();
    int n = cpus_online();
    if (!hz)
        return;
    /* Every core reads its own counters, this one too (by an interrupt to
     * itself: whichever core this thread is on by then). */
    for (int i = 0; i < n; i++) {
        __atomic_store_n(&online_cores[i]->clock_asked, 1, __ATOMIC_RELEASE);
        arch_wake_core(online_cores[i]);
    }
    uint64_t start = arch_counter();
    for (int i = 0; i < n; i++)
        while (__atomic_load_n(&online_cores[i]->clock_asked, __ATOMIC_ACQUIRE) &&
               arch_counter() - start < hz / 20)            /* 50 ms at most */
            arch_cpu_relax();
    bool first = !online_cores[0]->clock_last_at;
    uint64_t seconds = first ? 0 : (online_cores[0]->clock_at - online_cores[0]->clock_last_at) / hz;
    bool any = false;
    for (int i = 0; i < n; i++) {
        struct cpu *c = online_cores[i];
        if (__atomic_load_n(&c->clock_asked, __ATOMIC_ACQUIRE) || !c->clock_valid)
            continue;
        uint64_t mhz, running;
        clock_of(c->clock_actual - c->clock_last_actual, c->clock_base - c->clock_last_base,
                 c->clock_at - c->clock_last_at, hz, &mhz, &running);
        c->clock_last_actual = c->clock_actual;
        c->clock_last_base = c->clock_base;
        c->clock_last_at = c->clock_at;
        if (n == 1) {                       /* one core: the line as since 64t */
            if (first)
                kprintf("  clock: %lu MHz while running (base %lu MHz), running %lu.%lu%% of "
                        "the time since it was switched on\n", mhz, hz / 1000000,
                        running / 10, running % 10);
            else
                kprintf("  clock: %lu MHz while running (base %lu MHz), running %lu.%lu%% of "
                        "the time in the %lu s since the last look\n", mhz, hz / 1000000,
                        running / 10, running % 10, seconds);
            return;
        }
        if (!any) {
            if (first)
                kprintf("  clock (base %lu MHz), since the machine was switched on:\n",
                        hz / 1000000);
            else
                kprintf("  clock (base %lu MHz), in the %lu s since the last look:\n",
                        hz / 1000000, seconds);
            any = true;
        }
        kprintf("    core %2d: %4lu MHz while running, running %lu.%lu%% of the time\n",
                c->index, mhz, running / 10, running % 10);
    }
}

static uint64_t micros(uint64_t counts)
{
    uint64_t hz = arch_counter_hz();
    return hz ? counts / (hz / 1000000 ? hz / 1000000 : 1) : 0;
}

void cpus_start(const char *cmdline)
{
    char value[8];
    limit = MAX_CPUS;
    limit_given = cmdline_get(cmdline, "cpus", value, sizeof(value));
    if (limit_given) {
        int n = 0;
        for (const char *s = value; *s >= '0' && *s <= '9'; s++)
            n = n * 10 + (*s - '0');
        limit = n < 1 ? 1 : n > MAX_CPUS ? MAX_CPUS : n;
    }
    entry_count = arch_cpus_list(entries, MAX_CPUS);
    int online = 0, failed = 0, left = 0, refused = 0;
    uint64_t began = arch_counter();
    for (int i = 0; i < entry_count; i++) {
        struct cpu_entry *e = &entries[i];
        if (e->boot) {
            boot_cpu.hw_id = e->hw_id;
            cpus[i] = &boot_cpu;
            continue;
        }
        if (e->refused) {
            refused++;
            continue;
        }
        if (1 + online + failed >= limit) {
            left++;
            continue;
        }
        struct cpu *c = kzalloc(sizeof(*c));
        uint64_t stack = pmm_alloc_pages(CORE_STACK_PAGES);
        if (!c || !stack) {
            kfree(c);
            e->refused = "no memory for its block and stack";
            refused++;
            continue;
        }
        c->self = c;
        c->run_lock.name = "run queue";     /* (65e-39) */
        c->page_lock.name = "pmm cache";    /* (65e-47) */
        c->index = 1 + online + failed;
        c->hw_id = e->hw_id;
        c->state = CPU_STARTING;
        c->stack_top = (uint64_t)phys_to_virt(stack) + CORE_STACK_PAGES * PMM_PAGE_SIZE;
        cpus[i] = c;
        if (arch_cpu_start(c)) {
            online++;
        } else {
            /* It may still come later: its block and stack stay. */
            c->state = CPU_FAILED;
            failed++;
        }
    }
    uint64_t took = micros(arch_counter() - began);
    if (online)
        sched_share_cores();                /* (65c-2) every core takes threads now */
    char more[96] = "";
    int at = 0;
    if (failed)
        at += ksnprintf(more + at, sizeof(more) - at, ", %d did not start", failed);
    if (left)
        at += ksnprintf(more + at, sizeof(more) - at, ", %d left off (%s%d)", left,
                        limit_given ? "cpus=" : "at most ", limit);
    if (refused)
        ksnprintf(more + at, sizeof(more) - at, ", %d cannot be started", refused);
    char time[24];
    if (took < 1000)
        ksnprintf(time, sizeof(time), "%lu us", took);
    else
        ksnprintf(time, sizeof(time), "%lu.%lu ms", took / 1000, took % 1000 / 100);
    kprintf("  cores:         %d in the MADT: the boot core, %d more started (%s)%s\n",
            entry_count, online, time, more);
    for (int i = 0; i < entry_count; i++)
        if (entries[i].refused)
            kprintf("                 core 0x%lx: %s\n", entries[i].hw_id, entries[i].refused);
    pci_spread_interrupts();                /* (65d) the network cards' to other cores */
}

void cpus_print(void)
{
    kprintf("  core  id      state\n");
    for (int i = 0; i < entry_count; i++) {
        struct cpu_entry *e = &entries[i];
        struct cpu *c = cpus[i];
        char index[8] = "     -";
        if (c)
            ksnprintf(index, sizeof(index), "%6d", c->index);
        kprintf("%s  0x%-4lx  ", index, e->hw_id);
        int state = c ? __atomic_load_n(&c->state, __ATOMIC_ACQUIRE) : -1;
        /* (65e-39) And how many threads it took from other cores' queues. */
        if (e->boot) {
            kprintf("running (the boot core), %lu ticks, %lu device interrupts, %lu threads "
                    "taken from other cores\n", c->ticks, c->interrupts, c->taken);
        } else if (state == CPU_ONLINE) {
            uint64_t us = micros(c->start_counts);
            if (us < 1000)
                kprintf("online, started in %lu us, %lu ticks, %lu device interrupts, %lu "
                        "threads taken from other cores\n", us, c->ticks, c->interrupts,
                        c->taken);
            else
                kprintf("online, started in %lu.%lu ms, %lu ticks, %lu device interrupts, %lu "
                        "threads taken from other cores\n", us / 1000, us % 1000 / 100,
                        c->ticks, c->interrupts, c->taken);
        } else if (state == CPU_FAILED || state == CPU_STARTING) {
            kprintf("did not start\n");
        } else if (e->refused) {
            kprintf("cannot be started: %s\n", e->refused);
        } else {
            kprintf("left off (%s%d)\n", limit_given ? "cpus=" : "at most ", limit);
        }
    }
    lock_wait_report();                     /* (65e) what the cores wait for */
    rfs_hold_report();                      /* (65e) who holds the file systems' lock */
}

void cpus_idle_totals(uint64_t entries[], uint64_t ticks[])
{
    for (int i = 0; i < IDLE_STATES; i++)
        entries[i] = ticks[i] = 0;
    int n = cpus_online();
    for (int c = 0; c < n; c++) {
        struct cpu *core = cpu_online_at(c);
        for (int i = 0; core && i < IDLE_STATES; i++) {
            entries[i] += __atomic_load_n(&core->idle_entries[i], __ATOMIC_RELAXED);
            ticks[i] += __atomic_load_n(&core->idle_ticks[i], __ATOMIC_RELAXED);
        }
    }
}

/*
 * The processor's cores (65b-65c, roadmap item 31). The firmware lists them
 * (ACPI's MADT); cpus_start starts every other core (`cpus=N` on the
 * kernel command line: at most N in all), and each sets itself up and
 * becomes its idle thread.
 */
#ifndef RITTEROS_CPUS_H
#define RITTEROS_CPUS_H

#include <stdbool.h>
#include "percpu.h"

#define MAX_CPUS 64

/* Start the other cores (at the boot, after the scheduler); logs a line. */
void cpus_start(const char *cmdline);

/* Called by a starting core once it is set up (interrupts, timer): it
 * reports in and becomes its idle thread. Never returns. */
__attribute__((noreturn)) void cpu_online(struct cpu *c);

/* How many cores take interrupts now (1 until others are started). */
int cpus_online(void);

/* (65c-2) The i-th of them (0: the boot core), i < cpus_online(). */
struct cpu *cpu_online_at(int i);

/* (65c-2) With the scheduler's lock held: interrupt one idle core other
 * than this one (none kicked already), so that it takes a ready thread. */
void cpus_kick_idle(void);

/* (65e-39) The core with this index (the boot core's for none), and
 * whether c runs its idle thread with nothing queued (a look without its
 * lock: it may change at once). */
struct cpu *cpu_of(int index);
bool cpus_core_idle(const struct cpu *c);
/* An idle core other than this one, or 0. */
struct cpu *cpus_find_idle(const struct cpu *me);

/* (65e-42) Until every core's looks at descriptor slots without a lock
 * have been seen at none once: a look that began before is over. */
void cpus_wait_fd_peeks(void);

/* (65d) Interrupt the core with this index (sched_request_stop: a thread
 * running there should look at once). */
void cpus_kick(int index);

/* (65c-2) Wait until no other core has this address space in place (a
 * program's last thread may still be switching away there): before its
 * tables are freed. */
void cpus_wait_space_unused(uint64_t space);

/* The cores' list, for the rescue console's `cpus` and REPORT_CPUS. */
void cpus_print(void);

/* (65d) Each core's clock while it ran since the last look (APERF/MPERF on
 * x86; nothing where the processor has no such counters), for `threads`:
 * every core is asked by an interrupt to read its own counters, so nothing
 * is measured between looks. */
void cpus_print_clock(void);

/* (65d) A core's answers to what another asked it (cpus_print_clock):
 * called from the reschedule interrupt's handler. */
void cpus_answer(void);

/* (65e) Every core gives the free heap blocks it keeps back to the slabs
 * (heap_trim): this one at once, the others by an interrupt, waited for
 * 100 ms at most. */
void cpus_flush_heap_caches(void);

/* (37d-2) Every core's idle entries and counter ticks per state, added up
 * (read without a lock: a report's numbers). */
void cpus_idle_totals(uint64_t entries[], uint64_t ticks[]);

#endif

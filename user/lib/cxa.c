/*
 * (43a-4) What exit runs, and C++'s part of it: atexit's functions and the
 * C++ ABI's __cxa_atexit (a static object's destructor), one list run
 * backwards by exit (__cxa_finalize(0)); __cxa_thread_atexit_impl (a
 * thread_local object's destructor), run when its thread ends or, for the
 * thread calling exit, first. A file of its own: its per-thread list
 * needs per-thread data, which ld.so (linking cstd.c) cannot have.
 */
#include <stdlib.h>
#include <string.h>
#include "ritteros.h"

/* atexit's and (43a-4) C++'s __cxa_atexit's functions, one list in the
 * order given, run backwards by exit; a block of 32 here, more from the
 * heap (C++ registers a destructor per static object: no fixed limit). A
 * function given while exit runs them runs too, as C asks. */
struct exit_entry {
    void (*fn)(void *);                 /* (atexit's take no argument: called as fn()) */
    void *arg;
    void *dso;                          /* the module that gave it (__cxa_finalize) */
    int plain;                          /* atexit's */
};
#define EXIT_BLOCK 32
static struct exit_entry first_block[EXIT_BLOCK], *exit_entries = first_block;
static int exit_count, exit_room = EXIT_BLOCK;
static int exit_lock;


static void entries_lock(void)
{
    while (__atomic_exchange_n(&exit_lock, 1, __ATOMIC_ACQUIRE))
        syscall6(SYS_FUTEX, (int64_t)&exit_lock, FUTEX_WAIT, 1, 0, 0, 0);
}

static void entries_unlock(void)
{
    __atomic_store_n(&exit_lock, 0, __ATOMIC_RELEASE);
    syscall6(SYS_FUTEX, (int64_t)&exit_lock, FUTEX_WAKE, 1, 0, 0, 0);
}

static int add_exit(void (*fn)(void *), void *arg, void *dso, int plain)
{
    entries_lock();
    if (exit_count == exit_room) {
        struct exit_entry *more = malloc((size_t)exit_room * 2 * sizeof(*more));
        if (!more) {
            entries_unlock();
            return -1;
        }
        memcpy(more, exit_entries, (size_t)exit_count * sizeof(*more));
        if (exit_entries != first_block)
            free(exit_entries);
        exit_entries = more;
        exit_room *= 2;
    }
    exit_entries[exit_count++] = (struct exit_entry){ fn, arg, dso, plain };
    entries_unlock();
    return 0;
}

int atexit(void (*fn)(void))
{
    return add_exit((void (*)(void *))fn, 0, 0, 1);
}

/* (43a-4) The C++ ABI's: a static object's destructor with its object;
 * dso is the registering module's __dso_handle. */
int __cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    return add_exit(fn, arg, dso, 0);
}

/* Run (and forget) the functions dso gave, last first; all of them for
 * dso 0 (exit's). */
void __cxa_finalize(void *dso)
{
    for (;;) {
        entries_lock();
        int i = exit_count - 1;
        while (i >= 0 && dso && exit_entries[i].dso != dso)
            i--;
        if (i < 0) {
            entries_unlock();
            return;
        }
        struct exit_entry e = exit_entries[i];
        memmove(exit_entries + i, exit_entries + i + 1, (size_t)(exit_count - i - 1) * sizeof(e));
        exit_count--;
        entries_unlock();
        if (e.plain)
            ((void (*)(void))e.fn)();
        else
            e.fn(e.arg);
    }
}

/* (43a-4) Per-thread destructors (C++'s thread_local objects), each
 * thread's own list: run last first when the thread ends (thread_exit)
 * and, for the thread that calls exit, before the atexit functions. */
struct thread_dtor {
    void (*fn)(void *);
    void *obj;
    struct thread_dtor *next;
};
static __thread struct thread_dtor *thread_dtors;

int __cxa_thread_atexit_impl(void (*fn)(void *), void *obj, void *dso)
{
    (void)dso;                          /* (modules are never unloaded) */
    struct thread_dtor *d = malloc(sizeof(*d));
    if (!d)
        return -1;
    d->fn = fn;
    d->obj = obj;
    d->next = thread_dtors;
    thread_dtors = d;
    return 0;
}

int __cxa_thread_atexit(void (*fn)(void *), void *obj, void *dso)
{
    return __cxa_thread_atexit_impl(fn, obj, dso);
}

void __ritteros_thread_dtors(void)
{
    while (thread_dtors) {
        struct thread_dtor *d = thread_dtors;
        thread_dtors = d->next;
        d->fn(d->obj);
        free(d);
    }
}

/* exit's first steps (syscalls.c): this thread's per-thread destructors,
 * then the last function given runs first; one that calls exit again does
 * not see itself again. */
void ritteros_run_atexit(void)
{
    __ritteros_thread_dtors();
    __cxa_finalize(0);
}


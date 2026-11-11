/*
 * Deliberate crashes for testing the exception handlers, selected with
 * "crash=<kind>" on the kernel command line. Kinds that work the same on
 * every architecture are here; the others are in arch/<arch>/arch.c.
 */
#ifndef RITTEROS_CRASHTEST_H
#define RITTEROS_CRASHTEST_H

/* A canonical lower-half address that no page table maps. */
#define CRASH_UNMAPPED_ADDRESS 0x0000700000001000ULL

/*
 * Trigger the named crash: the shared kinds pf, null, wp, nx, stack,
 * doublefree, heapdouble and heapoverflow, or one of arch_crash_kinds. Does
 * not return for a known kind; returns for an unknown one after printing the
 * valid kinds.
 */
void crash_test(const char *kind);

/* Log which crash is about to happen (in the warning color). */
void crash_announce(const char *what);

#endif

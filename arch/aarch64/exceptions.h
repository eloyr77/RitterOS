/* AArch64 exception handling: crash reports for fatal exceptions. */
#ifndef RITTEROS_EXCEPTIONS_H
#define RITTEROS_EXCEPTIONS_H

#include <stdint.h>

/* Register state saved by vectors.S (304 bytes, lowest address first). */
struct exception_frame {
    uint64_t x[31];                 /* X0-X30 (X30 is the link register) */
    uint64_t sp;                    /* SP_EL0: the interrupted kernel stack */
    uint64_t elr;                   /* where the exception happened */
    uint64_t spsr;                  /* saved processor state */
    uint64_t esr;                   /* exception syndrome: what happened */
    uint64_t far;                   /* fault address (aborts) */
    uint64_t vector;                /* entry 0-15 of the vector table */
    uint64_t pad;
};

_Static_assert(sizeof(struct exception_frame) == 304, "frame size must match vectors.S");
_Static_assert(sizeof(struct exception_frame) <= 512, "fork copies the frame (ARCH_FRAME_MAX)");

/* Called by vectors.S. Returns only for exceptions the kernel can continue
 * from (currently the breakpoint). */
void exception_handler(struct exception_frame *frame);

#endif

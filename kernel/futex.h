/*
 * Futexes (kernel/futex.c, step 22y): waiting on a word of a program's
 * memory, the kernel's half of the C library's mutexes, condition
 * variables and call_once (threads.h). A program's threads share the word;
 * the program is the key's other half, so futexes never reach across
 * programs.
 */
#ifndef RITTEROS_FUTEX_H
#define RITTEROS_FUTEX_H

#include <stdint.h>

/* If the 32-bit word at addr (the calling program's) holds value, sleep
 * until futex_wake for it, timeout_ns nanoseconds (0: no limit) or a
 * signal: 0 when woken, -ERR_AGAIN if the word held something else,
 * -ERR_TIMEDOUT, -ERR_INTR, -ERR_FAULT. */
int64_t futex_wait(const void *program, uint64_t addr, uint32_t value, uint64_t timeout_ns);

/* Wake up to n threads of program waiting on addr (the longest waiting
 * first): how many were woken. */
int64_t futex_wake(const void *program, uint64_t addr, int64_t n);

#endif

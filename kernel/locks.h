/*
 * (43b-4) File locks, as Linux's: flock's whole-file locks, held by an open
 * file (every descriptor of it: dup's, a fork's, one passed over a socket)
 * until it unlocks or closes; and fcntl's record locks (POSIX's), byte
 * ranges held by a process, let go of when it unlocks them, closes any
 * descriptor of the file, or ends. The two kinds never meet. A file is
 * (object, id): a RitterFS's object, a memory file. Advisory only: reads
 * and writes never look at them.
 */
#ifndef RITTEROS_LOCKS_H
#define RITTEROS_LOCKS_H

#include <stdbool.h>
#include <stdint.h>

struct sys_flock;

/* flock(op): LOCK_SH 1, LOCK_EX 2, LOCK_UN 8, with LOCK_NB 4 (Linux's
 * numbers) for owner (an open file): 0, -ERR_AGAIN (LOCK_NB, held by
 * another), -ERR_INTR, -ERR_INVAL. A lock already held is changed by
 * letting go of it first (as Linux: not in one step). */
int64_t locks_flock(const void *object, uint64_t id, const void *owner, unsigned op);
void locks_flock_closed(const void *owner);

/* fcntl's FCNTL_GETLK, _SETLK, _SETLKW for process pid on l (start
 * absolute, length 0: to the end): 0 (GETLK: l is the first lock in the
 * way, or its type F_UNLCK), -ERR_AGAIN (SETLK: in the way), -ERR_INTR,
 * -ERR_INVAL, -ERR_NOMEM. */
int64_t locks_record(const void *object, uint64_t id, int pid, unsigned cmd, struct sys_flock *l);
void locks_record_closed(const void *object, uint64_t id, int pid);
void locks_process_ended(int pid);

#endif

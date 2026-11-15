/*
 * semaphore.h (43a-4): POSIX's counting semaphores between a program's
 * threads, and (43b-3) between programs: sem_init with pshared 1 in memory
 * they share (MAP_SHARED), or by name (sem_open: a memory file
 * /dev/shm/sem.NAME; O_CREAT takes a mode and a value, O_EXCL; sem_close
 * unmaps it, sem_unlink removes the name). sem_wait takes one (waiting
 * while the count is 0; a cancellation point), sem_trywait without
 * waiting (EAGAIN), sem_timedwait until an absolute CLOCK_REALTIME time
 * (ETIMEDOUT), sem_post gives one back (waking a waiter), sem_getvalue
 * the count.
 */
#ifndef RITTEROS_SEMAPHORE_H
#define RITTEROS_SEMAPHORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <time.h>

typedef struct {
    int value;                          /* the count */
    int waiters;                        /* threads sleeping on value */
    long reserved[2];
} sem_t;
#define SEM_FAILED ((sem_t *)0)

int sem_init(sem_t *s, int pshared, unsigned int value);
int sem_destroy(sem_t *s);
int sem_wait(sem_t *s);
int sem_trywait(sem_t *s);
int sem_timedwait(sem_t *s, const struct timespec *until);
int sem_post(sem_t *s);
int sem_getvalue(sem_t *s, int *value);
sem_t *sem_open(const char *name, int flags, ...);
int sem_close(sem_t *s);
int sem_unlink(const char *name);

#ifdef __cplusplus
}
#endif

#endif

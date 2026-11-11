/*
 * fork (56a, SYS_FORK) and pthread_atfork. The heap's lock is taken across
 * the call, after the prepare handlers (they may allocate) and given back
 * before the others, in both processes: a child has only the calling
 * thread, so a lock another thread held would stay taken there for good
 * (and, since 64i, the tickets of threads waiting for it would never come:
 * the child's lock starts again instead). (65e-53) So is the list of
 * streams', before the heap's; in the child every stream's lock but this
 * thread's starts free.
 */
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include "ritteros.h"

void __ritteros_malloc_lock(void);
void __ritteros_malloc_unlock(void);
void __ritteros_malloc_child(void);
void __ritteros_stdio_lock(void);
void __ritteros_stdio_unlock(void);
void __ritteros_stdio_child(void);

#define ATFORK_MAX 32

static struct {
    void (*prepare)(void), (*parent)(void), (*child)(void);
} handlers[ATFORK_MAX];
static int count;
static pthread_mutex_t handlers_lock = PTHREAD_MUTEX_INITIALIZER;

int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void))
{
    pthread_mutex_lock(&handlers_lock);
    int e = count == ATFORK_MAX ? ENOMEM : 0;
    if (!e) {
        handlers[count].prepare = prepare;
        handlers[count].parent = parent;
        handlers[count].child = child;
        count++;
    }
    pthread_mutex_unlock(&handlers_lock);
    return e;
}

pid_t fork(void)
{
    pthread_mutex_lock(&handlers_lock);
    int n = count;
    for (int i = n - 1; i >= 0; i--)
        if (handlers[i].prepare)
            handlers[i].prepare();
    __ritteros_stdio_lock();
    __ritteros_malloc_lock();
    int64_t r = syscall6(SYS_FORK, 0, 0, 0, 0, 0, 0);
    if (r == 0) {
        __ritteros_malloc_child();      /* (64i: the ticket lock starts again) */
        __ritteros_stdio_child();
    } else {
        __ritteros_malloc_unlock();
        __ritteros_stdio_unlock();
    }
    for (int i = 0; i < n; i++) {
        void (*after)(void) = r == 0 ? handlers[i].child : handlers[i].parent;
        if (after)
            after();
    }
    pthread_mutex_unlock(&handlers_lock);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return (pid_t)r;
}

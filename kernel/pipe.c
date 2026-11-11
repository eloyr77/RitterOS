/*
 * Pipes (see pipe.h).
 *
 * One ring buffer, binary-transparent: a write copies bytes in and wakes
 * readers, a read copies bytes out and wakes writers. Reference counts per
 * end give the end-of-file and ERR_PIPE conditions: a read returns 0 when
 * no write end is left, a write fails when no read end is left. The same
 * mutex guards the ring and the counts; threads wait on the two queues
 * (never holding the mutex while waiting), as the pty does.
 */
#include "pipe.h"
#include "heap.h"
#include "sched.h"
#include "sync.h"
#include "syscalls.h"

#define PIPE_SIZE   65536               /* a power of two: index with % */

struct pipe {
    struct mutex lock;
    int readers, writers;               /* references on each end */
    int holders;                        /* a blocking call in progress; keeps it alive */
    uint8_t buf[PIPE_SIZE];
    uint32_t head, tail;                /* monotonic; bytes = head - tail */
    struct wait_queue read_wait;        /* readers waiting for bytes */
    struct wait_queue write_wait;       /* writers waiting for room */
};

struct pipe *pipe_new(void)
{
    struct pipe *p = kzalloc(sizeof(*p));
    if (!p)
        return 0;
    p->readers = p->writers = 1;
    p->lock = (struct mutex)MUTEX_INIT("pipe");
    p->read_wait.pollable = p->write_wait.pollable = true;
    return p;
}

int pipe_poll(struct pipe *p, bool writer)
{
    poll_watch(&p->read_wait);              /* (65e) before the state is read */
    poll_watch(&p->write_wait);
    mutex_lock(&p->lock);
    int r = writer ? (p->readers == 0 ? POLLERR : PIPE_SIZE - (p->head - p->tail) > 0 ? POLLOUT : 0)
                   : (p->head != p->tail ? POLLIN : 0) | (p->writers == 0 ? POLLHUP : 0);
    mutex_unlock(&p->lock);
    return r;
}

static void free_if_unused(struct pipe *p)
{
    bool unused = p->readers == 0 && p->writers == 0 &&
                  __atomic_load_n(&p->holders, __ATOMIC_SEQ_CST) == 0;
    mutex_unlock(&p->lock);
    if (unused) {
        wait_queue_detach(&p->read_wait);   /* (65e) no poller left on them */
        wait_queue_detach(&p->write_wait);
        kfree(p);
    }
}

/* Atomic, without the lock (49l): taken with the process's descriptor lock
 * held (interrupts off, no sleeping), before anything that may sleep, so a
 * close by another thread meanwhile cannot free the pipe under the call. */
void pipe_hold(struct pipe *p)
{
    __atomic_add_fetch(&p->holders, 1, __ATOMIC_SEQ_CST);
}

void pipe_unhold(struct pipe *p)
{
    mutex_lock(&p->lock);
    __atomic_sub_fetch(&p->holders, 1, __ATOMIC_SEQ_CST);
    free_if_unused(p);
}

void pipe_ref(struct pipe *p, bool writer)
{
    mutex_lock(&p->lock);
    if (writer)
        p->writers++;
    else
        p->readers++;
    mutex_unlock(&p->lock);
}

void pipe_close(struct pipe *p, bool writer)
{
    mutex_lock(&p->lock);
    if (writer) {
        if (--p->writers == 0)
            wait_queue_wake_all(&p->read_wait);     /* readers see end of file */
    } else if (--p->readers == 0) {
        wait_queue_wake_all(&p->write_wait);        /* writers see ERR_PIPE */
    }
    free_if_unused(p);
}

int64_t pipe_read(struct pipe *p, uint8_t *buf, size_t size)
{
    if (size == 0)
        return 0;
    for (;;) {
        mutex_lock(&p->lock);
        uint32_t have = p->head - p->tail;
        if (have > 0) {
            size_t n = have < size ? have : size;
            for (size_t i = 0; i < n; i++)
                buf[i] = p->buf[p->tail++ % PIPE_SIZE];
            wait_queue_wake_all(&p->write_wait);
            mutex_unlock(&p->lock);
            return (int64_t)n;
        }
        bool ended = p->writers == 0;
        mutex_unlock(&p->lock);
        if (ended)
            return 0;
        if (thread_current()->dont_wait)
            return -ERR_AGAIN;                      /* (53b) */
        if (!wait_event_interruptible(&p->read_wait, p->head != p->tail || p->writers == 0))
            return -ERR_INTR;
    }
}

int64_t pipe_write(struct pipe *p, const uint8_t *buf, size_t size)
{
    size_t done = 0;
    while (done < size) {
        mutex_lock(&p->lock);
        if (p->readers == 0) {                      /* nobody will ever read it */
            mutex_unlock(&p->lock);
            return done > 0 ? (int64_t)done : -ERR_PIPE;
        }
        uint32_t room = PIPE_SIZE - (p->head - p->tail);
        if (room > 0) {
            size_t n = size - done;
            if (n > room)
                n = room;
            for (size_t i = 0; i < n; i++)
                p->buf[p->head++ % PIPE_SIZE] = buf[done++];
            wait_queue_wake_all(&p->read_wait);
            mutex_unlock(&p->lock);
            continue;
        }
        mutex_unlock(&p->lock);
        if (thread_current()->dont_wait)
            return done > 0 ? (int64_t)done : -ERR_AGAIN;   /* (53b) */
        if (!wait_event_interruptible(&p->write_wait,
                                      PIPE_SIZE - (p->head - p->tail) > 0 || p->readers == 0))
            return done > 0 ? (int64_t)done : -ERR_INTR;
    }
    return (int64_t)done;
}

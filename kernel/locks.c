/*
 * (43b-4) File locks (see locks.h): one list of every lock under a
 * spinlock, short sections only (the nodes a change may need are made
 * before it is taken, the ones it took out freed after), and one queue for
 * everyone waiting: every unlock wakes them all to look again. No
 * deadlock detection for FCNTL_SETLKW (Linux's EDEADLK): a wait ends with
 * the holder's unlock, its end or a signal.
 */
#include "locks.h"
#include "heap.h"
#include "sched.h"
#include "sync.h"
#include "syscalls.h"

#define TO_END (~0ULL)

struct file_lock {
    const void *object;
    uint64_t id;
    bool record;                            /* fcntl's (owner: pid) or flock's (an open file) */
    const void *owner;
    bool write;                             /* exclusive, else shared */
    uint64_t start, end;                    /* [start, end): flock's the whole file */
    struct file_lock *next;
};

static struct file_lock *all;
static struct spinlock lock = SPINLOCK_INIT("file locks");
static struct wait_queue waiters;

static bool same_file(const struct file_lock *x, const void *object, uint64_t id, bool record)
{
    return x->object == object && x->id == id && x->record == record;
}

/* The first lock of another owner in the way of (write, [start, end)). */
static struct file_lock *in_way(const void *object, uint64_t id, bool record, const void *owner,
                                bool write, uint64_t start, uint64_t end)
{
    for (struct file_lock *x = all; x; x = x->next)
        if (same_file(x, object, id, record) && x->owner != owner && x->start < end &&
            start < x->end && (x->write || write))
            return x;
    return 0;
}

/* The lock taken (state its interrupts) for one sleep: 0 after it, or
 * -ERR_AGAIN (not to wait) / -ERR_INTR, the lock given back either way. */
static int64_t wait_once(bool may_wait, uint64_t state)
{
    struct thread *self = thread_current();
    if (!may_wait) {
        spin_unlock(&lock, state);
        return -ERR_AGAIN;
    }
    if (self->stop_requested) {
        spin_unlock(&lock, state);
        return -ERR_INTR;
    }
    wait_prepare(&waiters, 0);
    spin_unlock(&lock, state);
    wait_commit(0);
    return 0;
}

/* Take out owner's locks of the file over [start, end) (lock held): what
 * is left of a lock outside the range stays; one cut inside takes `spare`
 * (*spare cleared then). The nodes taken out go onto *gone. True if any
 * lock changed. */
static bool cut(const void *object, uint64_t id, bool record, const void *owner, uint64_t start,
                uint64_t end, struct file_lock **spare, struct file_lock **gone)
{
    bool changed = false;
    for (struct file_lock **link = &all; *link;) {
        struct file_lock *x = *link;
        if (!same_file(x, object, id, record) || x->owner != owner || x->end <= start ||
            x->start >= end) {
            link = &x->next;
            continue;
        }
        changed = true;
        if (x->start < start && x->end > end) {     /* inside it: two parts left */
            struct file_lock *right = *spare;
            *spare = 0;
            *right = *x;
            right->start = end;
            x->end = start;
            right->next = x->next;
            x->next = right;
            return true;                            /* (nothing else of owner's overlaps) */
        }
        if (x->start < start) {
            x->end = start;
        } else if (x->end > end) {
            x->start = end;
        } else {
            *link = x->next;
            x->next = *gone;
            *gone = x;
            continue;
        }
        link = &x->next;
    }
    return changed;
}

static void free_list(struct file_lock *x)
{
    while (x) {
        struct file_lock *next = x->next;
        kfree(x);
        x = next;
    }
}

/* Set owner's lock on [start, end) (write, or unlock), waiting while
 * another's is in the way if may_wait. */
static int64_t set(const void *object, uint64_t id, bool record, const void *owner, bool unlock,
                   bool write, uint64_t start, uint64_t end, bool may_wait)
{
    struct file_lock *made = kmalloc(sizeof(*made)), *spare = kmalloc(sizeof(*spare)), *gone = 0;
    if (!made || !spare) {
        kfree(made);
        kfree(spare);
        return -ERR_NOMEM;
    }
    int64_t r = 0;
    bool woke = false;
    if (!record && !unlock) {
        /* flock: an own lock of the other kind is let go of first. */
        uint64_t state = spin_lock(&lock);
        woke = cut(object, id, false, owner, 0, TO_END, &spare, &gone);
        spin_unlock(&lock, state);
        if (woke)
            wait_queue_wake_all(&waiters);
    }
    for (;;) {
        uint64_t state = spin_lock(&lock);
        if (!unlock && in_way(object, id, record, owner, write, start, end)) {
            if ((r = wait_once(may_wait, state)))
                break;
            continue;
        }
        woke = cut(object, id, record, owner, start, end, &spare, &gone);
        if (!unlock) {
            *made = (struct file_lock){ object, id, record, owner, write, start, end, all };
            all = made;
            made = 0;
        }
        spin_unlock(&lock, state);
        if (woke)
            wait_queue_wake_all(&waiters);          /* (a write lock may have become a read one) */
        break;
    }
    kfree(made);
    kfree(spare);
    free_list(gone);
    return r;
}

int64_t locks_flock(const void *object, uint64_t id, const void *owner, unsigned op)
{
    unsigned what = op & ~4u;
    if (what != 1 && what != 2 && what != 8)
        return -ERR_INVAL;
    return set(object, id, false, owner, what == 8, what == 2, 0, TO_END, !(op & 4));
}

/* Every lock of owner (all files when object is 0) taken out. */
static void release(bool record, const void *object, uint64_t id, const void *owner)
{
    struct file_lock *gone = 0;
    uint64_t state = spin_lock(&lock);
    for (struct file_lock **link = &all; *link;) {
        struct file_lock *x = *link;
        if (x->record == record && x->owner == owner && (!object || (x->object == object && x->id == id))) {
            *link = x->next;
            x->next = gone;
            gone = x;
        } else {
            link = &x->next;
        }
    }
    spin_unlock(&lock, state);
    if (gone) {
        wait_queue_wake_all(&waiters);
        free_list(gone);
    }
}

void locks_flock_closed(const void *owner)
{
    release(false, 0, 0, owner);
}

static const void *pid_owner(int pid)
{
    return (const void *)(uintptr_t)(unsigned)pid;
}

int64_t locks_record(const void *object, uint64_t id, int pid, unsigned cmd, struct sys_flock *l)
{
    if (l->type != FCNTL_RDLCK && l->type != FCNTL_WRLCK && l->type != FCNTL_UNLCK)
        return -ERR_INVAL;
    uint64_t start = l->start, end = l->length ? l->start + l->length : TO_END;
    if (end <= start)
        return -ERR_INVAL;                          /* (past 2^64: the library refuses it) */
    if (cmd == FCNTL_GETLK) {
        if (l->type == FCNTL_UNLCK)
            return -ERR_INVAL;
        uint64_t state = spin_lock(&lock);
        struct file_lock *x = in_way(object, id, true, pid_owner(pid), l->type == FCNTL_WRLCK, start, end);
        if (x) {
            l->type = x->write ? FCNTL_WRLCK : FCNTL_RDLCK;
            l->start = x->start;
            l->length = x->end == TO_END ? 0 : x->end - x->start;
            l->pid = (uint32_t)(uintptr_t)x->owner;
        } else {
            l->type = FCNTL_UNLCK;
        }
        spin_unlock(&lock, state);
        return 0;
    }
    if (cmd != FCNTL_SETLK && cmd != FCNTL_SETLKW)
        return -ERR_INVAL;
    return set(object, id, true, pid_owner(pid), l->type == FCNTL_UNLCK, l->type == FCNTL_WRLCK, start,
               end, cmd == FCNTL_SETLKW);
}

void locks_record_closed(const void *object, uint64_t id, int pid)
{
    release(true, object, id, pid_owner(pid));
}

void locks_process_ended(int pid)
{
    release(true, 0, 0, pid_owner(pid));
}

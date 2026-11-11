/*
 * Test: POSIX threads (54a). Plain POSIX: the test builds it on the Mac
 * too and compares the "pthreadtest:" lines; "pthreadtest-own:" lines are
 * what the Mac does not have (pthread_mutex_timedlock, condattr_setclock,
 * RitterOS's 16 threads) and are checked apart.
 * Usage: pthreadtest [own]   (own: the RitterOS-only part too)
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *err(int e)
{
    return e == 0 ? "0" : e == EBUSY ? "EBUSY" : e == EDEADLK ? "EDEADLK" : e == EPERM ? "EPERM"
         : e == ETIMEDOUT ? "ETIMEDOUT" : e == EINVAL ? "EINVAL" : e == EAGAIN ? "EAGAIN"
         : e == ESRCH ? "ESRCH" : "another";
}

static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;

static void *count(void *arg)
{
    for (int i = 0; i < 10000; i++) {
        pthread_mutex_lock(&counter_lock);
        counter++;
        pthread_mutex_unlock(&counter_lock);
        if (i % 1000 == 0)
            sched_yield();
    }
    return (void *)((long)arg * 10);
}

static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_more = PTHREAD_COND_INITIALIZER, q_room = PTHREAD_COND_INITIALIZER;
static int queue[4], q_count, q_head;

static void *produce(void *arg)
{
    (void)arg;
    for (int i = 0; i < 100; i++) {
        pthread_mutex_lock(&q_lock);
        while (q_count == 4)
            pthread_cond_wait(&q_room, &q_lock);
        queue[(q_head + q_count++) % 4] = i;
        pthread_cond_signal(&q_more);
        pthread_mutex_unlock(&q_lock);
    }
    return 0;
}

static pthread_mutex_t shared;
static int other_result, rw_result;

static void *try_other(void *arg)
{
    (void)arg;
    other_result = pthread_mutex_trylock(&shared);
    if (other_result == 0)
        pthread_mutex_unlock(&shared);
    return 0;
}

static void *unlock_other(void *arg)
{
    (void)arg;
    other_result = pthread_mutex_unlock(&shared);
    return 0;
}

static int other(void *(*f)(void *))
{
    pthread_t t;
    other_result = rw_result = -1;
    pthread_create(&t, 0, f, 0);
    pthread_join(t, 0);
    return rw_result != -1 ? rw_result : other_result;
}

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int inits;

static void init_once(void)
{
    inits++;
}

static void *call_once_(void *arg)
{
    (void)arg;
    pthread_once(&once, init_once);
    return 0;
}

static pthread_key_t key;
static int destroyed;
static pthread_mutex_t destroyed_lock = PTHREAD_MUTEX_INITIALIZER;

static void destroy(void *value)
{
    pthread_mutex_lock(&destroyed_lock);
    destroyed += (int)(long)value;
    pthread_mutex_unlock(&destroyed_lock);
}

static void *keep(void *arg)
{
    pthread_setspecific(key, arg);
    return pthread_getspecific(key);
}

static void *leave(void *arg)
{
    (void)arg;
    pthread_exit((void *)42);
}

static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate = PTHREAD_COND_INITIALIZER;
static int gate_open, detached_ran;

static void *detached(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&gate_lock);
    detached_ran = 1;
    pthread_cond_broadcast(&gate);
    while (!gate_open)
        pthread_cond_wait(&gate, &gate_lock);
    pthread_mutex_unlock(&gate_lock);
    return 0;
}

static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static pthread_mutex_t seen_lock = PTHREAD_MUTEX_INITIALIZER;
static int inside, most;

static void *reader(void *arg)
{
    (void)arg;
    pthread_rwlock_rdlock(&rw);
    pthread_mutex_lock(&seen_lock);
    if (++inside > most)
        most = inside;
    pthread_mutex_unlock(&seen_lock);
    usleep(100000);                             /* (the others come in meanwhile) */
    pthread_mutex_lock(&seen_lock);
    inside--;
    pthread_mutex_unlock(&seen_lock);
    pthread_rwlock_unlock(&rw);
    return 0;
}

static void *try_read(void *arg)
{
    (void)arg;
    rw_result = pthread_rwlock_tryrdlock(&rw);
    if (!rw_result)
        pthread_rwlock_unlock(&rw);
    return 0;
}

static void *try_write(void *arg)
{
    (void)arg;
    rw_result = pthread_rwlock_trywrlock(&rw);
    if (!rw_result)
        pthread_rwlock_unlock(&rw);
    return 0;
}

static void rwlocks(void)
{
    pthread_t r[3];
    for (int i = 0; i < 3; i++)
        pthread_create(&r[i], 0, reader, 0);
    for (int i = 0; i < 3; i++)
        pthread_join(r[i], 0);
    printf("pthreadtest: rwlock: readers inside at once: %d\n", most);
    pthread_rwlock_wrlock(&rw);
    int read_try = other(try_read);
    int write_try = other(try_write);
    int again = pthread_rwlock_trywrlock(&rw);
    pthread_rwlock_unlock(&rw);
    pthread_rwlock_rdlock(&rw);
    int read_while_read = other(try_read);
    int write_while_read = other(try_write);
    pthread_rwlock_unlock(&rw);
    int free_write = other(try_write);
    printf("pthreadtest: rwlock: a writer in: others' tryrdlock %s, trywrlock %s, its own trywrlock "
           "%s; a reader in: tryrdlock %s, trywrlock %s; free: trywrlock %s\n", err(read_try),
           err(write_try), err(again), err(read_while_read), err(write_while_read), err(free_write));
    printf("pthreadtest: rwlock destroy %s\n", err(pthread_rwlock_destroy(&rw)));
}

static long ms_since(const struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (long)((t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000);
}

#ifndef __APPLE__                       /* (the Mac has neither call) */
static void *wait_gate(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&gate_lock);
    while (!gate_open)
        pthread_cond_wait(&gate, &gate_lock);
    pthread_mutex_unlock(&gate_lock);
    return 0;
}

static void *hold_and_time(void *arg)
{
    (void)arg;
    struct timespec until, t0;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 100000000;
    if (until.tv_nsec >= 1000000000)
        until.tv_nsec -= 1000000000, until.tv_sec++;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    other_result = pthread_mutex_timedlock(&shared, &until);
    long took = ms_since(&t0);
    printf("pthreadtest-own: pthread_mutex_timedlock on a held mutex: %s after %s\n", err(other_result),
           took >= 80 && took < 2000 ? "about 100 ms" : "a wrong time");
    return 0;
}

static pthread_barrier_t barrier;
static int arrived, early, serials;
static pthread_spinlock_t spin;
static long spun;

static void *at_barrier(void *arg)
{
    (void)arg;
    for (int round = 0; round < 3; round++) {
        __atomic_fetch_add(&arrived, 1, __ATOMIC_SEQ_CST);
        int r = pthread_barrier_wait(&barrier);
        if (__atomic_load_n(&arrived, __ATOMIC_SEQ_CST) < 4 * (round + 1))
            __atomic_fetch_add(&early, 1, __ATOMIC_SEQ_CST);    /* (left before all came) */
        if (r == PTHREAD_BARRIER_SERIAL_THREAD)
            __atomic_fetch_add(&serials, 1, __ATOMIC_SEQ_CST);
        pthread_barrier_wait(&barrier);         /* (all have looked before the next round) */
    }
    return 0;
}

static void *spin_count(void *arg)
{
    (void)arg;
    for (int i = 0; i < 2000; i++) {
        pthread_spin_lock(&spin);
        spun++;
        pthread_spin_unlock(&spin);
    }
    return 0;
}

/* (54d) n levels of 64 KiB frames each: n * 64 KiB of stack. Each frame
 * is filled and checked through functions called by a volatile pointer,
 * which the compiler cannot see into (with a few fixed elements touched it
 * split the array into scalars: a 32-byte frame, and the check passed with
 * 64 KiB stacks). */
static void fill_frame(char *frame, size_t size, int n)
{
    memset(frame, n, size);
}

static int check_frame(const char *frame, size_t size, int n)
{
    for (size_t i = 0; i < size; i++)
        if (frame[i] != (char)n)
            return 0;
    return 1;
}

static void (*volatile filler)(char *, size_t, int) = fill_frame;
static int (*volatile checker)(const char *, size_t, int) = check_frame;

static long deep(int n)
{
    char frame[65536];
    filler(frame, sizeof(frame), n);
    long below = n ? deep(n - 1) : 0;
    long same = checker(frame, sizeof(frame), n);
    return n ? below + same : 0;
}

static void *deep_thread(void *arg)
{
    return (void *)deep((int)(long)arg);
}

/* (65e-53) fflush(0) beside streams other threads hold (the Mac's libc,
 * glibc and musl wait for good in both): one thread holds a stream and
 * opens another while fflush(0) waits for it; two threads, each holding
 * one, flush all. A watchdog tells a deadlock (then the program ends at
 * once: exit's flush would wait too). */
static FILE *held_one, *held_two;
static pthread_mutex_t flushes_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t flushes_cond = PTHREAD_COND_INITIALIZER;
static int flushes_done;
static pthread_barrier_t flush_barrier;

static void flush_finished(void)
{
    pthread_mutex_lock(&flushes_lock);
    flushes_done++;
    pthread_cond_broadcast(&flushes_cond);
    pthread_mutex_unlock(&flushes_lock);
}

static void *hold_then_open(void *arg)
{
    (void)arg;
    flockfile(held_one);
    usleep(100000);                             /* (the other thread is in fflush(0) meanwhile) */
    FILE *f = fopen("/dev/null", "w");
    if (f)
        fclose(f);
    funlockfile(held_one);
    flush_finished();
    return 0;
}

static void *flush_all(void *arg)
{
    (void)arg;
    usleep(20000);
    fflush(0);
    flush_finished();
    return 0;
}

static void *hold_and_flush(void *arg)
{
    FILE *mine = arg ? held_two : held_one;
    flockfile(mine);
    pthread_barrier_wait(&flush_barrier);
    fflush(0);
    funlockfile(mine);
    flush_finished();
    return 0;
}

/* all `want` threads ended within 5 s? */
static int flushes_end(int want)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 5;
    pthread_mutex_lock(&flushes_lock);
    while (flushes_done < want)
        if (pthread_cond_timedwait(&flushes_cond, &flushes_lock, &until) == ETIMEDOUT)
            break;
    int ok = flushes_done >= want;
    flushes_done = 0;
    pthread_mutex_unlock(&flushes_lock);
    return ok;
}

static void flush_beside_held(void)
{
    held_one = fopen("/dev/null", "w");
    held_two = fopen("/dev/null", "w");
    fputs("one", held_one);
    fputs("two", held_two);
    pthread_t a, b;
    pthread_create(&a, 0, hold_then_open, 0);
    pthread_create(&b, 0, flush_all, 0);
    int first = flushes_end(2);
    printf("pthreadtest-own: fflush(0) while another thread holds a stream and opens one: %s\n",
           first ? "both ended" : "deadlocked");
    if (!first)
        _exit(1);
    pthread_join(a, 0);
    pthread_join(b, 0);
    pthread_barrier_init(&flush_barrier, 0, 2);
    pthread_create(&a, 0, hold_and_flush, 0);
    pthread_create(&b, 0, hold_and_flush, (void *)1);
    int second = flushes_end(2);
    printf("pthreadtest-own: two threads each holding a stream flush all: %s\n",
           second ? "both ended" : "deadlocked");
    if (!second)
        _exit(1);
    pthread_join(a, 0);
    pthread_join(b, 0);
    fclose(held_one);
    fclose(held_two);
}

static void own(void)
{
    pthread_mutex_init(&shared, 0);
    pthread_mutex_lock(&shared);
    pthread_t h;
    pthread_create(&h, 0, hold_and_time, 0);
    pthread_join(h, 0);
    pthread_mutex_unlock(&shared);

    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    int r = pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_t c;
    pthread_cond_init(&c, &ca);
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    struct timespec mono, t0;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    mono.tv_nsec += 150000000;
    if (mono.tv_nsec >= 1000000000)
        mono.tv_nsec -= 1000000000, mono.tv_sec++;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int w = pthread_cond_timedwait(&c, &m, &mono);
    long took = ms_since(&t0);
    pthread_mutex_unlock(&m);
    printf("pthreadtest-own: a CLOCK_MONOTONIC condition (setclock %d): %s after %s\n", r, err(w),
           took >= 120 && took < 2000 ? "about 150 ms" : "a wrong time");

    static pthread_t many[300];
    pthread_mutex_lock(&gate_lock);
    gate_open = 0;
    pthread_mutex_unlock(&gate_lock);
    int made = 0, last = 0;
    for (int i = 0; i < 300; i++) {
        if ((last = pthread_create(&many[i], 0, wait_gate, 0)))
            break;
        made++;
    }
    pthread_mutex_lock(&gate_lock);
    gate_open = 1;
    pthread_cond_broadcast(&gate);
    pthread_mutex_unlock(&gate_lock);
    for (int i = 0; i < made; i++)
        pthread_join(many[i], 0);
    printf("pthreadtest-own: threads made besides main: %d, then %s\n", made, err(last));

    printf("pthreadtest-own: the main thread 6.4 MiB deep: %ld levels\n", deep(100));
    pthread_t dt;
    void *levels = 0;
    pthread_create(&dt, 0, deep_thread, (void *)100L);
    pthread_join(dt, &levels);
    printf("pthreadtest-own: a thread 6.4 MiB deep on its default stack: %ld levels\n", (long)levels);
    pthread_attr_t big;
    pthread_attr_init(&big);
    size_t size = 0;
    pthread_attr_getstacksize(&big, &size);
    int r16 = pthread_attr_setstacksize(&big, 16 << 20), r64 = pthread_attr_setstacksize(&big, 64 << 20);
    pthread_create(&dt, &big, deep_thread, (void *)200L);
    pthread_join(dt, &levels);
    printf("pthreadtest-own: default stack %zu; 16 MiB %s, 64 MiB %s; 12.8 MiB deep: %ld levels\n",
           size, err(r16), err(r64), (long)levels);

    pthread_barrier_init(&barrier, 0, 4);
    pthread_t b[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&b[i], 0, at_barrier, 0);
    for (int i = 0; i < 4; i++)
        pthread_join(b[i], 0);
    printf("pthreadtest-own: a barrier of 4, 3 rounds: %d left early, %d serial thread(s), "
           "count 0 %s\n", early, serials, err(pthread_barrier_init(&barrier, 0, 0)));
    pthread_spin_init(&spin, PTHREAD_PROCESS_PRIVATE);
    pthread_t sp[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&sp[i], 0, spin_count, 0);
    for (int i = 0; i < 4; i++)
        pthread_join(sp[i], 0);
    pthread_spin_lock(&spin);
    int held = pthread_spin_trylock(&spin);
    pthread_spin_unlock(&spin);
    printf("pthreadtest-own: a spin lock: 4 threads counted %ld, trylock while held %s\n", spun,
           err(held));
    flush_beside_held();
}
#endif

/* (65e-53) stdio from threads: four write one stream at once, whole lines
 * by fprintf or char by char under flockfile; another thread's
 * ftrylockfile; streams opened and closed by four while fflush(0) goes
 * along the list */
#define STDIO_LINES 2000
static FILE *one_stream;

static void *stdio_lines(void *arg)
{
    int k = (int)(long)arg;
    for (int i = 0; i < STDIO_LINES; i++) {
        if (i % 2) {
            fprintf(one_stream, "thread %d line %04d\n", k, i);
            continue;
        }
        char line[32];
        snprintf(line, sizeof(line), "thread %d line %04d\n", k, i);
        flockfile(one_stream);
        for (char *c = line; *c; c++)
            putc_unlocked(*c, one_stream);
        funlockfile(one_stream);
    }
    return 0;
}

static void *stdio_try(void *arg)
{
    (void)arg;
    if (ftrylockfile(one_stream) != 0)
        return (void *)1;
    funlockfile(one_stream);
    return 0;
}

static void *stdio_open_close(void *arg)
{
    long opened = 0;
    for (int i = 0; i < 300; i++) {
        FILE *f = fopen("/dev/null", "w");
        if (!f)
            continue;
        opened++;
        fputs("x", f);
        if ((long)arg == 0 && i % 10 == 0)
            fflush(0);
        fclose(f);
    }
    return (void *)opened;
}

static void stdio_threads(void)
{
    one_stream = tmpfile();
    pthread_t t[4];
    for (long k = 0; k < 4; k++)
        pthread_create(&t[k], 0, stdio_lines, (void *)k);
    for (int k = 0; k < 4; k++)
        pthread_join(t[k], 0);
    rewind(one_stream);
    char line[64];
    int lines = 0, whole = 0, per[4] = { 0 };
    while (fgets(line, sizeof(line), one_stream)) {
        int k, i;
        lines++;
        if (strlen(line) == 19 && sscanf(line, "thread %d line %d", &k, &i) == 2 && k >= 0 && k < 4)
            whole++, per[k]++;
    }
    printf("pthreadtest: 4 threads writing one stream: %d lines, %d whole (%d %d %d %d)\n", lines, whole,
           per[0], per[1], per[2], per[3]);
    flockfile(one_stream);
    void *busy, *free_again;
    pthread_create(&t[0], 0, stdio_try, 0);
    pthread_join(t[0], &busy);
    funlockfile(one_stream);
    pthread_create(&t[0], 0, stdio_try, 0);
    pthread_join(t[0], &free_again);
    printf("pthreadtest: ftrylockfile from another thread: held %s, free %s\n",
           busy ? "busy" : "taken", free_again ? "busy" : "taken");
    fclose(one_stream);
    long opened = 0;
    for (long k = 0; k < 4; k++)
        pthread_create(&t[k], 0, stdio_open_close, (void *)k);
    for (int k = 0; k < 4; k++) {
        void *v;
        pthread_join(t[k], &v);
        opened += (long)v;
    }
    printf("pthreadtest: 4 threads opening and closing streams: %ld opened and closed, flush %d\n", opened,
           fflush(0));
}

int main(int argc, char **argv)
{
    pthread_t t[4];
    for (long i = 0; i < 4; i++)
        pthread_create(&t[i], 0, count, (void *)(i + 1));
    long sum = 0;
    for (int i = 0; i < 4; i++) {
        void *v;
        pthread_join(t[i], &v);
        sum += (long)v;
    }
    printf("pthreadtest: four threads counted %ld, their results add to %ld\n", counter, sum);

    pthread_t p;
    pthread_create(&p, 0, produce, 0);
    long got = 0;
    for (int i = 0; i < 100; i++) {
        pthread_mutex_lock(&q_lock);
        while (q_count == 0)
            pthread_cond_wait(&q_more, &q_lock);
        got += queue[q_head];
        q_head = (q_head + 1) % 4;
        q_count--;
        pthread_cond_signal(&q_room);
        pthread_mutex_unlock(&q_lock);
    }
    pthread_join(p, 0);
    printf("pthreadtest: a queue of 4 between two threads carried 100 numbers adding to %ld\n", got);
    stdio_threads();

    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    int kind = -1;
    pthread_mutexattr_gettype(&a, &kind);
    pthread_mutex_init(&shared, &a);
    int r1 = pthread_mutex_lock(&shared), r2 = pthread_mutex_lock(&shared);
    int busy = other(try_other);
    pthread_mutex_unlock(&shared);
    int still = other(try_other);
    pthread_mutex_unlock(&shared);
    int free_ = other(try_other);
    printf("pthreadtest: recursive (type %s): lock %s, again %s; another thread's trylock %s, "
           "after one unlock %s, after two %s\n", kind == PTHREAD_MUTEX_RECURSIVE ? "kept" : "lost",
           err(r1), err(r2), err(busy), err(still), err(free_));
    pthread_mutex_destroy(&shared);

    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&shared, &a);
    r1 = pthread_mutex_lock(&shared);
    r2 = pthread_mutex_lock(&shared);
    int theirs = other(unlock_other);
    int mine = pthread_mutex_unlock(&shared);
    int again = pthread_mutex_unlock(&shared);
    printf("pthreadtest: errorcheck: lock %s, again %s; another thread's unlock %s; unlock %s, "
           "again %s\n", err(r1), err(r2), err(theirs), err(mine), err(again));
    pthread_mutex_destroy(&shared);
    pthread_mutexattr_destroy(&a);

    pthread_mutex_init(&shared, 0);
    pthread_mutex_lock(&shared);
    printf("pthreadtest: a normal mutex held: another thread's trylock %s\n", err(other(try_other)));
    pthread_mutex_unlock(&shared);

    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t c = PTHREAD_COND_INITIALIZER;
    struct timespec until, t0;
    pthread_mutex_lock(&m);
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 150000000;
    if (until.tv_nsec >= 1000000000)
        until.tv_nsec -= 1000000000, until.tv_sec++;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int w = pthread_cond_timedwait(&c, &m, &until);
    long took = ms_since(&t0);
    pthread_mutex_unlock(&m);
    printf("pthreadtest: a timed wait nobody signals: %s after %s\n", err(w),
           took >= 120 && took < 2000 ? "about 150 ms" : "a wrong time");

    pthread_t o[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&o[i], 0, call_once_, 0);
    for (int i = 0; i < 4; i++)
        pthread_join(o[i], 0);
    pthread_once(&once, init_once);
    printf("pthreadtest: pthread_once from five threads: init ran %d time(s)\n", inits);

    pthread_key_create(&key, destroy);
    pthread_setspecific(key, (void *)1000);
    pthread_t k[3];
    long seen = 0;
    for (long i = 0; i < 3; i++)
        pthread_create(&k[i], 0, keep, (void *)(i + 1));
    for (int i = 0; i < 3; i++) {
        void *v;
        pthread_join(k[i], &v);
        seen += (long)v;
    }
    printf("pthreadtest: keys: each thread saw its own (%ld), destructors got %d, main's value %ld\n",
           seen, destroyed, (long)pthread_getspecific(key));
    printf("pthreadtest: pthread_key_delete %s\n", err(pthread_key_delete(key)));

    pthread_t e;
    pthread_create(&e, 0, leave, 0);
    void *v = 0;
    pthread_join(e, &v);
    printf("pthreadtest: pthread_exit's value: %ld\n", (long)v);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    int st = -1;
    pthread_attr_getdetachstate(&attr, &st);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t d;
    pthread_create(&d, &attr, detached, 0);
    pthread_mutex_lock(&gate_lock);
    while (!detached_ran)
        pthread_cond_wait(&gate, &gate_lock);
    gate_open = 1;
    pthread_cond_broadcast(&gate);
    pthread_mutex_unlock(&gate_lock);
    size_t size = 0;
    int big = pthread_attr_setstacksize(&attr, 1 << 20);
    pthread_attr_getstacksize(&attr, &size);
    printf("pthreadtest: attributes: joinable at first %s, a detached thread ran %s; stack size %s %zu, "
           "too small %s\n", st == PTHREAD_CREATE_JOINABLE ? "yes" : "no", detached_ran ? "yes" : "no",
           err(big), size, err(pthread_attr_setstacksize(&attr, 100)));
    pthread_attr_destroy(&attr);

    printf("pthreadtest: self equal to itself %s, to another %s; joining itself %s\n",
           pthread_equal(pthread_self(), pthread_self()) ? "yes" : "no",
           pthread_equal(pthread_self(), e) ? "yes" : "no", err(pthread_join(pthread_self(), 0)));
    sigset_t s, now;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR1);
    int r = pthread_sigmask(SIG_BLOCK, &s, 0);
    pthread_sigmask(SIG_BLOCK, 0, &now);
    printf("pthreadtest: pthread_sigmask %s, SIGUSR1 blocked %s, a wrong how %s\n", err(r),
           sigismember(&now, SIGUSR1) ? "yes" : "no", err(pthread_sigmask(99, &s, 0)));
    pthread_sigmask(SIG_UNBLOCK, &s, 0);
    rwlocks();

#ifndef __APPLE__
    if (argc == 2 && !strcmp(argv[1], "own"))
        own();
#else
    (void)argc, (void)argv;
#endif
    printf("pthreadtest: done\n");
    return 0;
}

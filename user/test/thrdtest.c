/*
 * Test: threads.h (22y). Mutexes under contention (every increment kept),
 * a queue with condition variables (every item through once), timed locks
 * and waits running out, a recursive mutex, call_once, thread-specific
 * values and their destructors, thread_local, detached threads (more than
 * the 16 numbers over time), thrd_exit's result, thrd_sleep; and a thread
 * waiting half a second in cnd_wait while the program uses (almost) no CPU
 * time: the kernel's futex sleeps, it does not spin.
 */
#include <stdio.h>
#include <threads.h>
#include <time.h>

#define T "thrdtest: "

static int failures;

static void ok(int good, const char *what)
{
    printf(T "%s %s\n", what, good ? "ok" : "FAIL");
    failures += !good;
}

static struct timespec in_ms(long ms)
{
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    t.tv_nsec += (ms % 1000) * 1000000;
    t.tv_sec += ms / 1000 + t.tv_nsec / 1000000000;
    t.tv_nsec %= 1000000000;
    return t;
}

static long ms_since(const struct timespec *start)
{
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    return (now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000;
}

/* --- a counter under contention --- */
#define WORKERS 8
#define ROUNDS 20000
static mtx_t counter_lock;
static volatile long counter;

static int add(void *arg)
{
    (void)arg;
    for (int i = 0; i < ROUNDS; i++) {
        mtx_lock(&counter_lock);
        long v = counter;
        if (i % 512 == 0)
            thrd_yield();                       /* (inside: others must wait for it) */
        counter = v + 1;
        mtx_unlock(&counter_lock);
    }
    return 0;
}

/* --- a queue: producers and consumers --- */
#define ITEMS 4000
static mtx_t queue_lock;
static cnd_t not_empty, not_full;
static int queue[16], head, tail, items_left = ITEMS;
static long taken_sum;

static int produce(void *arg)
{
    int base = *(int *)arg;
    for (int i = 0; i < ITEMS / 4; i++) {
        mtx_lock(&queue_lock);
        while (tail - head == 16)
            cnd_wait(&not_full, &queue_lock);
        queue[tail++ % 16] = base + i;
        cnd_signal(&not_empty);
        mtx_unlock(&queue_lock);
    }
    return 0;
}

static int consume(void *arg)
{
    (void)arg;
    for (;;) {
        mtx_lock(&queue_lock);
        while (tail == head && items_left > 0)
            cnd_wait(&not_empty, &queue_lock);
        if (items_left == 0) {
            mtx_unlock(&queue_lock);
            return 0;
        }
        taken_sum += queue[head++ % 16];
        if (--items_left == 0)
            cnd_broadcast(&not_empty);          /* (the others are done too) */
        cnd_signal(&not_full);
        mtx_unlock(&queue_lock);
    }
}

/* --- recursive, timed --- */
static mtx_t recursive, held;
static int try_result;

static int try_it(void *arg)
{
    (void)arg;
    try_result = mtx_trylock(&recursive);
    if (try_result == thrd_success)
        mtx_unlock(&recursive);
    return 0;
}

static int timed_lock(void *arg)
{
    struct timespec start;
    timespec_get(&start, TIME_UTC);
    struct timespec until = in_ms(60);
    int r = mtx_timedlock(&held, &until);
    *(long *)arg = ms_since(&start);
    return r;
}

/* --- once, thread-specific values, thread_local --- */
static once_flag once = ONCE_FLAG_INIT;
static volatile int once_runs;
static tss_t key;
static volatile int destroyed;
static thread_local int mine;

static void init_once(void)
{
    thrd_sleep(&(struct timespec){ 0, 20000000 }, 0);   /* (the others arrive meanwhile) */
    once_runs++;
}

static void destroy(void *value)
{
    __atomic_fetch_add(&destroyed, *(int *)value, __ATOMIC_SEQ_CST);
}

static int per_thread(void *arg)
{
    call_once(&once, init_once);
    static int values[WORKERS];
    int n = *(int *)arg;
    values[n] = n + 1;
    tss_set(key, &values[n]);
    mine = n * 10;
    thrd_yield();
    return mine == n * 10 && tss_get(key) == &values[n] ? 0 : 1;
}

/* --- detached threads, thrd_exit --- */
static volatile int detached_ran;

static int detached(void *arg)
{
    (void)arg;
    __atomic_fetch_add(&detached_ran, 1, __ATOMIC_SEQ_CST);
    return 0;
}

static int exits(void *arg)
{
    (void)arg;
    thrd_exit(42);
}

/* --- waiting without spinning --- */
static mtx_t wake_lock;
static cnd_t wake;
static int woken;

static int sleeper(void *arg)
{
    (void)arg;
    mtx_lock(&wake_lock);
    while (!woken)
        cnd_wait(&wake, &wake_lock);
    mtx_unlock(&wake_lock);
    return 0;
}

int main(void)
{
    thrd_t t[WORKERS];
    int args[WORKERS];

    mtx_init(&counter_lock, mtx_plain);
    for (int i = 0; i < WORKERS; i++)
        thrd_create(&t[i], add, 0);
    for (int i = 0; i < WORKERS; i++)
        thrd_join(t[i], 0);
    printf(T "counter %ld of %d\n", counter, WORKERS * ROUNDS);
    ok(counter == WORKERS * ROUNDS, "mutex");

    mtx_init(&queue_lock, mtx_plain);
    cnd_init(&not_empty);
    cnd_init(&not_full);
    for (int i = 0; i < 4; i++) {
        args[i] = i * 100000;
        thrd_create(&t[i], produce, &args[i]);
        thrd_create(&t[4 + i], consume, 0);
    }
    for (int i = 0; i < WORKERS; i++)
        thrd_join(t[i], 0);
    long want = 0;
    for (int p = 0; p < 4; p++)
        for (int i = 0; i < ITEMS / 4; i++)
            want += p * 100000 + i;
    ok(taken_sum == want && head == ITEMS, "condition variables");

    mtx_init(&recursive, mtx_plain | mtx_recursive);
    mtx_lock(&recursive);
    mtx_lock(&recursive);
    int again = mtx_trylock(&recursive);
    thrd_create(&t[0], try_it, 0);
    thrd_join(t[0], 0);
    int busy = try_result;
    mtx_unlock(&recursive);
    mtx_unlock(&recursive);
    int still = mtx_trylock(&recursive) == thrd_success;   /* (ours: once more held) */
    mtx_unlock(&recursive);
    mtx_unlock(&recursive);
    thrd_create(&t[0], try_it, 0);
    thrd_join(t[0], 0);
    ok(again == thrd_success && busy == thrd_busy && still && try_result == thrd_success,
       "recursive mutex");

    mtx_init(&held, mtx_timed);
    mtx_lock(&held);
    long waited = 0;
    int r = 0;
    thrd_create(&t[0], timed_lock, &waited);
    thrd_join(t[0], &r);
    mtx_unlock(&held);
    printf(T "timedlock %d after %ld ms\n", r, waited);
    struct timespec start, until = in_ms(50);
    timespec_get(&start, TIME_UTC);
    mtx_lock(&held);
    cnd_t never;
    cnd_init(&never);
    int tw = cnd_timedwait(&never, &held, &until);
    long cw = ms_since(&start);
    mtx_unlock(&held);
    printf(T "timedwait %d after %ld ms\n", tw, cw);
    ok(r == thrd_timedout && waited >= 50 && waited < 1000 && tw == thrd_timedout && cw >= 40 &&
       cw < 1000, "timeouts");

    tss_create(&key, destroy);
    int bad = 0;
    for (int i = 0; i < WORKERS; i++) {
        args[i] = i;
        thrd_create(&t[i], per_thread, &args[i]);
    }
    for (int i = 0; i < WORKERS; i++) {
        int res;
        thrd_join(t[i], &res);
        bad += res;
    }
    printf(T "once %d, destructors %d of %d\n", once_runs, destroyed, WORKERS * (WORKERS + 1) / 2);
    ok(once_runs == 1 && bad == 0 && destroyed == WORKERS * (WORKERS + 1) / 2 && mine == 0,
       "once, tss, thread_local");

    int made = 0;
    for (int round = 0; round < 40; round++) {
        thrd_t d;
        if (thrd_create(&d, detached, 0) == thrd_success) {
            thrd_detach(d);
            made++;
        }
        if (round % 10 == 9)
            thrd_sleep(&(struct timespec){ 0, 30000000 }, 0);
    }
    thrd_sleep(&(struct timespec){ 0, 100000000 }, 0);
    printf(T "detached: %d made, %d ran\n", made, detached_ran);
    int result = 0;
    thrd_t e;
    thrd_create(&e, exits, 0);
    thrd_join(e, &result);
    ok(made == 40 && detached_ran == 40 && result == 42 && thrd_equal(thrd_current(), thrd_current()) &&
       !thrd_equal(thrd_current(), e), "detach, exit");

    mtx_init(&wake_lock, mtx_plain);
    cnd_init(&wake);
    thrd_create(&t[0], sleeper, 0);
    clock_t before = clock();
    timespec_get(&start, TIME_UTC);
    thrd_sleep(&(struct timespec){ 0, 500000000 }, 0);
    mtx_lock(&wake_lock);
    woken = 1;
    cnd_signal(&wake);
    mtx_unlock(&wake_lock);
    thrd_join(t[0], 0);
    long cpu_ms = (long)((clock() - before) * 1000 / CLOCKS_PER_SEC), wall = ms_since(&start);
    printf(T "waited %ld ms using %ld ms of CPU\n", wall, cpu_ms);
    ok(wall >= 450 && cpu_ms < 100, "waiting sleeps");

    printf(T "%s\n", failures ? "FAILED" : "all ok");
    return failures != 0;
}

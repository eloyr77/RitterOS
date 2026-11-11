/*
 * Test (65c-2): threads on several cores at once. N threads (argument,
 * default 4) each compute the same sum, add to a counter with an atomic
 * instruction and to another under a mutex, many times; the sums must be
 * identical and both counters exact (on several cores the threads truly
 * overlap: a lost update shows as a smaller count). The computation is
 * timed alone and with all N at once (the counters after, untimed: the
 * mutex makes them take turns): on N free cores the N take about as long
 * as one ("parallel" is N times one thread's time divided by theirs).
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define ADDS 200000
#if defined(__x86_64__)
#define STEPS 3000000L                  /* emulated: slower */
#else
#define STEPS 200000000L
#endif

static volatile long atomic_counter;
static long locked_counter;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t sums[64];

static uint64_t work(void)
{
    uint64_t x = 12345, sum = 0;
    for (long i = 0; i < STEPS; i++) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        sum += x >> 33;
    }
    return sum;
}

static void *computer(void *arg)
{
    sums[(long)arg] = work();
    return 0;
}

static void *counter(void *arg)
{
    (void)arg;
    for (int i = 0; i < ADDS; i++) {
        __atomic_fetch_add(&atomic_counter, 1, __ATOMIC_RELAXED);
        pthread_mutex_lock(&lock);
        locked_counter++;
        pthread_mutex_unlock(&lock);
    }
    return 0;
}

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    long n = argc > 1 ? atol(argv[1]) : 4;
    if (n < 1 || n > 64)
        n = 4;
    long start = now_ms();
    uint64_t alone = work();
    long one = now_ms() - start;

    pthread_t t[64];
    start = now_ms();
    for (long i = 0; i < n; i++)
        pthread_create(&t[i], 0, computer, (void *)i);
    for (long i = 0; i < n; i++)
        pthread_join(t[i], 0);
    long all = now_ms() - start;
    for (long i = 0; i < n; i++)
        pthread_create(&t[i], 0, counter, 0);
    for (long i = 0; i < n; i++)
        pthread_join(t[i], 0);

    int same = 1;
    for (long i = 0; i < n; i++)
        same &= sums[i] == alone;
    printf("cores: %ld threads, sums %s, atomic counter %ld of %ld, locked counter %ld of %ld\n",
           n, same ? "identical" : "DIFFERENT", atomic_counter, n * ADDS, locked_counter,
           n * ADDS);
    long tenths = all ? n * one * 10 / all : 0;
    printf("cores: one thread alone %ld ms, %ld at once %ld ms: parallel %ld.%ld\n", one, n,
           all, tenths / 10, tenths % 10);
    return 0;
}

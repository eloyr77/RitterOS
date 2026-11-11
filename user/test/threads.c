/*
 * Test: threads of one program (thread.h).
 *
 * threads          four workers compute in parallel with floating point
 *                  (each must match the same calculation done alone, bit
 *                  for bit), increment a shared counter atomically, report
 *                  their numbers; all are joined; then 15 threads at once
 *                  (all numbers taken: the 16th must fail), joined again
 * threads exit     three threads compute forever, main ends the program
 *                  with exit(7): all of them must stop
 * threads spin     three threads compute forever, main waits for one of
 *                  them: Ctrl-C must end the program
 * threads fault    a thread writes to address 0: the program ends
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <thread.h>
#include <unistd.h>
#include "ritteros.h"

#define WORKERS 4
#define ADDS    100000

#if defined(__x86_64__)
#define STEPS 300000L                   /* emulated: slower */
#else
#define STEPS 3000000L
#endif

static volatile uint64_t counter;
static double results[WORKERS + 1];

static double work(int seed)
{
    double x = seed, y = 1.0 / seed, z = 0.5;
    for (long i = 1; i <= STEPS; i++) {
        x = x * 0.9999999 + y / (double)i;
        y = y * 0.999999 + z * 1e-9;
        z = z + x * 1e-12 - y * 1e-13;
    }
    return x + y + z;
}

static int worker(void *arg)
{
    int seed = (int)(intptr_t)arg;
    for (int i = 0; i < ADDS; i++)
        __atomic_fetch_add(&counter, 1, __ATOMIC_RELAXED);
    results[seed] = work(seed);
    return 100 + thread_self();
}

static int forever(void *arg)
{
    (void)arg;
    volatile uint64_t n = 0;
    for (;;)
        n++;
    return 0;
}

static int idle(void *arg)
{
    (void)arg;
    sleep(1);
    return 0;
}

static int crash(void *arg)
{
    (void)arg;
    *(volatile int *)0 = 1;
    return 0;
}

/* The descriptor-table stress (49b): threads of one program share the
 * table, so claiming, closing and using a descriptor must be safe when
 * they happen at once. */
#define FD_LOOPS 40
static volatile int mismatches, sockerr;
static volatile int blocker_fd = -1;    /* the socket the recv-during-close test blocks on */

/* Each worker opens its own file (holding its own id), reads it back and
 * checks it got its own file, not another thread's (a slot collision would
 * cross them); it also makes, binds and closes a UDP socket each time. */
static int fd_worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    char path[32];
    snprintf(path, sizeof(path), "/fdtest/f%d", id);
    for (int i = 0; i < FD_LOOPS; i++) {
        int fd = open(path, O_RDONLY);
        char c = 0;
        if (fd < 0 || read(fd, &c, 1) != 1 || c != (char)('0' + id))
            __atomic_fetch_add(&mismatches, 1, __ATOMIC_RELAXED);
        if (fd >= 0)
            close(fd);
        int s = ritteros_socket(SOCKET_UDP);
        if (s < 0 || ritteros_bind(s, 0) < 0)
            __atomic_fetch_add(&sockerr, 1, __ATOMIC_RELAXED);
        if (s >= 0)
            close(s);
    }
    return 0;
}

/* Blocks in recvfrom on a UDP socket; main closes that descriptor from
 * another thread meanwhile. With the fix the socket is held across the
 * call, so this returns a clean timeout; without it the socket is freed
 * under the sleeping thread. */
static int recv_blocker(void *arg)
{
    (void)arg;
    int fd = ritteros_socket(SOCKET_UDP);
    if (fd < 0 || ritteros_bind(fd, 0) < 0)
        return -1;
    blocker_fd = fd;
    char buf[8];
    ssize_t n = ritteros_recvfrom(fd, buf, sizeof(buf), 0, 0, 2000);
    return n < 0 && errno == ETIMEDOUT ? 0 : 1;   /* 0: survived with a clean timeout */
}

static int fds_mode(void)
{
    mkdir("/fdtest", 0755);
    for (int i = 0; i < WORKERS; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/fdtest/f%d", i);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        char c = (char)('0' + i);
        if (fd >= 0) {
            write(fd, &c, 1);
            close(fd);
        }
    }
    int recv_t = thread_create(recv_blocker, 0);
    int workers[WORKERS];
    for (int i = 0; i < WORKERS; i++)
        workers[i] = thread_create(fd_worker, (void *)(intptr_t)i);
    sleep(1);                           /* the blocker is now parked in recvfrom */
    int closed = blocker_fd >= 0 ? close(blocker_fd) : -1;
    for (int i = 0; i < WORKERS; i++)
        thread_join(workers[i], 0);
    int recv_result = 1;
    thread_join(recv_t, &recv_result);
    printf("threads: fds: %d opens and sockets across %d threads, %d slot mismatches, "
           "%d socket errors\n", WORKERS * FD_LOOPS, WORKERS, mismatches, sockerr);
    printf("threads: fds: close of a socket under a blocked recvfrom: %s, recv %s\n",
           closed == 0 ? "closed" : "FAILED",
           recv_result == 0 ? "survived (timed out)" : "WRONG");
    return mismatches == 0 && sockerr == 0 && closed == 0 && recv_result == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    if (strcmp(mode, "fds") == 0)
        return fds_mode();
    if (strcmp(mode, "exit") == 0 || strcmp(mode, "spin") == 0) {
        int first = -1;
        for (int i = 0; i < 3; i++) {
            int n = thread_create(forever, 0);
            if (first < 0)
                first = n;
        }
        printf("threads: 3 threads computing forever\n");
        if (strcmp(mode, "exit") == 0) {
            sleep(1);
            exit(7);                    /* ends all of them */
        }
        thread_join(first, 0);          /* waits until Ctrl-C */
        return 1;
    }
    if (strcmp(mode, "fault") == 0) {
        int n = thread_create(crash, 0);
        thread_join(n, 0);
        printf("threads: the crashing thread did not end the program\n");
        return 1;
    }

    /* The workers, side by side. */
    int numbers[WORKERS];
    for (int i = 0; i < WORKERS; i++) {
        numbers[i] = thread_create(worker, (void *)(intptr_t)(i + 1));
        if (numbers[i] < 0) {
            printf("threads: thread_create failed\n");
            return 1;
        }
    }
    int ok = 1;
    for (int i = 0; i < WORKERS; i++) {
        int result;
        if (thread_join(numbers[i], &result) < 0 || result != 100 + numbers[i])
            ok = 0;
    }
    /* The same calculations alone, here: they must match bit for bit. */
    int same = 0;
    for (int i = 1; i <= WORKERS; i++) {
        double alone = work(i);
        if (memcmp(&alone, &results[i], sizeof(alone)) == 0)
            same++;
    }
    printf("threads: %d workers (numbers %d %d %d %d), results %s, %d of %d sums identical, "
           "counter %lu of %d\n", WORKERS, numbers[0], numbers[1], numbers[2], numbers[3],
           ok ? "returned" : "WRONG", same, WORKERS, (unsigned long)counter, WORKERS * ADDS);

    /* All numbers: PROCESS_THREADS - 1 threads next to main (255 since 54d),
     * the next one refused. */
    static int many[PROCESS_THREADS];
    int started = 0;
    for (int i = 0; i < PROCESS_THREADS; i++) {
        many[i] = thread_create(idle, 0);
        if (many[i] >= 0)
            started++;
    }
    int refused = many[PROCESS_THREADS - 1] < 0 && errno == EAGAIN;
    for (int i = 0; i < PROCESS_THREADS; i++)
        if (many[i] >= 0)
            thread_join(many[i], 0);
    printf("threads: %d started at once, the next one %s, all joined; joining again: %s\n",
           started, refused ? "refused (EAGAIN)" : "NOT REFUSED",
           thread_join(many[0], 0) < 0 && errno == ESRCH ? "no such thread" : "WRONG");
    return ok && same == WORKERS && counter == WORKERS * ADDS && started == PROCESS_THREADS - 1 && refused ? 0 : 1;
}

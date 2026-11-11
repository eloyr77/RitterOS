/*
 * Test: swap at work (step 35b). `swaptest MIB [ROUNDS]` maps MIB MiB of
 * anonymous memory and, ROUNDS times (2 if not given), writes every page with
 * that round's own content, then reads all of it back and checks every word.
 * With more MiB than the machine has free, pageout writes pages to swap and
 * the reads bring them back; swap's counters (SWAP_STATUS) before and after
 * say how many pages went out and came in. A run killed in the middle leaves
 * its slots free again (the program's end gives them back). With `fork`
 * last, a child forked after the rounds reads every word (35c: its entries
 * name the parent's slots, the pages they share go to swap once), then
 * writes a round of its own over all of it and reads that back; the parent
 * then reads its own last round again (the child's writes are the child's).
 * With `shared` last instead (35c-2), the memory is MAP_SHARED: a forked
 * child writes each round and the parent reads every word of it (one memory
 * for both, its pages in swap meanwhile).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "ritteros.h"

static double seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* Word w of page p in round r: no two words alike. */
static uint64_t word(uint64_t r, uint64_t p, uint64_t w)
{
    return (r + 1) * 0x9E3779B97F4A7C15ULL ^ p * 0xD1B54A32D192ED03ULL ^ w;
}

/* (35c-2) A child writes each round into shared memory, the parent reads it
 * all: they take turns over two pipes. */
static int shared(long mib, long rounds)
{
    uint64_t pages = (uint64_t)mib * 256, words = 4096 / 8;
    uint64_t *m = mmap(0, pages * 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS,
                       -1, 0);
    int to_parent[2], to_child[2];
    if (m == MAP_FAILED || pipe(to_parent) || pipe(to_child)) {
        perror("swaptest: shared");
        return 1;
    }
    struct sys_swap before, after;
    ritteros_swap(SWAP_STATUS, &before);
    pid_t child = fork();
    if (child == 0) {
        char go = 0;
        for (long r = 0; r < rounds; r++) {
            for (uint64_t p = 0; p < pages; p++)
                for (uint64_t w = 0; w < words; w++)
                    m[p * words + w] = word((uint64_t)r, p, w);
            if (write(to_parent[1], &go, 1) != 1 || read(to_child[0], &go, 1) != 1)
                _exit(1);
        }
        _exit(0);
    }
    for (long r = 0; r < rounds; r++) {
        char go = 0;
        if (child < 0 || read(to_parent[0], &go, 1) != 1) {
            printf("swaptest: shared: the child stopped\n");
            return 1;
        }
        uint64_t bad = 0;
        for (uint64_t p = 0; p < pages; p++)
            for (uint64_t w = 0; w < words; w++)
                if (m[p * words + w] != word((uint64_t)r, p, w))
                    bad++;
        printf("swaptest: shared: round %ld: the child wrote %ld MiB, the parent read %s\n",
               r + 1, mib, bad ? "WRONG words" : "every word as written");
        if (bad || write(to_child[1], &go, 1) != 1)
            return 1;
    }
    int status = 0;
    waitpid(child, &status, 0);
    ritteros_swap(SWAP_STATUS, &after);
    printf("swaptest: shared: done; meanwhile %llu pages written to swap, %llu read back\n",
           (unsigned long long)(after.written - before.written),
           (unsigned long long)(after.read - before.read));
    return WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : 1;
}

int main(int argc, char **argv)
{
    long mib = argc > 1 ? atol(argv[1]) : 0, rounds = argc > 2 ? atol(argv[2]) : 2;
    int forking = argc > 3 && !strcmp(argv[3], "fork");
    int sharing = argc > 3 && !strcmp(argv[3], "shared");
    if (mib <= 0 || rounds <= 0 || (argc > 3 && !forking && !sharing) || argc > 4) {
        fprintf(stderr, "usage: swaptest MIB [ROUNDS [fork | shared]]\n");
        return 2;
    }
    if (sharing)
        return shared(mib, rounds);
    uint64_t pages = (uint64_t)mib * 256, words = 4096 / 8;
    uint64_t *m = mmap(0, pages * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                       -1, 0);
    if (m == MAP_FAILED) {
        perror("swaptest: mmap");
        return 1;
    }
    struct sys_swap before, after;
    ritteros_swap(SWAP_STATUS, &before);
    for (long r = 0; r < rounds; r++) {
        double t0 = seconds();
        for (uint64_t p = 0; p < pages; p++)
            for (uint64_t w = 0; w < words; w++)
                m[p * words + w] = word((uint64_t)r, p, w);
        double t1 = seconds();
        uint64_t bad = 0;
        for (uint64_t p = 0; p < pages; p++)
            for (uint64_t w = 0; w < words; w++)
                if (m[p * words + w] != word((uint64_t)r, p, w))
                    bad++;
        double t2 = seconds();
        printf("swaptest: round %ld: %ld MiB written in %.1f s, read back in %.1f s: %s\n",
               r + 1, mib, t1 - t0, t2 - t1, bad ? "WRONG" : "every word as written");
        if (bad) {
            printf("swaptest: %llu words differ\n", (unsigned long long)bad);
            return 1;
        }
    }
    if (forking) {
        struct sys_swap at_fork, ended;
        ritteros_swap(SWAP_STATUS, &at_fork);
        pid_t child = fork();
        if (child == 0) {
            uint64_t bad = 0;
            for (uint64_t p = 0; p < pages; p++)
                for (uint64_t w = 0; w < words; w++)
                    if (m[p * words + w] != word((uint64_t)rounds - 1, p, w))
                        bad++;
            for (uint64_t p = 0; p < pages; p++)
                for (uint64_t w = 0; w < words; w++)
                    m[p * words + w] = word((uint64_t)rounds, p, w);
            for (uint64_t p = 0; p < pages; p++)
                for (uint64_t w = 0; w < words; w++)
                    if (m[p * words + w] != word((uint64_t)rounds, p, w))
                        bad++;
            _exit(bad ? 1 : 0);
        }
        int status = 0;
        if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
            WEXITSTATUS(status)) {
            printf("swaptest: the forked child did not read every word as written\n");
            return 1;
        }
        ritteros_swap(SWAP_STATUS, &ended);
        uint64_t bad = 0;
        for (uint64_t p = 0; p < pages; p++)
            for (uint64_t w = 0; w < words; w++)
                if (m[p * words + w] != word((uint64_t)rounds - 1, p, w))
                    bad++;
        printf("swaptest: forked with %llu slots in use; the child read every word as written, "
               "wrote its own and read them back (%llu pages read back), then the parent %s\n",
               (unsigned long long)at_fork.used, (unsigned long long)(ended.read - at_fork.read),
               bad ? "did NOT" : "too");
        if (bad)
            return 1;
    }
    ritteros_swap(SWAP_STATUS, &after);
    printf("swaptest: done; meanwhile %llu pages written to swap, %llu read back\n",
           (unsigned long long)(after.written - before.written),
           (unsigned long long)(after.read - before.read));
    return 0;
}

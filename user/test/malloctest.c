/*
 * Test: the user-space heap (malloc/free/calloc/realloc). Allocates many
 * blocks with position-dependent patterns, frees them in a shuffled order and
 * checks nothing was corrupted, exercises calloc (zeroed) and realloc (keeps
 * contents, grows), and forces several kernel arenas with big allocations.
 * Since 22h: four threads allocating and freeing at once keep every block
 * intact (the heap's lock), and freed memory goes back to the kernel (32 MiB
 * of small blocks and a 16 MiB one: free memory is back after free, but for
 * the one arena kept and page tables).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ritteros.h"
#include "thread.h"

#define N 400

static uint8_t pat(int i, long k)
{
    return (uint8_t)(i * 131 + k * 7 + 3);
}

static int test_many(void)
{
    static void *p[N];
    static int sz[N];
    for (int i = 0; i < N; i++) {           /* allocate blocks of varied sizes, fill them */
        sz[i] = 1 + (i * 37) % 9000;
        p[i] = malloc((size_t)sz[i]);
        if (!p[i]) {
            printf("malloctest: FAIL malloc %d (%d bytes)\n", i, sz[i]);
            return 1;
        }
        for (int k = 0; k < sz[i]; k++)
            ((uint8_t *)p[i])[k] = pat(i, k);
    }
    for (int i = 0; i < N; i += 2) {        /* free every other one */
        for (int k = 0; k < sz[i]; k++)
            if (((uint8_t *)p[i])[k] != pat(i, k)) {
                printf("malloctest: FAIL corrupt at block %d byte %d\n", i, k);
                return 1;
            }
        free(p[i]);
        p[i] = 0;
    }
    for (int i = 0; i < N; i += 2) {        /* reallocate into the holes */
        sz[i] = 1 + (i * 53) % 7000;
        p[i] = malloc((size_t)sz[i]);
        if (!p[i]) {
            printf("malloctest: FAIL realloc-hole %d\n", i);
            return 1;
        }
        memset(p[i], 0, (size_t)sz[i]);
    }
    for (int i = 1; i < N; i += 2)          /* the untouched odd blocks must be intact */
        for (int k = 0; k < sz[i]; k++)
            if (((uint8_t *)p[i])[k] != pat(i, k)) {
                printf("malloctest: FAIL odd block %d changed at %d\n", i, k);
                return 1;
            }
    for (int i = 0; i < N; i++)
        free(p[i]);
    printf("malloctest: many ok\n");
    return 0;
}

static int test_calloc_realloc(void)
{
    size_t n = 5000;
    uint8_t *c = calloc(n, 1);
    if (!c) { printf("malloctest: FAIL calloc\n"); return 1; }
    for (size_t k = 0; k < n; k++)
        if (c[k] != 0) { printf("malloctest: FAIL calloc not zero at %lu\n", (unsigned long)k); return 1; }
    for (size_t k = 0; k < n; k++)
        c[k] = (uint8_t)(k * 29 + 1);
    c = realloc(c, n * 4);                  /* grow: contents must survive */
    if (!c) { printf("malloctest: FAIL realloc grow\n"); return 1; }
    for (size_t k = 0; k < n; k++)
        if (c[k] != (uint8_t)(k * 29 + 1)) {
            printf("malloctest: FAIL realloc lost data at %lu\n", (unsigned long)k);
            return 1;
        }
    free(c);
    printf("malloctest: calloc/realloc ok\n");
    return 0;
}

static int test_big(void)
{
    /* 24 x 1 MiB: each a mapping of its own (22h; until 22a the arenas were
     * 16 MiB shared buffers, and this made morecore run again). */
    enum { COUNT = 24, SIZE = 1024 * 1024 };
    static uint8_t *b[COUNT];
    for (int i = 0; i < COUNT; i++) {
        b[i] = malloc(SIZE);
        if (!b[i]) { printf("malloctest: FAIL big malloc %d\n", i); return 1; }
        b[i][0] = (uint8_t)i;
        b[i][SIZE - 1] = (uint8_t)(i + 100);
    }
    for (int i = 0; i < COUNT; i++)
        if (b[i][0] != (uint8_t)i || b[i][SIZE - 1] != (uint8_t)(i + 100)) {
            printf("malloctest: FAIL big readback %d\n", i);
            return 1;
        }
    for (int i = 0; i < COUNT; i++)
        free(b[i]);
    printf("malloctest: big ok\n");
    return 0;
}

#define THREADS 4
#define ROUNDS 6000
#define LIVE 48

static uint8_t tpat(int t, int slot, int round, long k)
{
    return (uint8_t)(t * 61 + slot * 13 + round * 5 + k * 3 + 1);
}

/* A thread's share: blocks of varied sizes in LIVE slots, each checked and
 * replaced over and over, while the other threads do the same. */
static int worker(void *arg)
{
    int t = (int)(intptr_t)arg;
    uint8_t *slot[LIVE] = { 0 };
    int size[LIVE] = { 0 }, made[LIVE] = { 0 };
    uint32_t seed = 12345u + (uint32_t)t * 7919u;
    int bad = 0;
    for (int r = 0; r < ROUNDS && !bad; r++) {
        seed = seed * 1103515245u + 12345u;
        int i = (int)((seed >> 16) % LIVE);
        if (slot[i]) {
            for (long k = 0; k < size[i]; k++)
                if (slot[i][k] != tpat(t, i, made[i], k)) {
                    bad = 1;
                    break;
                }
            free(slot[i]);
        }
        size[i] = 1 + (int)((seed >> 4) % 3000);
        made[i] = r;
        slot[i] = malloc((size_t)size[i]);
        if (!slot[i]) {
            bad = 1;
            break;
        }
        for (long k = 0; k < size[i]; k++)
            slot[i][k] = tpat(t, i, r, k);
    }
    for (int i = 0; i < LIVE; i++)
        free(slot[i]);
    return bad;
}

static int test_threads(void)
{
    int number[THREADS], bad = 0;
    for (int t = 0; t < THREADS; t++)
        number[t] = thread_create(worker, (void *)(intptr_t)t);
    for (int t = 0; t < THREADS; t++) {
        int result = 1;
        if (number[t] < 0 || thread_join(number[t], &result) != 0 || result)
            bad++;
    }
    if (bad) {
        printf("malloctest: FAIL threads: %d of %d found a block changed or got none\n", bad, THREADS);
        return 1;
    }
    printf("malloctest: threads ok (%d threads, %d blocks each, all intact)\n", THREADS, ROUNDS);
    return 0;
}

static long free_pages(void)
{
    struct sys_info info;
    return ritteros_sysinfo(&info) == 0 ? (long)info.free_pages : 0;
}

static int test_give_back(void)
{
    enum { SMALL = 2048, SMALL_SIZE = 16 * 1024, LARGE = 16 * 1024 * 1024 };
    static uint8_t *b[SMALL];
    long before = free_pages();
    for (int i = 0; i < SMALL; i++) {               /* 32 MiB in arenas */
        if (!(b[i] = malloc(SMALL_SIZE))) {
            printf("malloctest: FAIL give back: malloc %d\n", i);
            return 1;
        }
        memset(b[i], i, SMALL_SIZE);
    }
    uint8_t *large = malloc(LARGE);                 /* a mapping of its own */
    if (!large) {
        printf("malloctest: FAIL give back: a 16 MiB block\n");
        return 1;
    }
    memset(large, 0x5A, LARGE);
    long used = before - free_pages();
    for (int i = SMALL - 1; i >= 0; i -= 2)         /* in an order that splits and joins */
        free(b[i]);
    for (int i = 0; i < SMALL; i += 2)
        free(b[i]);
    free(large);
    long left = before - free_pages();
    /* used: 12288 pages and their tables, less what the heap had free from
     * the tests before (an arena or two); left: the arena kept (256) and the
     * page tables, which stay until the program ends. */
    int ok = used >= 12288 - 512 && left < 400 && left > -64;
    printf("malloctest: give back %s (48 MiB: %ld pages used, %ld after free)\n",
           ok ? "ok" : "FAILED", used, left);
    return !ok;
}

int main(void)
{
    if (test_many() || test_calloc_realloc() || test_big() || test_threads() || test_give_back())
        return 1;
    printf("malloctest: all ok\n");
    return 0;
}

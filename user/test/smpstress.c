/*
 * Test (65c-3): several cores at once, for a few seconds, every part
 * checked. Each check is something that one core cannot get wrong, only
 * cores running together:
 *
 *   tlb     one thread maps a fresh page over the same address again and
 *           again (MAP_FIXED: the old page is unmapped and freed), writing
 *           a new generation number into it; another thread reads the
 *           address all the time. A number older than the one announced
 *           means a core kept a translation the kernel took away.
 *   fork    threads fork while others write to the program's memory; each
 *           child checks the pages it got are what they were at its fork
 *           (its marker, then only its own writes), the parent that its own
 *           are untouched by the child's writes (copy-on-write).
 *   heap    threads allocate, fill and free blocks of random sizes; every
 *           byte is checked before it is freed.
 *   pipe    threads send numbered blocks through pipes to each other;
 *           every block arrives whole and in order.
 *   files   threads open, write, read back and close files of their own.
 *   spawn   threads start /bin/true-like programs (posix_spawn) and wait.
 *   append  (65e-36) threads append numbered lines to one file opened with
 *           O_APPEND; every line is in it at the end, whole (an append finds
 *           the end and writes in one step).
 *   tcp     (65e-35) threads send numbered blocks over TCP connections of
 *           their own (loopback, one listener each); every block arrives
 *           whole and in order, and every connection carries some.
 *   faults  (65e-38) threads of one program fault pages in at once: first
 *           all of them over one fresh area, page by page in the same order
 *           (two threads often fault one page together), each writing its
 *           own word of every page; then each maps fresh areas of its own,
 *           touches, checks and unmaps them. No word may be lost (a page
 *           made twice), every fresh page starts zeroed.
 *   wakeups (65e-39) pairs of threads pass a numbered token back and forth
 *           through two pipes each: every pass wakes the other thread of
 *           the pair (the scheduler's work); every token comes back as sent.
 *   descriptors (65e-42) one thread keeps putting a fresh pipe's read end
 *           (64 bytes of one letter in it, its write end closed) at one
 *           descriptor number with dup2, so each switch closes the open
 *           file there, and now and then closes the number; others read
 *           through that number all the time: every read is one letter
 *           only, the end of a pipe, or EBADF.
 *
 * smpstress [SECONDS [PART...]] (default 5 s, every part) prints a line per
 * part with its counts and "ok", or what went wrong.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static volatile int stop;
static long seconds = 5;

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* --- tlb ------------------------------------------------------------------- */

static volatile uint64_t *tlb_page;
static volatile uint64_t announced;         /* the generation the page holds now */
static long tlb_rounds, tlb_reads, tlb_stale;
static uint64_t tlb_worst_seen, tlb_worst_announced;

static void *tlb_writer(void *arg)
{
    (void)arg;
    for (uint64_t g = 1; !stop; g++) {
        void *p = mmap((void *)tlb_page, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (p != (void *)tlb_page) {
            printf("smpstress: tlb: mmap failed (%d)\n", errno);
            exit(1);
        }
        tlb_page[0] = g;
        __atomic_store_n(&announced, g, __ATOMIC_RELEASE);
        tlb_rounds++;
    }
    return 0;
}

static void *tlb_reader(void *arg)
{
    (void)arg;
    while (!stop) {
        uint64_t a = __atomic_load_n(&announced, __ATOMIC_ACQUIRE);
        uint64_t v = tlb_page[0];
        __atomic_fetch_add(&tlb_reads, 1, __ATOMIC_RELAXED);
        /* The page we read was mapped at the latest when a was announced:
         * it holds a, or a newer one (or 0 just after a new map). */
        if (v && v < a) {
            __atomic_fetch_add(&tlb_stale, 1, __ATOMIC_RELAXED);
            tlb_worst_seen = v;
            tlb_worst_announced = a;
        }
    }
    return 0;
}

/* --- fork ------------------------------------------------------------------ */

#define FORK_PAGES 16
static uint64_t *shared_area;               /* written by the writer threads */
static long forks, fork_bad;

static void *fork_scribbler(void *arg)
{
    long me = (long)arg;
    for (uint64_t n = 0; !stop; n++)
        shared_area[(me * 509 + n) % (FORK_PAGES * 512)] = n;
    return 0;
}

static void *forker(void *arg)
{
    (void)arg;
    static uint64_t mine[FORK_PAGES * 512];
    for (uint64_t round = 1; !stop; round++) {
        for (int i = 0; i < FORK_PAGES * 512; i++)
            mine[i] = round * 1000003 + i;
        pid_t pid = fork();
        if (pid == 0) {
            /* The child: the values as at the fork, then its own. */
            for (int i = 0; i < FORK_PAGES * 512; i++)
                if (mine[i] != round * 1000003 + i)
                    _exit(2);
            for (int i = 0; i < FORK_PAGES * 512; i++)
                mine[i] = ~(uint64_t)i;
            for (int i = 0; i < FORK_PAGES * 512; i++)
                if (mine[i] != ~(uint64_t)i)
                    _exit(3);
            _exit(0);
        }
        if (pid < 0) {
            printf("smpstress: fork: fork failed (%d)\n", errno);
            exit(1);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            __atomic_fetch_add(&fork_bad, 1, __ATOMIC_RELAXED);
        for (int i = 0; i < FORK_PAGES * 512; i++)
            if (mine[i] != round * 1000003 + i) {
                __atomic_fetch_add(&fork_bad, 1, __ATOMIC_RELAXED);
                break;
            }
        __atomic_fetch_add(&forks, 1, __ATOMIC_RELAXED);
    }
    return 0;
}

/* --- heap ------------------------------------------------------------------ */

static long heap_blocks, heap_bad;

static void *heaper(void *arg)
{
    uint32_t seed = (uint32_t)(long)arg * 2654435761u + 1;
    enum { KEEP = 64 };
    unsigned char *blocks[KEEP] = { 0 };
    size_t sizes[KEEP] = { 0 };
    for (long n = 0; !stop; n++) {
        int k = (int)(n % KEEP);
        if (blocks[k]) {
            for (size_t i = 0; i < sizes[k]; i++)
                if (blocks[k][i] != (unsigned char)(sizes[k] + i)) {
                    __atomic_fetch_add(&heap_bad, 1, __ATOMIC_RELAXED);
                    break;
                }
            free(blocks[k]);
        }
        seed = seed * 1103515245 + 12345;
        sizes[k] = 1 + (seed >> 8) % (seed & 1 ? 300 : 70000);
        blocks[k] = malloc(sizes[k]);
        if (!blocks[k]) {
            printf("smpstress: heap: malloc failed\n");
            exit(1);
        }
        for (size_t i = 0; i < sizes[k]; i++)
            blocks[k][i] = (unsigned char)(sizes[k] + i);
        __atomic_fetch_add(&heap_blocks, 1, __ATOMIC_RELAXED);
    }
    for (int k = 0; k < KEEP; k++)
        free(blocks[k]);
    return 0;
}

/* --- pipe ------------------------------------------------------------------ */

static long pipe_blocks, pipe_bad;

struct pipe_pair {
    int fd[2];
};

static void *pipe_sender(void *arg)
{
    struct pipe_pair *p = arg;
    uint32_t block[256];
    for (uint32_t n = 0; !stop; n++) {
        for (int i = 0; i < 256; i++)
            block[i] = n * 256 + (uint32_t)i;
        size_t done = 0;
        while (done < sizeof(block)) {
            ssize_t w = write(p->fd[1], (char *)block + done, sizeof(block) - done);
            if (w <= 0)
                return 0;
            done += (size_t)w;
        }
    }
    close(p->fd[1]);
    return 0;
}

static void *pipe_receiver(void *arg)
{
    struct pipe_pair *p = arg;
    uint32_t block[256];
    for (uint32_t n = 0;; n++) {
        size_t done = 0;
        while (done < sizeof(block)) {
            ssize_t r = read(p->fd[0], (char *)block + done, sizeof(block) - done);
            if (r <= 0)
                return 0;                   /* the sender stopped */
            done += (size_t)r;
        }
        for (int i = 0; i < 256; i++)
            if (block[i] != n * 256 + (uint32_t)i) {
                __atomic_fetch_add(&pipe_bad, 1, __ATOMIC_RELAXED);
                break;
            }
        __atomic_fetch_add(&pipe_blocks, 1, __ATOMIC_RELAXED);
    }
}

/* --- files ----------------------------------------------------------------- */

static long file_rounds, file_bad;

static void *filer(void *arg)
{
    long me = (long)arg;
    char name[64], data[512], back[512];
    snprintf(name, sizeof(name), "/tmp/smpstress.%d.%ld", (int)getpid(), me);
    for (long n = 0; !stop; n++) {
        for (int i = 0; i < 512; i++)
            data[i] = (char)(n + i + me);
        int fd = open(name, O_CREAT | O_TRUNC | O_RDWR, 0600);
        if (fd < 0) {
            if (!__atomic_fetch_add(&file_bad, 1, __ATOMIC_RELAXED))
                printf("smpstress: files: round %ld failed at open (errno %d)\n", n, errno);
            continue;
        }
        int step = write(fd, data, 512) != 512 ? 1 : lseek(fd, 0, SEEK_SET) != 0 ? 2 :
                   read(fd, back, 512) != 512 ? 3 : memcmp(data, back, 512) != 0 ? 4 : 0;
        if (step) {
            if (!__atomic_fetch_add(&file_bad, 1, __ATOMIC_RELAXED)) {
                int at = 0, same = 0;
                while (at < 512 && data[at] == back[at])
                    at++;
                for (int i = 0; i < 512; i++)
                    same += data[i] == back[i];
                printf("smpstress: files: round %ld (thread %ld) failed at %s (errno %d); first "
                       "difference at byte %d: wrote %d, read %d (%d of 512 bytes right)\n", n, me,
                       step == 1 ? "write" : step == 2 ? "seek" : step == 3 ? "read" : "compare",
                       errno, at, at < 512 ? data[at] : 0, at < 512 ? back[at] : 0, same);
            }
        }
        close(fd);
        __atomic_fetch_add(&file_rounds, 1, __ATOMIC_RELAXED);
    }
    unlink(name);
    return 0;
}

/* --- append ---------------------------------------------------------------- */

#define APPENDERS 4
static long appended, append_bad;
static char append_name[64];

static void *appender(void *arg)
{
    long me = (long)arg;
    int fd = open(append_name, O_WRONLY | O_APPEND);
    if (fd < 0) {
        __atomic_fetch_add(&append_bad, 1, __ATOMIC_RELAXED);
        return 0;
    }
    char line[17];
    for (long n = 0; !stop && n < 1000000; n++) {
        snprintf(line, sizeof(line), "%ld %013ld\n", me, n);     /* 15 bytes and the newline */
        if (write(fd, line, 16) != 16) {
            __atomic_fetch_add(&append_bad, 1, __ATOMIC_RELAXED);
            break;
        }
        __atomic_fetch_add(&appended, 1, __ATOMIC_RELAXED);
    }
    close(fd);
    return 0;
}

/* Every line there once: the file is appended lines times 16 bytes, each
 * line well formed. */
static long append_check(void)
{
    int fd = open(append_name, O_RDONLY);
    if (fd < 0)
        return 1;
    long lines = 0, bad = 0;
    char line[16];
    while (read(fd, line, 16) == 16) {
        lines++;
        if (line[15] != '\n' || line[1] != ' ' || line[0] < '0' || line[0] >= '0' + APPENDERS)
            bad++;
    }
    close(fd);
    unlink(append_name);
    if (lines != appended || bad)
        printf("smpstress: append: %ld lines appended, %ld in the file, %ld broken\n", appended,
               lines, bad);
    return (lines != appended) + bad;
}

/* --- faults ---------------------------------------------------------------- */

#define FAULTERS 8
#define FAULT_PAGES 256                     /* a faulter's own area: 1 MiB a round */
#define TOGETHER_PAGES 2048                 /* the area all of them touch: 8 MiB */
static uint64_t *together;
static int faulters_ready;
static long faulted, fault_bad;

static void *faulter(void *arg)
{
    long me = (long)arg;
    __atomic_fetch_add(&faulters_ready, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&faulters_ready, __ATOMIC_SEQ_CST) < FAULTERS)
        ;                                   /* (all start at once) */
    for (long p = 0; p < TOGETHER_PAGES; p++)
        together[p * 512 + me] = (uint64_t)(p * FAULTERS + me + 1);
    while (!stop) {
        uint64_t *a = mmap(0, FAULT_PAGES * 4096, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (a == MAP_FAILED) {
            printf("smpstress: faults: mmap failed (%d)\n", errno);
            exit(1);
        }
        long bad = 0;
        for (long p = 0; p < FAULT_PAGES; p++) {
            bad += a[p * 512 + 1] != 0;     /* (its first touch: zeroed) */
            a[p * 512] = (uint64_t)(me << 32 | p);
        }
        for (long p = 0; p < FAULT_PAGES; p++)
            bad += a[p * 512] != (uint64_t)(me << 32 | p) || a[p * 512 + 511] != 0;
        munmap(a, FAULT_PAGES * 4096);
        if (bad)
            __atomic_fetch_add(&fault_bad, bad, __ATOMIC_RELAXED);
        __atomic_fetch_add(&faulted, FAULT_PAGES, __ATOMIC_RELAXED);
    }
    return 0;
}

/* Every faulter's word in every page of the shared area, the rest zero. */
static long together_check(void)
{
    long bad = 0;
    for (long p = 0; p < TOGETHER_PAGES; p++)
        for (long w = 0; w < 512; w++)
            bad += together[p * 512 + w] != (w < FAULTERS ? (uint64_t)(p * FAULTERS + w + 1) : 0);
    if (bad)
        printf("smpstress: faults: %ld wrong words in the area all threads touched\n", bad);
    return bad;
}

/* --- wakeups --------------------------------------------------------------- */

#define WAKE_PAIRS 8
struct wake_pair {
    int there[2], back[2];                  /* pipes: the token out, and back */
};
static long wake_rounds, wake_bad;

static void *wake_starter(void *arg)
{
    struct wake_pair *w = arg;
    long n, got;
    for (n = 1; !stop; n++) {
        if (write(w->there[1], &n, sizeof(n)) != sizeof(n) ||
            read(w->back[0], &got, sizeof(got)) != sizeof(got) || got != n) {
            __atomic_fetch_add(&wake_bad, 1, __ATOMIC_RELAXED);
            break;
        }
        __atomic_fetch_add(&wake_rounds, 1, __ATOMIC_RELAXED);
    }
    n = 0;                                  /* (the end: the other one stops) */
    write(w->there[1], &n, sizeof(n));
    return 0;
}

static void *wake_answerer(void *arg)
{
    struct wake_pair *w = arg;
    for (long want = 1;; want++) {
        long n;
        if (read(w->there[0], &n, sizeof(n)) != sizeof(n) || n == 0)
            break;
        if (n != want)
            __atomic_fetch_add(&wake_bad, 1, __ATOMIC_RELAXED);
        if (write(w->back[1], &n, sizeof(n)) != sizeof(n))
            break;
    }
    return 0;
}

/* --- descriptors ----------------------------------------------------------- */

#define DESC_NUMBER 200
#define DESC_READERS 3
static long desc_switches, desc_reads, desc_bad;

static void *desc_switcher(void *arg)
{
    (void)arg;
    char letters[64];
    for (long n = 0; !stop; n++) {
        int q[2];
        if (pipe(q) != 0) {
            __atomic_fetch_add(&desc_bad, 1, __ATOMIC_RELAXED);
            break;
        }
        memset(letters, 'a' + (int)(n % 26), sizeof(letters));
        if (write(q[1], letters, sizeof(letters)) != sizeof(letters) || close(q[1]) != 0 ||
            dup2(q[0], DESC_NUMBER) != DESC_NUMBER || close(q[0]) != 0) {
            __atomic_fetch_add(&desc_bad, 1, __ATOMIC_RELAXED);
            break;
        }
        if (n % 8 == 7)
            close(DESC_NUMBER);             /* (the readers see EBADF meanwhile) */
        __atomic_fetch_add(&desc_switches, 1, __ATOMIC_RELAXED);
    }
    return 0;
}

static void *desc_reader(void *arg)
{
    (void)arg;
    char got[64];
    while (!stop) {
        ssize_t r = read(DESC_NUMBER, got, sizeof(got));
        if (r < 0 && errno == EBADF)
            continue;
        if (r < 0) {
            __atomic_fetch_add(&desc_bad, 1, __ATOMIC_RELAXED);
            continue;
        }
        for (ssize_t i = 0; i < r; i++)
            if (got[i] != got[0] || got[i] < 'a' || got[i] > 'z') {
                __atomic_fetch_add(&desc_bad, 1, __ATOMIC_RELAXED);
                break;
            }
        if (r > 0)
            __atomic_fetch_add(&desc_reads, 1, __ATOMIC_RELAXED);
    }
    return 0;
}

/* --- tcp ------------------------------------------------------------------- */

#define TCP_PAIRS 3
struct tcp_pair {
    int listener;
    uint16_t port;
    long blocks;
};
static long tcp_blocks, tcp_bad;

static void tcp_failed(const char *what, int pair)
{
    if (!__atomic_fetch_add(&tcp_bad, 1, __ATOMIC_RELAXED))
        printf("smpstress: tcp: connection %d: %s failed (errno %d)\n", pair, what, errno);
}

static void *tcp_sender(void *arg)
{
    struct tcp_pair *p = arg;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in to = { 0 };
    to.sin_family = AF_INET;
    to.sin_port = htons(p->port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || connect(fd, (struct sockaddr *)&to, sizeof(to)) != 0) {
        tcp_failed("connect", (int)p->port);
        if (fd >= 0)
            close(fd);
        return 0;
    }
    uint32_t block[256];
    for (uint32_t n = 0; !stop; n++) {
        for (int i = 0; i < 256; i++)
            block[i] = n * 256 + (uint32_t)i;
        size_t done = 0;
        while (done < sizeof(block)) {
            ssize_t w = write(fd, (char *)block + done, sizeof(block) - done);
            if (w <= 0) {
                tcp_failed("write", (int)p->port);
                close(fd);
                return 0;
            }
            done += (size_t)w;
        }
    }
    close(fd);                              /* (the receiver reads the end) */
    return 0;
}

static void *tcp_receiver(void *arg)
{
    struct tcp_pair *p = arg;
    int fd = accept(p->listener, 0, 0);
    if (fd < 0) {
        tcp_failed("accept", (int)p->port);
        return 0;
    }
    uint32_t block[256];
    for (uint32_t n = 0;; n++) {
        size_t done = 0;
        while (done < sizeof(block)) {
            ssize_t r = read(fd, (char *)block + done, sizeof(block) - done);
            if (r < 0)
                tcp_failed("read", (int)p->port);
            if (r <= 0) {
                close(fd);
                return 0;                   /* the sender stopped */
            }
            done += (size_t)r;
        }
        for (int i = 0; i < 256; i++)
            if (block[i] != n * 256 + (uint32_t)i) {
                if (!__atomic_fetch_add(&tcp_bad, 1, __ATOMIC_RELAXED))
                    printf("smpstress: tcp: port %u: block %u word %d is %u\n", p->port, n, i,
                           block[i]);
                break;
            }
        p->blocks++;
        __atomic_fetch_add(&tcp_blocks, 1, __ATOMIC_RELAXED);
    }
}

/* A listener on the loopback address, its port the kernel's choice. */
static int tcp_listen_any(struct tcp_pair *p)
{
    struct sockaddr_in at = { 0 };
    socklen_t length = sizeof(at);
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    p->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (p->listener < 0 || bind(p->listener, (struct sockaddr *)&at, sizeof(at)) != 0 ||
        listen(p->listener, 1) != 0 ||
        getsockname(p->listener, (struct sockaddr *)&at, &length) != 0)
        return -1;
    p->port = ntohs(at.sin_port);
    return 0;
}

/* --- spawn ----------------------------------------------------------------- */

static long spawns, spawn_bad;
static char *self_path;

static void *spawner(void *arg)
{
    (void)arg;
    char *argv[] = { self_path, "child", 0 };
    while (!stop) {
        pid_t pid;
        int e = posix_spawn(&pid, self_path, 0, 0, argv, environ);
        if (e != 0) {
            if (!__atomic_fetch_add(&spawn_bad, 1, __ATOMIC_RELAXED))
                printf("smpstress: spawn: posix_spawn failed: %s\n", strerror(e));
            continue;
        }
        int status = 0;
        pid_t w = waitpid(pid, &status, 0);
        if (w != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 7) {
            if (!__atomic_fetch_add(&spawn_bad, 1, __ATOMIC_RELAXED))
                printf("smpstress: spawn: pid %d: waitpid gave %d (errno %d), status 0x%x: %s %d\n",
                       (int)pid, (int)w, errno, status, WIFEXITED(status) ? "exit" :
                       WIFSIGNALED(status) ? "signal" : "?", WIFEXITED(status) ?
                       WEXITSTATUS(status) : WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        }
        __atomic_fetch_add(&spawns, 1, __ATOMIC_RELAXED);
    }
    return 0;
}

/* ---------------------------------------------------------------------------- */

static int part_count;
static char **part_names;

static int wanted(const char *part)
{
    if (!part_count)
        return 1;
    for (int i = 0; i < part_count; i++)
        if (strcmp(part_names[i], part) == 0)
            return 1;
    return 0;
}

static void verdict(const char *part, const char *counts, long bad)
{
    if (wanted(part))
        printf("smpstress: %s: %s: %s\n", part, counts, bad ? "FAILED" : "ok");
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "child") == 0)
        return 7;                           /* the spawned program */
    self_path = argv[0];
    if (argc > 1)
        seconds = atol(argv[1]) > 0 ? atol(argv[1]) : 5;
    part_count = argc > 2 ? argc - 2 : 0;
    part_names = argv + 2;

    tlb_page = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    shared_area = mmap(0, FORK_PAGES * 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    together = mmap(0, TOGETHER_PAGES * 4096, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tlb_page == MAP_FAILED || shared_area == MAP_FAILED || together == MAP_FAILED) {
        printf("smpstress: mmap failed\n");
        return 1;
    }
    static struct pipe_pair pipes[2];
    for (int i = 0; i < 2; i++)
        if (pipe(pipes[i].fd) != 0) {
            printf("smpstress: pipe failed\n");
            return 1;
        }

    pthread_t t[64];
    int n = 0;
    if (wanted("tlb")) {
        pthread_create(&t[n++], 0, tlb_writer, 0);
        pthread_create(&t[n++], 0, tlb_reader, 0);
        pthread_create(&t[n++], 0, tlb_reader, 0);
    }
    if (wanted("fork") || wanted("scribble"))       /* (the halves alone: forkonly, scribble) */
        for (long i = 0; i < 2; i++)
            pthread_create(&t[n++], 0, fork_scribbler, (void *)i);
    if (wanted("fork") || wanted("forkonly"))
        pthread_create(&t[n++], 0, forker, 0);
    if (wanted("heap"))
        for (long i = 0; i < 3; i++)
            pthread_create(&t[n++], 0, heaper, (void *)i);
    if (wanted("pipe"))
        for (int i = 0; i < 2; i++) {
            pthread_create(&t[n++], 0, pipe_sender, &pipes[i]);
            pthread_create(&t[n++], 0, pipe_receiver, &pipes[i]);
        }
    if (wanted("files"))
        for (long i = 0; i < 2; i++)
            pthread_create(&t[n++], 0, filer, (void *)i);
    if (wanted("spawn"))
        pthread_create(&t[n++], 0, spawner, 0);
    if (wanted("append")) {
        snprintf(append_name, sizeof(append_name), "/tmp/smpstress.append.%d", (int)getpid());
        int fd = open(append_name, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (fd >= 0)
            close(fd);
        for (long i = 0; i < APPENDERS; i++)
            pthread_create(&t[n++], 0, appender, (void *)i);
    }
    static struct tcp_pair pairs[TCP_PAIRS];
    if (wanted("tcp"))
        for (int i = 0; i < TCP_PAIRS; i++) {
            if (tcp_listen_any(&pairs[i]) != 0) {
                tcp_failed("listen", i);
                continue;
            }
            pthread_create(&t[n++], 0, tcp_receiver, &pairs[i]);
            pthread_create(&t[n++], 0, tcp_sender, &pairs[i]);
        }

    if (wanted("faults"))
        for (long i = 0; i < FAULTERS; i++)
            pthread_create(&t[n++], 0, faulter, (void *)i);

    static struct wake_pair wakes[WAKE_PAIRS];
    if (wanted("wakeups"))
        for (int i = 0; i < WAKE_PAIRS; i++) {
            if (pipe(wakes[i].there) != 0 || pipe(wakes[i].back) != 0) {
                printf("smpstress: pipe failed\n");
                return 1;
            }
            pthread_create(&t[n++], 0, wake_answerer, &wakes[i]);
            pthread_create(&t[n++], 0, wake_starter, &wakes[i]);
        }

    if (wanted("descriptors")) {
        pthread_create(&t[n++], 0, desc_switcher, 0);
        for (int i = 0; i < DESC_READERS; i++)
            pthread_create(&t[n++], 0, desc_reader, 0);
    }

    long end = now_ms() + seconds * 1000;
    while (now_ms() < end)
        usleep(100000);
    stop = 1;                               /* (senders close their ends: receivers end) */
    for (int i = 0; i < n; i++)
        pthread_join(t[i], 0);

    char c[160];
    snprintf(c, sizeof(c), "%ld new pages, %ld reads", tlb_rounds, tlb_reads);
    verdict("tlb", c, tlb_stale);
    if (tlb_stale)
        printf("smpstress: tlb: %ld stale reads (generation %lu read after %lu was announced)\n",
               tlb_stale, (unsigned long)tlb_worst_seen, (unsigned long)tlb_worst_announced);
    snprintf(c, sizeof(c), "%ld forks of %d pages, %ld wrong", forks, FORK_PAGES, fork_bad);
    verdict("fork", c, fork_bad);
    snprintf(c, sizeof(c), "%ld blocks, %ld wrong", heap_blocks, heap_bad);
    verdict("heap", c, heap_bad);
    snprintf(c, sizeof(c), "%ld blocks of 1 KiB, %ld wrong", pipe_blocks, pipe_bad);
    verdict("pipe", c, pipe_bad);
    snprintf(c, sizeof(c), "%ld rounds, %ld wrong", file_rounds, file_bad);
    verdict("files", c, file_bad);
    snprintf(c, sizeof(c), "%ld programs, %ld wrong", spawns, spawn_bad);
    verdict("spawn", c, spawn_bad);
    if (wanted("append"))
        append_bad += append_check();
    snprintf(c, sizeof(c), "%ld lines by %d threads, %ld wrong", appended, APPENDERS, append_bad);
    verdict("append", c, append_bad);
    int quiet = 0;                          /* (every connection must have carried some) */
    for (int i = 0; i < TCP_PAIRS; i++)
        quiet += pairs[i].blocks == 0;
    snprintf(c, sizeof(c), "%ld blocks of 1 KiB over %d connections at once, %ld wrong, %d carried "
             "none", tcp_blocks, TCP_PAIRS, tcp_bad, quiet);
    verdict("tcp", c, tcp_bad + quiet);
    if (wanted("faults"))
        fault_bad += together_check();
    snprintf(c, sizeof(c), "%ld pages faulted in by %d threads (%ld a second), %ld wrong", faulted,
             FAULTERS, faulted / seconds, fault_bad);
    verdict("faults", c, fault_bad);
    snprintf(c, sizeof(c), "%ld round trips by %d pairs of threads (%ld a second), %ld wrong",
             wake_rounds, WAKE_PAIRS, wake_rounds / seconds, wake_bad);
    verdict("wakeups", c, wake_bad);
    snprintf(c, sizeof(c), "%ld switches of a descriptor, %ld reads through it by %d threads, "
             "%ld wrong", desc_switches, desc_reads, DESC_READERS, desc_bad);
    verdict("descriptors", c, desc_bad);
    int all = !tlb_stale && !fork_bad && !heap_bad && !pipe_bad && !file_bad && !spawn_bad &&
              (!wanted("tcp") || (!tcp_bad && !quiet)) &&
              (!wanted("append") || (!append_bad && appended)) &&
              (!wanted("faults") || (!fault_bad && faulted)) &&
              (!wanted("wakeups") || (!wake_bad && wake_rounds)) &&
              (!wanted("descriptors") || (!desc_bad && desc_switches && desc_reads)) &&
              (tlb_rounds || !wanted("tlb")) && (forks || !(wanted("fork") || wanted("forkonly"))) &&
              (heap_blocks || !wanted("heap")) && (pipe_blocks || !wanted("pipe")) &&
              (file_rounds || !wanted("files")) && (spawns || !wanted("spawn"));
    printf("smpstress: %s\n", all ? "all parts ok" : "SOMETHING WENT WRONG");
    return all ? 0 : 1;
}

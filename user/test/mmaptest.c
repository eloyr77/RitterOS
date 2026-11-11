/*
 * Test: mmap and munmap (step 22a). `mmaptest FILE SCRATCH` checks anonymous
 * memory (and a malloc past the old 16 MiB limit), mapping FILE (a program
 * of several pages) against read(), a private writable copy, one copy of a
 * file's pages shared by four programs (free memory falls by about one copy,
 * and comes back when they end), a rewritten file being a new version while
 * an old mapping keeps the old bytes, MAP_FIXED inside a reservation and a
 * mapping split by munmap, the refusals, and faults in child programs. Since
 * 22e also: a 1 GiB mapping costs nothing until touched (demand paging), a
 * private writable mapping costs nothing until written and then a page per
 * page written (copy-on-write), and a mapping may reach past its file's end
 * (touching there is SIGBUS). Since 22f: mprotect (a page made read-only
 * faults on a write, keeps its bytes through PROT_NONE, splits a mapping;
 * the refusals). Since 22g, from the page cache's count of pages read
 * (REPORT_MEMORY): a file mapped again after its last mapping went is not
 * read again (the cache keeps it), a rewritten one is and the old version
 * is dropped; a program is loaded from the cache (hello: read once, not
 * again when run a second time); and a program's relocated data is
 * read-only (writing its GOT faults). SCRATCH is a path prefix for the files
 * it writes. The other modes are those children: `hold PATH` (maps PATH,
 * says so, waits for its input to end) and `fault-ro|fault-gone|fault-none|
 * fault-hole|fault-past|fault-protect|fault-relro FILE` (each must die:
 * SIGSEGV, fault-past SIGBUS).
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ritteros.h"

#define PAGE 4096
#define HOLDERS 4
#define BIG_PAGES 1024                  /* the shared file: 4 MiB */

static int failures;
static const char *self;

static void result(const char *name, int ok, const char *detail)
{
    if (ok) {
        printf("mmaptest: %s ok%s%s\n", name, detail[0] ? " " : "", detail);
    } else {
        printf("mmaptest: %s FAILED%s%s\n", name, detail[0] ? ": " : "", detail);
        failures++;
    }
}

static uint64_t free_pages(void)
{
    struct sys_info info;
    return ritteros_sysinfo(&info) == 0 ? info.free_pages : 0;
}

/* The page cache's counts (22g): files, pages kept unused, pages read. */
struct cache_counts {
    long files, kept, read;
};

static int cache_counts(struct cache_counts *c)
{
    char text[256];
    long n = (long)ritteros_report(REPORT_MEMORY, text, sizeof(text) - 1);
    if (n <= 0 || n >= (long)sizeof(text))
        return -1;
    text[n] = 0;
    /* "page cache: F file(s), P page(s) (K kept unused), R page(s) read" */
    long number[4];
    char *at = text;
    for (int i = 0; i < 4; i++) {
        while (*at && (*at < '0' || *at > '9'))
            at++;
        if (!*at)
            return -1;
        number[i] = 0;
        while (*at >= '0' && *at <= '9')
            number[i] = number[i] * 10 + (*at++ - '0');
    }
    c->files = number[0];
    c->kept = number[2];
    c->read = number[3];
    return 0;
}

/* The whole file in a new buffer (*size its length), or 0. */
static unsigned char *slurp(const char *path, long *size)
{
    struct stat st;
    int fd = open(path, O_RDONLY);
    if (fd < 0 || stat(path, &st) != 0) {
        if (fd >= 0)
            close(fd);
        return 0;
    }
    unsigned char *buf = malloc((size_t)st.st_size + 1);
    long done = 0;
    while (buf && done < (long)st.st_size) {
        ssize_t n = read(fd, buf + done, (size_t)(st.st_size - done));
        if (n <= 0)
            break;
        done += n;
    }
    close(fd);
    *size = done;
    return buf;
}

/* A file of `pages` pages, each byte `fill` (0: a pattern from the page). */
static int write_file(const char *path, int pages, int fill)
{
    static unsigned char page[PAGE];
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    for (int i = 0; i < pages; i++) {
        memset(page, fill ? fill : (i * 7 + 1) & 0xFF, sizeof(page));
        if (write(fd, page, sizeof(page)) != (ssize_t)sizeof(page)) {
            close(fd);
            return -1;
        }
    }
    return close(fd);
}

static void *map_file(const char *path, size_t length, int prot, int flags, off_t offset)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return MAP_FAILED;
    void *p = mmap(0, length, prot, flags, fd, offset);
    close(fd);                          /* the mapping does not need the descriptor */
    return p;
}

static void test_anonymous(void)
{
    size_t size = 40 << 20;             /* more than the old 16 MiB arenas */
    unsigned char *p = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int ok = p != MAP_FAILED;
    for (size_t i = 0; ok && i < size; i += PAGE)
        ok = p[i] == 0 && p[i + PAGE - 1] == 0;     /* zeroed */
    for (size_t i = 0; ok && i < size; i += PAGE)
        *(uint64_t *)(p + i) = i / PAGE;
    for (size_t i = 0; ok && i < size; i += PAGE)
        ok = *(uint64_t *)(p + i) == i / PAGE;
    ok = ok && munmap(p, size) == 0;
    result("anonymous", ok, "(40 MiB)");

    unsigned char *m = malloc(40 << 20);
    ok = m != 0;
    for (size_t i = 0; ok && i < (40 << 20); i += PAGE)
        m[i] = (unsigned char)i;
    free(m);
    result("malloc past 16 MiB", ok, "(40 MiB)");
}

static void test_file(const char *path)
{
    long size;
    unsigned char *want = slurp(path, &size);
    if (!want || size < 4 * PAGE) {
        result("file", 0, "the file is missing or too small");
        return;
    }
    unsigned char *p = map_file(path, (size_t)size, PROT_READ, MAP_PRIVATE, 0);
    int ok = p != MAP_FAILED && memcmp(p, want, (size_t)size) == 0;
    for (long i = size; ok && i % PAGE; i++)
        ok = p[i] == 0;                             /* the last page's tail is zeros */
    char detail[64];
    snprintf(detail, sizeof(detail), "(%ld bytes, as read() gives them)", size);
    result("file", ok, detail);
    unsigned char *q = map_file(path, PAGE, PROT_READ, MAP_SHARED, PAGE);
    result("file at an offset", q != MAP_FAILED && memcmp(q, want + PAGE, PAGE) == 0, "");

    unsigned char *w = map_file(path, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, 0);
    ok = w != MAP_FAILED;
    if (ok) {
        w[0] ^= 0xFF;
        w[PAGE + 5] ^= 0xFF;
        long again;
        unsigned char *now = slurp(path, &again);
        ok = now && memcmp(now, want, (size_t)size) == 0 &&     /* the file is unchanged */
             memcmp(p, want, 2 * PAGE) == 0 &&                  /* so is the shared mapping */
             w[0] == (want[0] ^ 0xFF) && w[1] == want[1];
        free(now);
    }
    result("private copy", ok, "(writes stay in this program)");
    munmap(p, (size_t)size);
    munmap(q, PAGE);
    munmap(w, 2 * PAGE);
    free(want);
}

static void test_sharing(const char *big)
{
    if (write_file(big, BIG_PAGES, 0) != 0) {
        result("sharing", 0, "cannot write the file");
        return;
    }
    int in[2], out[2];
    if (ritteros_pipe(in) < 0 || ritteros_pipe(out) < 0) {
        result("sharing", 0, "no pipes");
        return;
    }
    uint64_t before = free_pages();
    char *argv[] = { "mmaptest", "hold", (char *)big, 0 };
    int fds[3] = { in[0], out[1], 2 };
    int pids[HOLDERS], started = 0;
    for (int i = 0; i < HOLDERS; i++)
        if ((pids[i] = ritteros_spawn(self, argv, 0, fds)) > 0)
            started++;
    close(in[0]);
    close(out[1]);
    char buf[256];
    int holding = 0;
    long got = 0;
    while (holding < started) {                     /* each says "holding" once */
        ssize_t n = read(out[0], buf + got, sizeof(buf) - 1 - (size_t)got);
        if (n <= 0)
            break;
        got += n;
        buf[got] = 0;
        holding = 0;
        for (char *s = buf; (s = strstr(s, "holding")); s++)
            holding++;
    }
    uint64_t during = free_pages();
    close(in[1]);                                   /* their input ends: they end */
    for (int i = 0; i < HOLDERS; i++) {
        int status;
        if (pids[i] > 0)
            waitpid(pids[i], &status, 0);
    }
    close(out[0]);
    uint64_t after = free_pages();
    long used = (long)before - (long)during, left = (long)before - (long)after;
    char detail[128];
    snprintf(detail, sizeof(detail), "(%d programs mapped %d pages: %ld pages used, %ld after they ended)",
             holding, BIG_PAGES, used, left);
    /* One copy (BIG_PAGES) and each program's own few dozen pages; four
     * copies would be over 4000. After them, nothing more than noise. */
    result("sharing", holding == HOLDERS && used >= BIG_PAGES && used < 2 * BIG_PAGES &&
                      left < 64 && left > -64, detail);
    unlink(big);
}

/* Map path whole, read every page, unmap: 0 or -1. */
static int touch_file(const char *path, int pages)
{
    volatile unsigned char *p = map_file(path, (size_t)pages * PAGE, PROT_READ, MAP_SHARED, 0);
    if (p == MAP_FAILED)
        return -1;
    unsigned sum = 0;
    for (int i = 0; i < pages; i++)
        sum += p[i * PAGE];
    (void)sum;
    return munmap((void *)p, (size_t)pages * PAGE);
}

/* 22g: the cache keeps a file no mapping uses; a new version replaces it.
 * (No free_pages() in between: counting free memory empties the cache.) */
static void test_kept(const char *path)
{
    struct cache_counts a, b, c, d;
    int ok = write_file(path, 64, 'K') == 0 && cache_counts(&a) == 0 &&
             touch_file(path, 64) == 0 && cache_counts(&b) == 0 &&
             touch_file(path, 64) == 0 && cache_counts(&c) == 0 &&
             write_file(path, 64, 'L') == 0 && touch_file(path, 64) == 0 && cache_counts(&d) == 0;
    char detail[160];
    snprintf(detail, sizeof(detail),
             "(64 pages: %ld read, then %ld; rewritten: %ld; kept unused %ld, %ld after; files %ld, %ld)",
             b.read - a.read, c.read - b.read, d.read - c.read, c.kept, d.kept, c.files, d.files);
    result("kept", ok && b.read - a.read == 64 && c.read == b.read && d.read - c.read == 64 &&
                   c.kept >= 64 && d.kept == c.kept && d.files == c.files, detail);
    unlink(path);
}

/* Run program (its output into a pipe, read to the end) and wait for it. */
static int run_quietly(const char *program)
{
    int out[2], status = -1;
    if (ritteros_pipe(out) < 0)
        return -1;
    char *argv[] = { (char *)program, 0 };
    int fds[3] = { 0, out[1], 2 };
    int pid = ritteros_spawn(program, argv, 0, fds);
    close(out[1]);
    char buf[256];
    while (read(out[0], buf, sizeof(buf)) > 0)
        ;
    close(out[0]);
    if (pid > 0)
        waitpid(pid, &status, 0);
    return pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* 22g: a program's code and data come from the page cache: read at its
 * first start, not again at the second (kept). hello is in sh's folder. */
static void test_program(const char *sh)
{
    char hello[256];
    snprintf(hello, sizeof(hello), "%s", sh);
    char *slash = strrchr(hello, '/');
    if (!slash || (size_t)(slash - hello) + 7 > sizeof(hello)) {
        result("programs from the cache", 0, "no folder");
        return;
    }
    strcpy(slash + 1, "hello");
    free_pages();                                   /* (empties the cache of unused files) */
    struct cache_counts a, b, c;
    int ok = cache_counts(&a) == 0 && run_quietly(hello) == 0 && cache_counts(&b) == 0 &&
             run_quietly(hello) == 0 && cache_counts(&c) == 0;
    char detail[128];
    snprintf(detail, sizeof(detail), "(hello: %ld pages read at its first start, %ld at its second)",
             b.read - a.read, c.read - b.read);
    result("programs from the cache", ok && b.read - a.read > 0 && c.read == b.read, detail);
}

/* 22j: a writable shared mapping writes the file: msync, munmap. */
static void test_shared_write(const char *path)
{
    int ok = write_file(path, 2, 'A') == 0;
    int fd = open(path, O_RDWR);
    unsigned char *p = fd < 0 ? MAP_FAILED : mmap(0, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    ok = ok && p != MAP_FAILED;
    long size = 0;
    unsigned char *f = 0;
    if (ok) {
        p[0] = 'X';
        p[PAGE + 5] = 'Y';
        ok = msync(p, 2 * PAGE, MS_SYNC) == 0 && (f = slurp(path, &size)) && size == 2 * PAGE &&
             f[0] == 'X' && f[PAGE + 5] == 'Y' && f[1] == 'A';
        free(f);
    }
    result("shared write", ok, "(written through the mapping, msync: read() sees it)");
    ok = p != MAP_FAILED;
    if (ok) {
        p[1] = 'Z';
        ok = munmap(p, 2 * PAGE) == 0 && (f = slurp(path, &size)) && f[1] == 'Z' && f[0] == 'X';
        free(f);
    }
    result("munmap writes back", ok, "");
}

/* 22j: two programs mapping the same file shared see each other's writes at
 * once (one copy of the pages): a child pokes, this program looks. */
static void test_shared_between(const char *path)
{
    int ok = write_file(path, 1, 'B') == 0;
    int fd = open(path, O_RDWR);
    volatile unsigned char *p = fd < 0 ? MAP_FAILED : mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    int in[2], out[2];
    ok = ok && p != MAP_FAILED && ritteros_pipe(in) == 0 && ritteros_pipe(out) == 0;
    int pid = -1;
    if (ok) {
        char *argv[] = { "mmaptest", "poke", (char *)path, 0 };
        int fds[3] = { in[0], out[1], 2 };
        pid = ritteros_spawn(self, argv, 0, fds);
        close(in[0]);
        close(out[1]);
        char buf[64];
        ssize_t n = read(out[0], buf, sizeof(buf));     /* "poked": its write is done */
        ok = pid > 0 && n > 0 && p[100] == 'P' && p[0] == 'B';
        close(in[1]);                                   /* its input ends: it ends */
        int status;
        waitpid(pid, &status, 0);
        close(out[0]);
    }
    result("shared between programs", ok, "(a child's write seen here at once)");
    if (p != MAP_FAILED)
        munmap((void *)p, PAGE);
    unlink(path);
}

/* 22e: anonymous pages are made when first touched. */
static void test_lazy(void)
{
    size_t size = (size_t)1 << 30;
    uint64_t before = free_pages();
    unsigned char *p = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint64_t mapped = free_pages();
    for (size_t i = 0; p != MAP_FAILED && i < 100; i++)
        p[size / 2 + i * PAGE] = 1;                 /* 100 pages (and a page table or two) */
    uint64_t touched = free_pages();
    int ok = p != MAP_FAILED && munmap(p, size) == 0;
    uint64_t after = free_pages();
    long cost = (long)before - (long)mapped, used = (long)mapped - (long)touched;
    long left = (long)before - (long)after;
    char detail[128];
    snprintf(detail, sizeof(detail), "(1 GiB mapped: %ld pages; 100 touched: %ld more; after: %ld)",
             cost, used, left);
    /* (The page tables stay until the program ends: a few pages in left.) */
    result("lazy", ok && cost < 16 && used >= 100 && used < 110 && left < 16 && left > -16, detail);
}

/* 22e: a private writable mapping shares the cache's pages until written. */
static void test_cow(const char *path)
{
    unsigned char *shared = 0, *mine = 0;
    int ok = write_file(path, 256, 0) == 0 &&
             (shared = map_file(path, 256 * PAGE, PROT_READ, MAP_SHARED, 0)) != MAP_FAILED;
    uint64_t before = free_pages();
    ok = ok && (mine = map_file(path, 256 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, 0)) != MAP_FAILED;
    unsigned sum = 0;
    for (int i = 0; ok && i < 256; i++)
        sum += mine[i * PAGE];                      /* read every page: still the cache's */
    uint64_t read = free_pages();
    for (int i = 0; ok && i < 10; i++)
        mine[i * PAGE] = 0xEE;                      /* write 10: a copy of each */
    uint64_t written = free_pages();
    ok = ok && shared[0] == 1 && shared[9 * PAGE] == ((9 * 7 + 1) & 0xFF) && mine[0] == 0xEE &&
         mine[10 * PAGE] == shared[10 * PAGE] && sum > 0;
    long reading = (long)before - (long)read, writing = (long)read - (long)written;
    char detail[128];
    snprintf(detail, sizeof(detail), "(256 pages read: %ld pages; 10 written: %ld more)", reading, writing);
    result("copy-on-write", ok && reading < 16 && writing >= 10 && writing < 26, detail);
    munmap(shared, 256 * PAGE);
    munmap(mine, 256 * PAGE);
    unlink(path);
}

/* 22f: mprotect. */
static void test_protect(const char *path)
{
    unsigned char *p = mmap(0, 3 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int ok = p != MAP_FAILED;
    for (int i = 0; ok && i < 3; i++)
        p[i * PAGE] = (unsigned char)(10 + i);
    ok = ok && mprotect(p + PAGE, PAGE, PROT_READ) == 0;        /* the middle page: a split */
    ok = ok && (p[0] = 20, p[2 * PAGE] = 22, p[PAGE] == 11);     /* the ends still writable */
    ok = ok && mprotect(p, 3 * PAGE, PROT_NONE) == 0 && mprotect(p, 3 * PAGE, PROT_READ | PROT_WRITE) == 0;
    ok = ok && p[0] == 20 && p[PAGE] == 11 && p[2 * PAGE] == 22;  /* kept through PROT_NONE */
    ok = ok && (p[PAGE] = 31, p[PAGE] == 31);
    result("mprotect", ok, "(a split, through PROT_NONE and back)");
    munmap(p, 3 * PAGE);

    unsigned char *c = map_file(path, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, 0);
    unsigned char first = c != MAP_FAILED ? c[0] : 0;
    ok = c != MAP_FAILED && mprotect(c, 2 * PAGE, PROT_READ) == 0 &&
         mprotect(c, 2 * PAGE, PROT_READ | PROT_WRITE) == 0 && (c[0] ^= 0xFF, c[0] == (first ^ 0xFF));
    long size;
    unsigned char *file = slurp(path, &size);
    ok = ok && file && file[0] == first;                          /* still a private copy */
    free(file);
    result("mprotect keeps copy-on-write", ok, "");
    munmap(c, 2 * PAGE);

    unsigned char *a = mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char *s = map_file(path, PAGE, PROT_READ, MAP_SHARED, 0);
    errno = 0;
    result("mprotect refused: writable and executable",
           a != MAP_FAILED && mprotect(a, PAGE, PROT_WRITE | PROT_EXEC) == -1 && errno == ENOTSUP, "");
    errno = 0;
    result("mprotect refused: writing a shared file",
           s != MAP_FAILED && mprotect(s, PAGE, PROT_READ | PROT_WRITE) == -1 && errno == EACCES, "");
    errno = 0;
    result("mprotect refused: a hole in the range",
           a != MAP_FAILED && mprotect(a, 4 * PAGE, PROT_READ) == -1 && errno == ENOMEM, "");
    munmap(a, PAGE);
    munmap(s, PAGE);
}

static void test_versions(const char *path)
{
    unsigned char *a = 0, *b = 0;
    int ok = write_file(path, 2, 'A') == 0 &&
             (a = map_file(path, 2 * PAGE, PROT_READ, MAP_SHARED, 0)) != MAP_FAILED &&
             write_file(path, 2, 'B') == 0 &&       /* rewritten: a new version */
             (b = map_file(path, 2 * PAGE, PROT_READ, MAP_SHARED, 0)) != MAP_FAILED &&
             a[0] == 'A' && a[2 * PAGE - 1] == 'A' && b[0] == 'B' && b[2 * PAGE - 1] == 'B';
    result("versions", ok, "(the old mapping keeps the old bytes)");
    munmap(a, 2 * PAGE);
    munmap(b, 2 * PAGE);
    unlink(path);
}

/* A reservation of 16 pages, FILE's first 4 pages at page 4 of it. */
static unsigned char *reserve_and_map(const char *path)
{
    unsigned char *r = mmap(0, 16 * PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r == MAP_FAILED)
        return 0;
    int fd = open(path, O_RDONLY);
    void *f = fd < 0 ? MAP_FAILED : mmap(r + 4 * PAGE, 4 * PAGE, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
    if (fd >= 0)
        close(fd);
    return f == (void *)(r + 4 * PAGE) ? r : 0;
}

static void test_fixed(const char *path)
{
    long size;
    unsigned char *want = slurp(path, &size);
    unsigned char *r = want ? reserve_and_map(path) : 0;
    int ok = r && memcmp(r + 4 * PAGE, want, 4 * PAGE) == 0;
    result("fixed in a reservation", ok, "");
    /* munmap the middle two pages: the mapping splits, both ends still read. */
    ok = ok && munmap(r + 5 * PAGE, 2 * PAGE) == 0 &&
         memcmp(r + 4 * PAGE, want, PAGE) == 0 && memcmp(r + 7 * PAGE, want + 3 * PAGE, PAGE) == 0;
    /* Anonymous memory over the remaining file page 7 (MAP_FIXED replaces it). */
    unsigned char *z = ok ? mmap(r + 7 * PAGE, PAGE, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) : MAP_FAILED;
    ok = ok && z == r + 7 * PAGE && z[0] == 0 && z[PAGE - 1] == 0 && munmap(r, 16 * PAGE) == 0;
    result("split and replaced", ok, "");
    free(want);
}

static void refused(const char *name, void *p, int want)
{
    char detail[64];
    snprintf(detail, sizeof(detail), "(%s)", p == MAP_FAILED ? strerror(errno) : "it was mapped");
    result(name, p == MAP_FAILED && errno == want, detail);
}

static void test_refusals(const char *path)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    stat(path, &st);
    refused("refused: writable and executable",
            mmap(0, PAGE, PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), ENOTSUP);
    refused("refused: writing a shared file open for reading",
            mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0), EACCES);
    refused("refused: no such descriptor", mmap(0, PAGE, PROT_READ, MAP_PRIVATE, 99, 0), EBADF);
    refused("refused: an offset off a page", mmap(0, PAGE, PROT_READ, MAP_PRIVATE, fd, 100), EINVAL);
    refused("refused: no length", mmap(0, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), EINVAL);
    void *past = mmap(0, (size_t)st.st_size + 2 * PAGE, PROT_READ, MAP_PRIVATE, fd, 0);
    result("past the file's end is mapped", past != MAP_FAILED, "(touching there: fault-past)");
    if (past != MAP_FAILED)
        munmap(past, (size_t)st.st_size + 2 * PAGE);
    refused("refused: fixed outside the window",
            mmap((void *)0x400000, PAGE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0), EINVAL);
    int dir = open("/", O_RDONLY);
    refused("refused: a directory", mmap(0, PAGE, PROT_READ, MAP_PRIVATE, dir, 0), EACCES);
    close(dir);
    close(fd);
    result("refused: munmap off a page", munmap((void *)0x300000000001ULL, PAGE) == -1 && errno == EINVAL, "");
}

static void test_faults(const char *path)
{
    static const char *const modes[] = { "fault-ro", "fault-gone", "fault-none", "fault-hole",
                                         "fault-past", "fault-protect", "fault-relro" };
    for (int i = 0; i < 7; i++) {
        char *argv[] = { "mmaptest", (char *)modes[i], (char *)path, 0 };
        int fds[3] = { 0, 1, 2 };
        int pid = ritteros_spawn(self, argv, 0, fds), status = 0;
        if (pid > 0)
            waitpid(pid, &status, 0);
        char name[48];
        snprintf(name, sizeof(name), "%s faults", modes[i]);
        int want = i == 4 ? SIGBUS : SIGSEGV;
        result(name, pid > 0 && WIFSIGNALED(status) && WTERMSIG(status) == want, "");
    }
}

/* The children. */
static int child(const char *mode, const char *path)
{
    volatile unsigned char *p;
    if (strcmp(mode, "hold") == 0) {
        struct stat st;
        if (stat(path, &st) != 0)
            return 1;
        p = map_file(path, (size_t)st.st_size, PROT_READ, MAP_SHARED, 0);
        if (p == MAP_FAILED)
            return 1;
        unsigned sum = 0;
        for (long i = 0; i < (long)st.st_size; i += PAGE)
            sum += p[i];
        dprintf(STDOUT_FILENO, "holding %u\n", sum);
        char c;
        while (read(STDIN_FILENO, &c, 1) > 0)       /* until the parent closes our input */
            ;
        return 0;
    }
    if (strcmp(mode, "poke") == 0) {               /* 22j: write a shared mapping */
        int fd = open(path, O_RDWR);
        p = fd < 0 ? MAP_FAILED : mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED)
            return 1;
        p[100] = 'P';
        dprintf(STDOUT_FILENO, "poked\n");
        char c;
        while (read(STDIN_FILENO, &c, 1) > 0)
            ;
        return 0;
    }
    if (strcmp(mode, "fault-ro") == 0) {
        p = map_file(path, PAGE, PROT_READ, MAP_PRIVATE, 0);
        if (p != MAP_FAILED)
            p[0] = 1;                               /* writing a read-only mapping */
    } else if (strcmp(mode, "fault-gone") == 0) {
        p = mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED && munmap((void *)p, PAGE) == 0)
            (void)p[0];                             /* reading what was unmapped */
    } else if (strcmp(mode, "fault-none") == 0) {
        p = mmap(0, PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED)
            (void)p[0];                             /* reading a reservation */
    } else if (strcmp(mode, "fault-protect") == 0) {
        p = mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED && (p[0] = 1) && mprotect((void *)p, PAGE, PROT_READ) == 0)
            p[0] = 2;                               /* writing a page made read-only */
    } else if (strcmp(mode, "fault-relro") == 0) {
        extern char _GLOBAL_OFFSET_TABLE_[];        /* in PT_GNU_RELRO: read-only (22g) */
        ((volatile char *)_GLOBAL_OFFSET_TABLE_)[0] = 0;
    } else if (strcmp(mode, "fault-past") == 0) {
        struct stat st;
        size_t pages = stat(path, &st) == 0 ? ((size_t)st.st_size + PAGE - 1) / PAGE : 0;
        p = map_file(path, (pages + 2) * PAGE, PROT_READ, MAP_PRIVATE, 0);
        if (pages && p != MAP_FAILED)
            (void)p[(pages + 1) * PAGE];            /* a page past the file's end */
    } else if (strcmp(mode, "fault-hole") == 0) {
        p = reserve_and_map(path);
        if (p && munmap((void *)(p + 5 * PAGE), 2 * PAGE) == 0)
            (void)p[6 * PAGE];                      /* the hole a split left */
    }
    return 0;                                       /* no fault: the parent sees exit 0 */
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "hold") != 0 && strcmp(argv[1], "poke") != 0 &&
        strncmp(argv[1], "fault-", 6) != 0) {
        self = argv[0];
        char big[256], ver[256], cow[256], keep[256], shw[256];
        snprintf(big, sizeof(big), "%s.big", argv[2]);
        snprintf(ver, sizeof(ver), "%s.ver", argv[2]);
        snprintf(cow, sizeof(cow), "%s.cow", argv[2]);
        snprintf(keep, sizeof(keep), "%s.keep", argv[2]);
        snprintf(shw, sizeof(shw), "%s.shared", argv[2]);
        test_anonymous();
        test_lazy();
        test_cow(cow);
        test_file(argv[1]);
        test_sharing(big);
        test_versions(ver);
        test_fixed(argv[1]);
        test_protect(argv[1]);
        test_refusals(argv[1]);
        test_faults(argv[1]);
        test_kept(keep);
        test_program(argv[1]);
        test_shared_write(shw);
        test_shared_between(shw);
        if (failures)
            printf("mmaptest: %d failed\n", failures);
        else
            printf("mmaptest: all ok\n");
        return failures != 0;
    }
    if (argc == 3)
        return child(argv[1], argv[2]);
    dprintf(STDERR_FILENO, "usage: mmaptest FILE SCRATCH\n");
    return 2;
}

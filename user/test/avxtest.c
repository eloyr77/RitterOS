/*
 * Test (32a): AVX's YMM registers kept for programs. The kernel saves a
 * program thread's vector registers with XSAVE where the processor has AVX
 * (FXSAVE keeps only their lower halves, the XMM registers): each check
 * fills all 16 YMM registers, lets something happen that must keep them,
 * and reads them back.
 *
 * avxtest              what the processor and the system give (CPUID,
 *                      XGETBV); with AVX: the registers kept across thread
 *                      switches, signal handlers and fork, clean after exec,
 *                      an AVX2 addition, and a signal context forged with
 *                      bits XRSTOR refuses (the kernel cleans them: the
 *                      program goes on)
 * avxtest speed        (32b) what AVX2 gives: 32-bit integer adds with
 *                      SSE2 and with AVX2, in the cache and over 64 MiB,
 *                      and what saving the registers costs (FXSAVE and
 *                      XSAVE with their restores, as at a thread switch)
 * avxtest exec-child   (the exec check's new program)
 *
 * Our own as has no AVX (VEX) instructions, and RitterOS builds this file
 * with it: the few used are bytes, vmovdqu with a 32-bit displacement off
 * RDI (compared with clang's encoding) and one vpaddd; the speed part's
 * loops are clang's bytes for the instructions in their comments.
 */
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__x86_64__)

/* All 16 registers from hold->in, to hold->out (offsets 0 and 512). */
#define LOAD_ALL \
    ".byte 0xc5,0xfe,0x6f,0x87; .long 0\n\t" \
    ".byte 0xc5,0xfe,0x6f,0x8f; .long 32\n\t" \
    ".byte 0xc5,0xfe,0x6f,0x97; .long 64\n\t" \
    ".byte 0xc5,0xfe,0x6f,0x9f; .long 96\n\t" \
    ".byte 0xc5,0xfe,0x6f,0xa7; .long 128\n\t" \
    ".byte 0xc5,0xfe,0x6f,0xaf; .long 160\n\t" \
    ".byte 0xc5,0xfe,0x6f,0xb7; .long 192\n\t" \
    ".byte 0xc5,0xfe,0x6f,0xbf; .long 224\n\t" \
    ".byte 0xc5,0x7e,0x6f,0x87; .long 256\n\t" \
    ".byte 0xc5,0x7e,0x6f,0x8f; .long 288\n\t" \
    ".byte 0xc5,0x7e,0x6f,0x97; .long 320\n\t" \
    ".byte 0xc5,0x7e,0x6f,0x9f; .long 352\n\t" \
    ".byte 0xc5,0x7e,0x6f,0xa7; .long 384\n\t" \
    ".byte 0xc5,0x7e,0x6f,0xaf; .long 416\n\t" \
    ".byte 0xc5,0x7e,0x6f,0xb7; .long 448\n\t" \
    ".byte 0xc5,0x7e,0x6f,0xbf; .long 480\n\t"
#define STORE_ALL \
    ".byte 0xc5,0xfe,0x7f,0x87; .long 512\n\t" \
    ".byte 0xc5,0xfe,0x7f,0x8f; .long 544\n\t" \
    ".byte 0xc5,0xfe,0x7f,0x97; .long 576\n\t" \
    ".byte 0xc5,0xfe,0x7f,0x9f; .long 608\n\t" \
    ".byte 0xc5,0xfe,0x7f,0xa7; .long 640\n\t" \
    ".byte 0xc5,0xfe,0x7f,0xaf; .long 672\n\t" \
    ".byte 0xc5,0xfe,0x7f,0xb7; .long 704\n\t" \
    ".byte 0xc5,0xfe,0x7f,0xbf; .long 736\n\t" \
    ".byte 0xc5,0x7e,0x7f,0x87; .long 768\n\t" \
    ".byte 0xc5,0x7e,0x7f,0x8f; .long 800\n\t" \
    ".byte 0xc5,0x7e,0x7f,0x97; .long 832\n\t" \
    ".byte 0xc5,0x7e,0x7f,0x9f; .long 864\n\t" \
    ".byte 0xc5,0x7e,0x7f,0xa7; .long 896\n\t" \
    ".byte 0xc5,0x7e,0x7f,0xaf; .long 928\n\t" \
    ".byte 0xc5,0x7e,0x7f,0xb7; .long 960\n\t" \
    ".byte 0xc5,0x7e,0x7f,0xbf; .long 992\n\t"
#define YMM_CLOBBERS "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", \
    "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"

struct hold {
    uint8_t in[512], out[512];
    uint64_t until;                     /* at 1024: the TSC to spin until */
} __attribute__((aligned(32)));

#define ROUND_MS    50
#define ROUNDS      6
#define THREADS     4

static int failures;
static uint64_t ticks_per_ms;

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    __asm__ volatile ("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

static uint64_t tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

/* Load the registers, spin until h->until (in user mode: the kernel takes
 * the core away meanwhile and signals arrive), store them. */
static void hold_registers(struct hold *h)
{
    __asm__ volatile (LOAD_ALL
                      "1: rdtsc\n\t"
                      "shl $32, %%rdx\n\t"
                      "or %%rdx, %%rax\n\t"
                      "cmp 1024(%%rdi), %%rax\n\t"
                      "jb 1b\n\t"
                      STORE_ALL
                      : : "D"(h) : "rax", "rdx", "memory", YMM_CLOBBERS);
}

static void load_registers(struct hold *h)
{
    __asm__ volatile (LOAD_ALL : : "D"(h) : "memory", YMM_CLOBBERS);
}

static void store_registers(struct hold *h)
{
    __asm__ volatile (STORE_ALL : : "D"(h) : "memory");
}

static void fill(struct hold *h, int seed)
{
    for (int i = 0; i < 512; i++)
        h->in[i] = (uint8_t)(seed * 31 + i * 13 + 1);
    memset(h->out, 0, sizeof(h->out));
}

/* Registers whose bytes from..to-1 (of 32) differ between in and out. */
static int differing(const struct hold *h, int from, int to)
{
    int n = 0;
    for (int r = 0; r < 16; r++)
        if (memcmp(h->in + r * 32 + from, h->out + r * 32 + from, (size_t)(to - from)) != 0)
            n++;
    return n;
}

static void *switcher(void *arg)
{
    int seed = (int)(intptr_t)arg;
    struct hold h;
    fill(&h, seed);
    for (int round = 0; round < ROUNDS; round++) {
        h.until = tsc() + ROUND_MS * ticks_per_ms;
        hold_registers(&h);
        int bad = differing(&h, 0, 32);
        if (bad) {
            printf("avxtest: FAIL: thread %d round %d: %d of 16 YMM registers changed\n", seed, round, bad);
            return (void *)1;
        }
    }
    return 0;
}

static void switches(void)
{
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++)
        pthread_create(&t[i], 0, switcher, (void *)(intptr_t)(i + 1));
    int bad = 0;
    for (int i = 0; i < THREADS; i++) {
        void *r;
        pthread_join(t[i], &r);
        bad += r != 0;
    }
    if (bad)
        failures++;
    else
        printf("avxtest: switches: %d threads x %d rounds of %d ms, YMM registers kept\n", THREADS,
               ROUNDS, ROUND_MS);
}

static volatile int handled, holding;
static struct hold garbage;

static void handler(int signal)
{
    (void)signal;
    load_registers(&garbage);           /* every register changed in the handler */
    handled++;
}

/* Sends to the process, blocking the signal itself: only the holding
 * thread takes it. */
static void *sender(void *arg)
{
    (void)arg;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, 0);
    while (holding) {
        kill(getpid(), SIGUSR1);
        usleep(10000);
    }
    return 0;
}

static void signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigaction(SIGUSR1, &sa, 0);
    fill(&garbage, 99);
    struct hold h;
    fill(&h, 7);
    pthread_t t;
    holding = 1;
    pthread_create(&t, 0, sender, 0);
    h.until = tsc() + 300 * ticks_per_ms;
    hold_registers(&h);
    holding = 0;
    pthread_join(t, 0);
    int bad = differing(&h, 0, 32);
    if (bad || !handled) {
        printf("avxtest: FAIL: signals: %d handled, %d of 16 YMM registers changed\n", handled, bad);
        failures++;
    } else
        printf("avxtest: signals: handled during a hold, YMM registers kept\n");
}

/* The upper halves (bytes 16-31) survive the C code around fork and exec:
 * it is compiled without AVX, and SSE instructions keep them. */
static void forked(void)
{
    struct hold h;
    fill(&h, 11);
    load_registers(&h);
    pid_t pid = fork();
    store_registers(&h);
    int bad = differing(&h, 16, 32);
    if (pid == 0)
        _exit(bad ? 1 : 0);
    int status = 0;
    waitpid(pid, &status, 0);
    if (bad || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("avxtest: FAIL: fork: the parent's upper halves %s, the child's %s\n",
               bad ? "changed" : "kept", WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "kept" : "changed");
        failures++;
    } else
        printf("avxtest: fork: parent and child keep the YMM upper halves\n");
}

static int exec_child(void)
{
    struct hold h;
    memset(h.in, 0, sizeof(h.in));
    store_registers(&h);
    int bad = differing(&h, 16, 32);
    return bad ? 1 : 0;
}

static void execed(const char *self)
{
    pid_t pid = fork();
    if (pid == 0) {
        struct hold h;
        fill(&h, 13);
        load_registers(&h);
        char *argv[] = { (char *)self, "exec-child", 0 };
        execv(self, argv);
        _exit(2);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("avxtest: FAIL: exec: the new program's YMM upper halves were not zero (status %d)\n", status);
        failures++;
    } else
        printf("avxtest: exec: a new program starts with the YMM upper halves zero\n");
}

static void avx2_add(void)
{
    struct hold h;
    uint32_t a[8], b[8], sum[8];
    for (int i = 0; i < 8; i++)
        a[i] = 0x10000001u * (uint32_t)(i + 1), b[i] = 0xF0000000u + (uint32_t)i;
    memset(&h, 0, sizeof(h));
    memcpy(h.in, a, 32);
    memcpy(h.in + 32, b, 32);
    __asm__ volatile (LOAD_ALL
                      ".byte 0xc5,0xfd,0xfe,0xc1\n\t"       /* vpaddd %ymm1, %ymm0, %ymm0 */
                      STORE_ALL
                      : : "D"(&h) : "memory", YMM_CLOBBERS);
    memcpy(sum, h.out, 32);
    int wrong = 0;
    for (int i = 0; i < 8; i++)
        wrong += sum[i] != a[i] + b[i];
    if (wrong) {
        printf("avxtest: FAIL: AVX2 addition: %d of 8 lanes wrong\n", wrong);
        failures++;
    } else
        printf("avxtest: AVX2 addition: 8 lanes right\n");
}

/* (32b) Adding 32-bit integers from p up to end (a multiple of 64 or 128
 * bytes) in 4 independent sums, stored to out (64 or 128 bytes):
 *   SSE2: pxor xmm0-3; 1: paddd 0/16/32/48(%rsi), xmm0-3; add $64, %rsi;
 *         cmp %rdi, %rsi; jb 1b; movdqu xmm0-3 to 0/16/32/48(%rdx)
 *   AVX2: the same with vpxor, vpaddd ymm 0/32/64/96, sub $-128, vmovdqu,
 *         then vzeroupper */
static void add_sse2(const uint32_t *p, const uint32_t *end, uint32_t *out)
{
    __asm__ volatile (".byte 0x66,0x0f,0xef,0xc0,0x66,0x0f,0xef,0xc9,0x66,0x0f,0xef,0xd2,0x66,0x0f,0xef,0xdb,0x66,0x0f,0xfe,0x06,0x66,0x0f,0xfe,0x4e,0x10,0x66,0x0f,0xfe,0x56,0x20,0x66,0x0f,0xfe,0x5e,0x30,0x48,0x83,0xc6,0x40,0x48,0x39,0xfe,0x72,0xe4,0xf3,0x0f,0x7f,0x02,0xf3,0x0f,0x7f,0x4a,0x10,0xf3,0x0f,0x7f,0x52,0x20,0xf3,0x0f,0x7f,0x5a,0x30"
                      : "+S"(p) : "D"(end), "d"(out) : "memory", "xmm0", "xmm1", "xmm2", "xmm3");
}

static void add_avx2(const uint32_t *p, const uint32_t *end, uint32_t *out)
{
    __asm__ volatile (".byte 0xc5,0xfd,0xef,0xc0,0xc5,0xf5,0xef,0xc9,0xc5,0xed,0xef,0xd2,0xc5,0xe5,0xef,0xdb,0xc5,0xfd,0xfe,0x06,0xc5,0xf5,0xfe,0x4e,0x20,0xc5,0xed,0xfe,0x56,0x40,0xc5,0xe5,0xfe,0x5e,0x60,0x48,0x83,0xee,0x80,0x48,0x39,0xfe,0x72,0xe4,0xc5,0xfe,0x7f,0x02,0xc5,0xfe,0x7f,0x4a,0x20,0xc5,0xfe,0x7f,0x52,0x40,0xc5,0xfe,0x7f,0x5a,0x60,0xc5,0xf8,0x77"
                      : "+S"(p) : "D"(end), "d"(out) : "memory", "xmm0", "xmm1", "xmm2", "xmm3");
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* Bytes added per second over size bytes of buf, repeated for ~0.3 s (the
 * clock read only every 4 MiB: a 16 KiB pass is as short as reading it);
 * the lanes' sum to compare the two ways. */
static double add_rate(int avx, const uint32_t *buf, size_t size, uint32_t *total)
{
    uint32_t out[32];
    uint64_t bytes = 0, start = now_ns(), took;
    size_t passes = size < (4u << 20) ? (4u << 20) / size : 1;
    do {
        for (size_t i = 0; i < passes; i++) {
            memset(out, 0, sizeof(out));
            if (avx)
                add_avx2(buf, buf + size / 4, out);
            else
                add_sse2(buf, buf + size / 4, out);
        }
        bytes += size * passes;
        took = now_ns() - start;
    } while (took < 300000000u);
    *total = 0;
    for (int i = 0; i < (avx ? 32 : 16); i++)
        *total += out[i];
    return (double)bytes * 1e9 / (double)took;
}

/* One save and one restore, as a thread switch does, n times. */
static void fxsave_pairs(void *area, long n)
{
    __asm__ volatile ("1: fxsave64 (%%rdi)\n\t"
                      "fxrstor64 (%%rdi)\n\t"
                      "dec %%rcx\n\t"
                      "jnz 1b"
                      : "+c"(n) : "D"(area) : "memory");
}

static void xsave_pairs(void *area, long n)
{
    __asm__ volatile ("mov $-1, %%eax\n\t"
                      "mov $-1, %%edx\n\t"
                      "1: xsave64 (%%rdi)\n\t"
                      "xrstor64 (%%rdi)\n\t"
                      "dec %%rcx\n\t"
                      "jnz 1b"
                      : "+c"(n) : "D"(area) : "rax", "rdx", "memory");
}

static double pair_ns(int xsave, void *area)
{
    long n = 100000;
    uint64_t start = now_ns();
    if (xsave)
        xsave_pairs(area, n);
    else
        fxsave_pairs(area, n);
    return (double)(now_ns() - start) / (double)n;
}

static int speed(void)
{
    static uint32_t big[64 << 20 >> 2] __attribute__((aligned(64)));
    for (size_t i = 0; i < sizeof(big) / 4; i++)
        big[i] = (uint32_t)(i * 2654435761u);
    size_t sizes[2] = { 16384, sizeof(big) };
    const char *where[2] = { "16 KiB (in the cache)", "64 MiB (memory)" };
    for (int k = 0; k < 2; k++) {
        uint32_t t_sse, t_avx;
        double sse = add_rate(0, big, sizes[k], &t_sse);
        double avx = add_rate(1, big, sizes[k], &t_avx);
        printf("avxtest: speed: adds over %s: SSE2 %.2f GB/s, AVX2 %.2f GB/s (%.2fx)%s\n", where[k],
               sse / 1e9, avx / 1e9, avx / sse, t_sse == t_avx ? "" : " FAIL: the sums differ");
    }
    static uint8_t area[1024] __attribute__((aligned(64)));
    __asm__ volatile (".byte 0xc5,0xf8,0x77" : : : "memory");     /* vzeroupper */
    double fx = pair_ns(0, area), unused = pair_ns(1, area);
    struct hold h;
    fill(&h, 5);
    load_registers(&h);                 /* (the upper halves in use: XSAVE saves them) */
    double used = pair_ns(1, area);
    printf("avxtest: speed: saving and restoring the registers: FXSAVE %.1f ns, XSAVE %.1f ns "
           "(%.1f ns with the upper halves in use)\n", fx, unused, used);
    return 0;
}

/* The kernel's signal context (arch/x86_64/signal.c, private to it): the
 * magic at 152, the interrupted RSP at 136, the XSAVE area at 192 (MXCSR
 * at 24 in it, the header at 512). The handler finds it above its own
 * frame: the magic with the RSP above it (a spilled copy of the constant
 * has no such neighbour), the magic made at run time. */
static volatile uint32_t magic_hi = 0x52534947u, magic_lo = 0x434F4E54u;
static volatile int forged;

static void forging_handler(int signal)
{
    (void)signal;
    uint64_t here = 0, magic = (uint64_t)magic_hi << 32 | magic_lo;
    uint8_t *p = (uint8_t *)&here;
    for (int i = 0; i < 16384; i += 8) {
        uint64_t v, rsp;
        memcpy(&v, p + i, 8);
        memcpy(&rsp, p + i - 16, 8);
        if (v != magic || rsp <= (uint64_t)(uintptr_t)(p + i))
            continue;
        uint8_t *fpu = p + i + 40;
        uint32_t mxcsr;
        memcpy(&mxcsr, fpu + 24, 4);
        mxcsr |= 0xFFFF0000u;                       /* reserved: FXRSTOR and XRSTOR fault */
        memcpy(fpu + 24, &mxcsr, 4);
        uint64_t parts, compact = 1ULL << 63 | 7;
        memcpy(&parts, fpu + 512, 8);
        parts |= ~7ULL;                             /* parts XCR0 does not have on */
        memcpy(fpu + 512, &parts, 8);
        memcpy(fpu + 520, &compact, 8);             /* the compacted form */
        memset(fpu + 528, 0xFF, 48);                /* reserved */
        forged = 1;
        return;
    }
}

static void forged_context(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = forging_handler;
    sigaction(SIGUSR2, &sa, 0);
    raise(SIGUSR2);
    if (!forged) {
        printf("avxtest: FAIL: forged context: the handler found no context\n");
        failures++;
    } else
        printf("avxtest: forged context: cleaned by the kernel, the program goes on\n");
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "exec-child") == 0)
        return exec_child();
    uint32_t r[4], seven[4] = { 0 };
    cpuid(0, 0, r);
    uint32_t top = r[0];
    cpuid(1, 0, r);
    int has_avx = (r[2] >> 28) & 1, osxsave = (r[2] >> 27) & 1;
    if (top >= 7)
        cpuid(7, 0, seven);
    int has_avx2 = (seven[1] >> 5) & 1;
    uint64_t xcr0 = 0;
    if (osxsave) {
        uint32_t lo, hi;
        __asm__ volatile ("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        xcr0 = (uint64_t)hi << 32 | lo;
    }
    if (!has_avx || (xcr0 & 6) != 6) {
        printf("avxtest: no AVX: processor AVX %s, OSXSAVE %s\n", has_avx ? "yes" : "no", osxsave ? "on" : "off");
        return 0;
    }
    printf("avxtest: AVX on (XCR0 0x%llx), AVX2 %s\n", (unsigned long long)xcr0, has_avx2 ? "yes" : "no");
    if (argc > 1 && strcmp(argv[1], "speed") == 0) {
        if (!has_avx2) {
            printf("avxtest: speed: no AVX2 here\n");
            return 0;
        }
        return speed();
    }
    struct timespec ts = { 0, 20000000 }, t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t c0 = tsc();
    nanosleep(&ts, 0);
    uint64_t c1 = tsc();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000u + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
    ticks_per_ms = ns ? (c1 - c0) * 1000000u / ns : 1000000;
    switches();
    signals();
    forked();
    execed(argv[0]);
    if (has_avx2)
        avx2_add();
    forged_context();
    printf("avxtest: %s\n", failures ? "FAILED" : "all kept");
    return failures ? 1 : 0;
}

#else

int main(void)
{
    printf("avxtest: no AVX: not an x86 processor\n");
    return 0;
}

#endif

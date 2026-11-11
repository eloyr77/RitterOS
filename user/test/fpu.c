/*
 * Test: floating point in several programs at once. A long calculation
 * keeps its values in FP/SIMD registers; IEEE arithmetic gives the same bits
 * every time, so a result that differs means registers were lost or mixed up
 * when the kernel switched between programs.
 *
 * fpu                      compute pi, three reference results one after
 *                          another, then run three workers at the same time
 * fpu worker SEED EXPECTED a worker: compute, compare with EXPECTED (hex)
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__x86_64__)
#define STEPS 1500000L                  /* emulated: slower */
#else
#define STEPS 20000000L
#endif

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

static uint64_t bits(double d)
{
    uint64_t b;
    memcpy(&b, &d, sizeof(b));
    return b;
}

static uint64_t hex(const char *s)
{
    uint64_t v = 0;
    for (; *s; s++)
        v = v * 16 + (uint64_t)(*s <= '9' ? *s - '0' : (*s | 0x20) - 'a' + 10);
    return v;
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "worker") == 0) {
        int seed = argv[2][0] - '0';
        uint64_t got = bits(work(seed)), want = hex(argv[3]);
        printf("fpu: worker %d: %s\n", seed, got == want ? "correct" : "WRONG");
        return got == want ? 0 : 1;
    }

    /* pi from the Nilakantha series: 3 + 4/(2*3*4) - 4/(4*5*6) + ... */
    double pi = 3.0;
    for (int k = 1; k <= 2000; k++) {
        double n = 2.0 * k;
        pi += (k % 2 ? 4.0 : -4.0) / (n * (n + 1) * (n + 2));
    }
    printf("fpu: pi = %.9f (Nilakantha series)\n", pi);

    /* The expected results, computed alone. */
    char expected[3][20];
    for (int seed = 1; seed <= 3; seed++)
        snprintf(expected[seed - 1], sizeof(expected[0]), "%lx", (unsigned long)bits(work(seed)));

    /* The same three calculations at the same time, in three processes. */
    pid_t pids[3];
    for (int seed = 1; seed <= 3; seed++) {
        char number[2] = { (char)('0' + seed), 0 };
        char *args[] = { "fpu", "worker", number, expected[seed - 1], 0 };
        pids[seed - 1] = spawn(argv[0], args);
    }
    int correct = 0;
    for (int i = 0; i < 3; i++) {
        int status;
        if (waitpid(pids[i], &status, 0) == pids[i] && WIFEXITED(status) && !WEXITSTATUS(status))
            correct++;
    }
    printf("fpu: 3 workers computed at the same time: %d correct\n", correct);
    return correct == 3 ? 0 : 1;
}

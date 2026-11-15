/*
 * (43a) The older random number generators ported code uses:
 * - random/srandom/initstate/setstate: BSD's additive feedback generator,
 *   the one glibc and macOS share (a seed gives the same numbers on both):
 *   a table of r words seeded by the minimal standard generator (16807 x
 *   mod 2^31 - 1), then each number the sum of two entries sep apart,
 *   halved; the first 10 r numbers after a seed are thrown away. A state
 *   of 8 bytes is a plain linear congruential generator (type 0); larger
 *   ones (32, 64, 128, 256 bytes) tables of 7, 15, 31 and 63 words.
 * - the 48-bit generators of POSIX (drand48 ... lcong48): x = a x + c mod
 *   2^48, a = 0x5DEECE66D and c = 11 until lcong48 changes them.
 * - rand_r: a generator whose whole state is the caller's unsigned int.
 * Not for threads sharing one at once, as on Unix.
 */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The kinds of table, by the state's size: degree and separation. */
static const struct { int degree, separation; } kinds[] = {
    { 0, 0 }, { 7, 3 }, { 15, 1 }, { 31, 3 }, { 63, 1 },
};

/* A caller's state is an int32_t array: word 0 keeps the kind and the
 * front place (kind + 5 * front, as BSD's), the table follows. */
static int32_t default_state[32];
static int32_t *table = default_state + 1;
static int32_t *state_word = default_state;
static int kind = 3, degree = 31, separation = 3;
static int front = 3, rear = 0;                 /* table[front] += table[rear] */
static int seeded;                              /* (until then: as srandom(1)) */

static int32_t minimal_standard(int32_t x)
{
    /* 16807 x mod (2^31 - 1) without overflow (Schrage's method) */
    int32_t hi = x / 127773, lo = x % 127773;
    int32_t word = 16807 * lo - 2836 * hi;
    return word < 0 ? word + 2147483647 : word;
}

static long next(void)
{
    if (kind == 0) {
        table[0] = (int32_t)(((uint32_t)table[0] * 1103515245u + 12345u) & 0x7fffffff);
        return table[0];
    }
    uint32_t sum = (uint32_t)table[front] + (uint32_t)table[rear];
    table[front] = (int32_t)sum;
    long result = (long)((sum >> 1) & 0x7fffffff);
    if (++front >= degree)
        front = 0, ++rear;
    else if (++rear >= degree)
        rear = 0;
    return result;
}

static void save_place(void)
{
    *state_word = kind == 0 ? 0 : kind + 5 * front;
}

long random(void)
{
    if (!seeded)
        srandom(1);
    long r = next();
    save_place();
    return r;
}

void srandom(unsigned int seed)
{
    seeded = 1;
    if (seed == 0 && kind != 0)
        seed = 1;
    table[0] = (int32_t)seed;
    if (kind == 0) {
        save_place();
        return;
    }
    for (int i = 1; i < degree; i++)
        table[i] = minimal_standard(table[i - 1]);
    front = separation;
    rear = 0;
    for (int i = 0; i < 10 * degree; i++)
        next();
    save_place();
}

char *initstate(unsigned int seed, char *state, size_t size)
{
    if (size < 8) {
        errno = EINVAL;
        return 0;
    }
    if (!seeded)
        srandom(1);
    char *old = (char *)state_word;
    save_place();
    int k = size >= 256 ? 4 : size >= 128 ? 3 : size >= 64 ? 2 : size >= 32 ? 1 : 0;
    state_word = (int32_t *)(void *)state;
    table = state_word + 1;
    kind = k;
    degree = kinds[k].degree;
    separation = kinds[k].separation;
    srandom(seed);
    return old;
}

char *setstate(char *state)
{
    int32_t *w = (int32_t *)(void *)state;
    int k = w[0] % 5;
    if (k < 0 || k > 4) {
        errno = EINVAL;
        return 0;
    }
    if (!seeded)
        srandom(1);
    char *old = (char *)state_word;
    save_place();
    state_word = w;
    table = w + 1;
    kind = k;
    degree = kinds[k].degree;
    separation = kinds[k].separation;
    if (k != 0) {
        front = w[0] / 5;
        rear = (front + degree - separation) % degree;
    }
    return old;
}

/* The 48-bit generators. */
static unsigned short x48[3] = { 0x330E, 0xABCD, 0x1234 };
static unsigned short a48[3] = { 0xE66D, 0xDEEC, 0x0005 };
static unsigned short c48 = 0x000B;

static uint64_t step48(unsigned short x[3])
{
    uint64_t v = (uint64_t)x[0] | (uint64_t)x[1] << 16 | (uint64_t)x[2] << 32;
    uint64_t a = (uint64_t)a48[0] | (uint64_t)a48[1] << 16 | (uint64_t)a48[2] << 32;
    v = (v * a + c48) & 0xFFFFFFFFFFFFull;
    x[0] = (unsigned short)v;
    x[1] = (unsigned short)(v >> 16);
    x[2] = (unsigned short)(v >> 32);
    return v;
}

double erand48(unsigned short x[3])
{
    return (double)step48(x) / 281474976710656.0;       /* 2^48 */
}

double drand48(void) { return erand48(x48); }

long nrand48(unsigned short x[3])
{
    return (long)(step48(x) >> 17);
}

long lrand48(void) { return nrand48(x48); }

long jrand48(unsigned short x[3])
{
    return (long)(int32_t)(uint32_t)(step48(x) >> 16);
}

long mrand48(void) { return jrand48(x48); }

void srand48(long seed)
{
    x48[0] = 0x330E;
    x48[1] = (unsigned short)seed;
    x48[2] = (unsigned short)((unsigned long)seed >> 16);
    a48[0] = 0xE66D, a48[1] = 0xDEEC, a48[2] = 0x0005;
    c48 = 0x000B;
}

unsigned short *seed48(unsigned short seed[3])
{
    static unsigned short old[3];
    memcpy(old, x48, sizeof(old));
    memcpy(x48, seed, sizeof(x48));
    a48[0] = 0xE66D, a48[1] = 0xDEEC, a48[2] = 0x0005;
    c48 = 0x000B;
    return old;
}

void lcong48(unsigned short parameters[7])
{
    memcpy(x48, parameters, sizeof(x48));
    memcpy(a48, parameters + 3, sizeof(a48));
    c48 = parameters[6];
}

/* (POSIX's example generator, as glibc's: three steps of 11, 10 and 10
 * bits) */
int rand_r(unsigned int *seed)
{
    unsigned int s = *seed;
    s = s * 1103515245u + 12345u;
    int result = (int)((s / 65536) % 2048);
    s = s * 1103515245u + 12345u;
    result = (result << 10) ^ (int)((s / 65536) % 1024);
    s = s * 1103515245u + 12345u;
    result = (result << 10) ^ (int)((s / 65536) % 1024);
    *seed = s;
    return result;
}

int getloadavg(double loads[], int count)
{
    (void)loads;
    (void)count;
    return -1;
}

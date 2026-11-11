/* Test: SHA-256, HMAC-SHA256 and PBKDF2-HMAC-SHA256 against known
 * answers (scripts/test.sh computes them with Python's hashlib), random
 * bytes from the kernel, and how long a password hash takes. */
#include <stdio.h>
#include <string.h>
#include "ritteros.h"

static void show(const char *what, const uint8_t *h, size_t n)
{
    printf("crypto: %s: ", what);
    for (size_t i = 0; i < n; i++)
        printf("%02x", h[i]);
    printf("\n");
}

static char big[1000000];

int main(void)
{
    uint8_t h[SHA256_SIZE], dk[40];
    sha256("", 0, h);
    show("sha256('')", h, sizeof(h));
    sha256("abc", 3, h);
    show("sha256('abc')", h, sizeof(h));
    memset(big, 'a', sizeof(big));
    sha256(big, sizeof(big), h);                    /* many blocks, odd length */
    show("sha256(a million 'a')", h, sizeof(h));
    hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, h);
    show("hmac('Jefe')", h, sizeof(h));
    memset(big, 0xaa, 131);
    hmac_sha256(big, 131, "Test Using Larger Than Block-Size Key - Hash Key First", 54, h);
    show("hmac(131-byte key)", h, sizeof(h));
    pbkdf2_sha256("password", 8, (const uint8_t *)"salt", 4, 1, dk, 32);
    show("pbkdf2(1)", dk, 32);
    pbkdf2_sha256("password", 8, (const uint8_t *)"salt", 4, 4096, dk, 40);
    show("pbkdf2(4096, 40 bytes)", dk, 40);

    uint8_t a[16], b[16];
    ritteros_random(a, sizeof(a));
    ritteros_random(b, sizeof(b));
    show("random", a, sizeof(a));
    printf("crypto: two random draws differ: %s\n", memcmp(a, b, sizeof(a)) ? "yes" : "NO");

    struct sys_info before, after;
    ritteros_sysinfo(&before);
    pbkdf2_sha256("a password", 10, a, sizeof(a), PASSWORD_ITERATIONS, h, sizeof(h));
    ritteros_sysinfo(&after);
    printf("crypto: a password hash (%u iterations) took %lu ms\n",
           (unsigned)PASSWORD_ITERATIONS,
           (unsigned long)((after.ticks - before.ticks) * 1000 / after.ticks_per_second));
    return 0;
}

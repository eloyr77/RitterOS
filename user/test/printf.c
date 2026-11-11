/* Test: printf's conversions, flags, widths and precisions. */
#include <stdio.h>
#include <string.h>

int main(void)
{
    char buf[16];
    printf("printf: %d %i %u %x %X %o\n", 42, -7, 4294967295u, 255, 255, 8);
    printf("printf: %ld %lu %lx\n", -9000000000L, 18446744073709551615UL, 0xDEADBEEFCAFEUL);
    printf("printf: [%5d] [%-5d] [%05d] [%-8s] [%8s] [%.3s] [%*d] [%c] [%%]\n",
           42, 42, 42, "left", "right", "abcdef", 4, 7, 'x');
    printf("printf: %p %s\n", (void *)0x400000, (char *)0);
    int n = snprintf(buf, sizeof(buf), "%s-%d", "a long string", 12345);
    printf("printf: snprintf wrote '%s', wanted %d, length %d\n", buf, n, (int)strlen(buf));
    printf("printf: UTF-8 Grüße ☃ split %s\n", "über");
    printf("printf: %f %.3f %.1f [%8.2f] %.0f %f\n", 3.0, 3.14159, -2.75, 1.5, 41.6, 1e-7);
    return 0;
}

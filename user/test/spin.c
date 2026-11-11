/* Test: a program that computes for a while, so that timer interrupts
 * arrive while it runs in user mode (and must return to it unharmed). */
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    volatile uint64_t sum = 0;
    for (uint64_t i = 0; i < 100000000; i++)
        sum += i;
    printf("spin: done, sum %lu\n", (unsigned long)sum);
    return 0;
}

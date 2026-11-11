/* Test: a program writes into the kernel's memory. The page tables do not
 * allow it: the CPU raises a fault, and the kernel stops the program. */
#include <stdint.h>
#include <stdio.h>

#define KERNEL_CODE 0xFFFFFFFF80000000ULL      /* the kernel image (common/vmlayout.h) */

int main(void)
{
    printf("kwrite: writing to the kernel at %p\n", (void *)KERNEL_CODE);
    *(volatile uint64_t *)KERNEL_CODE = 0;
    printf("kwrite: the write went through!\n");
    return 0;
}

/* Test: a program uses an instruction only the kernel may use: it reads the
 * register that holds the kernel's page tables. The CPU refuses. */
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    uint64_t value;
    printf("privileged: reading the kernel's page table register\n");
#if defined(__x86_64__)
    __asm__ volatile ("mov %%cr3, %0" : "=r"(value));
#else
    __asm__ volatile ("mrs %0, ttbr1_el1" : "=r"(value));
#endif
    printf("privileged: it worked: %p\n", (void *)value);
    return 0;
}

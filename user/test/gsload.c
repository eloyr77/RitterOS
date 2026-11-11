/* Test (65a): a program that loads GS with a selector of its own, then
 * makes system calls and gets interrupted. x86's kernel finds each core's
 * block through the GS base and swaps it with the program's on every way in
 * and out (SWAPGS): without that, loading GS here would set the kernel's
 * base, and the next system call would find no core's block. On ARM the
 * kernel's register (TPIDR_EL1) is out of a program's reach: nothing to try. */
#include <stdio.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
#ifdef __x86_64__
    unsigned short selector = 0x18 | 3;     /* the user data segment (GDT_USER_DATA, ring 3) */
    __asm__ volatile ("movw %0, %%gs" : : "r"(selector));
    long pids = 0;
    for (int i = 0; i < 1000; i++)
        pids += getpid() > 0;               /* system calls */
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do                                      /* 200 ms of work: timer interrupts from ring 3 */
        clock_gettime(CLOCK_MONOTONIC, &now);
    while ((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000 < 200);
    usleep(50000);                          /* a sleep: a switch away and back */
    unsigned short gs;
    __asm__ volatile ("movw %%gs, %0" : "=r"(gs));
    printf("gsload: GS 0x%x loaded, %ld system calls, interrupts and a sleep: the kernel unharmed\n",
           gs, pids);
#else
    printf("gsload: not x86: the kernel's register is out of reach\n");
#endif
    return 0;
}

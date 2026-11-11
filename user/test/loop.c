/* Test: a program that never ends by itself: only Ctrl-C stops it. */
#include <stdio.h>

int main(void)
{
    printf("loop: running until Ctrl-C\n");
    for (volatile int i = 0;; i++)
        ;
}

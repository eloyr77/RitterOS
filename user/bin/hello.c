/* The first RitterOS program: runs in user mode and prints where it lives. */
#include <stdio.h>
#include "ritteros.h"

PROGRAM_HELP("hello", "the first RitterOS program: where it is in memory");

int main(void)
{
    char local = 0;
    printf("Hello from user mode!\n");
    printf("  my code is at  %p\n", (void *)main);
    printf("  my stack is at %p\n", (void *)&local);
    return 0;
}

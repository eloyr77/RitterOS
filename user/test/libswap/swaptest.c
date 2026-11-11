/*
 * Test: a library replaced under its programs (step 22d). swaptest was built
 * with the first version of libswap.so.1 and says which version it runs on;
 * `swaptest later SECONDS` says it, waits, and says it again -- a program
 * that was already running when the library was replaced keeps the version
 * it started with (its pages stay in memory), the next one started gets the
 * new one without being rebuilt. `swaptest relro` writes the library's
 * relocated data (22f): ld.so made it read-only, so the program must stop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "swap.h"

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "relro") == 0) {
        printf("swaptest: writing libswap's relocated data\n");
        *(const char *volatile *)(uintptr_t)swap_relro() = "changed";
        printf("swaptest: the relocated data was writable\n");
        return 1;
    }
    printf("swaptest: libswap says %s\n", swap_version());
    if (argc == 3 && strcmp(argv[1], "later") == 0) {
        sleep((unsigned)atoi(argv[2]));
        printf("swaptest: later, libswap says %s\n", swap_version());
    }
    return 0;
}

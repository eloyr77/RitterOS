/*
 * drivers (67a, roadmap item 34): the kernel's modules, the drivers kept
 * as files in /system/<arch>/drivers/: each one loaded (where, how large,
 * the devices it took), not needed (none of its hardware here) or refused
 * (why). The kernel's report (REPORT_DRIVERS), as its rescue console
 * shows it.
 */
#include <stdio.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("drivers", "list the kernel's modules (drivers): loaded, not needed or refused");

static char text[8 * 1024];

int main(void)
{
    int64_t n = ritteros_report(REPORT_DRIVERS, text, sizeof(text));
    if (n < 0)
        return 1;
    write(STDOUT_FILENO, text, n < (int64_t)sizeof(text) ? (size_t)n : sizeof(text) - 1);
    return 0;
}

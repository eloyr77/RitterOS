/* cpus: list the processor's cores (the kernel's report, as its rescue console shows it; 65b). */
#include <stdio.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("cpus", "list the processor's cores");

static char text[8 * 1024];

int main(void)
{
    int64_t n = ritteros_report(REPORT_CPUS, text, sizeof(text));
    if (n < 0)
        return 1;
    write(STDOUT_FILENO, text, n < (int64_t)sizeof(text) ? (size_t)n : sizeof(text) - 1);
    return 0;
}

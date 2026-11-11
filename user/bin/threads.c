/* threads: list the kernel's threads (the kernel's report, as its rescue console shows it). */
#include <stdio.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("threads", "list the kernel's threads");

static char text[32 * 1024];

int main(void)
{
    int64_t n = ritteros_report(REPORT_THREADS, text, sizeof(text));
    if (n < 0)
        return 1;
    write(STDOUT_FILENO, text, n < (int64_t)sizeof(text) ? (size_t)n : sizeof(text) - 1);
    return 0;
}

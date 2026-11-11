/* mem: free and usable memory, exact to the page, and the page cache. */
#include <stdio.h>
#include "ritteros.h"

static char report[256];

PROGRAM_HELP("mem", "free and usable memory");

int main(void)
{
    struct sys_info info;
    if (ritteros_sysinfo(&info) < 0)
        return 1;
    printf("memory: %lu MiB free of %lu MiB (%lu of %lu pages of %lu KiB)\n",
           (unsigned long)(info.free_pages * info.page_size >> 20),
           (unsigned long)(info.usable_pages * info.page_size >> 20),
           (unsigned long)info.free_pages, (unsigned long)info.usable_pages,
           (unsigned long)(info.page_size / 1024));
    long n = (long)ritteros_report(REPORT_MEMORY, report, sizeof(report) - 1);
    if (n > 0 && n < (long)sizeof(report)) {
        report[n] = 0;
        printf("%s", report);
    }
    return 0;
}

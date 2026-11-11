/* uptime: time since the kernel's timer started. */
#include <stdio.h>
#include "ritteros.h"

PROGRAM_HELP("uptime", "time since the kernel's timer started");

int main(void)
{
    struct sys_info info;
    if (ritteros_sysinfo(&info) < 0)
        return 1;
    uint64_t t = info.ticks, hz = info.ticks_per_second;
    printf("up %lu.%02lu s (%lu timer ticks at %lu Hz)\n", (unsigned long)(t / hz),
           (unsigned long)(t % hz * 100 / hz), (unsigned long)t, (unsigned long)hz);
    return 0;
}

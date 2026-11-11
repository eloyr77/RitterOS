/* poweroff: switch the machine off (logd writes what it keeps first). */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("poweroff", "switch the machine off");

int main(void)
{
    logs_flush();
    ritteros_power(POWER_OFF);
    dprintf(STDERR_FILENO,
            "poweroff: the machine could not be switched off: %s\n",
            strerror(errno));
    return 1;
}

/* reboot: restart the machine (logd writes what it keeps first). */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("reboot", "restart the machine");

int main(void)
{
    logs_flush();
    ritteros_power(POWER_REBOOT);
    dprintf(STDERR_FILENO,
            "reboot: the machine could not be restarted: %s\n",
            strerror(errno));
    return 1;
}

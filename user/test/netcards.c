/*
 * Test: the network cards as programs find them (ritteros_netcard): every
 * card the kernel lists, by its place, whatever its kind or name, and
 * ENODEV past the last one.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "ritteros.h"

int main(void)
{
    struct sys_netinfo info;
    int i = 0;
    for (; ritteros_netcard(i, &info) == 0; i++) {
        char address[16];
        ip_format(info.address, address, sizeof(address));
        printf("netcards: card %d: %s, link %s, address %s\n", i, info.name,
               info.link_speed ? "up" : "down", info.address ? address : "none");
    }
    printf("netcards: card %d: %s\n", i, strerror(errno));
    return errno == ENODEV ? 0 : 1;
}

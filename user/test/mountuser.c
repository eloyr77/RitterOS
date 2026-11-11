/*
 * Test: mountuser DEVICE PATH becomes user 1000, then tries to mount DEVICE
 * and to unmount PATH: both are root's.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

int main(int argc, char **argv)
{
    if (argc != 3 || setgid(1000) < 0 || setuid(1000) < 0)
        return 2;
    char path[128];
    int r = ritteros_mount(argv[1], 0, path, sizeof(path));
    printf("mountuser: mount as user 1000: %d, %s\n", r, r < 0 ? strerror(errno) : path);
    r = ritteros_unmount(argv[2]);
    printf("mountuser: unmount as user 1000: %d, %s\n", r, r < 0 ? strerror(errno) : "done");
    return 0;
}

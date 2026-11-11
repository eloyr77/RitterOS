/*
 * Test client for soundd: connect to /run/sound and send FRAMES frames of a
 * constant level LEVEL (stereo), then exit. Two of these at once let the test
 * confirm soundd sums them (the captured peak is about twice one client's).
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "ritteros.h"

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    int level = atoi(argv[1]);
    long frames = atol(argv[2]);
    int fd = ritteros_local_connect("/run/sound");
    if (fd < 0) {
        printf("toneclient: connect: %s\n", strerror(errno));
        return 1;
    }
    static int16_t buf[512 * 2];
    for (int i = 0; i < 512 * 2; i++)
        buf[i] = (int16_t)level;
    long left = frames;
    while (left > 0) {
        long n = left < 512 ? left : 512;
        if (write(fd, buf, (size_t)n * 4) <= 0)
            break;
        left -= n;
    }
    close(fd);
    printf("toneclient: sent %ld frames at %d\n", frames, level);
    return 0;
}

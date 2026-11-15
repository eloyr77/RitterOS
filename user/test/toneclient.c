/*
 * Test client for soundd: connect to /run/sound and send FRAMES frames of a
 * constant level LEVEL (stereo), then exit. Two of these at once let the test
 * confirm soundd sums them (the captured peak is about twice one client's).
 *
 * (68v) `toneclient device`: /dev/pcm0 itself, four constant levels the test
 * finds in the capture, each a run of its own: 3000 for 0.25 s with the
 * delay (SNDCTL_DSP_GETODELAY) read after the write, 100 ms later and 700
 * ms later (it must go down as it plays, then be 0, the device still open);
 * 9000 written for 1 s, then SNDCTL_DSP_RESET (the delay 0 at once, the
 * close without waiting, less than 1 s heard); 15000 for 0.2 s, 1 s with
 * nothing written (an underrun: silence, never the ring's old sound, and
 * the delay 0), then 21000 for 0.2 s.
 *
 * (68w) `toneclient rsnd`: soundd's messages ("RSND", then type and bytes):
 * 6000 for 0.3 s with its delay asked after sending and after 1 s (0); then
 * 12000 for 1 s, its delay, a reset and the delay again (the device's short
 * buffer at most), and the connection kept 0.5 s.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <time.h>
#include <unistd.h>
#include "ritteros.h"

static long long ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int play(int fd, int level, long frames)
{
    static int16_t buf[512 * 2];
    for (int i = 0; i < 512 * 2; i++)
        buf[i] = (int16_t)level;
    for (long left = frames; left > 0;) {
        long n = left < 512 ? left : 512;
        if (write(fd, buf, (size_t)n * 4) != n * 4)
            return -1;
        left -= n;
    }
    return 0;
}

static int delay_of(int fd)
{
    int d = -1;
    if (ioctl(fd, SNDCTL_DSP_GETODELAY, &d) < 0)
        printf("toneclient: SNDCTL_DSP_GETODELAY: %s\n", strerror(errno));
    return d;
}

static int device(void)
{
    int fd = open("/dev/pcm0", O_WRONLY);
    if (fd < 0) {
        printf("toneclient: /dev/pcm0: %s\n", strerror(errno));
        return 1;
    }
    if (play(fd, 3000, 12000) < 0)
        return 1;
    long long t0 = ms_now();
    int d0 = delay_of(fd);
    usleep(100000);
    long long t1 = ms_now();
    int d1 = delay_of(fd);
    usleep(600000);
    int d2 = delay_of(fd);
    long rate = t1 > t0 && d0 > d1 ? (long)(d0 - d1) * 1000 / (long)(t1 - t0) : 0;
    printf("toneclient: delay: %d bytes after writing 48000, %d %lld ms later (%ld bytes/s played), "
           "%d after 700 ms\n", d0, d1, t1 - t0, rate, d2);
    close(fd);
    usleep(300000);

    fd = open("/dev/pcm0", O_WRONLY);
    if (fd < 0 || play(fd, 9000, 48000) < 0)
        return 1;
    int before = delay_of(fd);
    int r = ioctl(fd, SNDCTL_DSP_RESET, 0);
    int after = delay_of(fd);
    long long c0 = ms_now();
    close(fd);
    printf("toneclient: reset: %d (delay %d before, %d after), the close in %lld ms\n", r, before,
           after, ms_now() - c0);
    usleep(300000);

    fd = open("/dev/pcm0", O_WRONLY);
    if (fd < 0 || play(fd, 15000, 9600) < 0)
        return 1;
    usleep(1000000);
    int gap = delay_of(fd);
    if (play(fd, 21000, 9600) < 0)
        return 1;
    int resumed = delay_of(fd);
    close(fd);
    printf("toneclient: underrun: delay %d after 1 s without writing, %d after writing 38400 "
           "again\n", gap, resumed);
    int bad = ioctl(1, SNDCTL_DSP_GETODELAY, &gap) == 0 || errno != ENOTTY;
    printf("toneclient: not a sound device: %s\n", bad ? "WRONG" : "ENOTTY");
    return 0;
}

static int message(int sock, uint32_t type, const void *bytes, uint32_t n)
{
    uint32_t header[2] = { type, n };
    if (write(sock, header, sizeof(header)) != sizeof(header))
        return -1;
    return n && write(sock, bytes, n) != (ssize_t)n ? -1 : 0;
}

static int mixed(int sock, int level, long frames)
{
    static int16_t buf[512 * 2];
    for (int i = 0; i < 512 * 2; i++)
        buf[i] = (int16_t)level;
    for (long left = frames; left > 0;) {
        long n = left < 512 ? left : 512;
        if (message(sock, 1, buf, (uint32_t)n * 4) < 0)
            return -1;
        left -= n;
    }
    return 0;
}

static long mixed_delay(int sock)
{
    uint32_t d;
    if (message(sock, 2, 0, 0) < 0 || read(sock, &d, sizeof(d)) != sizeof(d))
        return -1;
    return (long)d;
}

static int rsnd(void)
{
    int sock = ritteros_local_connect("/run/sound");
    if (sock < 0 || write(sock, "RSND", 4) != 4) {
        printf("toneclient: /run/sound: %s\n", strerror(errno));
        return 1;
    }
    if (mixed(sock, 6000, 14400) < 0)
        return 1;
    long d0 = mixed_delay(sock);
    usleep(1000000);
    long d1 = mixed_delay(sock);
    printf("toneclient: rsnd: delay %ld bytes after sending 57600, %ld after 1 s\n", d0, d1);
    if (mixed(sock, 12000, 48000) < 0)
        return 1;
    long before = mixed_delay(sock);
    if (message(sock, 3, 0, 0) < 0)
        return 1;
    long after = mixed_delay(sock);
    printf("toneclient: rsnd: reset: delay %ld before, %ld after\n", before, after);
    usleep(500000);
    close(sock);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "device"))
        return device();
    if (argc == 2 && !strcmp(argv[1], "rsnd"))
        return rsnd();
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

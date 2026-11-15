/*
 * volume: the master volume of everything played through the sound server
 * (68x). Asks soundd on /run/sound (its message 4: the volume 0-100 and
 * mute, -1 for as it is) and prints what it is now; soundd keeps it in
 * /system/config/sound.
 *   volume            what it is: "volume 80", or "volume 80 (muted)"
 *   volume N          set it to N (0-100)
 *   volume +N, -N     up or down by N (within 0-100)
 *   volume mute       nothing heard, the volume kept; volume unmute
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("volume [N | +N | -N | mute | unmute]",
             "the master volume of the sound server (0-100)");

static int usage(void)
{
    fprintf(stderr, "usage: volume [N | +N | -N | mute | unmute]\n");
    return 2;
}

static int read_full(int fd, void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, (char *)buf + done, n - done);
        if (r <= 0)
            return -1;
        done += (size_t)r;
    }
    return 0;
}

/* One volume message: set (each -1: as it is), the two as they are now. */
static int ask(int sock, int32_t v, int32_t m, int32_t now[2])
{
    struct { uint32_t type, bytes; int32_t v, m; } msg = { 4, 8, v, m };
    if (write(sock, &msg, sizeof(msg)) != sizeof(msg) || read_full(sock, now, 8) < 0)
        return -1;
    return 0;
}

/* A number 0-100 (all digits), or -1. */
static int number(const char *s)
{
    if (!*s || strlen(s) > 3 || strspn(s, "0123456789") != strlen(s))
        return -1;
    int v = atoi(s);
    return v <= 100 ? v : -1;
}

int main(int argc, char **argv)
{
    if (argc > 2)
        return usage();
    const char *a = argc == 2 ? argv[1] : 0;
    int32_t v = -1, m = -1, step = 0;
    if (!a) {
    } else if (!strcmp(a, "mute")) {
        m = 1;
    } else if (!strcmp(a, "unmute")) {
        m = 0;
    } else if ((a[0] == '+' || a[0] == '-') && number(a + 1) >= 0) {
        step = a[0] == '+' ? number(a + 1) : -number(a + 1);
    } else if ((v = number(a)) < 0) {
        return usage();
    }
    int sock = ritteros_local_connect("/run/sound");
    if (sock < 0) {
        fprintf(stderr, "volume: no sound server at /run/sound: %s\n", strerror(errno));
        return 1;
    }
    int32_t now[2];
    /* (68z) A refused connection is closed at once: the first write may
     * fail already, or the answer never comes. */
    if (write(sock, "RSND", 4) != 4 || ask(sock, step ? -1 : v, m, now) < 0) {
        fprintf(stderr, "volume: the sound server did not answer (it plays for root and the "
                        "user at the console only)\n");
        return 1;
    }
    if (step) {                             /* (from what it is now) */
        int32_t to = now[0] + step;
        if (ask(sock, to < 0 ? 0 : to > 100 ? 100 : to, -1, now) < 0) {
            fprintf(stderr, "volume: the sound server did not answer\n");
            return 1;
        }
    }
    close(sock);
    printf("volume %d%s\n", (int)now[0], now[1] ? " (muted)" : "");
    return 0;
}

/*
 * tune FILE: play a sound file (WAV so far) through the audio output.
 * Decodes with libaudio, resamples to the output's 48 kHz and makes it
 * stereo as needed, then streams it.
 *
 * (68y) The sound goes to the sound server soundd (/run/sound) when it
 * runs, so it plays beside other programs' sound and under the master
 * volume; else to /dev/pcm0 itself. To soundd it speaks in messages
 * ("RSND", then a type and a length before each: soundd's page): pieces of
 * sound kept at most AHEAD bytes ahead of what is heard (its delay asked
 * before each), and once all is sent it waits until all is heard, so
 * "tune: done" means done there too (as the device's close drains).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "audio.h"
#include "ritteros.h"

PROGRAM_HELP("tune FILE", "play a sound file (WAV) through the speakers");

#define RATE 48000
#define PIECE 8192                      /* (68y) bytes in one message to soundd (~43 ms) */
#define AHEAD (RATE * 4 / 5)            /* (68y) bytes sent ahead of what is heard (0.2 s) */

static int write_all(int fd, const void *p, long n)
{
    const char *b = p;
    while (n > 0) {
        ssize_t w = write(fd, b, (size_t)n);
        if (w <= 0)
            return -1;
        b += w, n -= w;
    }
    return 0;
}

static int message(int sock, uint32_t type, const void *p, uint32_t n)
{
    uint32_t h[2] = { type, n };
    if (write_all(sock, h, sizeof(h)) < 0)
        return -1;
    return n ? write_all(sock, p, n) : 0;
}

/* Our bytes soundd has not played yet (message 2), or -1. */
static long delay(int sock)
{
    uint32_t d;
    size_t got = 0;
    if (message(sock, 2, 0, 0) < 0)
        return -1;
    while (got < sizeof(d)) {
        ssize_t r = read(sock, (char *)&d + got, sizeof(d) - got);
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return (long)d;
}

/* (68y) soundd's: a connection speaking its messages, or -1: none runs,
 * or (68z) it refused us (it plays for root and the console's user only;
 * /dev/pcm0 then says why). */
static int server(void)
{
    int sock = ritteros_local_connect("/run/sound");
    if (sock >= 0 && (write_all(sock, "RSND", 4) < 0 || delay(sock) < 0)) {
        close(sock);
        sock = -1;
    }
    return sock;
}

/* n bytes through soundd, then wait until they are heard (at most 10 s
 * past the last piece: soundd's mix goes on at real time even without a
 * device, so its delay always comes down). */
static int play_server(int sock, const char *p, long n)
{
    long off = 0, d;
    while (off < n) {
        if ((d = delay(sock)) < 0)
            return -1;
        if (d > AHEAD) {
            usleep(10000);
            continue;
        }
        long k = n - off < PIECE ? n - off : PIECE;
        if (message(sock, 1, p + off, (uint32_t)k) < 0)
            return -1;
        off += k;
    }
    for (int i = 0; i < 1000; i++) {
        if ((d = delay(sock)) <= 0)
            return d < 0 ? -1 : 0;
        usleep(10000);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        dprintf(2, "usage: tune FILE\n");
        return 2;
    }
    struct audio_buffer a;
    if (audio_load(argv[1], &a) < 0) {
        dprintf(STDERR_FILENO, "tune: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    if (a.rate != RATE) {                            /* match the output's rate */
        struct audio_buffer r;
        if (audio_resample(&a, RATE, &r) < 0) {
            dprintf(STDERR_FILENO, "tune: resample: %s\n", strerror(errno));
            audio_free(&a);
            return 1;
        }
        audio_free(&a);
        a = r;
    }
    /* A machine without an audio controller has no /dev/pcm0 (soundd would
     * take the sound and play it nowhere). */
    int sock = access("/dev/pcm0", F_OK) == 0 ? server() : -1;
    int fd = sock < 0 ? open("/dev/pcm0", O_WRONLY) : -1;
    if (sock < 0 && fd < 0) {
        dprintf(STDERR_FILENO, "tune: /dev/pcm0: %s\n", strerror(errno));
        audio_free(&a);
        return 1;
    }
    printf("tune: playing %s (%ld frames)\n", argv[1], a.frames);
    int16_t *st = a.samples;                        /* already stereo 16-bit */
    if (a.channels != 2) {                          /* mono: duplicate to stereo */
        st = malloc((size_t)a.frames * 4);
        const int16_t *m = a.samples;
        for (long i = 0; st && i < a.frames; i++)
            st[i * 2] = st[i * 2 + 1] = m[i];
    }
    int rc = -1;
    if (st)
        rc = sock >= 0 ? play_server(sock, (const char *)st, a.frames * 4)
                       : write_all(fd, st, a.frames * 4);
    if (st != a.samples)
        free(st);
    close(sock >= 0 ? sock : fd);
    audio_free(&a);
    if (rc < 0) {
        dprintf(STDERR_FILENO, "tune: playback failed: %s\n", strerror(errno));
        return 1;
    }
    printf("tune: done\n");
    return 0;
}

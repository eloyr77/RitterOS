/*
 * tune FILE: play a sound file (WAV so far) through the audio output
 * (/dev/pcm0). Decodes with libaudio, resamples to the output's 48 kHz and
 * makes it stereo as needed, then streams it.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "audio.h"
#include "ritteros.h"

PROGRAM_HELP("tune FILE", "play a sound file (WAV) through the speakers");

#define RATE 48000

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
    int fd = open("/dev/pcm0", O_WRONLY);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "tune: /dev/pcm0: %s\n", strerror(errno));
        audio_free(&a);
        return 1;
    }
    printf("tune: playing %s (%ld frames)\n", argv[1], a.frames);
    int rc;
    if (a.channels == 2) {
        rc = write_all(fd, a.samples, a.frames * 4);        /* already stereo 16-bit */
    } else {                                                /* mono: duplicate to stereo */
        int16_t *st = malloc((size_t)a.frames * 4);
        const int16_t *m = a.samples;
        rc = st ? 0 : -1;
        for (long i = 0; st && i < a.frames; i++)
            st[i * 2] = st[i * 2 + 1] = m[i];
        if (st)
            rc = write_all(fd, st, a.frames * 4);
        free(st);
    }
    close(fd);
    audio_free(&a);
    if (rc < 0) {
        dprintf(STDERR_FILENO, "tune: playback failed: %s\n", strerror(errno));
        return 1;
    }
    printf("tune: done\n");
    return 0;
}

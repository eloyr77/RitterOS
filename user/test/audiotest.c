/*
 * Test: libaudio decode + round-trip. Decodes a WAV and prints its format and
 * the SHA-256 of its 16-bit samples (the host computes the same); then saves
 * it back out and reloads it, confirming the samples are identical.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "audio.h"
#include "ritteros.h"

static void sha_hex(const void *p, long n, char *hex)
{
    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, p, (size_t)n);
    uint8_t h[SHA256_SIZE];
    sha256_final(&s, h);
    for (int i = 0; i < SHA256_SIZE; i++)
        snprintf(hex + 2 * i, 3, "%02x", h[i]);
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    struct audio_buffer a;
    if (audio_load(argv[1], &a) < 0) {
        printf("audiotest: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    long bytes = a.frames * a.channels * 2;
    char hex[2 * SHA256_SIZE + 1];
    sha_hex(a.samples, bytes, hex);
    printf("audiotest: %s %dch %dHz %ld sha256 %s\n", argv[1], a.channels, a.rate, a.frames, hex);
    /* round-trip through save + load */
    if (audio_save("/home/root/rt.wav", &a) == 0) {
        struct audio_buffer b;
        if (audio_load("/home/root/rt.wav", &b) == 0) {
            char hex2[2 * SHA256_SIZE + 1];
            sha_hex(b.samples, b.frames * b.channels * 2, hex2);
            printf("audiotest: round-trip %s\n", strcmp(hex, hex2) == 0 ? "ok" : "FAIL");
            audio_free(&b);
        }
    }
    audio_free(&a);
    return 0;
}

/*
 * Test: libvideo decode. Reads every frame of a video and prints its geometry
 * and the SHA-256 of all the frames' RGBA8888 pixels, which the host computes
 * independently and compares.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ritteros.h"
#include "video.h"

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    struct video *v;
    if (video_open(argv[1], &v) < 0) {
        printf("videotest: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    int w, h, fps, has_audio;
    video_info(v, &w, &h, &fps, &has_audio);
    struct sha256 s;
    sha256_init(&s);
    struct image frame;
    int64_t pts;
    int n = 0;
    while (video_read_frame(v, &frame, &pts) == 1) {
        sha256_update(&s, frame.pixels, (size_t)frame.w * frame.h * 4);
        free(frame.pixels);
        n++;
    }
    uint8_t hash[SHA256_SIZE];
    sha256_final(&s, hash);
    char hex[2 * SHA256_SIZE + 1];
    for (int i = 0; i < SHA256_SIZE; i++)
        snprintf(hex + 2 * i, 3, "%02x", hash[i]);
    printf("videotest: %s %dx%d %dfps %dframes%s sha256 %s\n", argv[1], w, h, fps, n,
           has_audio ? " audio" : "", hex);
    video_close(v);
    return 0;
}

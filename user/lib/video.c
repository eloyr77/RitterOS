/*
 * libvideo (see video.h). The RVID container: a 36-byte header, the audio PCM
 * (16-bit, interleaved), then raw RGB888 frames. Our own simple format so a
 * player and its test have something real to run; a true codec would plug in
 * behind the same video_* interface.
 *
 * Header (little-endian): "RVID", u32 version(=1), u32 width, u32 height,
 * u32 fps, u32 frames, u32 audio_rate (0 = none), u32 audio_channels,
 * u32 audio_frames. Then audio_frames*channels*2 bytes of PCM, then
 * frames * width*height*3 bytes of RGB.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "video.h"

struct video {
    uint8_t *data;
    long size;
    int w, h, fps, frames, arate, achannels;
    long aframes;
    long audio_at, frames_at;           /* byte offsets */
    int next;                           /* next frame index */
};

static int fail(int e) { errno = e; return -1; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int video_open(const char *path, struct video **out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    long cap = 1 << 20, len = 0;
    uint8_t *buf = malloc((size_t)cap);
    if (!buf) { close(fd); return fail(ENOMEM); }
    for (;;) {
        if (len == cap) {
            uint8_t *nb = realloc(buf, (size_t)(cap * 2));
            if (!nb) { free(buf); close(fd); return fail(ENOMEM); }
            buf = nb, cap *= 2;
        }
        ssize_t r = read(fd, buf + len, (size_t)(cap - len));
        if (r < 0) { free(buf); close(fd); return -1; }
        if (r == 0) break;
        len += r;
    }
    close(fd);
    if (len < 36 || memcmp(buf, "RVID", 4) || le32(buf + 4) != 1) {
        free(buf);
        return fail(EINVAL);
    }
    struct video *v = calloc(1, sizeof(*v));
    if (!v) { free(buf); return fail(ENOMEM); }
    v->data = buf, v->size = len;
    v->w = (int)le32(buf + 8), v->h = (int)le32(buf + 12);
    v->fps = (int)le32(buf + 16), v->frames = (int)le32(buf + 20);
    v->arate = (int)le32(buf + 24), v->achannels = (int)le32(buf + 28);
    v->aframes = le32(buf + 32);
    v->audio_at = 36;
    v->frames_at = 36 + v->aframes * v->achannels * 2;
    if (v->w <= 0 || v->h <= 0 || v->fps <= 0 || v->frames < 0 ||
        v->frames_at + (long)v->frames * v->w * v->h * 3 > len) {
        free(buf); free(v);
        return fail(EINVAL);
    }
    *out = v;
    return 0;
}

void video_info(struct video *v, int *w, int *h, int *fps, int *has_audio)
{
    if (w) *w = v->w;
    if (h) *h = v->h;
    if (fps) *fps = v->fps;
    if (has_audio) *has_audio = v->arate > 0;
}

int video_read_frame(struct video *v, struct image *frame, int64_t *pts_ns)
{
    if (v->next >= v->frames)
        return 0;
    long fsz = (long)v->w * v->h * 3;
    const uint8_t *s = v->data + v->frames_at + (long)v->next * fsz;
    uint8_t *px = malloc((size_t)v->w * v->h * 4);
    if (!px)
        return fail(ENOMEM);
    for (long i = 0; i < (long)v->w * v->h; i++) {
        px[i * 4 + 0] = s[i * 3 + 0];
        px[i * 4 + 1] = s[i * 3 + 1];
        px[i * 4 + 2] = s[i * 3 + 2];
        px[i * 4 + 3] = 255;
    }
    frame->w = v->w, frame->h = v->h, frame->stride = v->w * 4;
    frame->format = PIXEL_RGBA8888, frame->pixels = px;
    if (pts_ns)
        *pts_ns = (int64_t)v->next * 1000000000 / v->fps;
    v->next++;
    return 1;
}

int video_audio(struct video *v, struct audio_buffer *out)
{
    if (v->arate <= 0)
        return fail(EINVAL);
    long bytes = v->aframes * v->achannels * 2;
    int16_t *s = malloc((size_t)bytes);
    if (!s)
        return fail(ENOMEM);
    memcpy(s, v->data + v->audio_at, (size_t)bytes);
    out->rate = v->arate, out->channels = v->achannels, out->format = SAMPLE_S16;
    out->frames = v->aframes, out->samples = s;
    return 0;
}

void video_close(struct video *v)
{
    if (v) {
        free(v->data);
        free(v);
    }
}

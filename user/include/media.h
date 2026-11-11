/*
 * media.h: the data types shared by the media libraries -- libimage and
 * libvideo pass frames as a `struct image`, libaudio and libvideo pass sound
 * as a `struct audio_buffer`. Both hold decoded data in memory the caller
 * frees. Kept free of any display or hardware dependency on purpose, so a GPU
 * or a sound card later changes drivers, not these types.
 */
#ifndef RITTEROS_MEDIA_H
#define RITTEROS_MEDIA_H

#include <stdint.h>

/* A pixel's byte layout in `struct image`. libimage decodes to RGBA8888
 * (bytes R, G, B, A); the others are for conversions. */
enum pixel_format {
    PIXEL_RGBA8888,             /* 4 bytes per pixel: R, G, B, A */
    PIXEL_RGB888,               /* 3 bytes per pixel: R, G, B */
    PIXEL_GRAY8                 /* 1 byte per pixel */
};

/* A decoded image (or one video frame). `pixels` is malloc'd; `image_free`
 * releases it. `stride` is the bytes per row (>= w * bytes-per-pixel). */
struct image {
    int w, h;
    int stride;
    enum pixel_format format;
    uint8_t *pixels;
};

/* A sample's type in `struct audio_buffer`. */
enum sample_format {
    SAMPLE_S16,                 /* signed 16-bit */
    SAMPLE_F32                  /* 32-bit float, -1.0 .. 1.0 */
};

/* Decoded PCM audio, interleaved by channel. `samples` is malloc'd;
 * `audio_free` releases it. */
struct audio_buffer {
    int rate;                   /* samples per second per channel, e.g. 44100 */
    int channels;               /* 1 mono, 2 stereo */
    enum sample_format format;
    long frames;                /* samples per channel */
    void *samples;              /* frames * channels * bytes-per-sample */
};

#endif
